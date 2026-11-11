import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID
from esphome.core import CORE

CONF_PIN_CS = "PIN_CS"
CONF_PIN_GDO0 = "PIN_GDO0"
CONF_PIN_GDO2 = "PIN_GDO2"

# ESP32 only: SPI bus pins, routed through the GPIO matrix so any usable
# GPIO works. Optional - unset means RadioLib's Arduino core defaults
# (VSPI: SCK 18, MISO 19, MOSI 23 on classic ESP32). The ESP8266 core
# fixes the SPI pins to GPIO14/12/13, so the keys are rejected there.
CONF_PIN_SCK = "PIN_SCK"
CONF_PIN_MISO = "PIN_MISO"
CONF_PIN_MOSI = "PIN_MOSI"

# Weather station model - selects the payload decoder:
#   "5in1" - Bresser 5-in-1 (legacy protocol)
#   "6in1" - Bresser 6-in-1 / new 5-in-1 / 7-in-1 / Froggit WH6000 protocol
CONF_MODEL = "model"
MODELS = {
    "5in1": 0,
    "6in1": 1,
}

# Only accept frames from this station id (e.g. 0x36582164), ignoring any
# neighbour stations on the same frequency. Find your station's id in the
# logs ("Id: [0x...]") or the sensor_id text sensor.
CONF_FILTER_SENSOR_ID = "filter_sensor_id"

# CC1101 RF configuration overrides.
# Defaults match the Bresser weather station protocol; override them from
# YAML if your CC1101 module or region differs (e.g. frequency: 868.3 or 915.0).
CONF_FREQUENCY = "frequency"  # MHz
CONF_BIT_RATE = "bit_rate"  # kbps
CONF_FREQUENCY_DEVIATION = "frequency_deviation"  # kHz
CONF_RECEIVER_BANDWIDTH = "receiver_bandwidth"  # kHz
CONF_POWER = "power"  # dBm
CONF_PREAMBLE_LENGTH = "preamble_length"  # bits


bresser_ns = cg.esphome_ns.namespace("bresser")
Bresser = bresser_ns.class_("Bresser", cg.PollingComponent)

CONF_Bresser_ID = "Bresser_id"

_SCHEMA = {
    cv.GenerateID(): cv.declare_id(Bresser),

    cv.Required(CONF_PIN_CS): pins.internal_gpio_input_pin_schema,
    cv.Required(CONF_PIN_GDO0): pins.internal_gpio_input_pin_schema,
    cv.Required(CONF_PIN_GDO2): pins.internal_gpio_input_pin_schema,

    cv.Optional(CONF_MODEL, default="5in1"): cv.one_of(*MODELS, lower=True),
    cv.Optional(CONF_FREQUENCY, default=868.35): cv.float_range(min=300.0, max=928.0),
    cv.Optional(CONF_BIT_RATE, default=8.22): cv.positive_float,
    cv.Optional(CONF_FREQUENCY_DEVIATION, default=57.136417): cv.positive_float,
    cv.Optional(CONF_RECEIVER_BANDWIDTH, default=270.0): cv.positive_float,
    cv.Optional(CONF_POWER, default=10): cv.int_range(min=-30, max=10),
    cv.Optional(CONF_PREAMBLE_LENGTH, default=32): cv.positive_int,
    cv.Optional(CONF_FILTER_SENSOR_ID): cv.int_range(min=0, max=0xFFFFFFFF),
}

if CORE.is_esp32:
    _SCHEMA.update(
        {
            cv.Optional(CONF_PIN_SCK): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_PIN_MISO): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_PIN_MOSI): pins.internal_gpio_output_pin_schema,
        }
    )
else:
    _SCHEMA.update(
        {
            cv.Optional(CONF_PIN_SCK): cv.invalid(
                "Custom SPI pins are only supported on ESP32 - "
                "the ESP8266 core fixes them to GPIO14/12/13"
            ),
            cv.Optional(CONF_PIN_MISO): cv.invalid(
                "Custom SPI pins are only supported on ESP32 - "
                "the ESP8266 core fixes them to GPIO14/12/13"
            ),
            cv.Optional(CONF_PIN_MOSI): cv.invalid(
                "Custom SPI pins are only supported on ESP32 - "
                "the ESP8266 core fixes them to GPIO14/12/13"
            ),
        }
    )

CONFIG_SCHEMA = cv.Schema(_SCHEMA).extend(cv.polling_component_schema("60s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CORE.is_esp8266:
        # RadioLib's Arduino HAL calls the Arduino core tone(), which pulls
        # in Tone.cpp -> startWaveformClockCycles(). ESPHome excludes the
        # ESP8266 core waveform generator from the build when no component
        # needs PWM, leaving that symbol undefined. Nothing here uses
        # tone()/AFSK (CC1101 receive only), so compile the call out -
        # no stub file needed.
        cg.add_build_flag("-DRADIOLIB_TONE_UNSUPPORTED")

    if CORE.using_arduino:
        # RadioLib's Module.h includes <SPI.h> on Arduino builds and its
        # default HAL drives the global SPI object (VSPI on ESP32, the
        # fixed hardware bus on ESP8266). ESPHome's own spi: component
        # would declare this library too, but a spi: block must not be
        # required just to pull it in: it initializes the bus through its
        # own driver (the ESP-IDF one on ESP32), which can clash with
        # RadioLib over the same bus. Declare the library here instead,
        # so no spi: block is needed in the YAML on either platform.
        cg.add_library("SPI", None)

    pin = await cg.gpio_pin_expression(config[CONF_PIN_CS])
    cg.add(var.set_pin_cs(pin))

    pin = await cg.gpio_pin_expression(config[CONF_PIN_GDO0])
    cg.add(var.set_pin_gdo0(pin))

    pin = await cg.gpio_pin_expression(config[CONF_PIN_GDO2])
    cg.add(var.set_pin_gdo2(pin))

    if CORE.is_esp32:
        if CONF_PIN_SCK in config:
            pin = await cg.gpio_pin_expression(config[CONF_PIN_SCK])
            cg.add(var.set_pin_sck(pin))
        if CONF_PIN_MISO in config:
            pin = await cg.gpio_pin_expression(config[CONF_PIN_MISO])
            cg.add(var.set_pin_miso(pin))
        if CONF_PIN_MOSI in config:
            pin = await cg.gpio_pin_expression(config[CONF_PIN_MOSI])
            cg.add(var.set_pin_mosi(pin))

    cg.add(var.set_model(MODELS[config[CONF_MODEL]]))
    cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_bit_rate(config[CONF_BIT_RATE]))
    cg.add(var.set_frequency_deviation(config[CONF_FREQUENCY_DEVIATION]))
    cg.add(var.set_receiver_bandwidth(config[CONF_RECEIVER_BANDWIDTH]))
    cg.add(var.set_power(config[CONF_POWER]))
    cg.add(var.set_preamble_length(config[CONF_PREAMBLE_LENGTH]))

    if CONF_FILTER_SENSOR_ID in config:
        cg.add(var.set_filter_sensor_id(config[CONF_FILTER_SENSOR_ID]))
