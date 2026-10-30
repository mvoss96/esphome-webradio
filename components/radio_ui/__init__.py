"""Carousel rendering helpers for the web radio display. No configuration options."""

import esphome.config_validation as cv

DEPENDENCIES = ["lvgl", "station_logos"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
