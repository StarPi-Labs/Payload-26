# The bridge is referenced in place, not copied: there is exactly one
# espnow_bridge.c, and one pilog_link.h shared with the rocket firmware and the
# P4 app, so the channel and message ID cannot drift apart.
idf_component_register(SRCS "app_main.c" "../../espnow_bridge.c"
                    INCLUDE_DIRS "." "../.." "../../../common"
                    REQUIRES esp_hosted esp_wifi esp_event nvs_flash)
