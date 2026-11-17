import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID, CONF_SUPPORTED_MODES

from . import HUB_SCHEMA, register_listener, truma_ns

DEPENDENCIES = ["truma_inetx"]

TrumaClimate = truma_ns.class_("TrumaClimate", climate.Climate)

CONF_APPLIANCE_ADDRESS = "appliance_address"

ALLOWED_MODES = ["HEAT", "FAN_ONLY", "COOL", "AUTO", "DRY"]

if hasattr(climate, "climate_schema"):
    _BASE = climate.climate_schema(TrumaClimate)
else:  # older ESPHome
    _BASE = climate.CLIMATE_SCHEMA.extend({cv.GenerateID(): cv.declare_id(TrumaClimate)})

CONFIG_SCHEMA = _BASE.extend(HUB_SCHEMA).extend(
    {
        cv.Optional(CONF_SUPPORTED_MODES, default=["HEAT", "FAN_ONLY"]): cv.ensure_list(
            cv.one_of(*ALLOWED_MODES, upper=True)
        ),
        # Bus address of the heater whose readings to show (0x0201 = first
        # heater). 0 takes whichever device reported last.
        cv.Optional(CONF_APPLIANCE_ADDRESS, default=0): cv.hex_uint16_t,
    }
)


async def to_code(config):
    if hasattr(climate, "new_climate"):
        var = await climate.new_climate(config)
    else:
        var = cg.new_Pvariable(config[CONF_ID])
        await climate.register_climate(var, config)
    await register_listener(var, config)
    cg.add(var.set_appliance_address(config[CONF_APPLIANCE_ADDRESS]))
    for mode in config[CONF_SUPPORTED_MODES]:
        cg.add(var.add_supported_mode(climate.CLIMATE_MODES[mode]))
