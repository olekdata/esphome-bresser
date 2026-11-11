import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_Bresser_ID, Bresser

# The startup flag in the station's frames is set for roughly one hour
# after the sensor is powered on or reset (e.g. after a battery change).
# On = station was power-cycled within the last hour - explains a
# rain-counter reset or an odd-looking reading.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_Bresser_ID): cv.use_id(Bresser),
        cv.Optional('startup'): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    var = await cg.get_variable(config[CONF_Bresser_ID])

    if startup_config := config.get('startup'):
        sens = await binary_sensor.new_binary_sensor(startup_config)
        cg.add(var.set_startup_binary_sensor(sens))
