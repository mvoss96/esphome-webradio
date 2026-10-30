"""Download, decode and cache radio station logos for LVGL."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["json"]
DEPENDENCIES = ["network", "lvgl", "psram"]

station_logos_ns = cg.esphome_ns.namespace("station_logos")
StationLogos = station_logos_ns.class_("StationLogos", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(StationLogos)}).extend(
    cv.COMPONENT_SCHEMA
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_library("pngle", "1.1.0")
