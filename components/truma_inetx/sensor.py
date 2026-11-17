import esphome.codegen as cg
from esphome.components import sensor

from . import KEY_SCHEMA, register_listener, truma_ns

DEPENDENCIES = ["truma_inetx"]

TrumaSensor = truma_ns.class_("TrumaSensor", sensor.Sensor)

CONFIG_SCHEMA = sensor.sensor_schema(TrumaSensor).extend(KEY_SCHEMA)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await register_listener(var, config)
