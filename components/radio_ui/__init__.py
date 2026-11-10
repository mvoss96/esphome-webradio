"""Carousel rendering helpers for the web radio display."""

import esphome.codegen as cg
import esphome.config_validation as cv

DEPENDENCIES = ["lvgl", "station_logos"]

CONF_SCREEN_WIDTH = "screen_width"
CONF_SCREEN_HEIGHT = "screen_height"
CONF_TILE_CENTER = "tile_center"
CONF_TILE_SIDE = "tile_side"

# Screen size as LVGL sees it and the carousel tile sizes; webradio.yaml sets them from its
# substitutions. The defaults are the 160x128 layout.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SCREEN_WIDTH, default=160): cv.int_range(min=1),
        cv.Optional(CONF_SCREEN_HEIGHT, default=128): cv.int_range(min=1),
        cv.Optional(CONF_TILE_CENTER, default=68): cv.int_range(min=1),
        cv.Optional(CONF_TILE_SIDE, default=40): cv.int_range(min=1),
    }
)


async def to_code(config):
    cg.add_define("RADIO_UI_SCREEN_WIDTH", config[CONF_SCREEN_WIDTH])
    cg.add_define("RADIO_UI_SCREEN_HEIGHT", config[CONF_SCREEN_HEIGHT])
    cg.add_define("RADIO_UI_TILE_CENTER", config[CONF_TILE_CENTER])
    cg.add_define("RADIO_UI_TILE_SIDE", config[CONF_TILE_SIDE])
