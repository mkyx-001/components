# Performeter 组件构建脚本
# https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html
idf_component_register(
    SRCS
        "src/performeter.c"
        "src/performeter_print.c"
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        esp_timer
        freertos
        log
)

# 暴露组件级 Kconfig（用户通过 menuconfig -> Performeter 配置）
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(COMPONENT_KCONFIG_PRESCALAR 1)
endif()
