import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_POWER,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_EMPTY,
    ICON_BATTERY,
)

from . import CONF_Bresser_ID, Bresser

# Battery: On = battery good, Off = the station reports a low battery.
# The flag is only carried by some frame types (see the 6-in-1 notes in
# the README), so the entity updates when a frame carrying it arrives.
#
# Startup: On while the station has been running for less than about one
# hour since it was powered on or reset (e.g. after a battery change) -
# explains a rain-counter reset or an odd-looking first reading.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_Bresser_ID): cv.use_id(Bresser),
        cv.Optional('battery'): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY,
            icon=ICON_BATTERY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional('startup'): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_POWER,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    var = await cg.get_variable(config[CONF_Bresser_ID])

    if battery_config := config.get('battery'):
        sens = await binary_sensor.new_binary_sensor(battery_config)
        cg.add(var.set_battery_binary_sensor(sens))

    if startup_config := config.get('startup'):
        sens = await binary_sensor.new_binary_sensor(startup_config)
        cg.add(var.set_startup_binary_sensor(sens))
