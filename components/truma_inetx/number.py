import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from . import KEY_SCHEMA, register_listener, truma_ns

DEPENDENCIES = ["truma_inetx"]

CONF_SCALE = "scale"

TrumaNumber = truma_ns.class_("TrumaNumber", number.Number)

CONFIG_SCHEMA = cv.All(
    number.number_schema(TrumaNumber)
    .extend(KEY_SCHEMA)
    .extend(
        {
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Optional(CONF_STEP, default=1): cv.positive_float,
            # displayed value = wire value * scale (temperatures: 0.1)
            cv.Optional(CONF_SCALE, default=1.0): cv.float_range(
                min=0, min_included=False
            ),
        }
    ),
)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await register_listener(var, config)
    cg.add(var.set_scale(config[CONF_SCALE]))
