import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import KEY_SCHEMA, register_listener, truma_ns

DEPENDENCIES = ["truma_inetx"]

CONF_ON_VALUE = "on_value"
CONF_OFF_VALUE = "off_value"

TrumaSwitch = truma_ns.class_("TrumaSwitch", switch.Switch)

CONFIG_SCHEMA = (
    switch.switch_schema(TrumaSwitch)
    .extend(KEY_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_ON_VALUE, default=1): cv.int_,
            cv.Optional(CONF_OFF_VALUE, default=0): cv.int_,
        }
    )
)


async def to_code(config):
    var = await switch.new_switch(config)
    await register_listener(var, config)
    cg.add(var.set_on_off(config[CONF_ON_VALUE], config[CONF_OFF_VALUE]))
