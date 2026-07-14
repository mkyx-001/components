/**
 * @file io_expander_test_runner.c
 * @brief 通过 Kconfig RUN_IO_EXPANDER_TEST 切换的测试入口
 *
 * 启用 RUN_IO_EXPANDER_TEST（idf.py menuconfig）后，本文件单独编译并接管 app_main，
 * 原 main.c 的 cellular_modem_basic 流程不会运行（CMakeLists 会从 SRCS 中移除 main.c）。
 */

#include <stdio.h>
#include "esp_log.h"
#include "io_expander.h"

void app_main(void)
{
    ESP_LOGI("IO_EXP_TEST_RUNNER", "RUN_IO_EXPANDER_TEST=y, entering io_expander_test_run()");
    io_expander_test_run();
}