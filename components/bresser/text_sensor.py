import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_Bresser_ID, Bresser

# The station ID is a 32-bit identifier which exceeds the precision of a
# float sensor, so it is exposed as a text sensor ("0x%08X").
# foreign_sensor_ids lists stations heard on the frequency other than
# filter_sensor_id ("0x12345678 (-67 dBm)", strongest first).
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_Bresser_ID): cv.use_id(Bresser),
        cv.Optional('sensor_id'): text_sensor.text_sensor_schema(),
        cv.Optional('foreign_sensor_ids'): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    var = await cg.get_variable(config[CONF_Bresser_ID])

    if sensor_id_config := config.get('sensor_id'):
        sens = await text_sensor.new_text_sensor(sensor_id_config)
        cg.add(var.set_sensor_id_text_sensor(sens))

    if foreign_sensor_ids_config := config.get('foreign_sensor_ids'):
        sens = await text_sensor.new_text_sensor(foreign_sensor_ids_config)
        cg.add(var.set_foreign_sensor_ids_text_sensor(sens))
