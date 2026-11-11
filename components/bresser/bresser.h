/*
 * ESPHome component receiving data from a Bresser weather station via a
 * CC1101 receiver module. Based on
 * https://github.com/matthias-bs/Bresser_Weather_Sensor_CC1101_MQTT and the
 * rtl_433 project.
 *
 * The weather station model and all CC1101 RF parameters are configurable
 * from YAML, so a different station generation or a slightly different
 * CC1101 module does not require editing (and re-forking) this component:
 *
 *   bresser:
 *     PIN_CS:   GPIO15
 *     PIN_GDO0: GPIO4
 *     PIN_GDO2: GPIO5
 *     PIN_SCK:  GPIO18   # ESP32 only, optional: SPI bus pins, any GPIO
 *     PIN_MISO: GPIO19   # via the GPIO matrix (unset = Arduino core
 *     PIN_MOSI: GPIO21   # defaults; the ESP8266 core fixes 14/12/13)
 *     model: 6in1                 # "5in1" (legacy) or "6in1" (6-in-1 / new 5-in-1 / 7-in-1)
 *     filter_sensor_id: 0x36582164 # ignore frames from other stations
 *     update_interval: 60s         # entity publish cadence (frame aggregation window)
 *     frequency: 868.35            # MHz
 *     bit_rate: 8.22               # kbps
 *     frequency_deviation: 57.1    # kHz
 *     receiver_bandwidth: 270.0    # kHz
 *     power: 10                    # dBm
 *     preamble_length: 32          # bits
 *
 * PIN_CS, PIN_GDO0 and PIN_GDO2 are required; everything else is
 * optional and defaults to the values shown.
 *
 * Reception is non-blocking: the CC1101 is kept in receive mode and GDO0 is
 * polled in loop(); decoded frames are merged into a buffer. Entities are
 * published on update_interval with the latest value per field - except wind
 * gust, which is the peak gust within the update window.
 */

#pragma once
#include "RadioLib.h"
#include "esphome/core/gpio.h"
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace bresser {

    enum BresserModel {
        BRESSER_MODEL_5IN1,   // Bresser 5-in-1 (legacy protocol)
        BRESSER_MODEL_6IN1,   // Bresser 6-in-1 / new 5-in-1 / 7-in-1 / Froggit WH6000
    };

	enum DecodeStatus {
        DECODE_OK, DECODE_PAR_ERR, DECODE_CHK_ERR, DECODE_DIG_ERR
    };

    struct WeatherData_S {
        uint8_t  s_type;               // only 6-in-1
        uint32_t sensor_id;            // 5-in-1: 1 byte / 6-in-1: 4 bytes
        uint8_t  chan;                 // only 6-in-1
        bool     temp_ok;              // only 6-in-1
        float    temp_c;
        int      humidity;
        bool     uv_ok;                // only 6-in-1
        float    uv;                   // only 6-in-1
        bool     wind_ok;              // only 6-in-1
        float    wind_direction_deg;
        float    wind_gust_meter_sec;
        float    wind_avg_meter_sec;
        bool     rain_ok;              // only 6-in-1
        float    rain_mm;
        bool     battery_ok;           // value of the battery flag
        bool     battery_valid;        // frame carries a battery flag
        bool     startup;              // true = station reset/power-up within the last ~1 h
        bool     moisture_ok;          // only 6-in-1
        int      moisture;             // only 6-in-1
    };

    typedef struct WeatherData_S WeatherData_t;


class Bresser : public PollingComponent  {
  public:

    void setup() override;
    void loop() override;
    void update() override;

	void set_temperature_sensor(sensor::Sensor *temperature_sensor) { temperature_sensor_ = temperature_sensor; }
    void set_humidity_sensor(sensor::Sensor *humidity_sensor) { humidity_sensor_ = humidity_sensor; }
	void set_wind_speed_sensor(sensor::Sensor *wind_speed_sensor) { wind_speed_sensor_ = wind_speed_sensor; }
	void set_wind_direction_sensor(sensor::Sensor *wind_direction_sensor) { wind_direction_sensor_ = wind_direction_sensor; }
	void set_rain_sensor(sensor::Sensor *rain_sensor) { rain_sensor_ = rain_sensor; }
	void set_wind_gust_sensor(sensor::Sensor *wind_gust_sensor) { wind_gust_sensor_ = wind_gust_sensor; }
	void set_battery_sensor(sensor::Sensor *battery_sensor) { battery_sensor_ = battery_sensor; }
	void set_startup_binary_sensor(binary_sensor::BinarySensor *startup_binary_sensor) { startup_binary_sensor_ = startup_binary_sensor; }
	void set_sensor_id_text_sensor(text_sensor::TextSensor *sensor_id_text_sensor) { sensor_id_text_sensor_ = sensor_id_text_sensor; }
	void set_foreign_sensor_ids_text_sensor(text_sensor::TextSensor *foreign_sensor_ids_text_sensor) { foreign_sensor_ids_text_sensor_ = foreign_sensor_ids_text_sensor; }
	void set_sensor_type_sensor(sensor::Sensor *sensor_type_sensor) { sensor_type_sensor_ = sensor_type_sensor; }
	void set_channel_sensor(sensor::Sensor *channel_sensor) { channel_sensor_ = channel_sensor; }
	void set_uv_sensor(sensor::Sensor *uv_sensor) { uv_sensor_ = uv_sensor; }
	void set_moisture_sensor(sensor::Sensor *moisture_sensor) { moisture_sensor_ = moisture_sensor; }
	void set_rssi_sensor(sensor::Sensor *rssi_sensor) { rssi_sensor_ = rssi_sensor; }
	void set_lqi_sensor(sensor::Sensor *lqi_sensor) { lqi_sensor_ = lqi_sensor; }

	void set_pin_cs(InternalGPIOPin *pin) { this->pin_CS_ = pin; }
	void set_pin_gdo0(InternalGPIOPin *pin) { this->pin_GDO0_ = pin; }
	void set_pin_gdo2(InternalGPIOPin *pin) { this->pin_GDO2_ = pin; }

	// SPI bus pins (ESP32 only - optional, nullptr = Arduino core defaults)
	void set_pin_sck(InternalGPIOPin *pin) { this->pin_SCK_ = pin; }
	void set_pin_miso(InternalGPIOPin *pin) { this->pin_MISO_ = pin; }
	void set_pin_mosi(InternalGPIOPin *pin) { this->pin_MOSI_ = pin; }

	// Weather station model - selects the payload decoder
	void set_model(uint8_t model) { this->model_ = (BresserModel) model; }

	// CC1101 RF configuration (values in natural units: MHz / kbps / kHz / dBm / bits)
	void set_frequency(float frequency) { this->frequency_ = frequency; }
	void set_bit_rate(float bit_rate) { this->bit_rate_ = bit_rate; }
	void set_frequency_deviation(float frequency_deviation) { this->frequency_deviation_ = frequency_deviation; }
	void set_receiver_bandwidth(float receiver_bandwidth) { this->receiver_bandwidth_ = receiver_bandwidth; }
	void set_power(int8_t power) { this->power_ = power; }
	void set_preamble_length(uint16_t preamble_length) { this->preamble_length_ = preamble_length; }

	// Only accept frames from this station id (e.g. 0x36582164)
	void set_filter_sensor_id(uint32_t filter_sensor_id) {
        this->filter_sensor_id_ = filter_sensor_id;
        this->has_filter_sensor_id_ = true;
    }

  protected:

	CC1101 *radio{nullptr};

	InternalGPIOPin  *pin_CS_{nullptr};
	InternalGPIOPin  *pin_GDO0_{nullptr};
	InternalGPIOPin  *pin_GDO2_{nullptr};

	// ESP32 only (custom SPI pins); nullptr = Arduino core defaults
	InternalGPIOPin  *pin_SCK_{nullptr};
	InternalGPIOPin  *pin_MISO_{nullptr};
	InternalGPIOPin  *pin_MOSI_{nullptr};

	BresserModel model_{BRESSER_MODEL_5IN1};

	float    frequency_{868.35f};          // MHz
	float    bit_rate_{8.22f};             // kbps
	float    frequency_deviation_{57.136417f}; // kHz
	float    receiver_bandwidth_{270.0f};      // kHz
	int8_t   power_{10};                   // dBm
	uint16_t preamble_length_{32};          // bits

	uint32_t filter_sensor_id_{0};
	bool     has_filter_sensor_id_{false};

	sensor::Sensor *temperature_sensor_{nullptr};
	sensor::Sensor *humidity_sensor_{nullptr};
	sensor::Sensor *wind_speed_sensor_{nullptr};
	sensor::Sensor *wind_direction_sensor_{nullptr};
	sensor::Sensor *rain_sensor_{nullptr};
	sensor::Sensor *wind_gust_sensor_{nullptr};
	sensor::Sensor *battery_sensor_{nullptr};
	binary_sensor::BinarySensor *startup_binary_sensor_{nullptr};
	text_sensor::TextSensor *sensor_id_text_sensor_{nullptr};
	text_sensor::TextSensor *foreign_sensor_ids_text_sensor_{nullptr};
	sensor::Sensor *sensor_type_sensor_{nullptr};
	sensor::Sensor *channel_sensor_{nullptr};
	sensor::Sensor *uv_sensor_{nullptr};
	sensor::Sensor *moisture_sensor_{nullptr};
	sensor::Sensor *rssi_sensor_{nullptr};
	sensor::Sensor *lqi_sensor_{nullptr};

	// Data of the frame currently being decoded
	WeatherData_t frame_{};

	// Data merged from all frames decoded since the last publish.
	// The *_ok flags mean "valid data seen since the last publish" (so both
	// alternating frame types publish in the same cycle) and
	// wind_gust_meter_sec holds the peak gust within the window.
	WeatherData_t buf_{};
	bool frame_seen_{false};

	// Radio link diagnostics of the last received frame
	float    last_rssi_{0.0f};
	uint8_t  last_lqi_{0};

	// Registry of stations heard on the frequency, for the
	// foreign_sensor_ids text sensor. Entries not heard from within
	// FOREIGN_STATION_TIMEOUT_MS drop off the list; once full, the
	// stalest entry is replaced. RAM cost: 8 x 12 bytes.
	struct ForeignStation {
		uint32_t id;
		float    rssi_max;     // strongest signal received from this station
		uint32_t last_seen_ms;
	};
	static constexpr uint8_t  MAX_FOREIGN_STATIONS = 8;
	static constexpr uint32_t FOREIGN_STATION_TIMEOUT_MS = 10UL * 60UL * 1000UL;
	ForeignStation foreign_stations_[MAX_FOREIGN_STATIONS]{};
	uint8_t        foreign_station_count_{0};
	// last published list (sentinel-init so the first publish always fires)
	char foreign_stations_last_[192]{'?'};

	uint16_t lfsr_digest16(uint8_t const message[], unsigned bytes, uint16_t gen, uint16_t key);
	int add_bytes(uint8_t const message[], unsigned num_bytes);
    DecodeStatus decodeBresser5In1Payload(uint8_t *msg, uint8_t msgSize, WeatherData_t *pOut);
    DecodeStatus decodeBresser6In1Payload(uint8_t *msg, uint8_t msgSize, WeatherData_t *pOut);
    void getWeatherdata(void);
    void mergeFrame(void);
    void publishWeatherdata(void);
    void trackStation(uint32_t id, float rssi);
    void publishForeignStations(void);
    void printRawdata(uint8_t *msg, uint8_t msgSize);

};

}  // namespace bresser
}  // namespace esphome
