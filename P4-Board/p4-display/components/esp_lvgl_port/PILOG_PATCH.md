# Local copy of espressif/esp_lvgl_port 2.9.0

This directory overrides the registry component of the same name. It exists
for one reason: **esp_lvgl_port 2.9.0 (the newest release) does not compile
against ESP-IDF v6.0.1.**

For every IDF >= 5.5 it registers the MIPI-DSI vsync callback as
`on_frame_buf_complete`. IDF 5.5 used that name; IDF 6.0.1's
`esp_lcd_dpi_panel_event_callbacks_t` calls the same callback
`on_refresh_done` again ("invoked when the internal frame buffer finishes
refreshing to the screen").

The only change, in `src/lvgl9/esp_lvgl_port_disp.c` and its
`src/lvgl8/` twin, is the version guard:

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    cbs.on_frame_buf_complete = ...;
#else
    cbs.on_refresh_done = ...;
#endif
```

The RGB-panel path was already correct for IDF 6.0 and is untouched.

**Delete this directory** once a newer esp_lvgl_port release builds against
your IDF version - the registry version is then used again automatically.
