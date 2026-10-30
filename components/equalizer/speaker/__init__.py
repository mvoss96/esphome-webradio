import esphome.codegen as cg
from esphome.components import audio, speaker
from esphome.components.esp32 import add_idf_component
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_FREQUENCY,
    CONF_GAIN,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_SAMPLE_RATE,
    CONF_TYPE,
    PLATFORM_ESP32,
)
from esphome.core.entity_helpers import inherit_property_from

CODEOWNERS = ["@meav33"]
AUTO_LOAD = ["audio"]
DEPENDENCIES = ["speaker"]

equalizer_speaker_ns = cg.esphome_ns.namespace("equalizer_speaker")
EqualizerSpeaker = equalizer_speaker_ns.class_(
    "EqualizerSpeaker", cg.Component, speaker.Speaker
)
BiquadType = equalizer_speaker_ns.enum("BiquadType")

CONF_BANDS = "bands"
CONF_Q = "q"

BIQUAD_TYPES = {
    "low_shelf": BiquadType.BIQUAD_LOW_SHELF,
    "peaking": BiquadType.BIQUAD_PEAKING,
    "high_shelf": BiquadType.BIQUAD_HIGH_SHELF,
    "low_pass": BiquadType.BIQUAD_LOW_PASS,
    "high_pass": BiquadType.BIQUAD_HIGH_PASS,
}

BAND_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TYPE): cv.enum(BIQUAD_TYPES, lower=True),
        cv.Required(CONF_FREQUENCY): cv.All(
            cv.frequency, cv.Range(min=10.0, max=24000.0)
        ),
        cv.Optional(CONF_GAIN, default="0dB"): cv.All(
            cv.decibel, cv.Range(min=-24.0, max=24.0)
        ),
        cv.Optional(CONF_Q, default=0.707): cv.float_range(min=0.1, max=20.0),
    }
)


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=2,
    )(config)
    return config


def _validate_audio_compatibility(config):
    inherit_property_from(CONF_BITS_PER_SAMPLE, CONF_OUTPUT_SPEAKER)(config)
    inherit_property_from(CONF_NUM_CHANNELS, CONF_OUTPUT_SPEAKER)(config)
    inherit_property_from(CONF_SAMPLE_RATE, CONF_OUTPUT_SPEAKER)(config)

    audio.final_validate_audio_schema(
        "equalizer",
        audio_device=CONF_OUTPUT_SPEAKER,
        bits_per_sample=config.get(CONF_BITS_PER_SAMPLE),
        channels=config.get(CONF_NUM_CHANNELS),
        sample_rate=config.get(CONF_SAMPLE_RATE),
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(EqualizerSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_BANDS, default=[]): cv.All(
                cv.ensure_list(BAND_SCHEMA),
                cv.Length(max=8),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _set_stream_limits,
)

FINAL_VALIDATE_SCHEMA = _validate_audio_compatibility


async def to_code(config):
    # Pull in Espressif's DSP library for biquad coefficient generation and
    # the SIMD-optimised cascade. Version pinned to match what ESPHome's own
    # idf_component.yml uses, so we don't trigger a version-conflict warning.
    add_idf_component(name="espressif/esp-dsp", ref="1.7.1")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))

    cg.add(var.init_bands(len(config[CONF_BANDS])))
    for band in config[CONF_BANDS]:
        cg.add(
            var.add_band(
                band[CONF_TYPE],
                band[CONF_FREQUENCY],
                band[CONF_GAIN],
                band[CONF_Q],
            )
        )
