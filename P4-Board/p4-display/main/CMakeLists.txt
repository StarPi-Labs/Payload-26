idf_component_register(
    SRCS "main.c" "ui.c"
    # ../../common holds pilog_link.h, shared verbatim with the rocket firmware
    # and the C6 bridge.
    INCLUDE_DIRS "." "../../common"
    REQUIRES telemetry_codec esp_timer
)
