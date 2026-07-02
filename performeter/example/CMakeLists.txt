# Performeter example 工程顶层 CMakeLists
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html
cmake_minimum_required(VERSION 3.22)

# 关键：把父目录作为额外组件目录，使本工程能引用 performeter 组件。
# （example 内嵌于组件目录时的标准写法）
set(EXTRA_COMPONENT_DIRS "../")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(performeter_example)
