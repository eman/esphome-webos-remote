"""Voice input for the TV: a speaker that encodes what it plays as mSBC and streams it to the TV the way a Magic
Remote streams its microphone (FE reports). Feed it from a speaker media_player at 16 kHz mono (WAV), e.g. TTS."""

import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID

from . import LGMagicRemote, lg_magic_remote_ns

AUTO_LOAD = ["audio", "ring_buffer"]
DEPENDENCIES = ["lg_magic_remote"]

CONF_LG_MAGIC_REMOTE_ID = "lg_magic_remote_id"
CONF_BUFFER_DURATION = "buffer_duration"

LGVoiceSpeaker = lg_magic_remote_ns.class_("LGVoiceSpeaker", cg.Component, speaker.Speaker)

CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(LGVoiceSpeaker),
        cv.GenerateID(CONF_LG_MAGIC_REMOTE_ID): cv.use_id(LGMagicRemote),
        # PCM held between the media pipeline and the 15 ms packet clock (16 kHz mono: 32 kB per second).
        cv.Optional(CONF_BUFFER_DURATION, default="500ms"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    cg.add_define("USE_LG_MAGIC_REMOTE_VOICE")
    parent = await cg.get_variable(config[CONF_LG_MAGIC_REMOTE_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_buffer_ms(config[CONF_BUFFER_DURATION].total_milliseconds))
