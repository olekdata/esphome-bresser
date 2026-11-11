# esphome-bresser

ESPHome component receiving weather data from Bresser stations via a CC1101
receiver. Decoder logic based on
https://github.com/matthias-bs/Bresser_Weather_Sensor_CC1101_MQTT and the
rtl_433 project.

More info
https://olekdata.pl/stacja-pogodowa-bresser5in1-wspolpraca-z-home-assistant/

> Works with RadioLib 7.x and 8.0.0+ (the `ConfigFSK_t` unit change in 8.0.0
> is detected and handled automatically - no library version pinning needed).

## YAML definition examples

```yaml

libraries:
    - jgromes/RadioLib

esp8266:
  board: d1_mini_lite

# No spi: block - RadioLib drives the Arduino SPI object (the ESP8266
# hardware SPI bus) itself and the component declares the SPI library
# automatically. The ESP8266 core fixes the SPI pins - they must be the
# defaults: SCLK GPIO14 (D5), MISO GPIO12 (D6), MOSI GPIO13 (D7).

external_components:
  source:
    type: git
    url: https://github.com/olekdata/esphome-bresser
    ref: main

bresser:
  PIN_CS:   2
  PIN_GDO0: 4
  PIN_GDO2: 5

  # Weather station model - selects the payload decoder (default: 5in1)
  #   5in1 - Bresser 5-in-1 (legacy protocol)
  #   6in1 - Bresser 6-in-1 / new 5-in-1 / 7-in-1 / Froggit WH6000 protocol
  model: 6in1

  # Only accept frames from this station id - drops any neighbour station
  # on the same frequency (find your id in the logs: "Id: [0x...]" or the
  # "ID stacji" text sensor)
  # filter_sensor_id: 0x36582164

  # Entity publish cadence (default: 60s). Reception runs non-blocking in
  # the background; every window publishes the latest value per field and
  # the strongest wind gust seen within the window. Raise it to keep
  # Home Assistant's database writes down, lower it for fresher data.
  update_interval: 60s

  # Optional CC1101 RF configuration overrides (defaults shown).
  # Override them when your CC1101 module or region differs - no need to
  # edit or fork the component code.
  # frequency: 868.35          # MHz
  # bit_rate: 8.22             # kbps
  # frequency_deviation: 57.1  # kHz
  # receiver_bandwidth: 270.0  # kHz
  # power: 10                  # dBm
  # preamble_length: 32        # bits


sensor:
  - platform: bresser
    temperature:
      name: "Temperatura"
    humidity:
      name: "Wilgotność"
    wind_speed:
      name: "Prędkość wiatru"
    wind_gust:
      name: "Poryw wiatru"
    wind_direction_degrees:
      name: "Kierunek wiatru"
    rain:
      name: "Deszcz"
    battery_ok:
      name: "Bateria"
    sensor_type:
      name: "Typ czujnika"
    channel:
      name: "Kanał"
    rssi:
      name: "RSSI"
    lqi:
      name: "LQI"
    # Optional - only if the station transmits them:
    #uv:
    #  name: "Indeks UV"
    #moisture:
    #  name: "Wilgotność gleby"

text_sensor:
  - platform: bresser
    # 32-bit station ID - exceeds float precision, published as "0x%08X"
    sensor_id:
      name: "ID stacji"
    # Foreign stations heard on the frequency, e.g.
    # "0x12345678 (-67 dBm), 0x9ABCDEF0 (-81 dBm)" (strongest first).
    # Entries not heard from for 10 minutes drop off (max 8 tracked).
    # With no filter_sensor_id set, all stations in range are listed -
    # handy for finding your own station's id before configuring the filter.
    foreign_sensor_ids:
      name: "Stacje obce"
```

## Notes

- Reception is non-blocking: the CC1101 stays in receive mode and GDO0 is
  polled in `loop()` - nothing stalls the ESPHome event loop.
- The station alternates between frame types (e.g. temperature/humidity
  and rain) roughly every 12 seconds. Frames decoded within one
  `update_interval` window are merged, so all fields publish together;
  wind gust publishes the peak within the window, not just the latest.
- The station ID is a 32-bit identifier which exceeds float precision,
  hence the `text_sensor` platform for it.
- Omitting any entity from YAML is safe - all publishes are null-checked.

## Connection example

```yaml

#  ***********************  Podłączenie fizyczne modułów ****************************************
#
#                                    +-----------------+| 
#                                    |    |  |  |  |    |
#                              [RST] |o                o| [TX ] [GPIO1]
#                     [ ADC1 ] [ A0] |o                o| [RX ] [GPIO3]
#                     [GPIO16] [ D0] |o   W E M O S    o| [D1 ] [GPIO5]   6 zielony
#  4 fiolet    [ SCK] [GPIO14] [ D5] |o                o| [D2 ] [GPIO4]   7 żółty
#  5 niebie    [MISO] [GPIO12] [ D6] |o   D1 MINI      o| [D3 ] [GPIO0]
#  3 biały     [MOSI] [GPIO13] [ D7] |o                o| [D4 ] [GPIO2]   8 pomar
#              [ SS ] [GPIO15] [ D8] |o                o| [GND]           2 szary
#  1 czarny                    [3v3] |o                o| [5V ]
#                                      |                |
#                                      |----|usb|-------|
#
#
#                                    +---------------------+
#  1 czarny                  [ VCC]  |o                    |                 
#  2 szary                   [ GND]  |o                    |                 
#  3 biały                   [MOSI]  |o                   o| [GND]
#  4 fiolet                  [SCLK]  |o     CC1101        o| [ATN] /\/\/\/\/               
#  5 niebie                  [MISO]  |o                   o| [GND]                
#  6 zielony                 [GDO2]  |o                    |                 
#  7 żółty                   [GDO0]  |o                    |                 
#  8 pomar                   [ CSN]  |o                    |                 
#                                    +---------------------+
#
#  ESP8266 hardware SPI pins (fixed by the core - must be the defaults):
#    SCLK GPIO14 (D5), MISO GPIO12 (D6), MOSI GPIO13 (D7)
#  PIN_CS:   GPIO2   (D4)
#  PIN_GDO0: GPIO4   (D2)
#  PIN_GDO2: GPIO5   (D1)

# https://esphome.io/components/sensor/custom.html
#
# instalacja poprzez VSC wiecej 
# https://olekdata.pl/blog/2024/01/14/stacja-pogodowa-bresser5in1-wspolpraca-z-home-assistant/
#
```

### ESP32 (WROOM-32 DevKit)

The CC1101 sits on the VSPI bus. ESP32 signals route through the GPIO
matrix, so even the SPI pins are configurable (`PIN_SCK`/`PIN_MISO`/
`PIN_MOSI`, optional - unset means RadioLib's Arduino core defaults
SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23). CS, GDO0 and GDO2 can be any
input-capable GPIOs (avoid the input-only GPIO34-39).

| CC1101 | ESP32 DevKit | Notes |
|---|---|---|
| VCC | 3V3 | never 5V - the CC1101 is a 3.3 V part |
| GND | GND | |
| SCLK | GPIO18 | VSPI SCK (default) |
| MOSI | GPIO21 | VSPI MOSI, moved off the default GPIO23 via PIN_MOSI |
| MISO | GPIO19 | VSPI MISO (default) |
| CSN | GPIO5 | VSPI default CS |
| GDO0 | GPIO2 | RX FIFO / sync-word signal; strapping pin, see below |
| GDO2 | GPIO4 | optional, clear-channel signal |

```yaml
esp32:
  board: esp32dev
  framework:
    type: arduino

bresser:
  PIN_CS:   GPIO5
  PIN_GDO0: GPIO2
  PIN_GDO2: GPIO4
  # SPI bus pins - optional, unset defaults are GPIO18/19/23 (VSPI)
  PIN_SCK:  GPIO18
  PIN_MISO: GPIO19
  PIN_MOSI: GPIO21
```

No `spi:` block is needed on ESP32: the component enables the bundled
Arduino SPI library (RadioLib requires it) and RadioLib drives VSPI
directly. ESPHome's `spi:` component uses the ESP-IDF driver instead
and can clash with it over the same bus - leave it out.

GPIO2 notes: it is a strapping pin - entering UART download mode (USB
flashing with esptool) requires it to be low or floating, so if a
re-flash ever fails to connect with the CC1101 attached, temporarily
disconnect GDO0. Normal boot and OTA updates are not affected, and on
many DevKits GPIO2 also drives the on-board LED (harmless here).

On WROVER-based modules GPIO16/17 are used by PSRAM - this pinout
avoids them, but keep that in mind when reshuffling.
