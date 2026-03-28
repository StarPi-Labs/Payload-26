#ifndef _HEALTH_MONITORING_H_
#define _HEALTH_MONITORING_H_

#include <stdint.h>
#include <string.h>

typedef struct HMBuffer HMBuffer;

HMBuffer * hm_buff_init(void);

void 
hm_send(
    HMBuffer *buf,
    uint8_t type,
    void *payload,
    uint16_t payload_size
);

void health_monitoring_task(void *arg);

#endif
