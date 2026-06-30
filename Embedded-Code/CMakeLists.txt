# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Unused by this project. Its S3 RGB-panel driver also triggers a GCC internal
# compiler error (segfault) on the current toolchain, so keep it out of the build.
set(EXCLUDE_COMPONENTS esp_lcd)

project(Embedded-Code)
