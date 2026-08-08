import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from . import DeskMitm, desk_mitm_ns

DEPENDENCIES = ["desk_mitm"]
CONF_DESK_MITM_ID = "desk_mitm_id"

DeskCover = desk_mitm_ns.class_("DeskCover", cover.Cover, cg.Component)

CONFIG_SCHEMA = cover.cover_schema(DeskCover).extend(
    {cv.GenerateID(CONF_DESK_MITM_ID): cv.use_id(DeskMitm)}
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_DESK_MITM_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_cover(var))
