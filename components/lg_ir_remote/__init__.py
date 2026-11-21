"""Infrared remote for LG webOS TVs.

Sends LG (NEC) key codes by name or number, holds keys with NEC repeat codes, and transmits UEI ZipIR code sets
(the TV's universal-control format). Usable on its own, or linked to lg_magic_remote (ir_remote_id) for IR fallback,
Bluetooth pairing and universal control.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import remote_transmitter
import esphome.config_validation as cv
from esphome.const import CONF_CODE, CONF_ID

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["remote_transmitter"]

CONF_TRANSMITTER_ID = "transmitter_id"
CONF_HOLD_STYLE = "hold_style"
CONF_HOLD = "hold"
CONF_KEY = "key"

lg_ir_remote_ns = cg.esphome_ns.namespace("lg_ir_remote")
LGIRRemote = lg_ir_remote_ns.class_("LGIRRemote", cg.Component)
HoldStyle = lg_ir_remote_ns.enum("HoldStyle")
HOLD_STYLES = {
    "repeat_codes": HoldStyle.HOLD_REPEAT_CODES,
    "full_frames": HoldStyle.HOLD_FULL_FRAMES,
}
SendAction = lg_ir_remote_ns.class_("SendAction", automation.Action)
PressAction = lg_ir_remote_ns.class_("PressAction", automation.Action)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LGIRRemote),
        cv.GenerateID(CONF_TRANSMITTER_ID): cv.use_id(
            remote_transmitter.RemoteTransmitterComponent
        ),
        cv.Optional(CONF_HOLD_STYLE, default="repeat_codes"): cv.enum(
            HOLD_STYLES, lower=True
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    tx = await cg.get_variable(config[CONF_TRANSMITTER_ID])
    cg.add(var.set_transmitter(tx))
    cg.add(var.set_hold_style(config[CONF_HOLD_STYLE]))


@automation.register_action(
    "lg_ir_remote.send",
    SendAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(LGIRRemote),
            cv.Required(CONF_CODE): cv.templatable(cv.hex_uint32_t),
            cv.Optional(CONF_HOLD, default="0ms"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def send_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_code(await cg.templatable(config[CONF_CODE], args, cg.uint32)))
    hold = config[CONF_HOLD]
    if not cg.is_template(hold):
        hold = hold.total_milliseconds
    cg.add(var.set_hold(await cg.templatable(hold, args, cg.uint32)))
    return var


@automation.register_action(
    "lg_ir_remote.press",
    PressAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(LGIRRemote),
            cv.Required(CONF_KEY): cv.templatable(cv.string),
            cv.Optional(CONF_HOLD, default="0ms"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def press_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_key(await cg.templatable(config[CONF_KEY], args, cg.std_string)))
    hold = config[CONF_HOLD]
    if not cg.is_template(hold):
        hold = hold.total_milliseconds
    cg.add(var.set_hold(await cg.templatable(hold, args, cg.uint32)))
    return var
