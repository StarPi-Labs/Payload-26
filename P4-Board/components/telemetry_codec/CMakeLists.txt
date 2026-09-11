idf_component_register(
    SRCS "telemetry_codec.c"
    INCLUDE_DIRS "include"
)

# Pure C, no ESP-IDF dependencies beyond libc and libm - deliberately, so the
# same source can be compiled on a host for testing against the Python parser.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
