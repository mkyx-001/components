cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../packages/cellular_modem")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cellular_modem_basic)
