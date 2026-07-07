/**
 * @file esp_target_flasher.h
 * @brief ESP 主机给 ESP 从机烧录固件的共享组件
 *
 * 底层依赖 espressif/esp-serial-flasher v2，封装了：
 * - 目标描述（芯片型号、UART、引脚、波特率）
 * - 完整烧录会话（connect → flash → finish → reset）
 * - 互斥锁（多目标共享 BOOT 引脚）
 * - 三种数据源：RAM 缓冲 / 文件流式 / 回调流式
 * - WDT 禁用、flash size 兜底、进度回调
 * - UART 生命周期钩子（分时复用）
 *
 * @see 设计文档 DESIGN.md
 */

#ifndef ESP_TARGET_FLASHER_H
#define ESP_TARGET_FLASHER_H

#include "esp_err.h"
#include "soc/gpio_num.h"
#include "esp_loader.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  错误码
 * ================================================================ */

typedef enum {
    ESP_TF_OK = 0,
    ESP_TF_ERR_INVALID_ARG,
    ESP_TF_ERR_NO_MEM,
    ESP_TF_ERR_NOT_FOUND,        /**< 文件不存在 */
    ESP_TF_ERR_CONNECT,          /**< esp_loader_connect* 失败 */
    ESP_TF_ERR_CHIP_MISMATCH,    /**< 期望 chip != esp_loader_get_target() 检测结果 */
    ESP_TF_ERR_FLASH_START,
    ESP_TF_ERR_FLASH_WRITE,
    ESP_TF_ERR_FLASH_FINISH,     /**< 含 MD5 失败 */
    ESP_TF_ERR_UART_HOOK,
} esp_tf_err_t;

/** 将错误码转为可读字符串 */
const char *esp_tf_err_to_str(esp_tf_err_t err);

/* ================================================================
 *  目标芯片型号别名（复用官方 target_chip_t）
 * ================================================================ */

/** 与 esp-serial-flasher 的 target_chip_t 一一对应 */
typedef target_chip_t esp_tf_chip_t;

#define ESP_TF_CHIP_AUTO        ESP_UNKNOWN_CHIP   /**< 仅信任自动检测，不做期望校验 */
#define ESP_TF_CHIP_ESP8266     ESP8266_CHIP
#define ESP_TF_CHIP_ESP32       ESP32_CHIP
#define ESP_TF_CHIP_ESP32_S2    ESP32S2_CHIP
#define ESP_TF_CHIP_ESP32_S3    ESP32S3_CHIP
#define ESP_TF_CHIP_ESP32_C2    ESP32C2_CHIP
#define ESP_TF_CHIP_ESP32_C3    ESP32C3_CHIP
#define ESP_TF_CHIP_ESP32_C5    ESP32C5_CHIP
#define ESP_TF_CHIP_ESP32_C6    ESP32C6_CHIP
#define ESP_TF_CHIP_ESP32_C61   ESP32C61_CHIP
#define ESP_TF_CHIP_ESP32_H2    ESP32H2_CHIP
#define ESP_TF_CHIP_ESP32_P4    ESP32P4_CHIP

/** 将 chip 枚举转为可读字符串，如 "ESP32-C5" */
const char *esp_tf_chip_to_str(esp_tf_chip_t chip);

/* ================================================================
 *  目标硬件描述
 * ================================================================ */

/**
 * @brief 描述一个被烧录目标的硬件连接
 */
typedef struct {
    const char     *label;             /**< 日志标签，如 "C2#1" */
    esp_tf_chip_t   chip;              /**< 期望目标型号；ESP_TF_CHIP_AUTO = 不校验 */
    uint32_t        uart_port;         /**< UART_NUM_x */
    gpio_num_t      tx_pin;            /**< 主机 TX → 目标 RX */
    gpio_num_t      rx_pin;            /**< 主机 RX ← 目标 TX */
    gpio_num_t      reset_pin;         /**< 目标 RST；GPIO_NUM_NC 表示由 gpio_ops 控制 */
    gpio_num_t      boot_pin;          /**< 目标 BOOT；GPIO_NUM_NC 表示由 gpio_ops 控制 */
    uint32_t        baud_connect;      /**< 连接波特率，通常 115200 */
    uint32_t        baud_flash;        /**< stub 连接后的目标波特率；0 = 不切换 */
    uint32_t        target_flash_size; /**< 0 = 自动检测；检测失败时的兜底（如 2MB） */
} esp_tf_target_t;

/**
 * @brief 常用默认值：C2 2MB Flash、115200 连接、期望 ESP32-C2
 */
#define ESP_TF_TARGET_C2_DEFAULT(uart, tx, rx, rst, boot, lbl) { \
    .label = (lbl), .chip = ESP_TF_CHIP_ESP32_C2, \
    .uart_port = (uart), .tx_pin = (tx), .rx_pin = (rx), \
    .reset_pin = (rst), .boot_pin = (boot), \
    .baud_connect = 115200, .baud_flash = 0, \
    .target_flash_size = 2 * 1024 * 1024, \
}

/* ================================================================
 *  烧录参数
 * ================================================================ */

/**
 * @brief 烧录镜像参数
 */
typedef struct {
    uint32_t offset;       /**< Flash 偏移，合并 bin 通常为 0x0 */
    uint32_t block_size;   /**< 写块大小，默认 4096 */
    bool     use_stub;     /**< true: 先 stub 再写；false: ROM only */
    bool     rom_chunked;  /**< ROM 模式下按块 erase+write（大镜像/C5） */
    bool     skip_md5;     /**< 跳过 finish 时 MD5（ROM 分块模式常用） */
} esp_tf_image_t;

#define ESP_TF_IMAGE_DEFAULT() { \
    .offset = 0, .block_size = 4096, \
    .use_stub = true, .rom_chunked = false, .skip_md5 = false \
}

/* ================================================================
 *  回调与钩子
 * ================================================================ */

/**
 * @brief 烧录进度回调
 * @param label    日志标签（如 "C2A"）
 * @param written  已写入字节数
 * @param total    固件总字节数
 * @param percent  百分比 0-100
 */
typedef void (*esp_tf_progress_cb_t)(const char *label,
                                     size_t written, size_t total, int percent);

/**
 * @brief UART 生命周期钩子（分时复用场景）
 *
 * prepare 在烧录前调用：停 RX 任务、uart_driver_delete。
 * restore 在烧录成功后调用：uart_driver_install、恢复业务。
 * 失败时**不调用** restore，提示需断电恢复。
 */
typedef esp_err_t (*esp_tf_uart_prepare_cb_t)(uint32_t uart_port, void *ctx);
typedef esp_err_t (*esp_tf_uart_restore_cb_t)(uint32_t uart_port, void *ctx);

typedef struct {
    esp_tf_uart_prepare_cb_t prepare;
    esp_tf_uart_restore_cb_t restore;
    void *ctx;
} esp_tf_hooks_t;

/**
 * @brief 自定义下载模式时序（可选）
 *
 * 若不用 esp-serial-flasher 内置 reset_pin/boot_pin 触发，
 * 可通过此回调在连接前手动拉电平进下载模式、烧录后手动复位。
 */
typedef void (*esp_tf_enter_download_cb_t)(void *ctx);
typedef void (*esp_tf_reset_target_cb_t)(void *ctx);

typedef struct {
    esp_tf_enter_download_cb_t enter_download;
    esp_tf_reset_target_cb_t   reset_target;
    void *ctx;
} esp_tf_gpio_ops_t;

/**
 * @brief 流式读取回调（从任意数据源按块提供固件数据）
 *
 * @param offset   本次读取在固件中的偏移
 * @param buf      读取缓冲区
 * @param max_len  最大读取长度
 * @param out_len  实际读取长度
 * @param ctx      用户上下文
 * @return ESP_OK 成功
 */
typedef esp_err_t (*esp_tf_read_cb_t)(size_t offset, uint8_t *buf,
                                      size_t max_len, size_t *out_len, void *ctx);

/* ================================================================
 *  全局 API
 * ================================================================ */

/**
 * @brief 初始化组件（创建互斥锁等），建议在 app_main 早期调用一次
 * @return ESP_OK 成功
 */
esp_err_t esp_tf_init(void);

/**
 * @brief 获取共享 BOOT 场景的全局互斥锁
 */
void esp_tf_lock(void);

/**
 * @brief 释放全局互斥锁
 */
void esp_tf_unlock(void);

/* ================================================================
 *  高级会话 API
 * ================================================================ */

typedef struct esp_tf_session esp_tf_session_t;

/**
 * @brief 打开烧录会话（初始化 UART + 连接目标 + WDT 禁用）
 *
 * 若 hooks 非 NULL，先调用 hooks->prepare()。
 * 若 gpio_ops 非 NULL，调用 enter_download() 进入下载模式；
 * 否则由 loader 内置 trigger pin 自动进入。
 *
 * @param out       输出会话句柄
 * @param target    目标硬件描述
 * @param hooks     UART 生命周期钩子（可为 NULL）
 * @param gpio_ops  自定义 GPIO 操作（可为 NULL）
 * @param use_stub  true: stub 连接；false: ROM-only
 * @return ESP_TF_OK 成功
 */
esp_tf_err_t esp_tf_session_open(esp_tf_session_t **out,
                                 const esp_tf_target_t *target,
                                 const esp_tf_hooks_t *hooks,
                                 const esp_tf_gpio_ops_t *gpio_ops,
                                 bool use_stub);

/**
 * @brief 在已打开的会话中烧录数据（流式回调）
 *
 * @param sess         会话句柄
 * @param image        烧录参数
 * @param read_cb      数据读取回调
 * @param read_ctx     回调上下文
 * @param total_size   固件总大小
 * @param progress_cb  进度回调（可为 NULL）
 * @return ESP_TF_OK 成功
 */
esp_tf_err_t esp_tf_session_flash(esp_tf_session_t *sess,
                                  const esp_tf_image_t *image,
                                  esp_tf_read_cb_t read_cb, void *read_ctx,
                                  size_t total_size,
                                  esp_tf_progress_cb_t progress_cb);

/**
 * @brief 关闭会话（deinit loader + 可选复位目标）
 *
 * 若 hooks 非 NULL 且 use_restore=true，调用 hooks->restore()。
 *
 * @param sess          会话句柄
 * @param reset_target  true: 复位目标使其运行新固件
 * @return ESP_TF_OK 成功
 */
esp_tf_err_t esp_tf_session_close(esp_tf_session_t *sess, bool reset_target);

/**
 * @brief 读取会话已连接时 loader 自动识别到的芯片型号
 */
esp_tf_chip_t esp_tf_session_get_detected_chip(esp_tf_session_t *sess);

/* ================================================================
 *  便捷烧录 API（内部使用会话 API）
 * ================================================================ */

/**
 * @brief 从内存缓冲一次性烧录
 *
 * 适用于固件已全部在内存中（PSRAM 缓冲、小体积 C2 固件）。
 */
esp_tf_err_t esp_tf_flash_buffer(const esp_tf_target_t *target,
                                 const esp_tf_image_t *image,
                                 const uint8_t *data, size_t size,
                                 const esp_tf_hooks_t *hooks,
                                 const esp_tf_gpio_ops_t *gpio_ops,
                                 esp_tf_progress_cb_t progress_cb);

/**
 * @brief 从文件流式烧录（按 block_size 分块读取）
 *
 * 适用于固件大于空闲堆的场景（如 C2 ~600KB / C5 ~838KB）。
 * 需要文件系统支持（VFS / LittleFS / SPIFFS 等）。
 */
esp_tf_err_t esp_tf_flash_file(const esp_tf_target_t *target,
                               const esp_tf_image_t *image,
                               const char *path,
                               const esp_tf_hooks_t *hooks,
                               const esp_tf_gpio_ops_t *gpio_ops,
                               esp_tf_progress_cb_t progress_cb);

/**
 * @brief 从回调流式烧录（无文件系统依赖）
 *
 * 数据源可以是 HTTP 下载、加密分区、自定义协议等任意来源。
 */
esp_tf_err_t esp_tf_flash_stream(const esp_tf_target_t *target,
                                 const esp_tf_image_t *image,
                                 size_t total_size,
                                 esp_tf_read_cb_t read_cb, void *read_ctx,
                                 const esp_tf_hooks_t *hooks,
                                 const esp_tf_gpio_ops_t *gpio_ops,
                                 esp_tf_progress_cb_t progress_cb);

#ifdef __cplusplus
}
#endif

#endif /* ESP_TARGET_FLASHER_H */
