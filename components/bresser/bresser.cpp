/*
 * ESPHome component receiving data from a Bresser weather station via a
 * CC1101 receiver module. Based on
 * https://github.com/matthias-bs/Bresser_Weather_Sensor_CC1101_MQTT and the
 * rtl_433 project (see the decoder sources referenced below).
 *
 * The payload decoder (5-in-1 legacy vs. 6-in-1) and all CC1101 RF
 * parameters are configured from YAML via the hub component
 * (see bresser.h for the full list of options).
 *
 * Reception is non-blocking: the CC1101 stays in receive mode and GDO0 is
 * polled in loop(). Each decoded frame is merged into a buffer; entities
 * are published on update_interval (latest value per field, peak wind gust
 * within the window). This keeps writes to the Home Assistant database at
 * the configured cadence while data stays as fresh as update_interval.
 */

#undef delay
#undef yield
#undef millis
#undef micros
#undef delayMicroseconds

// Enable RadioLib internal debug messages
//#define RADIOLIB_DEBUG

#include "bresser.h"
#include "RadioLib.h"
#include <type_traits>

namespace esphome {
namespace bresser {

static const char *TAG = "Bresser";

    void Bresser::setup() {
        ESP_LOGD(TAG, "setup");

        uint32_t cs = this->pin_CS_->get_pin();
        uint32_t g0 = this->pin_GDO0_->get_pin();
        uint32_t g2 = this->pin_GDO2_->get_pin();
        ESP_LOGD(TAG, "pins: cs [%u] gdo0 [%u] gdo2 [%u]", (unsigned) cs, (unsigned) g0, (unsigned) g2);
        ESP_LOGD(TAG, "model: [%s]", this->model_ == BRESSER_MODEL_6IN1 ? "6in1" : "5in1");
        ESP_LOGD(TAG, "frequency: [%.3f MHz] bit rate: [%.2f kbps] rx bandwidth: [%.1f kHz]",
                 this->frequency_, this->bit_rate_, this->receiver_bandwidth_);
        if (this->has_filter_sensor_id_)
            ESP_LOGD(TAG, "sensor id filter: [0x%08X]", (unsigned) this->filter_sensor_id_);

#ifdef USE_ESP32
        // Bring up the SPI bus with the configured pins before RadioLib
        // does: its Arduino HAL calls SPI.begin() without arguments, which
        // the ESP32 core turns into a no-op once the bus is already up -
        // so the pins set here win. Optional in YAML; when unset, RadioLib
        // uses the Arduino core defaults (VSPI on classic ESP32). The
        // ESP8266 core has no custom SPI pins (fixed to GPIO14/12/13),
        // so nothing to do there.
        if (this->pin_SCK_ && this->pin_MISO_ && this->pin_MOSI_) {
            SPI.begin((int8_t) this->pin_SCK_->get_pin(),
                      (int8_t) this->pin_MISO_->get_pin(),
                      (int8_t) this->pin_MOSI_->get_pin());
            ESP_LOGD(TAG, "spi pins: sck [%u] miso [%u] mosi [%u]",
                     (unsigned) this->pin_SCK_->get_pin(),
                     (unsigned) this->pin_MISO_->get_pin(),
                     (unsigned) this->pin_MOSI_->get_pin());
        }
#endif

        this->radio = new CC1101(new Module(cs, g0, RADIOLIB_NC, g2));

        ConfigFSK_t config;
        // RadioLib 8.0.0 changed the ConfigFSK_t fields from float
        // (MHz / kbps / kHz) to uint32_t (Hz / bps / Hz). Detect the field
        // type so the component works with both the old and the new API -
        // no library version pinning required.
        if constexpr (std::is_same<decltype(ConfigFSK_t{}.frequency), uint32_t>::value) {
            config.frequency         = (uint32_t) (this->frequency_ * 1000000.0f);
            config.bitRate           = (uint32_t) (this->bit_rate_ * 1000.0f);
            config.frequencyDeviation = (uint32_t) (this->frequency_deviation_ * 1000.0f);
            config.receiverBandwidth = (uint32_t) (this->receiver_bandwidth_ * 1000.0f);
        } else {
            config.frequency         = this->frequency_;
            config.bitRate           = this->bit_rate_;
            config.frequencyDeviation = this->frequency_deviation_;
            config.receiverBandwidth = this->receiver_bandwidth_;
        }
        config.power = this->power_;
        config.preambleLength = this->preamble_length_;

        int state = this->radio->begin(config);
        if (state != RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "Error initialising CC1101: [%d]", state);
            this->mark_failed();
            return;
        }

        state = this->radio->setCrcFiltering(false);
        if (state != RADIOLIB_ERR_NONE)
            ESP_LOGE(TAG, "Error disabling crc filtering: [%d]", state);

        state = this->radio->fixedPacketLengthMode(27);
        if (state != RADIOLIB_ERR_NONE)
            ESP_LOGE(TAG, "Error setting fixed packet length: [%d]", state);

        // Preamble: AA AA AA AA AA
        // Sync is: 2D D4
        // Preamble 40 bits but the CC1101 doesn't allow us to set that
        // so we use a preamble of 32 bits and then use the sync as AA 2D
        // which then uses the last byte of the preamble - we receive the last sync byte
        // as the 1st byte of the payload.
        state = this->radio->setSyncWord(0xAA, 0x2D, 0, false);
        if (state != RADIOLIB_ERR_NONE)
            ESP_LOGE(TAG, "Error setting sync words: [%d]", state);

        // Enter receive mode - GDO0 is mapped to "RX FIFO full or packet
        // end", so it goes high once a complete packet is in the FIFO.
        state = this->radio->startReceive();
        if (state != RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "Error entering receive mode: [%d]", state);
            this->mark_failed();
            return;
        }
        ESP_LOGD(TAG, "Setup complete - awaiting incoming messages...");
    }

    void Bresser::loop() {
        this->getWeatherdata();
    }

    void Bresser::update() {
        this->publishForeignStations();
        this->publishWeatherdata();
    }

    //
    // From the rtl_433 project - https://github.com/merbanan/rtl_433/blob/master/src/util.c
    //
    uint16_t Bresser::lfsr_digest16(uint8_t const message[], unsigned bytes, uint16_t gen, uint16_t key)
    {
        uint16_t sum = 0;
        for (unsigned k = 0; k < bytes; ++k) {
            uint8_t data = message[k];
            for (int i = 7; i >= 0; --i) {
                // if data bit is set then xor with key
                if ((data >> i) & 1)
                    sum ^= key;

                // roll the key right (actually the lsb is dropped here)
                // and apply the gen (needs to include the dropped lsb as msb)
                if (key & 1)
                    key = (key >> 1) ^ gen;
                else
                    key = (key >> 1);
            }
        }
        return sum;
    }


    //
    // From the rtl_433 project - https://github.com/merbanan/rtl_433/blob/master/src/util.c
    //
    int Bresser::add_bytes(uint8_t const message[], unsigned num_bytes)
    {
        int result = 0;
        for (unsigned i = 0; i < num_bytes; ++i) {
            result += message[i];
        }
        return result;
    }

    /*
    // Bresser 5-in-1 payload decoder, adapted from the rtl_433 project
    //
    // Example input data:
    //   EA EC 7F EB 5F EE EF FA FE 76 BB FA FF 15 13 80 14 A0 11 10 05 01 89 44 05 00
    //   CC CC CC CC CC CC CC CC CC CC CC CC CC uu II SS GG DG WW  W TT  T HH RR  R Bt
    // - C = Check, inverted data of 13 byte further
    // - uu = checksum (number/count of set bits within bytes 14-25)
    // - I = station ID (maybe)
    // - G = wind gust in 1/10 m/s, normal binary coded, GGxG = 0x76D1 => 0x0176 = 256 + 118 = 374 => 37.4 m/s.  MSB is out of sequence.
    // - D = wind direction 0..F = N..NNE..E..S..W..NNW
    // - W = wind speed in 1/10 m/s, BCD coded, WWxW = 0x7512 => 0x0275 = 275 => 27.5 m/s. MSB is out of sequence.
    // - T = temperature in 1/10 °C, BCD coded, TTxT = 1203 => 31.2 °C
    // - t = temperature sign, minus if unequal 0
    // - H = humidity in percent, BCD coded, HH = 23 => 23 %
    // - R = rain in mm, BCD coded, RRxR = 1203 => 31.2 mm
    // - B = Battery. 0=Ok, 8=Low.
    // - S = sensor type, only low nibble used, 0x9 for Bresser Professional Rain Gauge
    //
    // Parameters:
    //
    // msg     - Pointer to message
    // msgSize - Size of message
    // pOut    - Pointer to WeatherData
    //
    // Returns:
    //
    // DECODE_OK      - OK - WeatherData will contain the updated information
    // DECODE_PAR_ERR - Parity Error
    // DECODE_CHK_ERR - Checksum Error
    //
    */

    DecodeStatus Bresser::decodeBresser5In1Payload(uint8_t *msg, uint8_t msgSize, WeatherData_t *pOut) {
        // First 13 bytes need to match inverse of last 13 bytes
        for (unsigned col = 0; col < msgSize / 2; ++col) {
            if ((msg[col] ^ msg[col + 13]) != 0xff) {
                ESP_LOGV(TAG,"%s: Parity wrong at %u\n", __func__, col);
                return DECODE_PAR_ERR;
            }
        }

        // Verify checksum (number of set bits in bytes 14-25)
        uint8_t bitsSet = 0;
        uint8_t expectedBitsSet = msg[13];

        for(uint8_t p = 14 ; p < msgSize ; p++) {
        uint8_t currentByte = msg[p];
        while(currentByte) {
            bitsSet += (currentByte & 1);
            currentByte >>= 1;
        }
        }

        if (bitsSet != expectedBitsSet) {
        ESP_LOGV(TAG,"%s: Checksum wrong actual [%02X] != expected [%02X]\n", __func__, bitsSet, expectedBitsSet);
        return DECODE_CHK_ERR;
        }

        pOut->sensor_id = msg[14];

        int temp_raw = (msg[20] & 0x0f) + ((msg[20] & 0xf0) >> 4) * 10 + (msg[21] &0x0f) * 100;
        if (msg[25] & 0x0f) {
            temp_raw = -temp_raw;
        }
        pOut->temp_c = temp_raw * 0.1f;

        pOut->humidity = (msg[22] & 0x0f) + ((msg[22] & 0xf0) >> 4) * 10;

        pOut->wind_direction_deg = ((msg[17] & 0xf0) >> 4) * 22.5f;

        int gust_raw = ((msg[17] & 0x0f) << 8) + msg[16];
        pOut->wind_gust_meter_sec = gust_raw * 0.1f;

        int wind_raw = (msg[18] & 0x0f) + ((msg[18] & 0xf0) >> 4) * 10 + (msg[19] & 0x0f) * 100;
        pOut->wind_avg_meter_sec = wind_raw * 0.1f;

        int rain_raw = (msg[23] & 0x0f) + ((msg[23] & 0xf0) >> 4) * 10 + (msg[24] & 0x0f) * 100;
        pOut->rain_mm = rain_raw * 0.1f;

        pOut->battery_ok    = (msg[25] & 0x80) ? false : true;   // msg[25] bit 7: 0 = OK, 1 = low
        pOut->battery_valid = true;                              // 5-in-1 frames carry all fields
        pOut->startup       = (msg[15] & 0x80) == 0;             // msg[15] bit 7: 0 after reset, 1 after ~1 h

        return DECODE_OK;
    }


    //
    // From the rtl_433 project - https://github.com/merbanan/rtl_433/blob/master/src/devices/bresser_6in1.c
    //
    /**
    Decoder for Bresser Weather Center 6-in-1.
    - also Bresser Weather Center 7-in-1 indoor sensor.
    - also Bresser new 5-in-1 sensors.
    - also Froggit WH6000 sensors.
    - also rebranded as Ventus C8488A (W835)
    - also Bresser 3-in-1 Professional Wind Gauge / Anemometer PN 7002531
    There are at least two different message types:
    - 24 seconds interval for temperature, hum, uv and rain (alternating messages)
    - 12 seconds interval for wind data (every message)
    Also Bresser Explore Scientific SM60020 Soil moisture Sensor.
    https://www.bresser.de/en/Weather-Time/Accessories/EXPLORE-SCIENTIFIC-Soil-Moisture-and-Soil-Temperature-Sensor.html
    Moisture:
        f16e 187000e34 7 ffffff0000 252 2 16 fff 004 000 [25,2, 99%, CH 7]
        DIGEST:8h8h ID?8h8h8h8h STYPE:4h STARTUP:1b CH:3d 8h 8h8h 8h8h TEMP:12h 4h MOIST:8h TRAILER:8h8h8h8h4h
    Moisture is transmitted in the humidity field as index 1-16: 0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 99.
    {206}55555555545ba83e803100058631ff11fe6611ffffffff01cc00 [Hum 96% Temp 3.8 C Wind 0.7 m/s]
    {205}55555555545ba999263100058631fffffe66d006092bffe0cff8 [Hum 95% Temp 3.0 C Wind 0.0 m/s]
    {199}55555555545ba840523100058631ff77fe668000495fff0bbe [Hum 95% Temp 3.0 C Wind 0.4 m/s]
    {205}55555555545ba94d063100058631fffffe665006092bffe14ff8
    {206}55555555545ba860703100058631fffffe6651ffffffff0135fc [Hum 95% Temp 3.0 C Wind 0.0 m/s]
    {205}55555555545ba924d23100058631ff99fe68b004e92dffe073f8 [Hum 96% Temp 2.7 C Wind 0.4 m/s]
    {202}55555555545ba813403100058631ff77fe6810050929ffe1180 [Hum 94% Temp 2.8 C Wind 0.4 m/s]
    {205}55555555545ba98be83100058631fffffe6130050929ffe17800 [Hum 95% Temp 2.8 C Wind 0.8 m/s]
                                             TEMP  HUM
    2dd4  1f 40 18 80 02 c3 18 ff 88 ff 33 08 ff ff ff ff 80 e6 00 [Hum 96% Temp 3.8 C Wind 0.7 m/s]
    2dd4  cc 93 18 80 02 c3 18 ff ff ff 33 68 03 04 95 ff f0 67 3f [Hum 95% Temp 3.0 C Wind 0.0 m/s]
    2dd4  20 29 18 80 02 c3 18 ff bb ff 33 40 00 24 af ff 85 df    [Hum 95% Temp 3.0 C Wind 0.4 m/s]
    2dd4  a6 83 18 80 02 c3 18 ff ff ff 33 28 03 04 95 ff f0 a7 3f
    2dd4  30 38 18 80 02 c3 18 ff ff ff 33 28 ff ff ff ff 80 9a 7f [Hum 95% Temp 3.0 C Wind 0.0 m/s]
    2dd4  92 69 18 80 02 c3 18 ff cc ff 34 58 02 74 96 ff f0 39 3f [Hum 96% Temp 2.7 C Wind 0.4 m/s]
    2dd4  09 a0 18 80 02 c3 18 ff bb ff 34 08 02 84 94 ff f0 8c 0  [Hum 94% Temp 2.8 C Wind 0.4 m/s]
    2dd4  c5 f4 18 80 02 c3 18 ff ff ff 30 98 02 84 94 ff f0 bc 00 [Hum 95% Temp 2.8 C Wind 0.8 m/s]
    {147} 5e aa 18 80 02 c3 18 fa 8f fb 27 68 11 84 81 ff f0 72 00 [Temp 11.8 C  Hum 81%]
    {149} ae d1 18 80 02 c3 18 fa 8d fb 26 78 ff ff ff fe 02 db f0
    {150} f8 2e 18 80 02 c3 18 fc c6 fd 26 38 11 84 81 ff f0 68 00 [Temp 11.8 C  Hum 81%]
    {149} c4 7d 18 80 02 c3 18 fc 78 fd 29 28 ff ff ff fe 03 97 f0
    {149} 28 1e 18 80 02 c3 18 fb b7 fc 26 58 ff ff ff fe 02 c3 f0
    {150} 21 e8 18 80 02 c3 18 fb 9c fc 33 08 11 84 81 ff f0 b7 f8 [Temp 11.8 C  Hum 81%]
    {149} 83 ae 18 80 02 c3 18 fc 78 fc 29 28 ff ff ff fe 03 98 00
    {150} 5c e4 18 80 02 c3 18 fb ba fc 26 98 11 84 81 ff f0 16 00 [Temp 11.8 C  Hum 81%]
    {148} d0 bd 18 80 02 c3 18 f9 ad fa 26 48 ff ff ff fe 02 ff f0
    Wind and Temperature/Humidity or Rain:
        DIGEST:8h8h ID:8h8h8h8h STYPE:4h STARTUP:1b CH:3d WSPEED:~8h~4h ~4h~8h WDIR:12h ?4h TEMP:8h.4h TNEG:1b ?1b BATT:1b ?1b HUM:8h UV?~12h RAINFLAG:4h CHKSUM:8h
        DIGEST:8h8h ID:8h8h8h8h STYPE:4h STARTUP:1b CH:3d WSPEED:~8h~4h ~4h~8h WDIR:12h ?4h RAIN:~24h .......................... RAINFLAG:4h CHKSUM:8h
    Digest is LFSR-16 gen 0x8810 key 0x5412, excluding the add-checksum and trailer.
    Checksum is 8-bit add (with carry) to 0xff.
    Notes on different sensors:
    - 1910 084d 18 : RebeckaJohansson, VENTUS W835
    - 2030 088d 10 : mvdgrift, Wi-Fi Colour Weather Station with 5in1 Sensor, Art.No.: 7002580, ff 01 in the UV field is (obviously) invalid.
    - 1970 0d57 18 : danrhjones, bresser 5-in-1 model 7002580, no UV
    - 18b0 0301 18 : konserninjohtaja 6-in-1 outdoor sensor
    - 18c0 0f10 18 : rege245 BRESSER-PC-Weather-station-with-6-in-1-outdoor-sensor
    - 1880 02c3 18 : f4gqk 6-in-1
    - 18b0 0887 18 : npkap

    Parameters:

    msg     - Pointer to message
    msgSize - Size of message
    pOut    - Pointer to WeatherData

    Returns:

    DECODE_OK      - OK - WeatherData will contain the updated information
    DECODE_DIG_ERR - Digest Check Error
    DECODE_CHK_ERR - Checksum Error

    */
    DecodeStatus Bresser::decodeBresser6In1Payload(uint8_t *msg, uint8_t msgSize, WeatherData_t *pOut) {
        int const moisture_map[] = {0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 99}; // scale is 20/3

        // LFSR-16 digest, generator 0x8810 init 0x5412
        int chkdgst = (msg[0] << 8) | msg[1];
        int digest  = lfsr_digest16(&msg[2], 15, 0x8810, 0x5412);
        if (chkdgst != digest) {
            ESP_LOGV(TAG,"Digest check failed - %02X vs %02X",chkdgst,digest);
            return DECODE_DIG_ERR;
        }
        // Checksum, add with carry
        int chksum = msg[17];
        int sum    = add_bytes(&msg[2], 16); // msg[2] to msg[17]
        if ((sum & 0xff) != 0xff) {
            ESP_LOGV(TAG,"Checksum failed - %02X vs %02X",chksum,sum);
            return DECODE_CHK_ERR;
        }

        pOut->sensor_id  = ((uint32_t)msg[2] << 24) | (msg[3] << 16) | (msg[4] << 8) | (msg[5]);
        pOut->s_type     = (msg[6] >> 4); // 1: weather station, 2: indoor?, 4: soil probe
        // Raw bit 3 of msg[6] is 0 right after a power-on/reset and 1 from
        // about an hour on (rtl_433 #1214) - so "startup" = reset within ~1 h,
        // which also means the rain counter has just been cleared.
        pOut->startup = (msg[6] & 0x8) == 0;
        pOut->chan       = (msg[6] & 0x7);

        // temperature, humidity, shared with rain counter, only if valid BCD digits
        pOut->temp_ok  = msg[12] <= 0x99 && (msg[13] & 0xf0) <= 0x90 && msg[14] <= 0xA0 && (msg[14] & 0xf0) <= 0xA0;
        int temp_raw   = (msg[12] >> 4) * 100 + (msg[12] & 0x0f) * 10 + (msg[13] >> 4);
        float temp_c   = temp_raw * 0.1f;
        if (temp_raw > 600)
            temp_c = (temp_raw - 1000) * 0.1f;
        pOut->temp_c   = temp_c;
        pOut->humidity = (msg[14] >> 4) * 10 + (msg[14] & 0x0f);

        // Battery flag: msg[13] bit 1 is battery_good (rtl_433 #1993,
        // verified against the station console's low-battery icon). Only
        // temperature/humidity frames carry it - on rain frames msg[13]
        // holds rain-counter BCD digits, so the flag is invalid there.
        // Must be read before the rain decoding below XORs msg[13].
        pOut->battery_ok    = (msg[13] >> 1) & 1;
        pOut->battery_valid = pOut->temp_ok;

        // apparently ff0(1) if not available
        pOut->uv_ok  = msg[15] <= 0x99 && (msg[16] & 0xf0) <= 0x90;
        int uv_raw = ((msg[15] & 0xf0) >> 4) * 100 + (msg[15] & 0x0f) * 10 + ((msg[16] & 0xf0) >> 4);
        pOut->uv   = uv_raw * 0.1f;
        int flags  = (msg[16] & 0x0f); // looks like some flags, not sure

        // invert 3 bytes wind speeds
        msg[7] ^= 0xff;
        msg[8] ^= 0xff;
        msg[9] ^= 0xff;
        pOut->wind_ok = (msg[7] <= 0x99) && (msg[8] <= 0x99) && (msg[9] <= 0x99);

        int gust_raw              = (msg[7] >> 4) * 100 + (msg[7] & 0x0f) * 10 + (msg[8] >> 4);
        pOut->wind_gust_meter_sec = gust_raw * 0.1f;
        int wavg_raw              = (msg[9] >> 4) * 100 + (msg[9] & 0x0f) * 10 + (msg[8] & 0x0f);
        pOut->wind_avg_meter_sec  = wavg_raw * 0.1f;
        pOut->wind_direction_deg  = (((msg[10] & 0xf0) >> 4) * 100 + (msg[10] & 0x0f) * 10 + ((msg[11] & 0xf0) >> 4)) * 1.0f;

        // rain counter, inverted 3 bytes BCD, shared with temp/hum, only if valid digits
        msg[12] ^= 0xff;
        msg[13] ^= 0xff;
        msg[14] ^= 0xff;
        pOut->rain_ok   = msg[12] <= 0x99 && msg[13] <= 0x99 && msg[14] <= 0x99;
        int rain_raw    = (msg[12] >> 4) * 100000 + (msg[12] & 0x0f) * 10000
                + (msg[13] >> 4) * 1000 + (msg[13] & 0x0f) * 100
                + (msg[14] >> 4) * 10 + (msg[14] & 0x0f);
        pOut->rain_mm   = rain_raw * 0.1f;

        pOut->moisture_ok = false;
        if (pOut->s_type == 4 && pOut->temp_ok && pOut->humidity >= 1 && pOut->humidity <= 16) {
            pOut->moisture_ok = true;
            pOut->moisture = moisture_map[pOut->humidity - 1];
        }
        return DECODE_OK;
    }


    //
    // Get weather data from CC1101 receiver and decode it.
    // Non-blocking: only acts when GDO0 signals a packet in the FIFO.
    //
    void Bresser::getWeatherdata(void)
    {
        // GDO0 is mapped to "RX FIFO full or packet end" (see setup) -
        // high means a complete packet is waiting in the FIFO
        if (this->pin_GDO0_->digital_read() == false) {
            return;
        }

        uint8_t recvData[27];
        int state = this->radio->readData(recvData, 27);

        // readData leaves the CC1101 in idle with a flushed FIFO -
        // re-enter receive mode for the next frame
        int rearm = this->radio->startReceive();
        if (state != RADIOLIB_ERR_NONE || rearm != RADIOLIB_ERR_NONE) {
            ESP_LOGV(TAG, "[CC1101] Receive failed - read [%d], rearm [%d]", state, rearm);
            return;
        }

        // Remember radio link diagnostics of this frame
        this->last_rssi_ = radio->getRSSI();
        this->last_lqi_ = radio->getLQI();

        ESP_LOGV(TAG," R [0x%02X] RSSI: %f LQI: %d", recvData[0], this->last_rssi_, this->last_lqi_);

        // Verify last syncword is 1st byte of payload (see setup)
        if (recvData[0] != 0xD4) {
            return;
        }

        ESP_LOGV(TAG, " Data:");
        printRawdata(recvData, sizeof(recvData));

        // Decode the information - skip the last sync byte we used to check the data is OK
        bool decode_ok;
        if (this->model_ == BRESSER_MODEL_6IN1) {
            decode_ok = (decodeBresser6In1Payload(&recvData[1], sizeof(recvData) - 1, &frame_) == DECODE_OK);
            ESP_LOGV(TAG,"decode_ok: %d, temp_ok: %d, wind_ok: %d, rain_ok: %d",
                decode_ok, frame_.temp_ok, frame_.wind_ok, frame_.rain_ok);
        } else {
            decode_ok = (decodeBresser5In1Payload(&recvData[1], sizeof(recvData) - 1, &frame_) == DECODE_OK);

            // The 5-in-1 payload always carries all fields - mark them valid
            frame_.temp_ok     = true;
            frame_.uv_ok       = false;
            frame_.wind_ok     = true;
            frame_.rain_ok     = true;
            frame_.moisture_ok = false;
        }

        if (!decode_ok) {
            return;
        }

        // Track every station heard on the frequency (for the
        // foreign_sensor_ids text sensor; our own station is excluded
        // from that list only when filter_sensor_id is set)
        this->trackStation(frame_.sensor_id, this->last_rssi_);

        // Optionally ignore frames from foreign stations
        if (this->has_filter_sensor_id_ && (frame_.sensor_id != this->filter_sensor_id_)) {
            ESP_LOGD(TAG, "Ignoring foreign sensor id [0x%08X]", (unsigned) frame_.sensor_id);
            return;
        }

        ESP_LOGV(TAG,"Id: [0x%08X] Battery: [%s] Startup: [%d] ",
            (unsigned) frame_.sensor_id,
            frame_.battery_valid ? (frame_.battery_ok ? "OK " : "Low") : " - ",
            frame_.startup ? 1 : 0);
        if (this->model_ == BRESSER_MODEL_6IN1) {
            ESP_LOGV(TAG,"Ch: [%d] ", frame_.chan);
        }
        if (frame_.temp_ok) {
            ESP_LOGV(TAG,"Temp: [%3.1fC] Hum: [%3d%%] ",
                frame_.temp_c,
                frame_.humidity);
        } else {
            ESP_LOGV(TAG,"Temp: [---.-C] Hum: [---%%] ");
        }
        if (frame_.wind_ok) {
            ESP_LOGV(TAG,"Wind max: [%3.1fm/s] Wind avg: [%3.1fm/s] Wind dir: [%4.1fdeg] ",
                frame_.wind_gust_meter_sec,
                frame_.wind_avg_meter_sec,
                frame_.wind_direction_deg);
        } else {
            ESP_LOGV(TAG,"Wind max: [--.-m/s] Wind avg: [--.-m/s] ");
        }
        if (frame_.rain_ok) {
            ESP_LOGV(TAG,"Rain: [%6.1fmm] ",
                frame_.rain_mm);
        } else {
            ESP_LOGV(TAG,"Rain: [-----.-mm] ");
        }
        if (frame_.moisture_ok) {
            ESP_LOGV(TAG,"Moisture: [%2d%%]",
                frame_.moisture);
        }

        this->mergeFrame();
    }

    //
    // Merge a decoded frame into the buffer that update() publishes.
    // The station alternates between frame types (temperature/humidity and
    // rain), so both must accumulate within one update window - otherwise
    // only the fields of the most recent frame type would ever publish.
    // Wind gust keeps the peak value within the window.
    //
    void Bresser::mergeFrame(void)
    {
        // metadata - every frame carries it, keep the latest
        this->buf_.sensor_id  = frame_.sensor_id;
        this->buf_.s_type     = frame_.s_type;
        this->buf_.chan       = frame_.chan;
        this->buf_.startup    = frame_.startup;
        // The battery flag is not carried by every frame type (6-in-1 rain
        // frames use the same field for the rain counter) - keep the last
        // known value until a frame carrying it arrives.
        if (frame_.battery_valid) {
            this->buf_.battery_ok    = frame_.battery_ok;
            this->buf_.battery_valid = true;
        }

        if (frame_.temp_ok) {
            this->buf_.temp_ok  = true;
            this->buf_.temp_c   = frame_.temp_c;
            this->buf_.humidity = frame_.humidity;
        }
        if (frame_.wind_ok) {
            this->buf_.wind_ok = true;
            this->buf_.wind_avg_meter_sec = frame_.wind_avg_meter_sec;
            this->buf_.wind_direction_deg = frame_.wind_direction_deg;
            // strongest gust within the update window, not just the latest
            if (frame_.wind_gust_meter_sec > this->buf_.wind_gust_meter_sec)
                this->buf_.wind_gust_meter_sec = frame_.wind_gust_meter_sec;
        }
        if (frame_.rain_ok) {
            this->buf_.rain_ok = true;
            this->buf_.rain_mm = frame_.rain_mm;
        }
        if (frame_.uv_ok) {
            this->buf_.uv_ok = true;
            this->buf_.uv = frame_.uv;
        }
        if (frame_.moisture_ok) {
            this->buf_.moisture_ok = true;
            this->buf_.moisture = frame_.moisture;
        }

        this->frame_seen_ = true;
    }

    //
    // Publish the buffered data to the entities and start a new window.
    //
    void Bresser::publishWeatherdata(void)
    {
        if (!this->frame_seen_) {
            return;  // no usable frame decoded since the last publish
        }

        if (this->buf_.temp_ok){
            if (this->temperature_sensor_ != nullptr)
                this->temperature_sensor_->publish_state(this->buf_.temp_c);
            if (this->humidity_sensor_ != nullptr)
                this->humidity_sensor_->publish_state(this->buf_.humidity);
        }
        if (this->buf_.wind_ok){
            if (this->wind_speed_sensor_ != nullptr)
                this->wind_speed_sensor_->publish_state(this->buf_.wind_avg_meter_sec);
            if (this->wind_gust_sensor_ != nullptr)
                this->wind_gust_sensor_->publish_state(this->buf_.wind_gust_meter_sec);
            if (this->wind_direction_sensor_ != nullptr)
                this->wind_direction_sensor_->publish_state(this->buf_.wind_direction_deg);
        }
        if (this->buf_.rain_ok) {
            if (this->rain_sensor_ != nullptr)
                this->rain_sensor_->publish_state(this->buf_.rain_mm);
        }

        // Station metadata - present in every decoded frame
        if (this->battery_sensor_ != nullptr && this->buf_.battery_valid)
            this->battery_sensor_->publish_state(this->buf_.battery_ok ? 1.0f : 0.0f);
        if (this->startup_binary_sensor_ != nullptr)
            this->startup_binary_sensor_->publish_state(this->buf_.startup);
        // Station ID is a 32-bit identifier - exceeds float precision,
        // so it is exposed as text ("0x%08X") rather than a numeric sensor
        if (this->sensor_id_text_sensor_ != nullptr) {
            char id_buf[16];
            snprintf(id_buf, sizeof(id_buf), "0x%08X", (unsigned) this->buf_.sensor_id);
            this->sensor_id_text_sensor_->publish_state(id_buf);
        }

        // Channel and sensor type are 6-in-1 protocol fields
        if (this->model_ == BRESSER_MODEL_6IN1) {
            if (this->channel_sensor_ != nullptr)
                this->channel_sensor_->publish_state((float) this->buf_.chan);
            if (this->sensor_type_sensor_ != nullptr)
                this->sensor_type_sensor_->publish_state((float) this->buf_.s_type);
        }

        // Optional probe data - only when the station transmits it
        if (this->buf_.uv_ok && this->uv_sensor_ != nullptr)
            this->uv_sensor_->publish_state(this->buf_.uv);
        if (this->buf_.moisture_ok && this->moisture_sensor_ != nullptr)
            this->moisture_sensor_->publish_state((float) this->buf_.moisture);

        // Radio link diagnostics of the last received frame
        if (this->rssi_sensor_ != nullptr)
            this->rssi_sensor_->publish_state(this->last_rssi_);
        if (this->lqi_sensor_ != nullptr)
            this->lqi_sensor_->publish_state((float) this->last_lqi_);

        // start a new aggregation window
        this->frame_seen_ = false;
        this->buf_.temp_ok     = false;
        this->buf_.uv_ok       = false;
        this->buf_.wind_ok     = false;
        this->buf_.rain_ok     = false;
        this->buf_.moisture_ok = false;
        this->buf_.wind_gust_meter_sec = 0.0f;
    }

    //
    // Track a station heard on the frequency (sender of a decoded frame),
    // keeping its strongest RSSI. No-op unless the foreign_sensor_ids
    // text sensor is configured.
    //
    void Bresser::trackStation(uint32_t id, float rssi)
    {
        if (this->foreign_sensor_ids_text_sensor_ == nullptr)
            return;

        uint32_t now = millis();

        for (uint8_t i = 0; i < this->foreign_station_count_; i++) {
            if (this->foreign_stations_[i].id == id) {
                if (rssi > this->foreign_stations_[i].rssi_max)
                    this->foreign_stations_[i].rssi_max = rssi;
                this->foreign_stations_[i].last_seen_ms = now;
                return;
            }
        }

        // new station - take a free slot, or replace the stalest entry
        uint8_t slot = this->foreign_station_count_;
        if (slot >= MAX_FOREIGN_STATIONS) {
            slot = 0;
            for (uint8_t i = 1; i < MAX_FOREIGN_STATIONS; i++) {
                if ((now - this->foreign_stations_[i].last_seen_ms) > (now - this->foreign_stations_[slot].last_seen_ms))
                    slot = i;
            }
        } else {
            this->foreign_station_count_++;
        }
        this->foreign_stations_[slot].id           = id;
        this->foreign_stations_[slot].rssi_max      = rssi;
        this->foreign_stations_[slot].last_seen_ms = now;
    }

    //
    // Publish the list of foreign stations, e.g.
    //   "0x12345678 (-67 dBm), 0x9ABCDEF0 (-81 dBm)"
    // strongest signal first. Stations not heard from within
    // FOREIGN_STATION_TIMEOUT_MS drop off; our own station is excluded
    // when filter_sensor_id is set (with no filter set, every station
    // in range is listed). Only publishes when the list changed.
    //
    void Bresser::publishForeignStations(void)
    {
        if (this->foreign_sensor_ids_text_sensor_ == nullptr)
            return;

        uint32_t now = millis();

        // prune stations not heard from recently
        uint8_t n = 0;
        for (uint8_t i = 0; i < this->foreign_station_count_; i++) {
            if ((now - this->foreign_stations_[i].last_seen_ms) <= FOREIGN_STATION_TIMEOUT_MS)
                this->foreign_stations_[n++] = this->foreign_stations_[i];
        }
        this->foreign_station_count_ = n;

        // sort by strongest signal first
        for (uint8_t i = 1; i < this->foreign_station_count_; i++) {
            ForeignStation key = this->foreign_stations_[i];
            int8_t j = i - 1;
            while (j >= 0 && this->foreign_stations_[j].rssi_max < key.rssi_max) {
                this->foreign_stations_[j + 1] = this->foreign_stations_[j];
                j--;
            }
            this->foreign_stations_[j + 1] = key;
        }

        char list[sizeof(this->foreign_stations_last_)];
        list[0] = 0;
        for (uint8_t i = 0; i < this->foreign_station_count_; i++) {
            const ForeignStation &e = this->foreign_stations_[i];
            if (this->has_filter_sensor_id_ && (e.id == this->filter_sensor_id_))
                continue;  // our own station is not foreign
            char entry[32];
            snprintf(entry, sizeof(entry), "%s0x%08X (%d dBm)",
                     list[0] ? ", " : "", (unsigned) e.id, (int) e.rssi_max);
            strncat(list, entry, sizeof(list) - strlen(list) - 1);
        }

        if (strcmp(list, this->foreign_stations_last_) != 0) {
            strncpy(this->foreign_stations_last_, list, sizeof(this->foreign_stations_last_) - 1);
            this->foreign_stations_last_[sizeof(this->foreign_stations_last_) - 1] = 0;
            this->foreign_sensor_ids_text_sensor_->publish_state(list);
        }
    }

    //
    // Print raw payload
    //
    void Bresser::printRawdata(uint8_t *msg, uint8_t msgSize) {
        char s[120];
        s[0] = 0;
        ESP_LOGV(TAG,"Raw Data:");
        for (uint8_t p = 0 ; p < msgSize ; p++) {
            int len = strlen(s);
            if (len < (int) sizeof(s) - 4) {
                snprintf(s + len, 4, "%02X ", msg[p]);
            }
        }
        ESP_LOGV(TAG, " %s", s);
    }

}  // namespace bresser
}  // namespace esphome
