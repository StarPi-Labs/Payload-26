#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "health_monitoring.h"
#include "frame_logger.h"
#include "systemp2i.h"
#include "bt_serial_bridge.h"

#include "esp_log.h" // debug

#define HM_BUFFER_SZ    512 
struct HMBuffer {
    volatile size_t head;
    volatile size_t tail;
    uint8_t data[HM_BUFFER_SZ];
    portMUX_TYPE guard;
    SemaphoreHandle_t ready;
};

HMBuffer * hm_buff_init(void) {
    static HMBuffer buf; // Only buffer
    memset(buf.data, 0, HM_BUFFER_SZ);
    buf.head = 0;
    buf.tail = 0;
    buf.ready = xSemaphoreCreateCounting(HM_BUFFER_SZ, 0); // HM_BUFFER_SZ number of events.
    portMUX_INITIALIZE(&buf.guard);
    return &buf;
}

void 
hm_send(
    HMBuffer *buf, 
    uint8_t type, 
    uint8_t *payload, 
    uint16_t payload_size
) 
{
    uint16_t total_sz = sizeof(frame_header_t) + payload_size + 2; 
    uint16_t crc = 0xFFFF;

    frame_header_t header;

    header.frame_info = type;
    header.frame_separator[0] = 0xAA;
    header.frame_separator[1] = 0xAA;
    header.frame_separator[2] = 0xAA;
    header.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    crc16_ccitt(&crc, (uint8_t *) & header, sizeof(header));
    crc16_ccitt(&crc, (uint8_t *) payload, payload_size);

    taskENTER_CRITICAL(&buf->guard);


    if (buf->head + total_sz > HM_BUFFER_SZ) {
        memset(buf->data, 0, HM_BUFFER_SZ - buf->head);
        buf->head = 0;  
    }

    // Moving header
    memcpy(buf->data + buf->head, &header, sizeof(header));
    buf->head = buf->head + sizeof(header);

    // Moving payload
    memcpy(buf->data + buf->head, payload, payload_size);
    buf->head = buf->head + payload_size;

    // Moving CRC-16
    memcpy(buf->data + buf->head + payload_size, &crc, 2);
    buf->head = (buf->head + 2) % HM_BUFFER_SZ;

    taskEXIT_CRITICAL(&buf->guard);
    xSemaphoreGive(buf->ready);

}

#include "esp_rom_uart.h"   // debugging
#define BT_TX_CHUNK_SIZE    128
void health_monitoring_task(void *arg) {

    struct TaskParams *tparams = (struct TaskParams *) arg;
    HMBuffer *buf = tparams->hm_buffer;
    uint8_t byte;
    uint8_t tx_chunk[BT_TX_CHUNK_SIZE];
    uint16_t chunk_len = 0;

    bt_serial_init("GPS-Serial-Bluetooth");

    while(1) {
        xSemaphoreTake(buf->ready, portMAX_DELAY);
        while (buf->tail != buf->head && chunk_len < BT_TX_CHUNK_SIZE) {
            tx_chunk[chunk_len++] = buf->data[buf->tail];
            buf->tail = (buf->tail + 1) % HM_BUFFER_SZ;
        }

        if (chunk_len > 0) {
            bt_serial_write_chunk(tx_chunk, chunk_len);
        }
        chunk_len = 0;
    }
}





