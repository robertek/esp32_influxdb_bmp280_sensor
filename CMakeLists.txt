cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ./bmp280_ulp_driver)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(temp_sensor)
