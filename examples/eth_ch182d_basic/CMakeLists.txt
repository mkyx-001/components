cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../packages/eth_ch182d")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eth_ch182d_basic)
