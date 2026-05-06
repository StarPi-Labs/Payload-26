#include "nmea.h"

/* NMEA COMMA COUNTER SENTENCES */
#define NMEA_GGA_TIME           1       // TIME
#define NMEA_GGA_LAT            2       // LATITUDE
#define NMEA_GGA_LAT_NS         3       // LATITUDE DIR N/S
#define NMEA_GGA_LON            4       // LONGITUDE
#define NMEA_GGA_LON_WE         5       // LONGITUDE DIR W/E
#define NMEA_GGA_SATCOUNT       7       // NUMER OF SATELLITES CONNECTED
#define NMEA_GGA_ALT            9       // ALTITUDE
#define NMEA_RMC_STATUS         2       // STATUS
#define NMEA_RMC_SPEED          7       // SPEED
#define NMEA_RMC_COURSE         8       // COURSE OF MOVEMENT

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

            switch(comma_count) {
            case NMEA_RMC_STATUS: // STATUS
                gps_info->status = byte;
                break;
            case NMEA_RMC_SPEED: // SPEED
                gps_info->speed[char_count++] = byte;
                if (char_count > GPS_SPEED_STR_SZ){ 
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case NMEA_RMC_COURSE: // COURSE OF MOVEMENT
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


            switch(comma_count) {
            case NMEA_GGA_TIME: // TIME
                gps_info->time[char_count++] = byte;
                if (char_count > GPS_TIME_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case NMEA_GGA_LAT: // LATITUDE
                gps_info->lat[char_count++] = byte;
                if (char_count > GPS_LAT_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case NMEA_GGA_LAT_NS: // LATITUDE DIR N/S
                gps_info->lat_orientation = byte;
                break;
            case NMEA_GGA_LON: // LONGITUDE
                gps_info->lon[char_count++] = byte;
                if (char_count > GPS_LON_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case NMEA_GGA_LON_WE: // LONGITUDE DIR W/E
                gps_info->lon_orientation = byte;
                break;
            case NMEA_GGA_SATCOUNT: // NUMER OF SATELLITES CONNECTED
                gps_info->sat_count[char_count++] = byte;
                if (char_count > GPS_SATCOUNT_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
                break;
            case NMEA_GGA_ALT:
                gps_info->alt[char_count++] = byte;
                if (char_count > GPS_ALT_STR_SZ) {
                    state = WAIT_SYNC;
                    return;
                }
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
