#ifndef _BT_SERIAL_BRIDGE_H_
#define _BT_SERIAL_BRIDGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool bt_serial_init(const char *device_name);
void bt_serial_write_byte(uint8_t byte);
bool bt_serial_has_client(void);

#ifdef __cplusplus
}
#endif

#endif
