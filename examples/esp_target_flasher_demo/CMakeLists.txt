cmake_minimum_required(VERSION 3.16)

# 将组件目录加入 EXTRA_COMPONENT_DIRS，这样无需发布到 Registry 即可本地引用
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../packages/esp_target_flasher")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_target_flasher_demo)
