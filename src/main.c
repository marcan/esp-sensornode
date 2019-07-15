#include "config.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "ip_addr.h"
#include "user_interface.h"

#include "httpserver.h"
#include "printf.h"
#include "bme280.h"
#include "i2c_master.h"

httpserver_t *hs;

#define MAX_SENSORS 2

#define PIN_SDA 0
#define PIN_SCL 2
#define PIN_IR 3

int bme_present[MAX_SENSORS] = { 0 };
struct bme280_dev bme[MAX_SENSORS];
const int addresses[MAX_SENSORS] = {
  0x76, 0x77
};

ICACHE_FLASH_ATTR
uint32 user_rf_cal_sector_set(void)
{
    enum flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0;

    switch (size_map) {
        case FLASH_SIZE_4M_MAP_256_256:
            rf_cal_sec = 128 - 5;
            break;

        case FLASH_SIZE_8M_MAP_512_512:
            rf_cal_sec = 256 - 5;
            break;

        case FLASH_SIZE_16M_MAP_512_512:
        case FLASH_SIZE_16M_MAP_1024_1024:
            rf_cal_sec = 512 - 5;
            break;

        case FLASH_SIZE_32M_MAP_512_512:
        case FLASH_SIZE_32M_MAP_1024_1024:
            rf_cal_sec = 1024 - 5;
            break;

        case FLASH_SIZE_64M_MAP_1024_1024:
            rf_cal_sec = 2048 - 5;
            break;
        case FLASH_SIZE_128M_MAP_1024_1024:
            rf_cal_sec = 4096 - 5;
            break;
        default:
            rf_cal_sec = 0;
            break;
    }

    return rf_cal_sec;
}

void user_delay_ms(uint32_t period)
{
    os_delay_us(1000 * period);
}

int8_t user_i2c_read(uint8_t dev_id, uint8_t reg_addr, uint8_t *reg_data, uint16_t len)
{
    i2c_master_start();
    i2c_master_writeByte(dev_id << 1);
    if (i2c_master_getAck()) {
        i2c_master_stop();
        return -1;
    }
    i2c_master_writeByte(reg_addr);
    if (i2c_master_getAck()) {
        i2c_master_stop();
        return -1;
    }
    i2c_master_stop();
    i2c_master_start();
    i2c_master_writeByte((dev_id << 1) | 1);
    if (i2c_master_getAck()) {
        i2c_master_stop();
        return -1;
    }

    while (len--) {
        *reg_data++ = i2c_master_readByte();
        i2c_master_setAck(len == 0);
    }

    i2c_master_stop();
    return 0;
}

int8_t user_i2c_write(uint8_t dev_id, uint8_t reg_addr, uint8_t *reg_data, uint16_t len)
{
    i2c_master_start();
    i2c_master_writeByte(dev_id << 1);
    if (i2c_master_getAck()) {
        i2c_master_stop();
        return -1;
    }
    i2c_master_writeByte(reg_addr);
    if (i2c_master_getAck()) {
        i2c_master_stop();
        return -1;
    }

    while (len--) {
        i2c_master_writeByte(*reg_data++);
        if (i2c_master_getAck()) {
            i2c_master_stop();
            return -1;
        }
    }

    i2c_master_stop();
    return 0;
}

ICACHE_FLASH_ATTR
void handle_root(httpconn_t *conn, char *path, char *query_string)
{
    char lbuf[256];
    int ret;

    httpserver_end_request(conn);
    httpserver_start_response(conn, 200, "OK");
    httpserver_send_header(conn, "Content-Type", "text/html");
    httpserver_end_headers(conn);

    httpserver_write_string(conn,
"<!doctype html>\
  <html><head>\
    <meta http-equiv='refresh' content='30'>\
    <title>ESP8266 Temperature Sensor</title>\
  </head><body>\
    <h1>ESP8266 temperature & IR remote server</h1>\
    <h3>Status</h3>\
    <p>");

    sprintf(lbuf, "Hostname: %s<br>", wifi_station_get_hostname());
    httpserver_write_string(conn, lbuf);

    struct ip_info info;
    wifi_get_ip_info(STATION_IF, &info);
    sprintf(lbuf, "IP address: %d.%d.%d.%d<br>", IP2STR(&info.ip));
    httpserver_write_string(conn, lbuf);

    uint8 mac[6];
    wifi_get_macaddr(STATION_IF, mac);
    sprintf(lbuf, "MAC address: %02x:%02x:%02x:%02x:%02x:%02x<br></p>",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    httpserver_write_string(conn, lbuf);

    for (int i = 0; i < MAX_SENSORS; i++) {
        if (!bme_present[i])
            continue;

        sprintf(lbuf, "<h3>Sensor %d</h3><p>", i);
        httpserver_write_string(conn, lbuf);

        ret = bme280_set_sensor_mode(BME280_FORCED_MODE, &bme[i]);
        if (ret != BME280_OK) {
            sprintf(lbuf, "Failed to set mode (%d).</p>", ret);
            httpserver_write_string(conn, lbuf);
            continue;
        }
        for (int j = 0; j < 1000; j++) {
            if (!bme280_is_busy(&bme[i]))
                break;
            os_delay_us(100);
        }
        if (bme280_is_busy(&bme[i])) {
            httpserver_write_string(conn, "Timed out waiting for measurement.</p>");
            continue;
        }
        struct bme280_data comp_data;
        ret = bme280_get_sensor_data(BME280_ALL, &comp_data, &bme[i]);
        if (ret != BME280_OK) {
            sprintf(lbuf, "Failed to read data (%d).</p>", ret);
            httpserver_write_string(conn, lbuf);
            continue;
        }
        sprintf(lbuf,
                "Temperature: %.02f &deg;C<br>"
                "Pressure: %.02f hPa<br>"
                "Humidity: %.02f RH%%</p>",
                comp_data.temperature,
                comp_data.pressure / 100.0,
                comp_data.humidity);
        httpserver_write_string(conn, lbuf);
    }

    httpserver_write_string(conn, "</body></html>");
}

ICACHE_FLASH_ATTR
void handle_metrics(httpconn_t *conn, char *path, char *query_string)
{
    char lbuf[256];
    int read_ok[MAX_SENSORS];
    int ret;
    struct bme280_data comp_data[MAX_SENSORS];

    httpserver_end_request(conn);
    httpserver_start_response(conn, 200, "OK");
    httpserver_send_header(conn, "Content-Type", "text/plain; version=0.0.4");
    httpserver_end_headers(conn);

    for (int i = 0; i < MAX_SENSORS; i++) {
        read_ok[i] = 0;
        if (!bme_present[i])
            continue;
        ret = bme280_set_sensor_mode(BME280_FORCED_MODE, &bme[i]);
        if (ret != BME280_OK) {
            sprintf(lbuf, "# sensor=%d failed to set mode (%d)\n", i, ret);
            httpserver_write_string(conn, lbuf);
            sprintf(lbuf, "sensor_read_status{sensor=\"%d\"} %d\n", i, ret );
            httpserver_write_string(conn, lbuf);
            continue;
        }
        for (int j = 0; j < 1000; j++) {
            if (!bme280_is_busy(&bme[i]))
                break;
            os_delay_us(100);
        }
        if (bme280_is_busy(&bme[i])) {
            sprintf(lbuf, "# sensor=%d timed out\n", i);
            httpserver_write_string(conn, lbuf);
            sprintf(lbuf, "sensor_read_status{sensor=\"%d\"} %d\n", i, -100);
            httpserver_write_string(conn, lbuf);
            continue;
        }
        ret = bme280_get_sensor_data(BME280_ALL, &comp_data[i], &bme[i]);
        if (ret != BME280_OK) {
            sprintf(lbuf, "# sensor=%d failed to read data (%d)\n", i, ret);
            httpserver_write_string(conn, lbuf);
            sprintf(lbuf, "sensor_read_status{sensor=\"%d\"} %d\n", i, ret - 200);
            httpserver_write_string(conn, lbuf);
            continue;
        }
        read_ok[i] = 1;
        if (read_ok[i]) {
            sprintf(lbuf, "sensor_read_status{sensor=\"%d\"} 0\n", i);
            httpserver_write_string(conn, lbuf);
        }
    }

    for (int i = 0; i < MAX_SENSORS; i++) {
        if (read_ok[i]) {
            sprintf(lbuf, "sensor_temperature_celsius{sensor=\"%d\"} %.02f\n",
                    i, comp_data[i].temperature);
            httpserver_write_string(conn, lbuf);
        }
    }

    for (int i = 0; i < MAX_SENSORS; i++) {
        if (read_ok[i]) {
            sprintf(lbuf, "sensor_pressure_pascals{sensor=\"%d\"} %.02f\n",
                    i, comp_data[i].pressure);
            httpserver_write_string(conn, lbuf);
        }
    }

    for (int i = 0; i < MAX_SENSORS; i++) {
        if (read_ok[i]) {
            sprintf(lbuf, "sensor_humidity_relative{sensor=\"%d\"} %.05f\n",
                    i, comp_data[i].humidity / 100.0f);
            httpserver_write_string(conn, lbuf);
        }
    }

}

ICACHE_FLASH_ATTR
void handle_ir(httpconn_t *conn, char *path, char *query_string)
{
    httpserver_end_request(conn);
    httpserver_start_response(conn, 200, "OK");
    httpserver_end_headers(conn);
}

ICACHE_FLASH_ATTR
void user_set_station_config(void)
{
    char ssid[32] = CONFIG_WIFI_SSID;
    char password[64] = CONFIG_WIFI_PASSWORD;
    struct station_config stationConf;

    stationConf.bssid_set = 0;
    os_memcpy(&stationConf.ssid, ssid, 32);
    os_memcpy(&stationConf.password, password, 64);
    wifi_station_set_config(&stationConf);
}

ICACHE_FLASH_ATTR
void init_done(void)
{
    os_printf("init_done\n");
    wifi_set_opmode(STATION_MODE);
    wifi_set_sleep_type(LIGHT_SLEEP_T);

    user_set_station_config();

    for (int i = 0; i < MAX_SENSORS; i++) {
        bme_present[i] = 0;
        bme[i].dev_id = addresses[i];
        bme[i].intf = BME280_I2C_INTF;
        bme[i].read = user_i2c_read;
        bme[i].write = user_i2c_write;
        bme[i].delay_ms = user_delay_ms;

        if (bme280_init(&bme[i]) != BME280_OK) {
            os_printf("bme[%d]: absent\n", i);
            continue;
        }

        os_printf("bme[%d]: present\n", i);
        if (bme280_soft_reset(&bme[i]) != BME280_OK) {
            os_printf("bme[%d]: soft reset failed\n", i);
            continue;
        }
        if (bme280_set_sensor_mode(BME280_SLEEP_MODE, &bme[i]) != BME280_OK) {
            os_printf("bme[%d]: set sleep mode failed\n", i);
            continue;
        }
        bme[i].settings.osr_h = BME280_OVERSAMPLING_16X;
        bme[i].settings.osr_p = BME280_OVERSAMPLING_2X;
        bme[i].settings.osr_t = BME280_OVERSAMPLING_2X;
        bme[i].settings.filter = BME280_FILTER_COEFF_OFF;

        int settings_sel = BME280_OSR_PRESS_SEL | BME280_OSR_TEMP_SEL |
                           BME280_OSR_HUM_SEL | BME280_FILTER_SEL;

        if (bme280_set_sensor_settings(settings_sel, &bme[i]) != BME280_OK) {
            os_printf("bme[%d]: set sensor settings failed\n", i);
            continue;
        }
        if (bme280_set_sensor_mode(BME280_FORCED_MODE, &bme[i]) != BME280_OK) {
            os_printf("bme[%d]: set forced mode failed\n", i);
            continue;
        }
        for (int j = 0; j < 1000; j++) {
            if (!bme280_is_busy(&bme[i]))
                break;
            os_delay_us(100);
        }
        if (bme280_is_busy(&bme[i])) {
            os_printf("bme[%d]: timed out testing measurement\n", i);
            continue;
        }
        struct bme280_data comp_data;
        bme280_get_sensor_data(BME280_ALL, &comp_data, &bme[i]);
        printf("bme[%d]: %0.2f C   %0.2f %%   %0.2f Pa\r\n", i, 
               comp_data.temperature, comp_data.pressure, comp_data.humidity);
        bme_present[i] = 1;
    }

    hs = httpserver_init(80, 2);
    if (!hs) {
        os_printf("HTTP server init failed!\n");
        return;
    }

    httpserver_route(hs, "/", handle_root);
    httpserver_route(hs, "/metrics", handle_metrics);
    httpserver_route(hs, "/ir", handle_ir);

    httpserver_start(hs);

}

ICACHE_FLASH_ATTR
void user_init(void)
{
    uart_div_modify(0, UART_CLK_FREQ / 115200);
    os_printf("\n\n\n");
    os_printf("SDK version:%s\n", system_get_sdk_version());

    system_init_done_cb(init_done);

    i2c_master_gpio_init();

    os_printf("user_init done\n");
}
