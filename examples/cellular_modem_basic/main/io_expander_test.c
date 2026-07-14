/**
 * @file io_expander_test.c
 * @brief IO Expander 移植测试入口（与 cellular_modem_basic 共存）
 *
 * 默认从 app_main 调用；如果你只想跑 IO Expander 测试，
 * 注释掉 main.c 里的 app_main 调用即可。
 *
 * 接线（基于 io_expander.c 默认配置）：
 *   - I2C SCL = GPIO20
 *   - I2C SDA = GPIO21
 *   - 板载 TCA9555（A0=0, A1=0，默认地址 0x20）
 *
 * 测试流程：
 *   1. 初始化 IO Expander
 *   2. 把 Pin 0~3 设为输出，Pin 4~7 设为输入
 *   3. 循环：Pin 0~3 走马灯翻转，每 500ms 读取一次 Pin 4~7 的电平并打印
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "io_expander.h"

static const char *TAG = "IO_EXP_TEST";

/* 要演示的输出引脚（Pin 0..3） */
#define DEMO_OUT_PINS   4
/* 要回读输入的引脚（Pin 4..7） */
#define DEMO_IN_PINS    4
/* 翻转周期 */
#define DEMO_PERIOD_MS  500

void io_expander_test_run(void)
{
    ESP_LOGI(TAG, "=== IO Expander 移植测试开始 ===");

    /* 1. 默认配置初始化（GPIO20 SCL / GPIO21 SDA / 地址 0x20） */
    esp_err_t ret = io_expander_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "io_expander_init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查：1) I2C 接线  2) TCA9555 地址（A0/A1）  3) 上拉电阻");
        return;
    }

    /* 2. Pin 0~3 输出，Pin 4~7 输入 */
    ESP_ERROR_CHECK(io_expander_set_dir_mask(0x000F, IO_EXP_DIR_OUTPUT));
    ESP_ERROR_CHECK(io_expander_set_dir_mask(0x00F0, IO_EXP_DIR_INPUT));
    ESP_LOGI(TAG, "Pin 0-3 配置为输出，Pin 4-7 配置为输入");

    /* 3. 起始全 LOW */
    ESP_ERROR_CHECK(io_expander_set_level_mask(0x000F, IO_EXP_LEVEL_LOW));

    uint32_t tick = 0;
    while (1) {
        /* 输出走马灯 */
        uint8_t led_pin = tick % DEMO_OUT_PINS;
        ESP_ERROR_CHECK(io_expander_set_level((uint8_t)led_pin, IO_EXP_LEVEL_HIGH));
        ESP_ERROR_CHECK(io_expander_set_level_mask((uint16_t)(0x000F & ~(1U << led_pin)),
                                                   IO_EXP_LEVEL_LOW));

        /* 读输入 */
        uint16_t in_mask = 0;
        ESP_ERROR_CHECK(io_expander_get_level_mask(0x00F0, &in_mask));

        ESP_LOGI(TAG, "tick=%u  LED on Pin%d  Pin4-7=0x%04X (P4=%d P5=%d P6=%d P7=%d)",
                 (unsigned)tick, led_pin, in_mask,
                 (in_mask >> 4) & 0x1,
                 (in_mask >> 5) & 0x1,
                 (in_mask >> 6) & 0x1,
                 (in_mask >> 7) & 0x1);

        vTaskDelay(pdMS_TO_TICKS(DEMO_PERIOD_MS));
        tick++;
    }
}