#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "health_monitoring.h"
#include "frame_logger.h"
#include "systemp2i.h"
#include "bt_serial_bridge.h"

#include "esp_log.h" // debug

#define HM_BUFFER_SZ    512 
struct  HMBuffer {
    portMUX_TYPE guard;
    SemaphoreHandle_t ready;
    SemaphoreHandle_t lock;
    volatile size_t head;
    volatile size_t tail;
    uint8_t data[HM_BUFFER_SZ];
} __attribute__((aligned(4)));

HMBuffer * hm_buff_init(void) {
    static HMBuffer buf __attribute__((aligned(4))); // Only buffer
    memset(buf.data, 0, HM_BUFFER_SZ);
    buf.head = 0;
    buf.tail = 0;
    buf.ready = xSemaphoreCreateCounting(HM_BUFFER_SZ, 0); // HM_BUFFER_SZ number of events.
    buf.lock = xSemaphoreCreateMutex();
    portMUX_INITIALIZE(&buf.guard);
    printf("BUFFER_INIT_ADDR: %p\n", (void*)&buf);
    return &buf;
}
/*
void hm_send(HMBuffer *buf, uint8_t type, uint8_t *payload, uint16_t payload_size) {
    uint16_t total_sz = sizeof(frame_header_t) + payload_size + 2;
    
    // 1. Prepare the header LOCALLY (not in critical section)
    frame_header_t header = {
        .frame_info = type,
        .frame_separator = {0xAA, 0xAA, 0xAA},
        .timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS
    };

    uint16_t crc = 0xFFFF;
    crc16_ccitt(&crc, (uint8_t *)&header, sizeof(header));
    crc16_ccitt(&crc, payload, payload_size);

    // 2. Use a Semaphore/Mutex instead of Critical Section
    // Critical sections kill the Bluetooth radio interrupts!
    if (payload_size < 4) return;
    ESP_LOGE("HM", "N> %d", payload_size);
    if (xSemaphoreTake(buf->lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        
        // Wrap-around logic
        if (buf->head + total_sz > HM_BUFFER_SZ) {
            if (buf->tail < total_sz) {
                // Buffer is full, forget this reading.
                xSemaphoreGive(buf->lock);
                return;
            }
            buf->head = 0; 
        }

        // Copy everything in one sequence
        memcpy(buf->data + buf->head, &header, sizeof(header));
        buf->head += sizeof(header);
        
        memcpy(buf->data + buf->head, payload, payload_size);
        buf->head += payload_size;
        
        memcpy(buf->data + buf->head, &crc, 2);
        buf->head += 2;

        xSemaphoreGive(buf->lock);
        xSemaphoreGive(buf->ready);
    }

    ESP_LOGE("HM", "X< %d", payload_size);
}
*/

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

    xSemaphoreTake(buf->lock, portMAX_DELAY);
    //taskENTER_CRITICAL(&buf->guard);

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

    //taskEXIT_CRITICAL(&buf->guard);
    xSemaphoreGive(buf->lock);
    xSemaphoreGive(buf->ready);
}

#include "esp_rom_uart.h"   // debugging
#define BT_TX_CHUNK_SIZE        128
void health_monitoring_task(void *arg) {
    struct TaskParams *tparams = (struct TaskParams *) arg;
    HMBuffer *buf = tparams->hm_buffer;
    uint8_t tx_chunk[BT_TX_CHUNK_SIZE];
    uint16_t chunk_len = 0;
    
    while(1) {
        xSemaphoreTake(buf->ready, portMAX_DELAY);

        xSemaphoreTake(buf->lock, portMAX_DELAY);

        while (buf->tail != buf->head && chunk_len < BT_TX_CHUNK_SIZE) {
            tx_chunk[chunk_len] = buf->data[buf->tail];
            buf->tail = (buf->tail + 1) % HM_BUFFER_SZ;
            chunk_len++;
        }
        xSemaphoreGive(buf->lock);

        if (chunk_len > 0) {
            bt_serial_write_chunk(tx_chunk, chunk_len);
            chunk_len = 0;
        }
    }
}

