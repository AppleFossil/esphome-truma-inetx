import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_OPTIONS

from . import KEY_SCHEMA, register_listener, truma_ns

DEPENDENCIES = ["truma_inetx"]

TrumaSelect = truma_ns.class_("TrumaSelect", select.Select)


def validate_options(value):
    value = cv.Schema({cv.int_: cv.string_strict})(value)
    if not value:
        raise cv.Invalid("at least one option is required")
    labels = list(value.values())
    if len(set(labels)) != len(labels):
        raise cv.Invalid("option labels must be unique")
    return value


CONFIG_SCHEMA = (
    select.select_schema(TrumaSelect)
    .extend(KEY_SCHEMA)
    .extend({cv.Required(CONF_OPTIONS): validate_options})
)


async def to_code(config):
    options = config[CONF_OPTIONS]
    var = await select.new_select(config, options=list(options.values()))
    await register_listener(var, config)
    cg.add(var.set_values(list(options.keys())))
