import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_TYPE

from . import OPTIONAL_KEY_SCHEMA, register_listener, truma_ns, validate_key_or_type

DEPENDENCIES = ["truma_inetx"]

TrumaTextSensor = truma_ns.class_("TrumaTextSensor", text_sensor.TextSensor)

STATUS_TYPES = {"status": 4, "version": 6, "last_disconnect": 7}

CONF_FORMAT = "format"
# raw: value as text; timer: TimerConfig.TimerN as "07:00-09:00, Mo Di";
# firmware: Identify SwMaj.SwMin.SwBgFx of the device at `address`.
FORMATS = {"raw": 0, "timer": 1, "firmware": 2}


def validate_firmware(config):
    if config.get(CONF_FORMAT) == "firmware":
        config.setdefault("topic", "Identify")
        config.setdefault("param", "SwMaj")
    return config

CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(TrumaTextSensor)
    .extend(OPTIONAL_KEY_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_TYPE): cv.one_of(*STATUS_TYPES, lower=True),
            cv.Optional(CONF_FORMAT, default="raw"): cv.one_of(*FORMATS, lower=True),
        }
    ),
    validate_firmware,
    validate_key_or_type(CONF_TYPE),
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await register_listener(var, config)
    if CONF_TYPE in config:
        cg.add(var.set_status_type(STATUS_TYPES[config[CONF_TYPE]]))
    cg.add(var.set_format(FORMATS[config[CONF_FORMAT]]))
