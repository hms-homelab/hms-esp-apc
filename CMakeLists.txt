# Minimum CMake version
cmake_minimum_required(VERSION 3.16)

# main/version.h is the single source of truth for the firmware version. Lift it
# out here too so ESP-IDF stamps the same string into the app descriptor that OTA
# and esp_app_get_description() read, instead of it living only in the C sources.
file(READ "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _version_h)
string(REGEX MATCH "HMS_ESP_APC_VERSION[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_version_h}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Could not parse HMS_ESP_APC_VERSION from main/version.h")
endif()
set(PROJECT_VER "${CMAKE_MATCH_1}")

# Include the ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name
project(apc_usb_mqtt_bridge)
