"""Web page on the radio: control playback and edit the station list."""

import gzip
from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["web_server_base"]
DEPENDENCIES = ["network"]

CONF_STATIONS = "stations"
CONF_STATUS = "status"
CONF_ON_SAVE = "on_save"
CONF_ON_PLAY = "on_play"
CONF_ON_VOLUME = "on_volume"
CONF_ON_STOP = "on_stop"
CONF_ON_SETTING = "on_setting"

radio_web_ns = cg.esphome_ns.namespace("radio_web")
RadioWeb = radio_web_ns.class_("RadioWeb", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RadioWeb),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Required(CONF_STATIONS): cv.returning_lambda,
        cv.Required(CONF_STATUS): cv.returning_lambda,
        cv.Optional(CONF_ON_SAVE): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_PLAY): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_VOLUME): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_STOP): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_SETTING): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)

    # index.html is compressed at build time and served as is.
    page = gzip.compress((Path(__file__).parent / "index.html").read_bytes(), mtime=0)
    cg.add_global(
        cg.RawStatement(
            "static const uint8_t RADIO_WEB_PAGE[] = {" + ",".join(str(b) for b in page) + "};"
        )
    )
    cg.add(var.set_page(cg.RawExpression("RADIO_WEB_PAGE"), len(page)))

    stations = await cg.process_lambda(config[CONF_STATIONS], [], return_type=cg.std_string)
    cg.add(var.set_stations_lambda(stations))
    status = await cg.process_lambda(config[CONF_STATUS], [], return_type=cg.std_string)
    cg.add(var.set_status_lambda(status))

    for key, trigger, args in (
        (CONF_ON_SAVE, var.get_save_trigger(), [(cg.std_string, "text")]),
        (CONF_ON_PLAY, var.get_play_trigger(), [(cg.int_, "index")]),
        (CONF_ON_VOLUME, var.get_volume_trigger(), [(cg.int_, "volume")]),
        (CONF_ON_STOP, var.get_stop_trigger(), []),
        (CONF_ON_SETTING, var.get_setting_trigger(), [(cg.std_string, "key"), (cg.float_, "value")]),
    ):
        if conf := config.get(key):
            await automation.build_automation(trigger, args, conf)
