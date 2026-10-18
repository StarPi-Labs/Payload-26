# SPDX-License-Identifier: Apache-2.0
#
# Pi-LOG ground station: ESP32-C6 co-processor firmware for the
# ESP32-P4-Function-EV-Board. Generated from Espressif's esp_hosted 3.0.9
# example peer_data_transfer/cp, with its echo demo replaced by the ESP-NOW
# bridge in ../espnow_bridge.c.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pilog_c6_cp)
