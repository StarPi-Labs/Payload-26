#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "health_monitoring.h"
#include "frame_logger.h"
#include "systemp2i.h"
#include "bt_serial_bridge.h"

#define BUF_SIZE    512 
struct HMBuffer {
    volatile size_t head;
    volatile size_t tail;
    uint8_t data[BUF_SIZE];
    portMUX_TYPE guard;
    SemaphoreHandle_t ready;
};

HMBuffer * hm_buff_init(void) {
    static HMBuffer buf; // Only buffer
    buf.head = 0;
    buf.tail = 0;
    buf.ready = xSemaphoreCreateCounting(BUF_SIZE, 0); // BUF_SIZE number of events.
    portMUX_INITIALIZE(&buf.guard);
    return &buf;
}

void 
hm_send(
    HMBuffer *buf, 
    uint8_t type, 
    void *payload, 
    uint16_t payload_size
) 
{
    // header_sz + payload_size + crc_sz
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

    if (buf->head + total_sz > BUF_SIZE) {
        memset(buf->data, 0, BUF_SIZE - buf->head);
        buf->head = 0;  
    }

    // Moving header
    memcpy(buf->data + buf->head, &header, sizeof(header));
    buf->head = (buf->head + sizeof(header));

    // Moving payload
    memcpy(buf->data + buf->head, payload, payload_size);
    buf->head = buf->head + payload_size;

    // Moving CRC-16
    memcpy(buf->data + buf->head + payload_size, &crc, 2);
    buf->head = buf->head + 2;

    // Trigger semaphore
    xSemaphoreGive(buf->ready);

    taskEXIT_CRITICAL(&buf->guard);

}

#include "esp_rom_uart.h"
void health_monitoring_task(void *arg) {

    struct TaskParams *tparams = (struct TaskParams *) arg;
    HMBuffer *buf = tparams->hm_buffer;
    uint8_t byte;

    bt_serial_init("GPS-Serial-Bluetooth");

    while(1) {
        xSemaphoreTake(buf->ready, portMAX_DELAY);

        while (buf->tail != buf->head) {
            byte = buf->data[buf->tail];
            buf->tail = (buf->tail + 1) % BUF_SIZE;
            bt_serial_write_byte(byte);
            esp_rom_output_tx_one_char(byte); // debugging mirror
            // TODO: UART_NUM_0 needs to be replaced here
            // uart_write_bytes(UART_NUM_0, &byte, 1);
        }
    }
}

