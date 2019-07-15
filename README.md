# ESP8266 sensor node

ESP8266 firmware that talks to one or two BME280 sensors via I²C and exposes
their data as Prometheus metrics. Uses a coroutine-based HTTP server to
support concurrent connections (mostly to avoid hanging the whole server
if a single client hangs).

Various third-party code is vendored under libs/ and licensed under various
licenses (see the file headers). The code under src/ is licensed under the
MIT license.

TODO: add IR blasting support

## Building

Copy `src/config.h.sample` to `src/config.h` and fill in your WiFi SSID/PSK.

    make ESP_SDK=<path to esp-open-sdk> SDK_BASE=<path to ESP8266_NONOS_SDK-2.2.1>

