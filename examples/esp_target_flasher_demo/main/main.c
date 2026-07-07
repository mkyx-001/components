/**
 * @file main.c
 * @brief esp_target_flasher 组件示例 — ESP32-P4 Host 给 ESP32-C3 Target 烧录
 *
 * 本例程演示 esp_target_flasher 组件的三种核心用法：
 *
 *   A) RAM 缓冲烧录  — esp_tf_flash_buffer()
 *   B) 文件流式烧录  — esp_tf_flash_file()     (需 LittleFS / SPIFFS)
 *   C) 回调流式烧录  — esp_tf_flash_stream()   (从内存模拟分块)
 *
 * 硬件连接（P4 → C3）：
 *
 *   P4 GPIO4  ──► C3 GPIO20  (P4 UART1 TX → C3 UART RX)
 *   P4 GPIO5  ◄── C3 GPIO19  (P4 UART1 RX ← C3 UART TX)
 *   P4 GPIO6  ──► C3 EN/RST  (复位)
 *   P4 GPIO7  ──► C3 GPIO9   (BOOT)
 *   GND       ──── GND       (共地)
 *
 * 按键交互（通过串口终端输入）：
 *   1 → RAM 缓冲烧录（演示用：内置虚拟数据）
 *   2 → 回调流式烧录（演示用：内置虚拟数据）
 *   3 → 文件流式烧录（需先用 0 将数据写入 /spiffs/target.bin）
 *   0 → 写入测试文件到 /spiffs/target.bin（用 RAM 数据模拟）
 *
 * ⚠️ 实际烧录需要准备一份 C3 的 merged-bin 固件（esptool merge-bin）。
 *    例程内附的 g_demo_payload 仅用于演示 API 调用流程，
 *    写入后目标不会正常启动——真实使用时替换为你的固件数据。
 */

#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_target_flasher.h"

/* ---- 如果要演示文件流式烧录（功能 0/3），取消下行注释 ---- */
/* #define USE_SPIFFS */

#ifdef USE_SPIFFS
#include "esp_spiffs.h"
#include <stdio.h>
#endif

static const char *TAG = "tf_demo";

/* ================================================================
 *  板级配置（P4 Host → C3 Target）
 * ================================================================ */

#ifndef TF_UART_PORT
#define TF_UART_PORT     UART_NUM_1     /* 主机 UART 端口 */
#endif
#ifndef TF_TX_PIN
#define TF_TX_PIN        GPIO_NUM_4     /* P4 TX → C3 RX */
#endif
#ifndef TF_RX_PIN
#define TF_RX_PIN        GPIO_NUM_5     /* P4 RX ← C3 TX */
#endif
#ifndef TF_RST_PIN
#define TF_RST_PIN       GPIO_NUM_6     /* C3 EN/RST */
#endif
#ifndef TF_BOOT_PIN
#define TF_BOOT_PIN      GPIO_NUM_7     /* C3 GPIO9/BOOT */
#endif

#define TF_BAUD_CONNECT  115200
#define TF_BAUD_FLASH    0              /* 0 = 不切换波特率 */
#define TF_FLASH_SIZE    (4 * 1024 * 1024)  /* C3 典型 4MB Flash */

/* ================================================================
 *  演示用虚拟数据（模拟固件 payload）
 * ================================================================ */

/* 16KB 测试数据：0x00..0xFF 循环填充。
 * 体积足够演示分块写入与进度回调，但**不是**可启动固件。
 * 真实使用时替换为 esptool merge-bin 产出的合并镜像。 */
#define DEMO_PAYLOAD_SIZE  (16 * 1024)

static uint8_t s_demo_payload[DEMO_PAYLOAD_SIZE];

static void prepare_demo_payload(void)
{
    for (size_t i = 0; i < DEMO_PAYLOAD_SIZE; i++) {
        s_demo_payload[i] = (uint8_t)(i & 0xFF);
    }
    ESP_LOGI(TAG, "Demo payload ready: %u bytes", (unsigned)DEMO_PAYLOAD_SIZE);
}

/* ================================================================
 *  进度回调
 * ================================================================ */

static void my_progress_cb(const char *label, size_t written,
                           size_t total, int percent)
{
    /* 节流由组件内部完成（每 10% 触发一次） */
    ESP_LOGI(TAG, "[%s] Progress: %u/%u (%d%%)",
             label, (unsigned)written, (unsigned)total, percent);
}

/* ================================================================
 *  目标描述（复用全局结构）
 * ================================================================ */

static esp_tf_target_t s_c3_target = {
    .label             = "C3",
    .chip              = ESP_TF_CHIP_ESP32_C3,
    .uart_port         = TF_UART_PORT,
    .tx_pin            = TF_TX_PIN,
    .rx_pin            = TF_RX_PIN,
    .reset_pin         = TF_RST_PIN,
    .boot_pin          = TF_BOOT_PIN,
    .baud_connect      = TF_BAUD_CONNECT,
    .baud_flash        = TF_BAUD_FLASH,
    .target_flash_size = TF_FLASH_SIZE,
};

/* ================================================================
 *  演示 A：RAM 缓冲烧录
 * ================================================================ */

static void demo_flash_buffer(void)
{
    ESP_LOGI(TAG, "===== Demo A: esp_tf_flash_buffer() =====");

    esp_tf_image_t image = ESP_TF_IMAGE_DEFAULT();
    /* image.offset = 0 (合并 bin 从 0x0 开始)
     * image.use_stub = true (优先 stub 模式，带 MD5 校验) */

    esp_tf_err_t err = esp_tf_flash_buffer(
        &s_c3_target, &image,
        s_demo_payload, DEMO_PAYLOAD_SIZE,
        NULL,        /* hooks: 无 UART 分时复用 */
        NULL,        /* gpio_ops: 用 loader 内置 trigger pin */
        my_progress_cb);

    if (err != ESP_TF_OK) {
        ESP_LOGE(TAG, "Flash buffer failed: %s", esp_tf_err_to_str(err));
    } else {
        ESP_LOGI(TAG, "Flash buffer OK!");
    }
}

/* ================================================================
 *  演示 B：回调流式烧录
 *
 * 数据源适配器：从内存数组按 offset 读取（模拟 HTTP 分块下载等场景）
 * ================================================================ */

static esp_err_t mem_read_cb(size_t offset, uint8_t *buf,
                             size_t max_len, size_t *out_len, void *ctx)
{
    (void)ctx;
    size_t remain = DEMO_PAYLOAD_SIZE - offset;
    size_t want = (remain < max_len) ? remain : max_len;
    memcpy(buf, s_demo_payload + offset, want);
    *out_len = want;
    return ESP_OK;
}

static void demo_flash_stream(void)
{
    ESP_LOGI(TAG, "===== Demo B: esp_tf_flash_stream() =====");

    esp_tf_image_t image = ESP_TF_IMAGE_DEFAULT();

    esp_tf_err_t err = esp_tf_flash_stream(
        &s_c3_target, &image,
        DEMO_PAYLOAD_SIZE,
        mem_read_cb, NULL,
        NULL, NULL,
        my_progress_cb);

    if (err != ESP_TF_OK) {
        ESP_LOGE(TAG, "Flash stream failed: %s", esp_tf_err_to_str(err));
    } else {
        ESP_LOGI(TAG, "Flash stream OK!");
    }
}

/* ================================================================
 *  演示 C：文件流式烧录（需 SPIFFS）
 * ================================================================ */

#ifdef USE_SPIFFS

#define FW_PATH "/spiffs/target.bin"

static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 3,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%u used=%u", (unsigned)total, (unsigned)used);
}

/** 将演示 payload 写入文件，供文件烧录演示使用 */
static void write_demo_file(void)
{
    ESP_LOGI(TAG, "Writing demo payload to %s ...", FW_PATH);
    FILE *f = fopen(FW_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s", FW_PATH);
        return;
    }
    fwrite(s_demo_payload, 1, DEMO_PAYLOAD_SIZE, f);
    fclose(f);
    ESP_LOGI(TAG, "Written %u bytes to %s", (unsigned)DEMO_PAYLOAD_SIZE, FW_PATH);
}

static void demo_flash_file(void)
{
    ESP_LOGI(TAG, "===== Demo C: esp_tf_flash_file() =====");

    esp_tf_image_t image = ESP_TF_IMAGE_DEFAULT();

    esp_tf_err_t err = esp_tf_flash_file(
        &s_c3_target, &image,
        FW_PATH,
        NULL, NULL,
        my_progress_cb);

    if (err != ESP_TF_OK) {
        ESP_LOGE(TAG, "Flash file failed: %s", esp_tf_err_to_str(err));
    } else {
        ESP_LOGI(TAG, "Flash file OK!");
    }
}
#endif /* USE_SPIFFS */

/* ================================================================
 *  串口命令交互
 * ================================================================ */

static void read_command(char *buf, int len)
{
    memset(buf, 0, len);
    int pos = 0;
    while (pos < len - 1) {
        int ch = getchar();
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch >= '0' && ch <= '9') {
            buf[pos++] = (char)ch;
            /* 回显 */
            putchar(ch);
            fflush(stdout);
        }
    }
    putchar('\n');
    fflush(stdout);
}

static void print_menu(void)
{
    ESP_LOGI(TAG, "========= esp_target_flasher demo menu =========");
    ESP_LOGI(TAG, "  1 — RAM buffer flash  (esp_tf_flash_buffer)");
    ESP_LOGI(TAG, "  2 — Stream flash      (esp_tf_flash_stream)");
#ifdef USE_SPIFFS
    ESP_LOGI(TAG, "  3 — File flash        (esp_tf_flash_file)");
    ESP_LOGI(TAG, "  0 — Write demo file to /spiffs/target.bin");
#else
    ESP_LOGI(TAG, "  (File flash disabled — #define USE_SPIFFS to enable)");
#endif
    ESP_LOGI(TAG, "  ? — Reprint this menu");
    ESP_LOGI(TAG, "================================================");
}

/* ================================================================
 *  主入口
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== esp_target_flasher demo (P4 Host → C3 Target) ===");
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d RST=GPIO%d BOOT=GPIO%d",
             TF_UART_PORT, TF_TX_PIN, TF_RX_PIN, TF_RST_PIN, TF_BOOT_PIN);

    /* 初始化组件 */
    esp_err_t ret = esp_tf_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_tf_init failed: %s", esp_err_to_name(ret));
        return;
    }

#ifdef USE_SPIFFS
    init_spiffs();
#endif

    prepare_demo_payload();
    print_menu();

    char cmd[16];
    while (1) {
        ESP_LOGI(TAG, "Enter command (1/2%s ?): ",
#ifdef USE_SPIFFS
                 "/3/0");
#else
                 "");
#endif

        read_command(cmd, sizeof(cmd));

        switch (cmd[0]) {
        case '1':
            demo_flash_buffer();
            break;
        case '2':
            demo_flash_stream();
            break;
#ifdef USE_SPIFFS
        case '3':
            demo_flash_file();
            break;
        case '0':
            write_demo_file();
            break;
#endif
        case '?':
            print_menu();
            break;
        default:
            if (cmd[0] != '\0') {
                ESP_LOGW(TAG, "Unknown command: '%c'", cmd[0]);
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
