/**
 * @file esp_target_flasher.c
 * @brief ESP 主机给 ESP 从机烧录固件 — 公共 API 实现
 *
 * 从 V4PRO flasher_common.c 提炼，去掉项目硬编码，参数化目标描述。
 *
 * @see DESIGN.md
 */

#include "esp_target_flasher.h"
#include "esp_tf_internal.h"

#include "esp_log.h"
#include "esp_err.h"

/* esp-serial-flasher */
#include "esp_loader.h"
#include "esp32_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "esp_tf";

/* ================================================================
 *  错误码字符串
 * ================================================================ */

const char *esp_tf_err_to_str(esp_tf_err_t err)
{
    switch (err) {
    case ESP_TF_OK:               return "OK";
    case ESP_TF_ERR_INVALID_ARG:  return "INVALID_ARG";
    case ESP_TF_ERR_NO_MEM:       return "NO_MEM";
    case ESP_TF_ERR_NOT_FOUND:    return "NOT_FOUND";
    case ESP_TF_ERR_CONNECT:      return "CONNECT failed";
    case ESP_TF_ERR_CHIP_MISMATCH:return "CHIP_MISMATCH";
    case ESP_TF_ERR_FLASH_START:  return "FLASH_START failed";
    case ESP_TF_ERR_FLASH_WRITE:  return "FLASH_WRITE failed";
    case ESP_TF_ERR_FLASH_FINISH: return "FLASH_FINISH failed (MD5?)";
    case ESP_TF_ERR_UART_HOOK:    return "UART_HOOK failed";
    default:                      return "UNKNOWN";
    }
}

/* ================================================================
 *  芯片型号字符串
 * ================================================================ */

const char *esp_tf_chip_to_str(esp_tf_chip_t chip)
{
    switch (chip) {
    case ESP8266_CHIP:   return "ESP8266";
    case ESP32_CHIP:     return "ESP32";
    case ESP32S2_CHIP:   return "ESP32-S2";
    case ESP32S3_CHIP:   return "ESP32-S3";
    case ESP32C2_CHIP:   return "ESP32-C2";
    case ESP32C3_CHIP:   return "ESP32-C3";
    case ESP32C5_CHIP:   return "ESP32-C5";
    case ESP32C6_CHIP:   return "ESP32-C6";
    case ESP32C61_CHIP:  return "ESP32-C61";
    case ESP32H2_CHIP:   return "ESP32-H2";
    case ESP32P4_CHIP:   return "ESP32-P4";
    case ESP_UNKNOWN_CHIP: return "AUTO/UNKNOWN";
    default:             return "UNKNOWN";
    }
}

/* ================================================================
 *  全局互斥锁
 * ================================================================ */

static SemaphoreHandle_t s_tf_mutex = NULL;

esp_err_t esp_tf_init(void)
{
    if (s_tf_mutex == NULL) {
        s_tf_mutex = xSemaphoreCreateMutex();
        if (s_tf_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "esp_target_flasher initialized");
    return ESP_OK;
}

void esp_tf_lock(void)
{
    if (s_tf_mutex == NULL) {
        s_tf_mutex = xSemaphoreCreateMutex();
    }
    if (s_tf_mutex) {
        xSemaphoreTake(s_tf_mutex, portMAX_DELAY);
    }
}

void esp_tf_unlock(void)
{
    if (s_tf_mutex) {
        xSemaphoreGive(s_tf_mutex);
    }
}

/* ================================================================
 *  内部辅助：WDT 禁用
 * ================================================================ */

/*
 * esptool 在 _post_connect() 中通过 WRITE_REG 禁用目标芯片的 RTC WDT 和
 * 启用 SWD 自动喂狗。esp-serial-flasher 库没有这个步骤。
 * 如果不禁用，长时间的 flash 擦写操作可能触发看门狗复位，
 * 导致 FLASH_BEGIN 或后续 FLASH_DATA 超时。
 *
 * C2/C3/C5/C6/H2 系列寄存器地址（LP WDT）：
 *   DR_REG_LP_WDT_BASE      = 0x600B1C00
 *   WDTCONFIG0 (disable)    = BASE + 0x00
 *   WDTWPROTECT (unlock)    = BASE + 0x18
 *   SWD_CONF (auto-feed)    = BASE + 0x1C
 *   SWD_WPROTECT (unlock)   = BASE + 0x20
 *   WDT_KEY / SWD_KEY       = 0x50D83AA1
 *   SWD_AUTO_FEED_EN        = 1 << 18
 *
 * ESP32 经典款使用 TIMG1 WDT：
 *   DR_REG_TIMG1_BASE       = 0x3FF1F000  (TIMG1)
 *   WDTCONFIG0              = BASE + 0x48
 *   WDTWPROTECT             = BASE + 0x64
 *   WDT_KEY                 = 0x50D83AA1  (同)
 */
#define LP_WDT_BASE          0x600B1C00U
#define LP_WDT_CONFIG0_REG   (LP_WDT_BASE + 0x00)
#define LP_WDT_WPROTECT_REG  (LP_WDT_BASE + 0x18)
#define LP_SWD_CONF_REG      (LP_WDT_BASE + 0x1C)
#define LP_SWD_WPROTECT_REG  (LP_WDT_BASE + 0x20)

#define TIMG1_WDT_BASE       0x3FF1F000U
#define TIMG1_WDT_CONFIG0    (TIMG1_WDT_BASE + 0x48)
#define TIMG1_WDT_WPROTECT   (TIMG1_WDT_BASE + 0x64)

#define WDT_WRITE_KEY        0x50D83AA1U
#define SWD_AUTO_FEED_EN     (1U << 18)

void esp_tf_disable_target_watchdogs(esp_loader_t *loader, esp_tf_chip_t chip,
                                     const char *label)
{
    bool is_classic_esp32 = (chip == ESP32_CHIP);

    uint32_t wdt_config0   = is_classic_esp32 ? TIMG1_WDT_CONFIG0   : LP_WDT_CONFIG0_REG;
    uint32_t wdt_wprotect  = is_classic_esp32 ? TIMG1_WDT_WPROTECT  : LP_WDT_WPROTECT_REG;

    /* 禁用 RTC/TIMG WDT */
    if (esp_loader_write_register(loader, wdt_wprotect, WDT_WRITE_KEY) != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "[%s] WDT unlock failed", label);
        return;
    }
    esp_loader_write_register(loader, wdt_config0, 0);
    esp_loader_write_register(loader, wdt_wprotect, 0);

    /* 启用 SWD 自动喂狗（仅 LP WDT 同族有效；ESP32 经典款无此寄存器） */
    if (!is_classic_esp32) {
        uint32_t swd_conf = 0;
        if (esp_loader_read_register(loader, LP_SWD_CONF_REG, &swd_conf) == ESP_LOADER_SUCCESS) {
            esp_loader_write_register(loader, LP_SWD_WPROTECT_REG, WDT_WRITE_KEY);
            esp_loader_write_register(loader, LP_SWD_CONF_REG, swd_conf | SWD_AUTO_FEED_EN);
            esp_loader_write_register(loader, LP_SWD_WPROTECT_REG, 0);
        }
    }

    ESP_LOGI(TAG, "[%s] Watchdogs disabled (%s)", label,
             is_classic_esp32 ? "ESP32 TIMG" : "LP WDT");
}

/* ================================================================
 *  内部辅助：进度回调节流
 * ================================================================ */

void esp_tf_report_progress(esp_tf_progress_cb_t cb, const char *label,
                            size_t written, size_t total, int *last_percent)
{
    if (cb == NULL || total == 0) return;
    int percent = (int)(written * 100 / total);
    if (percent >= *last_percent + 10 || percent >= 100) {
        *last_percent = (percent / 10) * 10;
        cb(label, written, total, percent);
    }
}

/* ================================================================
 *  内部辅助：填充 port 结构
 * ================================================================ */

void esp_tf_fill_port(struct esp32_port *port, const esp_tf_target_t *target,
                      bool use_gpio_ops)
{
    memset(port, 0, sizeof(*port));
    port->port.ops              = &esp32_uart_ops;
    port->baud_rate             = target->baud_connect ? target->baud_connect : 115200;
    port->uart_port             = target->uart_port;
    port->uart_tx_pin           = target->tx_pin;
    port->uart_rx_pin           = target->rx_pin;
    /* 若 gpio_ops 接管下载时序，则 loader 不控制 RST/BOOT */
    port->reset_pin             = use_gpio_ops ? GPIO_NUM_NC : target->reset_pin;
    port->boot_pin              = use_gpio_ops ? GPIO_NUM_NC : target->boot_pin;
    port->rx_buffer_size        = 0;
    port->tx_buffer_size        = 0;
    port->queue_size            = 0;
    port->uart_queue            = NULL;
    port->dont_initialize_peripheral = false;
}

/* ================================================================
 *  会话结构体
 * ================================================================ */

struct esp_tf_session {
    esp_loader_t      loader;
    esp32_port_t      port;
    esp_tf_chip_t     detected_chip;
    const esp_tf_target_t *target;
    const esp_tf_hooks_t  *hooks;
    const esp_tf_gpio_ops_t *gpio_ops;
    bool uart_prepared;
};

/* ================================================================
 *  内部辅助：连接目标
 * ================================================================ */

static esp_tf_err_t do_connect(esp_tf_session_t *sess, bool use_stub)
{
    esp_loader_t *loader = &sess->loader;
    const char *label = sess->target->label;

    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    args.trials = 10;

    esp_loader_error_t err;

    if (use_stub) {
        err = esp_loader_connect_with_stub(loader, &args);
        if (err == ESP_LOADER_SUCCESS) {
            ESP_LOGI(TAG, "[%s] Connected (stub mode)", label);
        } else {
            ESP_LOGW(TAG, "[%s] Stub connect failed (%d), trying ROM-only...", label, err);
            err = esp_loader_connect(loader, &args);
            if (err == ESP_LOADER_SUCCESS) {
                ESP_LOGI(TAG, "[%s] Connected (ROM-only mode)", label);
            }
        }
    } else {
        err = esp_loader_connect(loader, &args);
        if (err == ESP_LOADER_SUCCESS) {
            ESP_LOGI(TAG, "[%s] Connected (ROM-only mode, forced)", label);
        }
    }

    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "[%s] Connect failed: %d", label, err);
        return ESP_TF_ERR_CONNECT;
    }

    /* 读取检测到的芯片型号 */
    target_chip_t detected = esp_loader_get_target(loader);
    sess->detected_chip = (esp_tf_chip_t)detected;

    /* 期望型号校验 */
    if (sess->target->chip != ESP_TF_CHIP_AUTO && detected != sess->target->chip) {
        ESP_LOGE(TAG, "[%s] Chip mismatch: expected %s, detected %s",
                 label, esp_tf_chip_to_str(sess->target->chip),
                 esp_tf_chip_to_str((esp_tf_chip_t)detected));
        return ESP_TF_ERR_CHIP_MISMATCH;
    }

    ESP_LOGI(TAG, "[%s] Detected: %s", label, esp_tf_chip_to_str(sess->detected_chip));

    /* 禁用目标 WDT */
    esp_tf_disable_target_watchdogs(loader, sess->detected_chip, label);

    /* flash size 兜底：自动检测可能返回 0 或过小值，强制为目标声明的值 */
    if (sess->target->target_flash_size > 0 &&
        loader->_target_flash_size < sess->target->target_flash_size) {
        loader->_target_flash_size = sess->target->target_flash_size;
        ESP_LOGI(TAG, "[%s] Flash size forced to %u bytes (fallback)",
                 label, (unsigned)sess->target->target_flash_size);
    }

    /* stub 模式下可选切换到高速波特率 */
    if (use_stub && sess->target->baud_flash > 0 &&
        sess->target->baud_flash != sess->target->baud_connect) {
        esp_loader_error_t br_err = esp_loader_change_transmission_rate(
            loader, sess->target->baud_flash);
        if (br_err != ESP_LOADER_SUCCESS) {
            ESP_LOGW(TAG, "[%s] Change baud to %u failed (%d), keeping %u",
                     label, (unsigned)sess->target->baud_flash, br_err,
                     (unsigned)sess->target->baud_connect);
        } else {
            ESP_LOGI(TAG, "[%s] Baud changed to %u", label,
                     (unsigned)sess->target->baud_flash);
        }
    }

    return ESP_TF_OK;
}

/* ================================================================
 *  会话 API
 * ================================================================ */

esp_tf_err_t esp_tf_session_open(esp_tf_session_t **out,
                                 const esp_tf_target_t *target,
                                 const esp_tf_hooks_t *hooks,
                                 const esp_tf_gpio_ops_t *gpio_ops,
                                 bool use_stub)
{
    if (out == NULL || target == NULL) {
        return ESP_TF_ERR_INVALID_ARG;
    }
    if (target->label == NULL) {
        return ESP_TF_ERR_INVALID_ARG;
    }

    esp_tf_session_t *sess = calloc(1, sizeof(*sess));
    if (sess == NULL) {
        return ESP_TF_ERR_NO_MEM;
    }
    sess->target    = target;
    sess->hooks     = hooks;
    sess->gpio_ops  = gpio_ops;
    sess->uart_prepared = false;

    /* ① UART prepare hook */
    if (hooks && hooks->prepare) {
        esp_err_t e = hooks->prepare(target->uart_port, hooks->ctx);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "[%s] UART prepare hook failed: %s", target->label, esp_err_to_name(e));
            free(sess);
            return ESP_TF_ERR_UART_HOOK;
        }
        sess->uart_prepared = true;
    }

    /* ② 填充 port 并初始化 loader */
    esp_tf_fill_port(&sess->port, target, gpio_ops != NULL);

    esp_loader_error_t err = esp_loader_init_serial(&sess->loader, &sess->port.port);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "[%s] UART port init failed: %d", target->label, err);
        free(sess);
        return ESP_TF_ERR_CONNECT;
    }

    /* ③ 进入下载模式（自定义 GPIO ops 或 loader 内置 trigger） */
    if (gpio_ops && gpio_ops->enter_download) {
        gpio_ops->enter_download(gpio_ops->ctx);
    }

    /* ④ 连接目标 */
    esp_tf_err_t tf_err = do_connect(sess, use_stub);
    if (tf_err != ESP_TF_OK) {
        esp_loader_deinit(&sess->loader);
        free(sess);
        return tf_err;
    }

    *out = sess;
    return ESP_TF_OK;
}

esp_tf_err_t esp_tf_session_flash(esp_tf_session_t *sess,
                                  const esp_tf_image_t *image,
                                  esp_tf_read_cb_t read_cb, void *read_ctx,
                                  size_t total_size,
                                  esp_tf_progress_cb_t progress_cb)
{
    if (sess == NULL || image == NULL || read_cb == NULL || total_size == 0) {
        return ESP_TF_ERR_INVALID_ARG;
    }

    esp_loader_t *loader = &sess->loader;
    const char *label = sess->target->label;
    uint32_t block_size = image->block_size ? image->block_size : 4096;

    ESP_LOGI(TAG, "[%s] Flash start: offset=0x%x size=%u block=%u mode=%s",
             label, (unsigned)image->offset, (unsigned)total_size,
             (unsigned)block_size,
             image->rom_chunked ? "ROM-chunked" :
             image->use_stub   ? "stub" : "ROM");

    /* 分配单块缓冲 */
    uint8_t *block = malloc(block_size);
    if (block == NULL) {
        ESP_LOGE(TAG, "[%s] Failed to alloc %u-byte block buffer",
                 label, (unsigned)block_size);
        return ESP_TF_ERR_NO_MEM;
    }

    esp_tf_err_t result = ESP_TF_OK;
    size_t written = 0;
    int last_percent = -1;

    if (image->rom_chunked) {
        /* ROM 分块模式：每块单独 start→write→finish */
        uint32_t seq = 0;
        while (written < total_size) {
            size_t want = (total_size - written > block_size) ?
                          block_size : (total_size - written);

            size_t got = 0;
            esp_err_t e = read_cb(written, block, want, &got, read_ctx);
            if (e != ESP_OK || got != want) {
                ESP_LOGE(TAG, "[%s] read_cb failed at offset %u", label, (unsigned)written);
                result = ESP_TF_ERR_FLASH_WRITE;
                goto done;
            }

            if (seq < 5 || seq % 50 == 0) {
                ESP_LOGI(TAG, "[%s] Chunk %u: offset=%u size=%u (%u%%)",
                         label, (unsigned)seq, (unsigned)written,
                         (unsigned)want, (unsigned)(written * 100 / total_size));
            }

            esp_loader_flash_cfg_t chunk_cfg = {
                .offset      = image->offset + (uint32_t)written,
                .image_size  = (uint32_t)want,
                .block_size  = block_size,
                .skip_verify = true,
            };

            esp_loader_error_t err = esp_loader_flash_start(loader, &chunk_cfg);
            if (err != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "[%s] Chunk %u flash_start: %d", label, (unsigned)seq, err);
                result = ESP_TF_ERR_FLASH_START;
                goto done;
            }

            err = esp_loader_flash_write(loader, &chunk_cfg, block, (uint32_t)want);
            if (err != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "[%s] Chunk %u flash_write: %d", label, (unsigned)seq, err);
                result = ESP_TF_ERR_FLASH_WRITE;
                goto done;
            }

            esp_loader_flash_finish(loader, &chunk_cfg);

            written += want;
            seq++;
            esp_tf_report_progress(progress_cb, label, written, total_size, &last_percent);
        }
        ESP_LOGI(TAG, "[%s] ROM-chunked complete (%u bytes, no MD5)",
                 label, (unsigned)total_size);

    } else {
        /* 标准模式：单次 start → 多次 write → finish */
        /*
         * 关键：flash_cfg 必须在 start→write*→finish 全程复用同一变量，
         * 否则 _md5_context 丢失，finish 误报 INVALID_MD5。
         */
        esp_loader_flash_cfg_t flash_cfg = {
            .offset      = image->offset,
            .image_size  = (uint32_t)total_size,
            .block_size  = block_size,
            .skip_verify = image->skip_md5,
        };

        esp_loader_error_t err = esp_loader_flash_start(loader, &flash_cfg);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "[%s] flash_start: %d", label, err);
            result = ESP_TF_ERR_FLASH_START;
            goto done;
        }

        uint32_t seq = 0;
        while (written < total_size) {
            size_t want = (total_size - written > block_size) ?
                          block_size : (total_size - written);

            size_t got = 0;
            esp_err_t e = read_cb(written, block, want, &got, read_ctx);
            if (e != ESP_OK || got != want) {
                ESP_LOGE(TAG, "[%s] read_cb failed at offset %u", label, (unsigned)written);
                result = ESP_TF_ERR_FLASH_WRITE;
                goto done;
            }

            if (seq < 5) {
                ESP_LOGI(TAG, "[%s] Writing block %u: offset=%u size=%u",
                         label, (unsigned)seq, (unsigned)written, (unsigned)want);
            }

            err = esp_loader_flash_write(loader, &flash_cfg, block, (uint32_t)want);
            if (err != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "[%s] flash_write at offset %u: %d",
                         label, (unsigned)written, err);
                result = ESP_TF_ERR_FLASH_WRITE;
                goto done;
            }

            written += want;
            seq++;
            esp_tf_report_progress(progress_cb, label, written, total_size, &last_percent);
        }

        /* 确保 100% 回调被触发 */
        if (progress_cb) {
            progress_cb(label, total_size, total_size, 100);
        }

        /* finish（skip_md5=false 时含 MD5 校验） */
        err = esp_loader_flash_finish(loader, &flash_cfg);
        if (err != ESP_LOADER_SUCCESS) {
            if (err == ESP_LOADER_ERROR_INVALID_MD5) {
                ESP_LOGE(TAG, "[%s] MD5 verification FAILED!", label);
            } else {
                ESP_LOGE(TAG, "[%s] flash_finish: %d", label, err);
            }
            result = ESP_TF_ERR_FLASH_FINISH;
            goto done;
        }
        ESP_LOGI(TAG, "[%s] Flash complete (%u bytes), MD5 verified",
                 label, (unsigned)total_size);
    }

done:
    free(block);
    return result;
}

esp_tf_err_t esp_tf_session_close(esp_tf_session_t *sess, bool reset_target)
{
    if (sess == NULL) {
        return ESP_TF_ERR_INVALID_ARG;
    }

    const char *label = sess->target->label;

    /* 复位目标（先回 BOOT 运行电平再 RST 脉冲） */
    if (reset_target) {
        if (sess->gpio_ops && sess->gpio_ops->reset_target) {
            sess->gpio_ops->reset_target(sess->gpio_ops->ctx);
        } else {
            esp_loader_reset_target(&sess->loader);
        }
        ESP_LOGI(TAG, "[%s] Target reset", label);
    }

    esp_loader_deinit(&sess->loader);

    /* 仅在成功时 restore UART */
    if (sess->uart_prepared && sess->hooks && sess->hooks->restore) {
        esp_err_t e = sess->hooks->restore(sess->target->uart_port, sess->hooks->ctx);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "[%s] UART restore hook failed: %s", label, esp_err_to_name(e));
            free(sess);
            return ESP_TF_ERR_UART_HOOK;
        }
    }

    free(sess);
    return ESP_TF_OK;
}

esp_tf_chip_t esp_tf_session_get_detected_chip(esp_tf_session_t *sess)
{
    if (sess == NULL) {
        return ESP_TF_CHIP_AUTO;
    }
    return sess->detected_chip;
}

/* ================================================================
 *  便捷 API：RAM 缓冲烧录
 * ================================================================ */

/* read_cb 适配器：从内存缓冲读取 */
typedef struct {
    const uint8_t *data;
    size_t         size;
} ram_ctx_t;

static esp_err_t ram_read_cb(size_t offset, uint8_t *buf,
                             size_t max_len, size_t *out_len, void *ctx)
{
    ram_ctx_t *rc = (ram_ctx_t *)ctx;
    size_t remain = rc->size - offset;
    size_t want = (remain < max_len) ? remain : max_len;
    memcpy(buf, rc->data + offset, want);
    *out_len = want;
    return ESP_OK;
}

esp_tf_err_t esp_tf_flash_buffer(const esp_tf_target_t *target,
                                 const esp_tf_image_t *image,
                                 const uint8_t *data, size_t size,
                                 const esp_tf_hooks_t *hooks,
                                 const esp_tf_gpio_ops_t *gpio_ops,
                                 esp_tf_progress_cb_t progress_cb)
{
    if (target == NULL || image == NULL || data == NULL || size == 0) {
        return ESP_TF_ERR_INVALID_ARG;
    }

    ram_ctx_t ctx = { .data = data, .size = size };

    esp_tf_session_t *sess = NULL;
    esp_tf_err_t err = esp_tf_session_open(&sess, target, hooks, gpio_ops, image->use_stub);
    if (err != ESP_TF_OK) {
        return err;
    }

    err = esp_tf_session_flash(sess, image, ram_read_cb, &ctx, size, progress_cb);
    esp_tf_session_close(sess, err == ESP_TF_OK);
    return err;
}

/* ================================================================
 *  便捷 API：文件流式烧录
 * ================================================================ */

typedef struct {
    FILE *fp;
} file_ctx_t;

static esp_err_t file_read_cb(size_t offset, uint8_t *buf,
                              size_t max_len, size_t *out_len, void *ctx)
{
    (void)offset;  /* 顺序读取，offset 不用于定位 */
    file_ctx_t *fc = (file_ctx_t *)ctx;
    size_t got = fread(buf, 1, max_len, fc->fp);
    if (got == 0 && ferror(fc->fp)) {
        return ESP_FAIL;
    }
    *out_len = got;
    return ESP_OK;
}

esp_tf_err_t esp_tf_flash_file(const esp_tf_target_t *target,
                               const esp_tf_image_t *image,
                               const char *path,
                               const esp_tf_hooks_t *hooks,
                               const esp_tf_gpio_ops_t *gpio_ops,
                               esp_tf_progress_cb_t progress_cb)
{
    if (target == NULL || image == NULL || path == NULL) {
        return ESP_TF_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "[%s] Firmware file not found: %s", target->label, path);
        return ESP_TF_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total <= 0) {
        ESP_LOGE(TAG, "[%s] Invalid firmware size: %ld", target->label, total);
        fclose(f);
        return ESP_TF_ERR_INVALID_ARG;
    }

    file_ctx_t fctx = { .fp = f };

    esp_tf_session_t *sess = NULL;
    esp_tf_err_t err = esp_tf_session_open(&sess, target, hooks, gpio_ops, image->use_stub);
    if (err != ESP_TF_OK) {
        fclose(f);
        return err;
    }

    err = esp_tf_session_flash(sess, image, file_read_cb, &fctx,
                               (size_t)total, progress_cb);
    esp_tf_session_close(sess, err == ESP_TF_OK);
    fclose(f);
    return err;
}

/* ================================================================
 *  便捷 API：回调流式烧录
 * ================================================================ */

esp_tf_err_t esp_tf_flash_stream(const esp_tf_target_t *target,
                                 const esp_tf_image_t *image,
                                 size_t total_size,
                                 esp_tf_read_cb_t read_cb, void *read_ctx,
                                 const esp_tf_hooks_t *hooks,
                                 const esp_tf_gpio_ops_t *gpio_ops,
                                 esp_tf_progress_cb_t progress_cb)
{
    if (target == NULL || image == NULL || read_cb == NULL || total_size == 0) {
        return ESP_TF_ERR_INVALID_ARG;
    }

    esp_tf_session_t *sess = NULL;
    esp_tf_err_t err = esp_tf_session_open(&sess, target, hooks, gpio_ops, image->use_stub);
    if (err != ESP_TF_OK) {
        return err;
    }

    err = esp_tf_session_flash(sess, image, read_cb, read_ctx,
                               total_size, progress_cb);
    esp_tf_session_close(sess, err == ESP_TF_OK);
    return err;
}
