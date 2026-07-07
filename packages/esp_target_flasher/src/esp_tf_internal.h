/**
 * @file esp_tf_internal.h
 * @brief esp_target_flasher 内部辅助函数（不对外暴露）
 */

#ifndef ESP_TF_INTERNAL_H
#define ESP_TF_INTERNAL_H

#include "esp_loader.h"
#include "esp_target_flasher.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 禁用目标芯片的 RTC WDT + 启用 SWD 自动喂狗
 *
 * 长时间 FLASH_BEGIN 擦除会触发目标 RTC WDT 复位，导致主机侧超时。
 * 根据 esptool _post_connect() 的行为，在连接成功后写入 WDT 寄存器。
 *
 * 当前覆盖 C2/C3/C5/C6/H2 同族（LP WDT 基址 0x600B1C00）。
 * 若检测到 ESP32 经典款，走不同的寄存器路径（TIMG WDT）。
 */
void esp_tf_disable_target_watchdogs(esp_loader_t *loader, esp_tf_chip_t chip,
                                     const char *label);

/**
 * @brief 进度回调节流：仅在跨越 10% 边界或到达 100% 时回调
 */
void esp_tf_report_progress(esp_tf_progress_cb_t cb, const char *label,
                            size_t written, size_t total, int *last_percent);

/**
 * @brief 填充 esp32_port_t 结构体（从 esp_tf_target_t）
 *
 * 若 gpio_ops 非 NULL，reset_pin/boot_pin 设为 NC（由调用方控制）。
 */
void esp_tf_fill_port(struct esp32_port *port, const esp_tf_target_t *target,
                      bool use_gpio_ops);

#ifdef __cplusplus
}
#endif

#endif /* ESP_TF_INTERNAL_H */
