import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP
from . import DeskMitm, desk_mitm_ns

DEPENDENCIES = ["desk_mitm"]
CONF_DESK_MITM_ID = "desk_mitm_id"

DeskNumber = desk_mitm_ns.class_("DeskNumber", number.Number, cg.Component)

CONFIG_SCHEMA = number.number_schema(DeskNumber).extend(
    {
        cv.GenerateID(CONF_DESK_MITM_ID): cv.use_id(DeskMitm),
        cv.Optional(CONF_MIN_VALUE, default=60.0): cv.float_,
        cv.Optional(CONF_MAX_VALUE, default=121.0): cv.float_,
        cv.Optional(CONF_STEP, default=0.1): cv.positive_float,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DESK_MITM_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_target_number(var))
