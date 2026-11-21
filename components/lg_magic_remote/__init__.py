"""Bluetooth LG Magic Remote: the ESP32 presents itself to an LG webOS TV as its Bluetooth Magic Remote.

Keys, pointer, scroll wheel, voice (speaker platform), wake from standby, and universal control (with lg_ir_remote
linked through ir_remote_id). Uses NimBLE directly, not ESPHome's esp32_ble (Bluedroid), so it can't be combined
with esp32_ble / esp32_ble_tracker / bluetooth_proxy on the same device. Needs Bluetooth 5 extended advertising:
ESP32-C3, C5, C6, H2 or S3 (not the original ESP32).
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import add_idf_sdkconfig_option, request_bluetooth
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_CODE, CONF_DURATION, CONF_ID

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["esp32"]
CONFLICTS_WITH = ["esp32_ble", "esp32_ble_tracker", "esp32_ble_server"]

CONF_IR_REMOTE_ID = "ir_remote_id"
CONF_DEVICE_NAME = "device_name"
CONF_MANUFACTURER_STRING = "manufacturer_string"
CONF_ADVERTISE_ON_BOOT = "advertise_on_boot"
CONF_DEBUG = "debug"
CONF_CAPTURE = "capture"
CONF_SERIAL_COMMANDS = "serial_commands"
CONF_KEY = "key"
CONF_HOLD = "hold"
CONF_DX = "dx"
CONF_DY = "dy"
CONF_NOTCHES = "notches"

lg_magic_remote_ns = cg.esphome_ns.namespace("lg_magic_remote")
LGMagicRemote = lg_magic_remote_ns.class_("LGMagicRemote", cg.Component)
# Declared here rather than imported, so this component loads without lg_ir_remote.
LGIRRemote = cg.esphome_ns.namespace("lg_ir_remote").class_("LGIRRemote", cg.Component)

KeyAction = lg_magic_remote_ns.class_("KeyAction", automation.Action)
PressCodeAction = lg_magic_remote_ns.class_("PressCodeAction", automation.Action)
MovePointerAction = lg_magic_remote_ns.class_("MovePointerAction", automation.Action)
ScrollAction = lg_magic_remote_ns.class_("ScrollAction", automation.Action)
WakeAction = lg_magic_remote_ns.class_("WakeAction", automation.Action)
PairAction = lg_magic_remote_ns.class_("PairAction", automation.Action)
ForgetAction = lg_magic_remote_ns.class_("ForgetAction", automation.Action)

DEBUG_SCHEMA = cv.Schema(
    {
        # Capture mode: the ESP32 acts as a TV for a genuine Magic Remote and logs what it sends (docs/porting.md).
        cv.Optional(CONF_CAPTURE, default=False): cv.boolean,
        # Line commands on the USB serial console (chips with USB Serial/JTAG only).
        cv.Optional(CONF_SERIAL_COMMANDS, default=False): cv.boolean,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LGMagicRemote),
            cv.Optional(CONF_IR_REMOTE_ID): cv.use_id(LGIRRemote),
            # The name the TV sees. "LGE MR23" (LG's MR20GA replacement remote) is the tested one: as
            # "LGE MR19BA" the TV ignored the Netflix and Prime Video keys.
            cv.Optional(CONF_DEVICE_NAME, default="LGE MR23"): cv.All(
                cv.string, cv.Length(max=29)
            ),
            cv.Optional(
                CONF_MANUFACTURER_STRING, default="SCD 21.2,BA 35,webOS"
            ): cv.All(cv.string, cv.Length(max=24)),
            cv.Optional(CONF_ADVERTISE_ON_BOOT, default=True): cv.boolean,
            # Bluetooth public address to use instead of the chip's own.
            cv.Optional(CONF_ADDRESS): cv.mac_address,
            cv.Optional(CONF_DEBUG, default={}): DEBUG_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    esp32.only_on_variant(
        supported=[
            esp32.VARIANT_ESP32C3,
            esp32.VARIANT_ESP32C5,
            esp32.VARIANT_ESP32C6,
            esp32.VARIANT_ESP32H2,
            esp32.VARIANT_ESP32S3,
        ],
        msg_prefix="lg_magic_remote (needs Bluetooth 5 extended advertising)",
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_IR_REMOTE_ID in config:
        cg.add_build_flag("-DLGMR_HAVE_IR")
        ir = await cg.get_variable(config[CONF_IR_REMOTE_ID])
        cg.add(var.set_ir_remote(ir))
    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_manufacturer_string(config[CONF_MANUFACTURER_STRING]))
    cg.add(var.set_advertise_on_boot(config[CONF_ADVERTISE_ON_BOOT]))
    if CONF_ADDRESS in config:
        cg.add(var.set_address(config[CONF_ADDRESS].as_hex))

    debug = config[CONF_DEBUG]
    capture = debug[CONF_CAPTURE]
    if capture:
        cg.add_build_flag("-DLGMR_CAPTURE")
    if debug[CONF_SERIAL_COMMANDS]:
        cg.add_build_flag("-DLGMR_SERIAL_COMMANDS")
    cg.add(var.set_serial_commands(debug[CONF_SERIAL_COMMANDS]))

    request_bluetooth()
    for name, value in {
        "CONFIG_BT_ENABLED": True,
        "CONFIG_BT_BLUEDROID_ENABLED": False,
        "CONFIG_BT_NIMBLE_ENABLED": True,
        "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL": True,
        "CONFIG_BT_NIMBLE_ROLE_BROADCASTER": True,
        "CONFIG_BT_NIMBLE_ROLE_CENTRAL": True,  # needed for the GATT client MTU exchange
        "CONFIG_BT_NIMBLE_GATT_CLIENT": True,
        "CONFIG_BT_NIMBLE_ROLE_OBSERVER": capture,  # capture mode scans for a real remote
        "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": 2 if capture else 1,  # the TV (+ a real remote)
        "CONFIG_BT_NIMBLE_MAX_BONDS": 4,
        "CONFIG_BT_NIMBLE_MAX_CCCDS": 16,
        "CONFIG_BT_NIMBLE_NVS_PERSIST": True,
        "CONFIG_BT_NIMBLE_SM_LEGACY": True,
        "CONFIG_BT_NIMBLE_SM_SC": True,
        "CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU": 247,
        "CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING": True,
        "CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT": True,
        "CONFIG_BT_NIMBLE_EXT_ADV": True,  # scan-request reports and the wake burst
        "CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES": 1,
    }.items():
        add_idf_sdkconfig_option(name, value)


_ID = {cv.GenerateID(): cv.use_id(LGMagicRemote)}
_HOLD = cv.templatable(cv.positive_time_period_milliseconds)


def _ms(value):
    return value if cg.is_template(value) else value.total_milliseconds


async def _parented(config, action_id, template_arg):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "lg_magic_remote.key",
    KeyAction,
    cv.Schema({**_ID, cv.Required(CONF_KEY): cv.templatable(cv.string)}),
    synchronous=True,
)
async def key_to_code(config, action_id, template_arg, args):
    var = await _parented(config, action_id, template_arg)
    cg.add(var.set_key(await cg.templatable(config[CONF_KEY], args, cg.std_string)))
    return var


@automation.register_action(
    "lg_magic_remote.press_code",
    PressCodeAction,
    cv.Schema(
        {
            **_ID,
            cv.Required(CONF_CODE): cv.templatable(cv.hex_uint16_t),
            cv.Optional(CONF_HOLD, default="0ms"): _HOLD,
        }
    ),
    synchronous=True,
)
async def press_code_to_code(config, action_id, template_arg, args):
    var = await _parented(config, action_id, template_arg)
    cg.add(var.set_code(await cg.templatable(config[CONF_CODE], args, cg.uint16)))
    cg.add(var.set_hold(await cg.templatable(_ms(config[CONF_HOLD]), args, cg.uint32)))
    return var


@automation.register_action(
    "lg_magic_remote.move_pointer",
    MovePointerAction,
    cv.Schema(
        {
            **_ID,
            cv.Required(CONF_DX): cv.templatable(cv.int_),
            cv.Required(CONF_DY): cv.templatable(cv.int_),
        }
    ),
    synchronous=True,
)
async def move_pointer_to_code(config, action_id, template_arg, args):
    var = await _parented(config, action_id, template_arg)
    cg.add(var.set_dx(await cg.templatable(config[CONF_DX], args, cg.int_)))
    cg.add(var.set_dy(await cg.templatable(config[CONF_DY], args, cg.int_)))
    return var


@automation.register_action(
    "lg_magic_remote.scroll",
    ScrollAction,
    cv.Schema(
        {**_ID, cv.Required(CONF_NOTCHES): cv.templatable(cv.int_range(-9, 9))}
    ),
    synchronous=True,
)
async def scroll_to_code(config, action_id, template_arg, args):
    var = await _parented(config, action_id, template_arg)
    cg.add(var.set_notches(await cg.templatable(config[CONF_NOTCHES], args, cg.int_)))
    return var


@automation.register_action(
    "lg_magic_remote.wake_tv",
    WakeAction,
    automation.maybe_simple_id({**_ID, cv.Optional(CONF_DURATION, default="4s"): _HOLD}),
    synchronous=True,
)
async def wake_to_code(config, action_id, template_arg, args):
    var = await _parented(config, action_id, template_arg)
    cg.add(
        var.set_duration(
            await cg.templatable(_ms(config[CONF_DURATION]), args, cg.uint32)
        )
    )
    return var


@automation.register_action(
    "lg_magic_remote.pair", PairAction, automation.maybe_simple_id(_ID), synchronous=True
)
async def pair_to_code(config, action_id, template_arg, args):
    return await _parented(config, action_id, template_arg)


@automation.register_action(
    "lg_magic_remote.forget", ForgetAction, automation.maybe_simple_id(_ID), synchronous=True
)
async def forget_to_code(config, action_id, template_arg, args):
    return await _parented(config, action_id, template_arg)
