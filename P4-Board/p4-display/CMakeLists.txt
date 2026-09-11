cmake_minimum_required(VERSION 3.16)

# telemetry_codec is shared with any other ground-side firmware; common/ holds
# pilog_link.h, shared verbatim with the rocket so the link constants cannot
# drift apart between the two codebases.
set(EXTRA_COMPONENT_DIRS "../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pilog_p4_display)
