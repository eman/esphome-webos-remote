// Capture mode: the C6 plays the TV (BLE central) for a real LG Magic Remote, to record what a genuine remote sends.
// Logs its advertisements, its whole GATT table and every readable value, subscribes to all of its reports, sends
// the F9 requests the TV sends (13, 11, 19, 90, 17, then 01 = activeMotion), and logs the replies. FD reports
// (keys and motion) are logged in part and kept in a RAM ring buffer for a paced dump (lgmr_cap_dump).
// Runs beside the TV-side peripheral (CONFIG_BT_NIMBLE_MAX_CONNECTIONS 2); it never touches that link.
#include "lgmr_ble.h"

#ifdef LGMR_CAPTURE

#include <stdio.h>
#include <string.h>

#include <esp_timer.h>
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"

#define LOGW(...) lgmr_logf(2, __VA_ARGS__)
#define LOGI(...) lgmr_logf(3, __VA_ARGS__)

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }

static volatile bool active;       // capture mode on
static volatile bool scanning;
static volatile uint16_t cconn = BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t peer;
static volatile bool ready;        // discovery, reads and subscriptions done
static volatile int auto_cmd = -1;  // index into AUTO, -1 = idle
static uint32_t auto_next;

/* ---- attribute table ---- */
#define MAX_CHR 48
#define MAX_DSC 64
struct chr { uint16_t def, val, end; uint8_t props; uint16_t uuid16; uint8_t rid, rtype; };
struct dsc { uint16_t handle, uuid16, chr; };
static struct chr chrs[MAX_CHR];
static int n_chr;
static struct dsc dscs[MAX_DSC];
static int n_dsc;
static int step;  // index for sequential reads / subscriptions
static uint8_t rbuf[600];
static uint16_t rlen;

static void hexs(const uint8_t *d, int n, char *out, int max) {
  int o = 0;
  for (int i = 0; i < n && o + 3 < max; i++)
    o += sprintf(out + o, "%02x", d[i]);
  out[o] = 0;
}
static const char *astr(const ble_addr_t *a, char *b) {
  sprintf(b, "%02X:%02X:%02X:%02X:%02X:%02X/%d", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0],
          a->type);
  return b;
}
static uint16_t u16of(const ble_uuid_any_t *u) { return u->u.type == BLE_UUID_TYPE_16 ? u->u16.value : 0; }

static struct chr *chr_for(uint16_t h) {
  for (int i = 0; i < n_chr; i++)
    if (h > chrs[i].val && h <= chrs[i].end)
      return &chrs[i];
  return NULL;
}
static struct chr *chr_by_val(uint16_t h) {
  for (int i = 0; i < n_chr; i++)
    if (chrs[i].val == h)
      return &chrs[i];
  return NULL;
}
static struct chr *report(uint8_t id, uint8_t type) {
  for (int i = 0; i < n_chr; i++)
    if (chrs[i].uuid16 == 0x2A4D && chrs[i].rid == id && chrs[i].rtype == type)
      return &chrs[i];
  return NULL;
}

/* ---- FD ring buffer ---- */
#define RING 1500
struct rec { uint32_t t; uint8_t len; uint8_t d[20]; };
static struct rec ring[RING];
static volatile uint32_t ring_w;   // total written
static uint32_t dump_r, dump_end;  // paced dump
static volatile bool dumping;
static uint32_t fd_count, fd_sec_start, fd_sec_count;

static int gap_cb(struct ble_gap_event *event, void *arg);
static void next_read(void);
static void next_sub(void);

static void start_scan(void) {
  if (!active || cconn != BLE_HS_CONN_HANDLE_NONE)
    return;
  struct ble_gap_ext_disc_params p = {.itvl = BLE_GAP_SCAN_ITVL_MS(50), .window = BLE_GAP_SCAN_WIN_MS(40), .passive = 0};
  int rc = ble_gap_ext_disc(lgmr_own_addr_type(), 0, 0, 0, BLE_HCI_SCAN_FILT_NO_WL, 0, &p, NULL, gap_cb, NULL);
  scanning = rc == 0;
  LOGI("capture: scanning for LG remotes: %d", rc);
}

/* ---- GATT client chain: services -> characteristics -> descriptors -> reads -> subscriptions ---- */
static int on_dsc(uint16_t c, const struct ble_gatt_error *e, uint16_t chr_val, const struct ble_gatt_dsc *d, void *a) {
  if (e->status == 0) {
    uint16_t u = u16of(&d->uuid);
    struct chr *ch = chr_for(d->handle);
    if (ch && (u == 0x2902 || u == 0x2908 || u == 0x2901 || u == 0x2904) && n_dsc < MAX_DSC)
      dscs[n_dsc++] = (struct dsc){d->handle, u, ch->val};
    if (ch && d->handle > ch->val) {
      char ub[BLE_UUID_STR_LEN];
      LOGI("capture:   dsc %u uuid %s (chr %u)", d->handle, ble_uuid_to_str(&d->uuid.u, ub), ch->val);
    }
    return 0;
  }
  LOGI("capture: descriptors done (%d kept, status %d); reading values", n_dsc, e->status);
  step = 0;
  next_read();
  return 0;
}

static int on_chr(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_chr *ch, void *a) {
  if (e->status == 0) {
    char ub[BLE_UUID_STR_LEN];
    LOGI("capture:  chr def %u val %u props 0x%02x uuid %s", ch->def_handle, ch->val_handle, ch->properties,
         ble_uuid_to_str(&ch->uuid.u, ub));
    if (n_chr < MAX_CHR) {
      if (n_chr > 0 && chrs[n_chr - 1].end > ch->def_handle - 1)
        chrs[n_chr - 1].end = ch->def_handle - 1;
      chrs[n_chr++] = (struct chr){ch->def_handle, ch->val_handle, 0xffff, ch->properties, u16of(&ch->uuid), 0, 0};
    }
    return 0;
  }
  LOGI("capture: %d characteristics (status %d); discovering descriptors", n_chr, e->status);
  int rc = ble_gattc_disc_all_dscs(c, 1, 0xffff, on_dsc, NULL);
  if (rc)
    LOGW("capture: disc_all_dscs: %d", rc);
  return 0;
}

static int on_svc(uint16_t c, const struct ble_gatt_error *e, const struct ble_gatt_svc *s, void *a) {
  if (e->status == 0) {
    char ub[BLE_UUID_STR_LEN];
    LOGI("capture: service %u-%u uuid %s", s->start_handle, s->end_handle, ble_uuid_to_str(&s->uuid.u, ub));
    // a characteristic never extends past its service
    return 0;
  }
  n_chr = 0;
  n_dsc = 0;
  int rc = ble_gattc_disc_all_chrs(c, 1, 0xffff, on_chr, NULL);
  if (rc)
    LOGW("capture: disc_all_chrs: %d", rc);
  return 0;
}

static void log_value(const char *what, uint16_t h, const uint8_t *d, int n) {
  char hx[1210];
  hexs(d, n, hx, sizeof(hx));
  LOGI("capture: %s %u (%d bytes): %s", what, h, n, hx);
}

// Reads: every readable characteristic, then every Report Reference / user description / format descriptor.
static int on_read(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *at, void *a) {
  uint16_t h = (uint16_t) (uintptr_t) a;
  if (e->status == 0 && at) {
    uint16_t l = OS_MBUF_PKTLEN(at->om);
    if (rlen + l <= sizeof(rbuf)) {
      os_mbuf_copydata(at->om, 0, l, rbuf + rlen);
      rlen += l;
    }
    return 0;
  }
  if (e->status == BLE_HS_EDONE) {
    log_value("read", h, rbuf, rlen);
    for (int i = 0; i < n_dsc; i++)
      if (dscs[i].handle == h && dscs[i].uuid16 == 0x2908 && rlen >= 2) {
        struct chr *ch = chr_by_val(dscs[i].chr);
        if (ch) {
          ch->rid = rbuf[0];
          ch->rtype = rbuf[1];
          LOGI("capture: report %02X type %d (1 in, 2 out, 3 feature) = handle %u, props 0x%02x", rbuf[0], rbuf[1],
               ch->val, ch->props);
        }
      }
  } else {
    LOGI("capture: read %u failed: 0x%x", h, e->status);
  }
  step++;
  next_read();
  return 0;
}

static void next_read(void) {
  if (cconn == BLE_HS_CONN_HANDLE_NONE)
    return;
  int total = n_chr + n_dsc;
  while (step < total) {
    uint16_t h = 0;
    if (step < n_chr) {
      if (chrs[step].props & BLE_GATT_CHR_PROP_READ)
        h = chrs[step].val;
    } else {
      struct dsc *d = &dscs[step - n_chr];
      if (d->uuid16 != 0x2902)
        h = d->handle;
    }
    if (h) {
      rlen = 0;
      int rc = ble_gattc_read_long(cconn, h, 0, on_read, (void *) (uintptr_t) h);
      if (rc == 0)
        return;
      LOGW("capture: read %u: %d", h, rc);
    }
    step++;
  }
  LOGI("capture: reads done; subscribing");
  step = 0;
  next_sub();
}

static int on_sub(uint16_t c, const struct ble_gatt_error *e, struct ble_gatt_attr *at, void *a) {
  LOGI("capture: subscribe via %u: status 0x%x", (unsigned) (uintptr_t) a, e->status);
  step++;
  next_sub();
  return 0;
}

static void next_sub(void) {
  if (cconn == BLE_HS_CONN_HANDLE_NONE)
    return;
  while (step < n_dsc) {
    struct dsc *d = &dscs[step];
    struct chr *ch = chr_by_val(d->chr);
    if (d->uuid16 == 0x2902 && ch) {
      uint8_t v[2] = {(ch->props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2, 0};
      int rc = ble_gattc_write_flat(cconn, d->handle, v, 2, on_sub, (void *) (uintptr_t) d->handle);
      if (rc == 0)
        return;
      LOGW("capture: subscribe %u: %d", d->handle, rc);
    }
    step++;
  }
  ready = true;
  struct chr *o = report(0xF9, 2), *f = report(0xF9, 3), *fd = report(0xFD, 1);
  LOGI("capture: ready. F9 out %u, F9 feature %u, FD in %u. Sending the TV's requests in 1 s.", o ? o->val : 0,
       f ? f->val : 0, fd ? fd->val : 0);
  auto_cmd = 0;
  auto_next = now_ms() + 1000;
}

/* ---- commands to the remote ---- */
int lgmr_cap_cmd(const uint8_t *d, uint16_t n) {
  if (cconn == BLE_HS_CONN_HANDLE_NONE || !ready)
    return -1;
  struct chr *o = report(0xF9, 2);
  if (!o)
    o = report(0xF9, 3);
  if (!o) {
    LOGW("capture: no F9 output or feature report");
    return -2;
  }
  int rc = (o->props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) ? ble_gattc_write_no_rsp_flat(cconn, o->val, d, n)
                                                       : ble_gattc_write_flat(cconn, o->val, d, n, NULL, NULL);
  char hx[80];
  hexs(d, n, hx, sizeof(hx));
  LOGI("capture: F9 -> remote (handle %u): %s: %d", o->val, hx, rc);
  return rc;
}

static const uint8_t AUTO[][1] = {{0x13}, {0x11}, {0x19}, {0x90}, {0x17}, {0x01}};

/* ---- GAP ---- */
static bool lg_adv(const uint8_t *d, uint8_t n, char *name, int name_max, uint8_t *mfg, uint8_t *mfg_len) {
  struct ble_hs_adv_fields f;
  name[0] = 0;
  *mfg_len = 0;
  if (ble_hs_adv_parse_fields(&f, d, n) != 0)
    return false;
  if (f.name && f.name_len) {
    int l = f.name_len < name_max - 1 ? f.name_len : name_max - 1;
    memcpy(name, f.name, l);
    name[l] = 0;
  }
  if (f.mfg_data && f.mfg_data_len) {
    *mfg_len = f.mfg_data_len > 40 ? 40 : f.mfg_data_len;
    memcpy(mfg, f.mfg_data, *mfg_len);
  }
  bool lg_mfg = *mfg_len >= 2 && mfg[0] == 0xC4 && mfg[1] == 0x00;
  return lg_mfg || strncmp(name, "LGE", 3) == 0;
}

// A remote, not the TV (which also advertises LG manufacturer data): named "LGE MR..", or LG data with "SCD".
static bool remote_like(const char *name, const uint8_t *mfg, uint8_t ml) {
  if (strncmp(name, "LGE MR", 6) == 0)
    return true;
  for (int i = 2; i + 3 <= ml; i++)
    if (mfg[i] == 'S' && mfg[i + 1] == 'C' && mfg[i + 2] == 'D')
      return true;
  return false;
}

static int gap_cb(struct ble_gap_event *event, void *arg) {
  char ab[24], ab2[24];
  struct ble_gap_conn_desc desc;
  switch (event->type) {
    case BLE_GAP_EVENT_EXT_DISC: {
      const struct ble_gap_ext_disc_desc *e = &event->ext_disc;
      char name[32], hx[200];
      uint8_t mfg[40], ml;
      bool directed = e->props & BLE_HCI_ADV_DIRECT_MASK;
      bool lg = lg_adv(e->data, e->length_data, name, sizeof(name), mfg, &ml);
      // Every advertiser above -75 dBm, once per address per 10 s (to find a remote that doesn't look like LG).
      {
        static ble_addr_t seen[24];
        static uint32_t seen_t[24];
        int slot = -1, oldest = 0;
        for (int i = 0; i < 24; i++) {
          if (memcmp(&seen[i], &e->addr, sizeof(ble_addr_t)) == 0) { slot = i; break; }
          if (seen_t[i] < seen_t[oldest]) oldest = i;
        }
        if (e->rssi > -75 && (slot < 0 || now_ms() - seen_t[slot] > 10000)) {
          if (slot < 0) { slot = oldest; seen[slot] = e->addr; }
          seen_t[slot] = now_ms();
          hexs(e->data, e->length_data, hx, sizeof(hx));
          LOGI("capture: near %s rssi %d props 0x%02x%s%s name '%s' data %s", astr(&e->addr, ab), e->rssi, e->props,
               directed ? " directed to " : "", directed ? astr(&e->direct_addr, ab2) : "", name, hx);
        }
      }
      if (!lg)
        return 0;
      // log each address + payload once per 3 s
      static ble_addr_t la;
      static uint8_t llen, lprops;
      static uint32_t lt;
      if (memcmp(&la, &e->addr, sizeof(la)) || llen != e->length_data || lprops != e->props || now_ms() - lt > 3000) {
        la = e->addr;
        llen = e->length_data;
        lprops = e->props;
        lt = now_ms();
        hexs(e->data, e->length_data, hx, sizeof(hx));
        LOGI("capture: adv %s rssi %d props 0x%02x%s%s name '%s' data %s", astr(&e->addr, ab), e->rssi, e->props,
             directed ? " directed to " : "", directed ? astr(&e->direct_addr, ab2) : "", name, hx);
      }
      bool own_name = strcmp(name, lgmr_device_name()) == 0;
      static uint32_t last_try;
      struct ble_gap_conn_desc cd;
      if (lg && !own_name && remote_like(name, mfg, ml) && (e->props & BLE_HCI_ADV_CONN_MASK) &&
          cconn == BLE_HS_CONN_HANDLE_NONE && active && ble_gap_conn_find_by_addr(&e->addr, &cd) != 0 &&
          now_ms() - last_try > 5000) {
        last_try = now_ms();
        ble_gap_disc_cancel();
        scanning = false;
        peer = e->addr;
        struct ble_gap_conn_params cp = {.scan_itvl = 16, .scan_window = 16, .itvl_min = 6, .itvl_max = 12,
                                         .latency = 0, .supervision_timeout = 300, .min_ce_len = 0, .max_ce_len = 0};
        int rc = ble_gap_ext_connect(lgmr_own_addr_type(), &peer, 10000, BLE_GAP_LE_PHY_1M_MASK, &cp, NULL, NULL,
                                     gap_cb, NULL);
        LOGI("capture: connecting to %s ('%s'): %d", astr(&peer, ab), name, rc);
        if (rc)
          start_scan();
      }
      return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
      scanning = false;
      return 0;

    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status) {
        LOGW("capture: connection failed: %d", event->connect.status);
        start_scan();
        return 0;
      }
      cconn = event->connect.conn_handle;
      ready = false;
      auto_cmd = -1;
      if (ble_gap_conn_find(cconn, &desc) == 0)
        LOGI("capture: connected to %s, itvl %u, latency %u, bonded %d", astr(&desc.peer_id_addr, ab), desc.conn_itvl,
             desc.conn_latency, desc.sec_state.bonded);
      LOGI("capture: security: %d", ble_gap_security_initiate(cconn));
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
        LOGI("capture: encryption status 0x%x, encrypted %d, bonded %d, key size %d", event->enc_change.status,
             desc.sec_state.encrypted, desc.sec_state.bonded, desc.sec_state.key_size);
      ble_gattc_exchange_mtu(event->enc_change.conn_handle, NULL, NULL);
      {
        int rc = ble_gattc_disc_all_svcs(event->enc_change.conn_handle, on_svc, NULL);
        LOGI("capture: discovering services: %d", rc);
      }
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      // An old bond with this remote: drop it and pair again.
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      LOGI("capture: passkey action %d", event->passkey.params.action);
      if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
        struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1};
        ble_sm_inject_io(event->passkey.conn_handle, &io);
      }
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      uint8_t d[260];
      uint16_t n = OS_MBUF_PKTLEN(event->notify_rx.om);
      if (n > sizeof(d))
        n = sizeof(d);
      os_mbuf_copydata(event->notify_rx.om, 0, n, d);
      uint16_t h = event->notify_rx.attr_handle;
      struct chr *ch = chr_by_val(h);
      uint32_t t = now_ms();
      if (ch && ch->rid == 0xFD) {
        struct rec *r = &ring[ring_w % RING];
        r->t = t;
        r->len = n > 20 ? 20 : n;
        memcpy(r->d, d, r->len);
        ring_w++;
        fd_count++;
        if (t - fd_sec_start >= 1000) {
          if (fd_sec_count)
            LOGI("capture: FD %u reports in the last %u ms (%u total)", (unsigned) fd_sec_count,
                 (unsigned) (t - fd_sec_start), (unsigned) fd_count);
          fd_sec_start = t;
          fd_sec_count = 0;
        }
        fd_sec_count++;
        bool key = n >= 19 && (d[16] || d[17] || d[18]);
        if (fd_count <= 40 || fd_count % 50 == 0 || key) {
          char hx[60];
          hexs(d, n, hx, sizeof(hx));
          LOGI("capture: FD #%u %s", (unsigned) fd_count, hx);
        }
      } else {
        char hx[530];
        hexs(d, n, hx, sizeof(hx));
        LOGI("capture: notify %s%02X handle %u (%u bytes): %s", ch ? "report " : "", ch ? ch->rid : 0, h, n, hx);
      }
      return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      LOGI("capture: remote asks for interval %u-%u, latency %u, timeout %u", event->conn_update_req.peer_params->itvl_min,
           event->conn_update_req.peer_params->itvl_max, event->conn_update_req.peer_params->latency,
           event->conn_update_req.peer_params->supervision_timeout);
      return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
        LOGI("capture: connection now itvl %u, latency %u, timeout %u", desc.conn_itvl, desc.conn_latency,
             desc.supervision_timeout);
      return 0;
    case BLE_GAP_EVENT_MTU:
      LOGI("capture: MTU %u", event->mtu.value);
      return 0;
    case BLE_GAP_EVENT_PARING_COMPLETE:
      LOGI("capture: pairing complete: status 0x%x", event->pairing_complete.status);
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      LOGI("capture: remote disconnected, reason 0x%x (%u FD reports)", event->disconnect.reason, (unsigned) fd_count);
      cconn = BLE_HS_CONN_HANDLE_NONE;
      ready = false;
      auto_cmd = -1;
      start_scan();
      return 0;
  }
  return 0;
}

/* ---- control, from the main loop ---- */
void lgmr_cap_start(void) {
  active = true;
  fd_count = 0;
  ring_w = 0;
  start_scan();
}

void lgmr_cap_stop(void) {
  active = false;
  if (scanning)
    ble_gap_disc_cancel();
  scanning = false;
  if (cconn != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(cconn, BLE_ERR_REM_USER_CONN_TERM);
  LOGI("capture: stopped");
}

void lgmr_cap_forget(void) {
  if (peer.val[0] || peer.val[5]) {
    int rc = ble_gap_unpair(&peer);
    char ab[24];
    LOGI("capture: bond with %s deleted: %d", astr(&peer, ab), rc);
  }
}

void lgmr_cap_dump(void) {
  uint32_t w = ring_w;
  dump_r = w > RING ? w - RING : 0;
  dump_end = w;
  dumping = true;
  LOGI("capture: dumping %u FD reports (of %u)", (unsigned) (dump_end - dump_r), (unsigned) w);
}

bool lgmr_cap_active(void) { return active; }

void lgmr_cap_poll(void) {
  uint32_t t = now_ms();
  if (auto_cmd >= 0 && ready && (int32_t) (t - auto_next) >= 0) {
    lgmr_cap_cmd(AUTO[auto_cmd], 1);
    auto_cmd++;
    auto_next = t + 700;
    if (auto_cmd >= (int) (sizeof(AUTO) / sizeof(AUTO[0]))) {
      auto_cmd = -1;
      LOGI("capture: requests sent; move the remote now (don't press keys)");
    }
  }
  static uint32_t last_dump;
  if (dumping && t - last_dump >= 6) {
    last_dump = t;
    if (dump_r >= dump_end) {
      dumping = false;
      LOGI("capture: dump done");
      return;
    }
    struct rec *r = &ring[dump_r % RING];
    char hx[50];
    hexs(r->d, r->len, hx, sizeof(hx));
    LOGI("FDLOG %u %u %s", (unsigned) dump_r, (unsigned) r->t, hx);
    dump_r++;
  }
}

#else  // capture mode not built (debug: capture: false): these do nothing

void lgmr_cap_start(void) { lgmr_logf(2, "capture mode is not built in (set debug: capture: true)"); }
void lgmr_cap_stop(void) {}
void lgmr_cap_forget(void) {}
void lgmr_cap_dump(void) {}
bool lgmr_cap_active(void) { return false; }
int lgmr_cap_cmd(const uint8_t *d, uint16_t n) { return -1; }
void lgmr_cap_poll(void) {}

#endif  // LGMR_CAPTURE
