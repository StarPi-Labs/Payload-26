/**
 * @file gps.c
 * @brief GPS UART driver — reads NMEA sentences in a background task.
 *
 * A FreeRTOS task continuously reads from the GPS UART and stores the
 * latest complete NMEA sentence.  The main sensor-read loop can poll
 * gps_read() to grab the most recent line.
 */

#include "gps.h"
#include "sensor_config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frame_logger.h"
#include "health_monitoring.h"
#include "flight_stats.h"
#include "nmea.h"

#include <string.h>

static const char *TAG = "GPS";


/* Overall GPS Interface parameters */
#define MAX_FAILED_ATTEMPTS     5       // if the GPS' uart fails to read 5 
                                        // consecutive times, gps is flagged
                                        // dead.

#define GPS_BUF_SIZE            1024
#define TIMEOUT_INIT            20      // Timeout on receiving the dataset
#define GPS_MAX_INIT_ATTEMPTS   1024    // Number of attempts including returned 
                                        // read bytes being 0, this depends on 
                                        // the number of NMEA sentences we are 
                                        // having.

#define MIN_NMEA                "$xxGGA"// According to the doc, we only need:
                                        // - $xxRMC // status the most important
                                        // - $xxGGA // time, lat and long.
                                        // where: 'xx' means it could be any two words there.
                                        // Remember we are using this onnly to 
                                        // see if the GPS is alive.
#define MIN_NMEA_SZ             6       // strlen(MIN_NMEA)
                                        //
#define NMEA_READY_INFO ((1 << RMC_FLAG_SENTENCE) | (1 << GGA_FLAG_SENTENCE))

/* UBX Configuration Command */
#define UBX_M10_NMEA_NO_GLL "$PUBX,40,GLL,0,0,0,0,0,0*5C\r\n\0"
#define UBX_M10_NMEA_NO_GSA "$PUBX,40,GSA,0,0,0,0,0,0*4E\r\n\0"
#define UBX_M10_NMEA_NO_GSV "$PUBX,40,GSV,0,0,0,0,0,0*59\r\n\0"
#define UBX_M10_NMEA_NO_VTG "$PUBX,40,VTG,0,0,0,0,0,0*5E\r\n\0"

/* ── UART background task ─────────────────────────────────── */
void gps_rx_task(void *arg) {
    
    // TODO: 
    // - we might need to pass the system health instead, probabily, idk
    //   so we can flag it dead.
    // - fix the waiting time in uart_read_bytes
    uint8_t bytes[128];
    uint8_t attempts = 0;
    uint8_t telemetry_counter = 0;
    struct GPSInfo gps_info = {0};

    struct TaskParams *tparams = (struct TaskParams *) arg;

    ESP_LOGI(TAG, "GPS RX task running");

    while (1) {
        int len = uart_read_bytes(GPS_UART, &bytes, 128, pdMS_TO_TICKS(10));  // I think 1 second waiting would be enough
                                                                            // because we are reading this at 1Hz as well
        if (len < 0) {
            attempts++;
            if (MAX_FAILED_ATTEMPTS == attempts) {
                // TODO: should i report this event and report GPS is dead?
                break;
            }
        } else if (0 == len) {
            continue;
        }

        attempts = 0;
        
        for (int i = 0; i < len; i++) {
            parse_nmea(&gps_info, bytes[i]);
        }

        if (gps_info.available == NMEA_READY_INFO) {
            flight_stats_tick(STAT_GPS);
            /* TODO: After parsed, this task should sleep for a second. */
            switch(tparams->context->mode) {
            case MODE_POST:
            case MODE_ARMED:
                ESP_LOGI(TAG, "status=%c speed=%s course=%s time=%s lat=%s%c lon=%s%c sats=%s alt=%s",
                    gps_info.status,
                    gps_info.speed,
                    gps_info.course,
                    gps_info.time,
                    gps_info.lat,
                    gps_info.lat_orientation,
                    gps_info.lon,
                    gps_info.lon_orientation,
                    gps_info.sat_count,
                    gps_info.alt);

                telemetry_counter++;
                if (telemetry_counter >= GPS_HM_SKIP_SAMPLES) {
                    gps_info.available = 0;
                    hm_send(
                        tparams->hm_buffer, 
                        SBIT_MQ10, 
                        (uint8_t *)&gps_info, 
                        sizeof(struct GPSInfo) - 1
                    );  // the available field is not send
                    telemetry_counter = 0;
                }
                break;
 
            case MODE_BOOST:
            case MODE_COAST:
                write_to_ring_buffer(
                    tparams->log_buffer,
                    SBIT_MQ10, 
                    (void *)&gps_info, 
                    sizeof(struct GPSInfo)
                );
                break;
            }
            memset(&gps_info, 0, sizeof(struct GPSInfo));
        }
        
    }

    // if GPS dead
    ESP_LOGE(TAG, "GPS stopped after repeated read failures");
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────── */


esp_err_t gps_probe(uart_port_t port) {
    int attempts;

    /* to read GPS data */
    uint8_t data[GPS_BUF_SIZE] = {0};
    int len;

    /* to find NMEA */
    const char required_min_NMEA[] = MIN_NMEA;
    int j;
    uint8_t *d;

    /* Wait for NMEA of interest defined in MIN_NMEA */
    j = 0;
    for (attempts = 0; attempts < GPS_MAX_INIT_ATTEMPTS; attempts++) {
        len = uart_read_bytes(port, data, GPS_BUF_SIZE, TIMEOUT_INIT / portTICK_PERIOD_MS);

        if (len < 0) {
            ESP_LOGE(TAG, "Nothing received on GPS UART PORT, timeout.");
            return ESP_ERR_TIMEOUT;

        } else if (len > 0) {
            // NOTE: 
            // Length check is done only because uart_read_bytes might touch
            // the buffer even if len returns zero. It's a way to say, i don't trust 
            // you, uart_read_bytes. I don't like it, but safety reasons. 
            for (d = data; *d; d++) {
                if ((required_min_NMEA[j] == *d) || (1 == j) || (2 == j)) {
                    j++;
                    if (MIN_NMEA_SZ == j)
                        return ESP_OK;

                } else {
                    j = 0;
                }

                *d = 0;
            }
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

void gps_upgrade_baud_115200(uart_port_t port) {
    // NOTE: 
    // GPS for prototyping was actually M8 not M10 as said, so, the configuration
    // for M10 was not being tested yet.
    
    // M8 Series configuration 
    const uint8_t m8q_baud_115200[] = {
    0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xD0, 0x08, 0x00, 0x00, 0x00, 0xC2, 0x01, 0x00, 0x07, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x7E
    };

    // M10 Series (M10Q) 115200 Baud Command (VALSET)
    /*
    const uint8_t m10_baud[] = {
        0xB5, 0x62, 0x06, 0x8A, 0x0C, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00,
        0x01, 0x40, 0x00, 0xC2, 0x01, 0x00, 0xB1, 0x72
    };
    */

    uart_write_bytes(port, (const char*)m8q_baud_115200, 28);
    uart_wait_tx_done(port, pdMS_TO_TICKS(40));
}
void gps_disable_non_essential_NMEA(uart_port_t port) {
    uart_write_bytes(port, UBX_M10_NMEA_NO_GLL, strlen(UBX_M10_NMEA_NO_GLL));
    uart_write_bytes(port, UBX_M10_NMEA_NO_GSA, strlen(UBX_M10_NMEA_NO_GSA));
    uart_write_bytes(port, UBX_M10_NMEA_NO_GSV, strlen(UBX_M10_NMEA_NO_GSV));
    uart_write_bytes(port, UBX_M10_NMEA_NO_VTG, strlen(UBX_M10_NMEA_NO_VTG));
}

// TODO: remove this drivers
const sensor_driver_t gps_driver = {
    .name     = "GPS",
    .data_len = GPS_MAX_SENTENCE_LEN,
    .init     = NULL, // gps_init,
};
