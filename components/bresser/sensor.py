import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_HUMIDITY,
    CONF_WIND_SPEED,
    CONF_WIND_DIRECTION_DEGREES,
    CONF_ID,
    CONF_TEMPERATURE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_DEGREES,
    UNIT_MILLIMETER,
    UNIT_DECIBEL_MILLIWATT,
)

from . import CONF_Bresser_ID, Bresser

# The update_interval belongs to the bresser hub (it throttles entity
# publishing while reception runs non-blocking in the background).
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_Bresser_ID): cv.use_id(Bresser),

        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_WIND_SPEED): sensor.sensor_schema(
            unit_of_measurement="m/s",
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_WIND_DIRECTION_DEGREES): sensor.sensor_schema(
            unit_of_measurement=UNIT_DEGREES,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional('rain'): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLIMETER,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional('wind_gust'): sensor.sensor_schema(
            unit_of_measurement="m/s",
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # battery moved to the binary_sensor platform (On = battery good)
        cv.Optional('sensor_type'): sensor.sensor_schema(
            accuracy_decimals=0,
        ),
        cv.Optional('channel'): sensor.sensor_schema(
            accuracy_decimals=0,
        ),
        cv.Optional('uv'): sensor.sensor_schema(
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional('moisture'): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional('rssi'): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional('lqi'): sensor.sensor_schema(
            accuracy_decimals=0,
        ),
    }
)


async def to_code(config):
    var = await cg.get_variable(config[CONF_Bresser_ID])

    if temperature_config := config.get(CONF_TEMPERATURE):
        sens = await sensor.new_sensor(temperature_config)
        cg.add(var.set_temperature_sensor(sens))

    if humidity_config := config.get(CONF_HUMIDITY):
        sens = await sensor.new_sensor(humidity_config)
        cg.add(var.set_humidity_sensor(sens))

    if wind_speed_config := config.get(CONF_WIND_SPEED):
        sens = await sensor.new_sensor(wind_speed_config)
        cg.add(var.set_wind_speed_sensor(sens))

    if wind_direction_degrees_config := config.get(CONF_WIND_DIRECTION_DEGREES):
        sens = await sensor.new_sensor(wind_direction_degrees_config)
        cg.add(var.set_wind_direction_sensor(sens))

    if rain_config := config.get('rain'):
        sens = await sensor.new_sensor(rain_config)
        cg.add(var.set_rain_sensor(sens))

    if wind_gust_config := config.get('wind_gust'):
        sens = await sensor.new_sensor(wind_gust_config)
        cg.add(var.set_wind_gust_sensor(sens))

    if sensor_type_config := config.get('sensor_type'):
        sens = await sensor.new_sensor(sensor_type_config)
        cg.add(var.set_sensor_type_sensor(sens))

    if channel_config := config.get('channel'):
        sens = await sensor.new_sensor(channel_config)
        cg.add(var.set_channel_sensor(sens))

    if uv_config := config.get('uv'):
        sens = await sensor.new_sensor(uv_config)
        cg.add(var.set_uv_sensor(sens))

    if moisture_config := config.get('moisture'):
        sens = await sensor.new_sensor(moisture_config)
        cg.add(var.set_moisture_sensor(sens))

    if rssi_config := config.get('rssi'):
        sens = await sensor.new_sensor(rssi_config)
        cg.add(var.set_rssi_sensor(sens))

    if lqi_config := config.get('lqi'):
        sens = await sensor.new_sensor(lqi_config)
        cg.add(var.set_lqi_sensor(sens))
