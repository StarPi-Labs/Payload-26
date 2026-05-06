#ifndef _NMEA_H_
#define _NMEA_H_

#include "gps.h"
#include <stdint.h>

/* SENTENCES OF INTEREST ALREADY RECEIVED */
enum {
    RMC_FLAG_SENTENCE,
    GGA_FLAG_SENTENCE,
};

void parse_nmea(struct GPSInfo *gps_info, uint8_t byte);

#endif
