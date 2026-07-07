# esp_target_flasher

> ESP 主机给 ESP 从机烧录固件的共享组件，封装 [espressif/esp-serial-flasher](https://components.espressif.com/components/espressif/esp-serial-flasher) v2。

## 概述

本组件提供 **ESP → ESP** UART 烧录的统一接口，从 V4PRO 量产代码提炼，去掉了项目硬编码：

| 特性 | 说明 |
|------|------|
| 目标型号安全校验 | 连接后比对 `expected` vs `detected`，防错刷（C5 固件不会误刷到 C2） |
| Stub / ROM 双模式 | Stub 高速 + MD5 校验；ROM 分块模式兜底（C5 大镜像） |
| 三种数据源 | RAM 缓冲 / 文件流式 / 回调流式（HTTP、加密分区等） |
| 目标 WDT 禁用 | 连接后自动禁用 RTC WDT + 启用 SWD 自动喂狗 |
| Flash size 兜底 | 自动检测失败时强制指定值（如 2MB） |
| 共享 BOOT 互斥锁 | 多目标共用 BOOT 引脚时 `esp_tf_lock/unlock` |
| UART 生命周期钩子 | `prepare/restore` 回调支持 UART 分时复用 |

## 依赖

```yaml
dependencies:
  espressif/esp-serial-flasher: "~2.0.0"
  idf: ">=5.5"
```

## 快速开始

```c
#include "esp_target_flasher.h"

/* 1. 初始化组件 */
esp_tf_init();

/* 2. 描述目标 */
esp_tf_target_t target = {
    .label           = "C3",
    .chip            = ESP_TF_CHIP_ESP32_C3,
    .uart_port       = UART_NUM_1,
    .tx_pin          = GPIO_NUM_4,
    .rx_pin          = GPIO_NUM_5,
    .reset_pin       = GPIO_NUM_6,
    .boot_pin        = GPIO_NUM_7,
    .baud_connect    = 115200,
    .baud_flash      = 0,        /* 不切换波特率 */
    .target_flash_size = 4 * 1024 * 1024,
};

/* 3. 烧录参数 */
esp_tf_image_t image = ESP_TF_IMAGE_DEFAULT();

/* 4. 从内存缓冲烧录 */
esp_tf_err_t err = esp_tf_flash_buffer(&target, &image,
                                       fw_data, fw_size,
                                       NULL, NULL, my_progress_cb);
if (err != ESP_TF_OK) {
    ESP_LOGE(TAG, "Flash failed: %s", esp_tf_err_to_str(err));
}
```

## API 分层

### 便捷 API（一步到位）

| 函数 | 数据源 | 典型场景 |
|------|--------|----------|
| `esp_tf_flash_buffer()` | RAM 缓冲 | 小体积固件 / PSRAM 缓冲 |
| `esp_tf_flash_file()` | 文件路径 | LittleFS / SPIFFS 固件 |
| `esp_tf_flash_stream()` | 回调 | HTTP 下载 / 加密分区 |

### 会话 API（需要多步操作时）

```c
esp_tf_session_t *sess;
esp_tf_session_open(&sess, &target, &hooks, &gpio_ops, true);
// 可在此读取 MAC、改 baud 等
esp_tf_session_flash(sess, &image, read_cb, ctx, total, progress_cb);
esp_tf_session_close(sess, true);  /* true = 复位目标运行新固件 */
```

## 烧录模式选择

| 模式 | `use_stub` | `rom_chunked` | 速度 | MD5 | 适用 |
|------|------------|---------------|------|-----|------|
| Stub 标准 | `true` | `false` | 高速 | ✅ | **推荐**，>2MB 镜像 |
| ROM 标准 | `false` | `false` | 慢 | ✅ | Flash/RAM 紧张 |
| ROM 分块 | `false` | `true` | 慢 | ❌ | C5 大镜像兜底 |

## 自定义下载时序

若不用 loader 内置 `reset_pin`/`boot_pin` 触发（如经模拟开关选路）：

```c
static void my_enter_download(void *ctx) {
    gpio_set_level(MUX_A0, ...);  /* 选通目标 */
    gpio_set_level(BOOT_PIN, 0);   /* 拉 BOOT */
    gpio_set_level(RST_PIN, 0);    /* RST 低 */
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(RST_PIN, 1);    /* 释放 RST */
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOOT_PIN, 1);   /* 释放 BOOT */
}

static void my_reset_target(void *ctx) {
    gpio_set_level(BOOT_PIN, 1);   /* 确保 BOOT 回运行电平 */
    gpio_set_level(RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(RST_PIN, 1);
}

esp_tf_gpio_ops_t gpio = {
    .enter_download = my_enter_download,
    .reset_target   = my_reset_target,
    .ctx            = NULL,
};
esp_tf_flash_file(&target, &image, "/lfs/fw.bin", NULL, &gpio, progress_cb);
```

## 常见故障

| 现象 | 原因 | 解决 |
|------|------|------|
| `CHIP_MISMATCH` | UART 接错了型号的目标 | 检查接线，或设 `ESP_TF_CHIP_AUTO` |
| `FLASH_FINISH (MD5)` | `flash_cfg` 未全程复用同一变量 | 组件已处理，若自定义调用注意 |
| 连接超时 | RST/BOOT 极性反了 | 检查 `CONFIG_SERIAL_FLASHER_*_INVERT` |
| 擦写超时 | 目标 WDT 复位 | 组件已自动禁用 WDT |

## 许可证

Apache License 2.0
