# On-target unit tests for all components (Unity). Collects components/*/test/test_*.c.
#   idf.py -C test/unit -p /dev/ttyUSB0 flash monitor
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sa_unit_tests)
