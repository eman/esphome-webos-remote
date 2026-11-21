#include <esp_timer.h>
#include "lgmr_ble.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

#define LOGE(...) lgmr_logf(1, __VA_ARGS__)
#define LOGW(...) lgmr_logf(2, __VA_ARGS__)
#define LOGI(...) lgmr_logf(3, __VA_ARGS__)

static lgmr_write_cb_t write_cb;
static lgmr_subscribed_cb_t sub_cb;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool synced;
static volatile bool adv_wanted;
static volatile bool sub[3];
// Nothing is notified before the link is encrypted: a notification racing a bonded TV's encryption start
// ended the link with 0x3D (MIC failure) on 2026-11-19.
static volatile bool encrypted;
static volatile bool sec_wait;  // bonded TV connected; send a Security Request if it hasn't encrypted in 0.5 s
static volatile uint32_t sec_wait_since;
static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
static uint8_t own_addr_type;
static uint8_t bt_addr[6];
static bool bt_addr_set;

void lgmr_set_address(const uint8_t addr[6]) {
  memcpy(bt_addr, addr, 6);
  bt_addr_set = true;
}

static char dev_name[32] = "LGE MR23";
static uint8_t adv_data[31];
static uint8_t adv_len;
static uint8_t rsp_data[31];
static uint8_t rsp_len;
static uint8_t adv_flags = 0x05;  // LE Limited Discoverable, no BR/EDR, as the real remote

/* ---- GATT values (MR25GB, github.com/wittenbe/magic-remote-bridge docs/protocol.md) ---- */

static const uint8_t REPORT_MAP[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xfd, 0x95, 0x1e, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81,
    0x00, 0xc0, 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0xf9, 0x95, 0x1e, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x81, 0x00, 0x95, 0xc8, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0xb1, 0x00, 0xc0, 0x05, 0x0c, 0x09,
    0x01, 0xa1, 0x01, 0x85, 0xfe, 0x95, 0x7c, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x81, 0x00, 0xc0,
};
static const uint8_t HID_INFO[] = {0x00, 0x00, 0x00, 0x01};
static const uint8_t EXT_REPORT_REF[] = {0x19, 0x2a};
static uint8_t protocol_mode = 0x01;
static const uint8_t BATTERY[] = {100};
// Broadcom 000f:3412: the AN-MR19BA / MR20 family (sirfragles/lgmagic). The MR25 is Realtek 005d:8762.
static const uint8_t PNP_ID[] = {0x01, 0x0f, 0x00, 0x12, 0x34, 0x00, 0x01};
static const uint8_t SYSTEM_ID[] = {0x00, 0x01, 0x02, 0x00, 0x00, 0x03, 0x04, 0x05};

typedef struct {
  const void *data;
  uint16_t len;
  const char *label;
} blob_t;
#define BLOB(x, l) (&(blob_t){(x), sizeof(x), (l)})
#define STR(s, l) (&(blob_t){(s), sizeof(s) - 1, (l)})
#define EMPTY(l) (&(blob_t){NULL, 0, (l)})

typedef struct {
  uint8_t ref[2];
  const char *desc;
  const char *label;
  bool read_error;  // the real remote answers reads with ATT 0x0A
} report_t;

static const report_t R_F9_IN = {{0xf9, 0x01}, "HID_0xF9_IN_USER_SESCR", "F9 in", true};
static const report_t R_F9_FEAT = {{0xf9, 0x03}, "HID_0xF9_FEATURE_USER_SESCR", "F9 feature", true};
static const report_t R_FD_IN = {{0xfd, 0x01}, "HID_0xFD_IN_USER_SESCR", "FD in", false};
static const report_t R_FE_IN = {{0xfe, 0x01}, "HID_0xFE_IN_USER_SESCR", "FE in", true};
static const report_t R_F9_OUT = {{0xf9, 0x02}, "HID_0xF9_OUT_USER_SESCR", "F9 out", true};

static uint16_t h_f9_in, h_fd_in, h_fe_in, h_battery, h_a001;

static int append(struct ble_gatt_access_ctxt *ctxt, const void *data, uint16_t len) {
  return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void forward_write(const char *what, struct ble_gatt_access_ctxt *ctxt) {
  uint8_t buf[256];
  uint16_t len = 0;
  ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
  if (write_cb)
    write_cb(what, buf, len);
}

static int blob_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  const blob_t *b = arg;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
    LOGI("TV read %s (0x%04x)", b->label, attr_handle);
    return b->len ? append(ctxt, b->data, b->len) : 0;
  }
  forward_write(b->label, ctxt);
  return 0;
}

static int report_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  const report_t *r = arg;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    LOGI("TV read report %s", r->label);
    return r->read_error ? BLE_ATT_ERR_ATTR_NOT_FOUND : 0;
  }
  forward_write(r->label, ctxt);
  return 0;
}

static int report_dsc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                             void *arg) {
  const report_t *r = arg;
  uint16_t uuid = ble_uuid_u16(ctxt->dsc->uuid);
  if (uuid == 0x2908) {
    LOGI("TV read report reference of %s", r->label);
    return append(ctxt, r->ref, 2);
  }
  return append(ctxt, r->desc, strlen(r->desc));
}

static int protocol_mode_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                                void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    LOGI("TV read protocol mode");
    return append(ctxt, &protocol_mode, 1);
  }
  ble_hs_mbuf_to_flat(ctxt->om, &protocol_mode, 1, NULL);
  LOGI("TV set protocol mode %u", protocol_mode);
  return 0;
}

#define REPORT_DSCS(r) \
  (struct ble_gatt_dsc_def[]) { \
    {.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ, .access_cb = report_dsc_access, \
     .arg = (void *) &(r)}, \
        {.uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, .access_cb = report_dsc_access, \
         .arg = (void *) &(r)}, \
        {0}, \
  }

#define RO(uuid16, blob) \
  { .uuid = BLE_UUID16_DECLARE(uuid16), .access_cb = blob_access, .arg = (void *) (blob), .flags = BLE_GATT_CHR_F_READ }

static const ble_uuid128_t UUID_D1FF =
    BLE_UUID128_INIT(0x12, 0xa2, 0x4d, 0x2e, 0xfe, 0x14, 0x48, 0x8e, 0x93, 0xd2, 0x17, 0x3c, 0xff, 0xd1, 0x00, 0x00);

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),  // HID
        .characteristics =
            (struct ble_gatt_chr_def[]){
                RO(0x2a4a, BLOB(HID_INFO, "HID information")),
                {.uuid = BLE_UUID16_DECLARE(0x2a4c), .access_cb = blob_access, .arg = (void *) EMPTY("HID control point"),
                 .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = BLE_UUID16_DECLARE(0x2a4e), .access_cb = protocol_mode_access,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = BLE_UUID16_DECLARE(0x2a4b), .access_cb = blob_access,
                 .arg = (void *) BLOB(REPORT_MAP, "report map"), .flags = BLE_GATT_CHR_F_READ,
                 .descriptors =
                     (struct ble_gatt_dsc_def[]){
                         {.uuid = BLE_UUID16_DECLARE(0x2907), .att_flags = BLE_ATT_F_READ, .access_cb = blob_access,
                          .arg = (void *) BLOB(EXT_REPORT_REF, "external report reference")},
                         {0}}},
                {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *) &R_F9_IN,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_f9_in,
                 .descriptors = REPORT_DSCS(R_F9_IN)},
                {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *) &R_F9_FEAT,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                 .descriptors = REPORT_DSCS(R_F9_FEAT)},
                {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *) &R_FD_IN,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &h_fd_in, .descriptors = REPORT_DSCS(R_FD_IN)},
                {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *) &R_FE_IN,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_fe_in,
                 .descriptors = REPORT_DSCS(R_FE_IN)},
                {.uuid = BLE_UUID16_DECLARE(0x2a4d), .access_cb = report_access, .arg = (void *) &R_F9_OUT,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                 .descriptors = REPORT_DSCS(R_F9_OUT)},
                {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180f),  // Battery
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = BLE_UUID16_DECLARE(0x2a19), .access_cb = blob_access, .arg = (void *) BLOB(BATTERY, "battery"),
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_battery},
                {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180a),  // Device Information
        .characteristics =
            (struct ble_gatt_chr_def[]){
                RO(0x2a29, STR("Realtek BT", "DIS manufacturer")), RO(0x2a24, STR("Model Nbr 0.9", "DIS model")),
                RO(0x2a25, STR("RTKBeeSerialNum", "DIS serial")), RO(0x2a27, STR("RTKBeeHardwareRev", "DIS hw rev")),
                RO(0x2a26, STR("RTKBeeFirmwareRev", "DIS fw rev")), RO(0x2a28, STR("RTKBeeSoftwareRev", "DIS sw rev")),
                RO(0x2a23, BLOB(SYSTEM_ID, "DIS system id")), RO(0x2a2a, STR("RTKBeeIEEEDatalist", "DIS IEEE")),
                RO(0x2a50, BLOB(PNP_ID, "DIS PnP ID")), {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1813),  // Scan Parameters
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = BLE_UUID16_DECLARE(0x2a4f), .access_cb = blob_access, .arg = (void *) EMPTY("scan interval"),
                 .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = BLE_UUID16_DECLARE(0x2a31), .access_cb = blob_access, .arg = (void *) EMPTY("scan refresh"),
                 .flags = BLE_GATT_CHR_F_NOTIFY},
                {0}},
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_D1FF.u,  // LG vendor service (a 2025 TV never used it; logged in case a 2019 one does)
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = BLE_UUID16_DECLARE(0xa001), .access_cb = blob_access, .arg = (void *) EMPTY("LG a001"),
                 .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_a001},
                {.uuid = BLE_UUID16_DECLARE(0xa002), .access_cb = blob_access, .arg = (void *) EMPTY("LG a002"),
                 .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE},
                {0}},
    },
    {0},
};

/* ---- advertising ---- */

static bool adv_hid;      // add HID service UUID 0x1812 and appearance 0x03C1 to the advertisement
static bool adv_name_main; // name in the advertisement (seen by passive scanners), LG data in the scan response

static uint8_t *put_field(uint8_t *p, const uint8_t *limit, uint8_t type, const uint8_t *data, uint8_t len) {
  if (p + 2 + len > limit)
    len = (uint8_t) (limit - p > 2 ? limit - p - 2 : 0);
  if (!len && type != 0xff)
    return p;
  *p++ = (uint8_t) (len + 1);
  *p++ = type;
  memcpy(p, data, len);
  return p + len;
}

static void build_adv(const uint8_t *mfg, uint8_t mfg_len) {
  uint8_t lg[2 + 24];
  lg[0] = 0xc4;  // company 0x00C4 = LG Electronics
  lg[1] = 0x00;
  if (mfg_len > 24)
    mfg_len = 24;
  memcpy(lg + 2, mfg, mfg_len);
  uint8_t name_len = (uint8_t) strnlen(dev_name, 29);

  uint8_t *p = adv_data, *lim = adv_data + 31;
  p = put_field(p, lim, 0x01, &adv_flags, 1);
  if (adv_hid) {
    static const uint8_t uuid[] = {0x12, 0x18}, app[] = {0xc1, 0x03};
    p = put_field(p, lim, 0x03, uuid, 2);
    p = put_field(p, lim, 0x19, app, 2);
  }
  if (adv_name_main)
    p = put_field(p, lim, 0x09, (const uint8_t *) dev_name, name_len);
  else if (mfg_len)
    p = put_field(p, lim, 0xff, lg, (uint8_t) (2 + mfg_len));
  adv_len = (uint8_t) (p - adv_data);

  p = rsp_data;
  lim = rsp_data + 31;
  if (adv_name_main) {
    if (mfg_len)
      p = put_field(p, lim, 0xff, lg, (uint8_t) (2 + mfg_len));
  } else {
    p = put_field(p, lim, 0x09, (const uint8_t *) dev_name, name_len);
  }
  rsp_len = (uint8_t) (p - rsp_data);
}

static int gap_event(struct ble_gap_event *event, void *arg);

static bool adv_on(void) { return ble_gap_ext_adv_active(0); }
static void adv_off(void) {
  if (ble_gap_ext_adv_active(0))
    ble_gap_ext_adv_stop(0);
}

static void hexify(const uint8_t *d, int n, char *out) {
  int o = 0;
  for (int i = 0; i < n && o < 96; i++)
    o += sprintf(out + o, "%02x", d[i]);
  out[o] = 0;
}

// Set to make the next advertise() a 1.28 s high-duty directed burst to the bonded TV, which is how a bonded
// remote reconnects (after a failed reconnect the TV scanned us but connected only to directed advertising).
static volatile bool direct_next;
// A wake-over-Bluetooth burst (lgmr_woble_burst) owns advertising instance 0 until it completes.
static volatile bool woble_active;

static void advertise(void) {
  if (!synced || !adv_wanted || conn != BLE_HS_CONN_HANDLE_NONE || adv_on() || woble_active)
    return;
  ble_addr_t peers[4];
  int npeers = 0;
  bool direct = direct_next && ble_store_util_bonded_peers(peers, &npeers, 4) == 0 && npeers > 0;
  direct_next = false;
  // Extended-advertising API with legacy PDUs (what a remote sends), because only this API reports
  // the scan requests a central sends us: that shows whether the TV is looking at us at all.
  struct ble_gap_ext_adv_params p;
  memset(&p, 0, sizeof(p));
  p.connectable = 1;
  p.scannable = !direct;
  p.legacy_pdu = 1;
  p.scan_req_notif = !direct;
  if (direct) {
    p.directed = 1;
    p.high_duty_directed = 1;
    p.peer = peers[0];
  }
  p.own_addr_type = own_addr_type;
  p.primary_phy = BLE_HCI_LE_PHY_1M;
  p.secondary_phy = BLE_HCI_LE_PHY_1M;
  p.itvl_min = 0x20;  // 20 ms; the real remote advertises every ~35 ms
  p.itvl_max = 0x30;
  p.tx_power = 127;
  p.sid = 0;
  // Start from an empty instance: the controller rejects directed parameters (0x12) while the instance still
  // holds the undirected advertising data.
  ble_gap_ext_adv_remove(0);
  int rc = ble_gap_ext_adv_configure(0, &p, NULL, gap_event, NULL);
  struct os_mbuf *d = NULL;
  if (direct) {
    // Directed PDUs carry no advertising data; the controller ends the burst after 1.28 s (ADV_COMPLETE).
    if (rc == 0)
      rc = ble_gap_ext_adv_start(0, 128, 0);
    char ab[18];
    sprintf(ab, "%02X:%02X:%02X:%02X:%02X:%02X", p.peer.val[5], p.peer.val[4], p.peer.val[3], p.peer.val[2],
            p.peer.val[1], p.peer.val[0]);
    if (rc == 0) {
      LOGI("directed advertising to %s", ab);
      return;
    }
    LOGW("directed advertising to %s failed: %d; advertising undirected", ab, rc);
    advertise();  // direct_next is already cleared, so this is the undirected path
    return;
  }
  if (rc == 0) {
    d = os_msys_get_pkthdr(adv_len, 0);
    rc = d ? os_mbuf_append(d, adv_data, adv_len) : BLE_HS_ENOMEM;
    if (rc == 0)
      rc = ble_gap_ext_adv_set_data(0, d);
  }
  if (rc == 0) {
    d = os_msys_get_pkthdr(rsp_len, 0);
    rc = d ? os_mbuf_append(d, rsp_data, rsp_len) : BLE_HS_ENOMEM;
    if (rc == 0)
      rc = ble_gap_ext_adv_rsp_set_data(0, d);
  }
  if (rc == 0)
    rc = ble_gap_ext_adv_start(0, 0, 0);
  char hx[100];
  hexify(adv_data, adv_len, hx);
  LOGI("adv data %s", hx);
  hexify(rsp_data, rsp_len, hx);
  LOGI("scan response %s", hx);
  if (rc)
    LOGW("advertising failed: %d", rc);
  else
    LOGI("advertising as '%s', flags 0x%02x", dev_name, adv_flags);
}

static const char *addr_str(const ble_addr_t *a, char *buf) {
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
  return buf;
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  struct ble_gap_conn_desc desc;
  char ab[18];
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status) {
        LOGW("connection failed: %d", event->connect.status);
        advertise();
        return 0;
      }
      conn = event->connect.conn_handle;
      encrypted = false;
      memset((void *) sub, 0, sizeof(sub));
      if (ble_gap_conn_find(conn, &desc) == 0)
        LOGI("TV connected: %s (type %d), itvl %u, latency %u, timeout %u, bonded %d",
             addr_str(&desc.peer_id_addr, ab), desc.peer_id_addr.type, desc.conn_itvl, desc.conn_latency,
             desc.supervision_timeout, desc.sec_state.bonded);
      return 0;

    case BLE_GAP_EVENT_LINK_ESTAB: {
      // Security Request must go out here, not on CONNECT (on CONNECT NimBLE drops it and the TV never pairs).
      // A bonded TV reconnecting encrypts the link itself at once; a Security Request sent as well then never
      // completes and NimBLE drops the link after 30 s (status 13). So when bonded, wait 0.5 s (lgmr_poll): a reconnecting TV encrypts within milliseconds, while a TV registering us
      // again waits for the request and gives up after about 3 s.
      struct ble_store_key_sec sk;
      struct ble_store_value_sec sv;
      memset(&sk, 0, sizeof(sk));
      bool have_bond = false;
      if (ble_gap_conn_find(event->link_estab.conn_handle, &desc) == 0) {
        sk.peer_addr = desc.peer_id_addr;
        have_bond = desc.sec_state.bonded || ble_store_read_peer_sec(&sk, &sv) == 0;
      }
      if (have_bond) {
        sec_wait_since = now_ms();
        sec_wait = true;
        LOGI("link established, bonded: waiting for the TV to encrypt");
      } else {
        int rc = ble_gap_security_initiate(event->link_estab.conn_handle);
        LOGI("link established, security request sent: %d", rc);
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
      LOGI("TV disconnected, reason 0x%x", event->disconnect.reason);
      conn = BLE_HS_CONN_HANDLE_NONE;
      encrypted = false;
      direct_next = true;
      sec_wait = false;
      for (int i = 0; i < 3; i++) {
        if (sub[i] && sub_cb)
          sub_cb(i, false);
        sub[i] = false;
      }
      advertise();
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (woble_active) {
        woble_active = false;
        LOGI("wake burst finished (reason %d)", event->adv_complete.reason);
      }
      advertise();
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
        LOGI("encryption: status %d, encrypted %d, authenticated %d, bonded %d, key size %d", event->enc_change.status,
             desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded, desc.sec_state.key_size);
      // The TV doesn't start an MTU exchange; the remote must, or 125-byte F9 answers get truncated.
      if (event->enc_change.status == 0 && ble_att_mtu(event->enc_change.conn_handle) < 247)
        ble_gattc_exchange_mtu(event->enc_change.conn_handle, NULL, NULL);
      // Our bond no longer matches the TV's (key missing, or an SMP error from either side): delete ours, so the
      // next registration (IR pairing code) starts clean; keeping it made later attempts fail (SMP 0x09).
      // Not on a timeout (13), which says nothing about the keys.
      if ((event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
           (event->enc_change.status >= BLE_HS_ERR_SM_US_BASE && event->enc_change.status < BLE_HS_ERR_SM_PEER_BASE + 0x100)) &&
          ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
        int rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        LOGW("encryption failed (0x%x): our bond for the TV deleted: %d", event->enc_change.status, rc);
      } else if (event->enc_change.status != 0) {
        LOGW("encryption failed (0x%x)", event->enc_change.status);
      }
      if (event->enc_change.status == 0)
        sec_wait = false;
      if (event->enc_change.status == 0) {
        encrypted = true;
        // Subscriptions restored from the bond arrived before encryption: report them again now.
        for (int i = 0; i < 3; i++)
          if (sub[i] && sub_cb)
            sub_cb(i, true);
      }
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      // The TV registers us again while we still hold its old bond: drop the old one and accept.
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
        ble_store_util_delete_peer(&desc.peer_id_addr);
      LOGI("TV is pairing again: old bond dropped");
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE: {
      uint16_t h = event->subscribe.attr_handle;
      int i = h == h_f9_in ? LGMR_F9_IN : h == h_fd_in ? LGMR_FD_IN : h == h_fe_in ? LGMR_FE_IN : -1;
      const char *n = i == LGMR_F9_IN ? "F9" : i == LGMR_FD_IN ? "FD" : i == LGMR_FE_IN ? "FE" :
                      h == h_battery  ? "battery" : h == h_a001 ? "LG a001" : "other";
      LOGI("TV %s notifications on %s (handle 0x%04x)%s", event->subscribe.cur_notify ? "enabled" : "disabled", n, h,
           event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_RESTORE ? " (restored from bond)" : "");
      if (i >= 0) {
        sub[i] = event->subscribe.cur_notify;
        if (sub_cb)
          sub_cb(i, sub[i]);
      }
      return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0)
        LOGI("link parameters: itvl %u, latency %u, timeout %u", desc.conn_itvl, desc.conn_latency,
             desc.supervision_timeout);
      return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      LOGI("TV requests link parameters itvl %u-%u, latency %u, timeout %u", event->conn_update_req.peer_params->itvl_min,
           event->conn_update_req.peer_params->itvl_max, event->conn_update_req.peer_params->latency,
           event->conn_update_req.peer_params->supervision_timeout);
      return 0;

    case BLE_GAP_EVENT_MTU:
      LOGI("ATT MTU %u", event->mtu.value);
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      LOGI("pairing: passkey action %d", event->passkey.params.action);
      if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
        struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1};
        ble_sm_inject_io(event->passkey.conn_handle, &io);
      }
      return 0;

    case BLE_GAP_EVENT_PARING_COMPLETE:
      LOGI("pairing complete: status %d", event->pairing_complete.status);
      return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
      if (event->notify_tx.status && event->notify_tx.status != BLE_HS_EDONE)
        LOGW("notification to TV failed: %d", event->notify_tx.status);
      return 0;

    case BLE_GAP_EVENT_SCAN_REQ_RCVD: {
      // Rate-limited: log a scanner the first time and then once per 2 s with a count.
      static ble_addr_t last;
      static uint32_t last_ms, count;
      uint32_t now = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
      count++;
      if (memcmp(&last, &event->scan_req_rcvd.scan_addr, sizeof(last)) != 0 || now - last_ms > 2000) {
        LOGI("scan request from %s (type %d), %u since last log", addr_str(&event->scan_req_rcvd.scan_addr, ab),
             event->scan_req_rcvd.scan_addr.type, (unsigned) count);
        last = event->scan_req_rcvd.scan_addr;
        last_ms = now;
        count = 0;
      }
      return 0;
    }

    case BLE_GAP_EVENT_DATA_LEN_CHG:
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
      return 0;
  }
  LOGI("gap event %d", event->type);
  return 0;
}

/* ---- host ---- */

static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &own_addr_type);
  uint8_t a[6] = {0};
  ble_hs_id_copy_addr(own_addr_type, a, NULL);
  LOGI("BLE host ready, address %02X:%02X:%02X:%02X:%02X:%02X (type %d)", a[5], a[4], a[3], a[2], a[1], a[0],
       own_addr_type);
  synced = true;
  direct_next = true;
  advertise();
}

static void on_reset(int reason) {
  synced = false;
  LOGW("BLE host reset, reason %d", reason);
}

static void host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void lgmr_init(const char *name, const uint8_t *mfg, uint8_t mfg_len, lgmr_write_cb_t on_write,
               lgmr_subscribed_cb_t on_sub) {
  write_cb = on_write;
  sub_cb = on_sub;
  strncpy(dev_name, name, sizeof(dev_name) - 1);
  build_adv(mfg, mfg_len);

  if (bt_addr_set) {
    // Must happen before the controller starts; the controller then uses it as its public address.
    esp_err_t e = esp_iface_mac_addr_set(bt_addr, ESP_MAC_BT);
    LOGI("Bluetooth address override %02X:%02X:%02X:%02X:%02X:%02X: %d", bt_addr[0], bt_addr[1], bt_addr[2],
         bt_addr[3], bt_addr[4], bt_addr[5], e);
  }

  int rc = nimble_port_init();
  if (rc) {
    LOGE("nimble_port_init failed: %d", rc);
    return;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(dev_name);
  ble_svc_gap_device_appearance_set(0x03C1);  // HID keyboard, as the MR25GB reports
  rc = ble_gatts_count_cfg(services);
  if (rc == 0)
    rc = ble_gatts_add_svcs(services);
  if (rc) {
    LOGE("GATT table registration failed: %d", rc);
    return;
  }
  ble_store_config_init();
  nimble_port_freertos_init(host_task);
}

bool lgmr_synced(void) { return synced; }
uint8_t lgmr_own_addr_type(void) { return own_addr_type; }

void lgmr_own_address(uint8_t out[6]) {
  uint8_t a[6] = {0};
  ble_hs_id_copy_addr(own_addr_type, a, NULL);  // least significant byte first
  for (int i = 0; i < 6; i++)
    out[i] = a[5 - i];
}
const char *lgmr_device_name(void) { return dev_name; }

void lgmr_set_identity(const char *name, const uint8_t *mfg, uint8_t mfg_len) {
  strncpy(dev_name, name, sizeof(dev_name) - 1);
  dev_name[sizeof(dev_name) - 1] = 0;
  build_adv(mfg, mfg_len);
  ble_svc_gap_device_name_set(dev_name);
  adv_off();
  advertise();
}

void lgmr_set_layout(bool hid, bool name_main, const uint8_t *mfg, uint8_t mfg_len) {
  adv_hid = hid;
  adv_name_main = name_main;
  lgmr_set_identity(dev_name, mfg, mfg_len);
}

void lgmr_set_flags(uint8_t flags, const uint8_t *mfg, uint8_t mfg_len) {
  adv_flags = flags;
  lgmr_set_identity(dev_name, mfg, mfg_len);
}

void lgmr_advertise(bool on) {
  adv_wanted = on;
  if (on)
    advertise();
  else
    adv_off();
}

void lgmr_poll(void) {
  uint16_t c = conn;
  if (!sec_wait || c == BLE_HS_CONN_HANDLE_NONE || now_ms() - sec_wait_since < 500)
    return;
  sec_wait = false;
  if (!encrypted) {
    int rc = ble_gap_security_initiate(c);
    LOGI("TV didn't encrypt within 0.5 s: security request sent: %d", rc);
  }
}

void lgmr_reconnect(void) {
  if (!synced || !adv_wanted || conn != BLE_HS_CONN_HANDLE_NONE)
    return;
  direct_next = true;
  int rc = adv_on() ? ble_gap_ext_adv_stop(0) : 0;  // no ADV_COMPLETE event for a stop, so restart here
  LOGI("reconnect: stop %d, still advertising %d", rc, adv_on());
  advertise();
}

bool lgmr_connected(void) { return conn != BLE_HS_CONN_HANDLE_NONE; }
bool lgmr_subscribed(int report) { return encrypted && report >= 0 && report < 3 && sub[report]; }

int lgmr_notify(int report, const uint8_t *data, uint16_t len) {
  uint16_t c = conn;
  if (c == BLE_HS_CONN_HANDLE_NONE || report < 0 || report > 2 || !sub[report])
    return -1;
  if (!encrypted)
    return -3;
  uint16_t h = report == LGMR_F9_IN ? h_f9_in : report == LGMR_FD_IN ? h_fd_in : h_fe_in;
  if (len + 3 > ble_att_mtu(c))
    len = ble_att_mtu(c) - 3;  // the real remote's answers arrive truncated at a small MTU too
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
  if (!om)
    return -2;
  return ble_gatts_notify_custom(c, h, om);
}

void lgmr_status(void) {
  struct ble_gap_conn_desc desc;
  char ab[18];
  uint16_t c = conn;
  int bonds = 0;
  ble_addr_t peers[4];
  ble_store_util_bonded_peers(peers, &bonds, 4);
  LOGI("name '%s', synced %d, advertising %d (wanted %d), bonds %d", dev_name, synced, adv_on(),
       adv_wanted, bonds);
  for (int i = 0; i < bonds; i++)
    LOGI("  bond %d: %s (type %d)", i, addr_str(&peers[i], ab), peers[i].type);
  if (c != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(c, &desc) == 0)
    LOGI("connected to %s: encrypted %d, bonded %d, MTU %u, itvl %u, subscribed F9 %d FD %d FE %d",
         addr_str(&desc.peer_id_addr, ab), desc.sec_state.encrypted, desc.sec_state.bonded, ble_att_mtu(c),
         desc.conn_itvl, sub[0], sub[1], sub[2]);
  else
    LOGI("not connected");
}

// Wake the TV from standby over Bluetooth ("Turn on via Bluetooth" / Mobile TV On, docs/protocol/wake.md):
// sends advertising data d as legacy advertising every 20 ms for ms milliseconds, then returns to normal
// advertising. Only while the TV isn't connected. lgmr_woble_burst builds the packet the TV wakes on.
int lgmr_adv_burst(const uint8_t *d, size_t len, uint32_t ms, int pdu) {
  if (!synced)
    return BLE_HS_ENOTSYNCED;
  if (conn != BLE_HS_CONN_HANDLE_NONE)
    return LGMR_WOBLE_CONNECTED;
  if (len > 31)
    return BLE_HS_EINVAL;
  if (adv_on())
    ble_gap_ext_adv_stop(0);
  struct ble_gap_ext_adv_params p;
  memset(&p, 0, sizeof(p));
  p.legacy_pdu = 1;  // pdu 0: ADV_NONCONN_IND, 1: ADV_IND (connectable + scannable), 2: ADV_SCAN_IND
  p.connectable = pdu == 1;
  p.scannable = pdu == 1 || pdu == 2;
  p.own_addr_type = own_addr_type;
  p.primary_phy = BLE_HCI_LE_PHY_1M;
  p.secondary_phy = BLE_HCI_LE_PHY_1M;
  p.itvl_min = 0x20;  // 20 ms
  p.itvl_max = 0x20;
  p.tx_power = 127;
  p.sid = 0;
  ble_gap_ext_adv_remove(0);
  int rc = ble_gap_ext_adv_configure(0, &p, NULL, gap_event, NULL);
  if (rc == 0) {
    struct os_mbuf *m = os_msys_get_pkthdr(len, 0);
    rc = m ? os_mbuf_append(m, d, len) : BLE_HS_ENOMEM;
    if (rc == 0)
      rc = ble_gap_ext_adv_set_data(0, m);
  }
  if (rc == 0) {
    woble_active = true;
    rc = ble_gap_ext_adv_start(0, (int) ((ms + 9) / 10), 0);
    if (rc != 0)
      woble_active = false;
  }
  char hx[64];
  hexify(d, len, hx);
  if (rc == 0)
    LOGI("wake burst for %u ms (%s): %s", (unsigned) ms, pdu == 1 ? "ADV_IND" : pdu == 2 ? "ADV_SCAN_IND" : "ADV_NONCONN_IND", hx);
  else {
    LOGW("wake burst failed: %d", rc);
    advertise();
  }
  return rc;
}

int lgmr_woble_burst(const uint8_t id[6], uint32_t ms, int pdu) {
  // Flags, then LG manufacturer data: company 0x00C4, the registered 6 bytes, 07 00, then the TV's own Bluetooth
  // address. This 17-byte form (a "ThinQ" registration) is what the TV's controller matches in deep standby
  // (tested 2026-11-20); without the TV address it only wakes the TV in the first minute or so of standby, while
  // the TV's software still scans. The TV address is the bonded TV's (written most significant byte first).
  ble_addr_t peers[4];
  int n = 0;
  if (ble_store_util_bonded_peers(peers, &n, 4) != 0 || n == 0) {
    LOGW("wake burst: no bonded TV, so no TV address; sending the short form (wakes only from light standby)");
    uint8_t d[15] = {0x02, 0x01, 0x06, 0x0b, 0xff, 0xc4, 0x00, id[0], id[1], id[2], id[3], id[4], id[5], 0x07, 0x00};
    return lgmr_adv_burst(d, sizeof(d), ms, pdu);
  }
  const uint8_t *t = peers[0].val;  // NimBLE keeps addresses least significant byte first
  uint8_t d[21] = {0x02, 0x01, 0x06, 0x11, 0xff, 0xc4, 0x00, id[0], id[1], id[2], id[3], id[4], id[5], 0x07, 0x00,
                   t[5], t[4], t[3], t[2], t[1], t[0]};
  return lgmr_adv_burst(d, sizeof(d), ms, pdu);
}

void lgmr_forget(void) {
  int rc = ble_store_clear();
  LOGI("all bonds deleted: %d", rc);
}

void lgmr_disconnect(void) {
  uint16_t c = conn;
  if (c != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}
