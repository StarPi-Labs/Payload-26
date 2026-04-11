#ifndef _HEALTH_MONITORING_H_
#define _HEALTH_MONITORING_H_

#include <stdint.h>
#include <string.h>

#include "bt_serial_bridge.h"

typedef struct HMBuffer HMBuffer;

HMBuffer * hm_buff_init(void);

void 
hm_send(
    HMBuffer *buf,
    uint8_t type,
    uint8_t *payload,
    uint16_t payload_size
);

void health_monitoring_task(void *arg);

#endif
