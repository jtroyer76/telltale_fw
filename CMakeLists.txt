cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The component lock is per chip: dependencies.lock is the ESP32-S3's, other targets get their own.
if(NOT "${IDF_TARGET}" STREQUAL "esp32s3")
    idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.${IDF_TARGET}")
endif()
project(telltale_fw)
