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

#include <string.h>

static const char *TAG = "GPS";

#define BUF_SIZE                1024
#define TIMEOUT_INIT            20          // Timeout on receiving the dataset
#define GPS_MAX_INIT_ATTEMPTS   1024        // Number of attempts including returned 
                                        // read bytes being 0, this depends on 
                                        // the number of NMEA sentences we are 
                                        // having.

#define MIN_NMEA                "$xxGGA"    // According to the doc, we only need:
                                            // - $xxRMC // status the most important
                                            // - $xxGGA // time, lat and long.
                                            // where: 'xx' means it could be any two words there.
                                            // Remember we are using this onnly to 
                                            // see if the GPS is alive.
#define MIN_NMEA_SZ             6           // strlen(MIN_NMEA)



/* Double-buffered latest NMEA sentence */
static volatile bool s_new_data = false;

#define MAX_FAILED_ATTEMPTS     5   // if the GPS' uart fails to read 5 
                                        // consecutive times, gps is flagged
                                        // dead.

/* SENTENCES OF INTEREST ALREADY RECEIVED */
enum {
    RMC_FLAG_SENTENCE,
    GGA_FLAG_SENTENCE,
};
#define NMEA_READY_INFO ((1 << RMC_FLAG_SENTENCE) | (1 << GGA_FLAG_SENTENCE))

/* NMEA PARSING */
enum {
    WAIT_SYNC, SKIP_TALKER_1, SKIP_TALKER_2, CHECK_ID_1, CHECK_ID_2, CHECK_ID_3,
    WAIT_COMMA, PARSE_RMC, PARSE_GGA, IGNORE_LINE
};

void parse_nmea(struct GPSInfo *gps_info, uint8_t byte) {
    static uint8_t state = WAIT_SYNC;
    static uint8_t candidate_state = 0; 
    static int comma_count = 0;
    static int char_count = 0;

    if (byte == '$') {
        state = SKIP_TALKER_1;
        return;
    }


    switch(state) {
        case SKIP_TALKER_1:
            state = SKIP_TALKER_2;
            break;

        case SKIP_TALKER_2:
            state = CHECK_ID_1;
            break;

        case CHECK_ID_1:
            switch(byte) {
            case 'R': 
                candidate_state = PARSE_RMC;
                state = CHECK_ID_2;
                break;
            case 'G':
                candidate_state = PARSE_GGA;
                state = CHECK_ID_2;
                break;
            default:
                state = IGNORE_LINE;
            }
           break;
        case CHECK_ID_2:
            if ((candidate_state == PARSE_RMC && byte == 'M') || 
                (candidate_state == PARSE_GGA && byte == 'G'))
                state = CHECK_ID_3;
            else 
                state = IGNORE_LINE;
            break;

        case CHECK_ID_3:

            comma_count = 0;
            if ((candidate_state == PARSE_RMC && byte == 'C') ||
                (candidate_state == PARSE_GGA && byte == 'A'))
                state = WAIT_COMMA;
            else 
                state = IGNORE_LINE;
            break;

        case WAIT_COMMA:
            if (byte == ',') {
                state = candidate_state;
                comma_count = 1;
            }
            break;

        case PARSE_RMC:

            if (byte == '*') { 
                gps_info->available |= 1 << RMC_FLAG_SENTENCE;
                state = WAIT_SYNC; 
                return; 
            }

            if (byte == ',') { 
                comma_count++; 
                char_count = 0;
                return; 
            }

            // You are now online parsing RMC!
            switch(comma_count) {
            case 2: // STATUS
                gps_info->status = byte;
                break;
            case 7: // SPEED
                gps_info->speed[char_count++] = byte;
                if (char_count > GPS_SPEED_STR_SZ){ 
                    // This technique will avoid segmentation faults by 
                    // avoiding buffer over flow
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case 8: // COURSE OF MOVEMENT
                gps_info->course[char_count++] = byte;
                if (char_count > GPS_COURSE_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            }
            break;

        case PARSE_GGA:

            if (byte == '*') {
                gps_info->available |= 1 << GGA_FLAG_SENTENCE; 
                state = WAIT_SYNC; 
                return;
            } 
            if (byte == ',') { 
                comma_count++; 
                char_count = 0;
                return; 
            }

            // You are now online parsing GGA!
            switch(comma_count) {
            case 1: // TIME
                gps_info->time[char_count++] = byte;
                if (char_count > GPS_TIME_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case 2: // LATITUDE
                gps_info->lat[char_count++] = byte;
                if (char_count > GPS_LAT_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case 3: // LATITUDE DIR N/S
                gps_info->lat_orientation = byte;
                break;
            case 4: // LONGITUDE
                gps_info->lon[char_count++] = byte;
                if (char_count > GPS_LON_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case 5: // LONGITUDE DIR W/E
                gps_info->lon_orientation = byte;
                break;
            case 7: // NUMER OF SATELLITES CONNECTED
                gps_info->sat_count[char_count++] = byte;
                if (char_count > GPS_SATCOUNT_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            }
            break;

        case IGNORE_LINE:
            // Do absolutely nothing until the next '$' arrives
            break;

        default:
            state = WAIT_SYNC;
            break;
    }
}

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
            /* TODO: After parsed, this task should sleep for a second. */
            switch(tparams->context->mode) {
            case MODE_POST:
            case MODE_ARMED:
                // This for debugging only
                /*
                ESP_LOGI(TAG,"Status %c, speed %s, course %s, time %s, lat %s %c, lon %s %c, sats %s", \
                    gps_info.status, \
                    gps_info.speed, \
                    gps_info.course, \
                    gps_info.time, \
                    gps_info.lat, \
                    gps_info.lat_orientation, \
                    gps_info.lon, \
                    gps_info.lon_orientation, \
                    gps_info.sat_count
                    );
                */

                telemetry_counter++;
                if (telemetry_counter >= GPS_HM_SKIP_SAMPLES) {
                    hm_send(
                        tparams->hm_buffer, 
                        SBIT_MQ10, 
                        (uint8_t *)&gps_info, 
                        sizeof(struct GPSInfo) - 1
                    );  // the available field is not send
                    telemetry_counter = 0;
                    gps_info.available = 0;
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
    while(1);
}

/* ── Public API ───────────────────────────────────────────── */


esp_err_t gps_init(uart_port_t port) {
    int attempts;

    /* to read GPS data */
    uint8_t data[BUF_SIZE] = {0};
    int len;

    /* to find NMEA */
    const char required_min_NMEA[] = MIN_NMEA;
    int j;
    uint8_t *d;

    /* Wait for NMEA of interest defined in MIN_NMEA */
    j = 0;
    for (attempts = 0; attempts < GPS_MAX_INIT_ATTEMPTS; attempts++) {
        len = uart_read_bytes(port, data, BUF_SIZE, TIMEOUT_INIT / portTICK_PERIOD_MS);

        if (len < 0) {
            ESP_LOGE(TAG, "Nothing received on GPS UART PORT, timeout.");
            return ESP_ERR_TIMEOUT;

        } else if (len > 0) {
            // NOTE: This length check is only because uart_read_bytes might touch
            // the buffer even if it returns zero. It's a way to say, i don't trust 
            // you, uart_read_bytes. I don't like it, but safety reasons. 
            for (d = data; *d; d++) {
                if ((required_min_NMEA[j] == *d) || (1 == j) || (2 == j)) {
                    j++;
                } else {
                    j = 0;
                }

                if (MIN_NMEA_SZ == j)
                    return ESP_OK;

                *d = 0;
            }
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

// TODO: remove this drivers
const sensor_driver_t gps_driver = {
    .name     = "GPS",
    .data_len = GPS_MAX_SENTENCE_LEN,
    .init     = NULL, // gps_init,
};
