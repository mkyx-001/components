# esp_target_flasher_demo

> ESP32-P4 主机给 ESP32-C3 从机烧录固件 — 示例工程

## 硬件连接

```
P4 (Host)                    C3 (Target)
─────────────────────────────────────────
GPIO4  ────────────────────► GPIO20 (UART RX)
GPIO5  ◄──────────────────── GPIO19 (UART TX)
GPIO6  ────────────────────► EN/RST
GPIO7  ────────────────────► GPIO9  (BOOT)
GND    ────────────────────► GND
```

> ⚠️ 下载模式需要 P4 能控制 C3 的 RST 和 BOOT 引脚。如果你的 C3 开发板
> 有自动复位电路（USB→UART 芯片控制 DTR/RTS），需要额外接线或断开现有电路。

## 功能说明

串口终端交互菜单：

| 按键 | 功能 | API |
|------|------|-----|
| `1` | RAM 缓冲烧录 | `esp_tf_flash_buffer()` |
| `2` | 回调流式烧录 | `esp_tf_flash_stream()` |
| `3` | 文件流式烧录 | `esp_tf_flash_file()` |
| `0` | 写演示数据到 SPIFFS | （辅助功能） |

- 功能 `1`、`2` 使用内置 16KB 测试数据（非可启动固件，仅演示 API）
- 功能 `3` 需要取消 `main.c` 中 `#define USE_SPIFFS` 注释，并在分区表中添加 SPIFFS 分区
- **真实使用时**：替换 `s_demo_payload` 为 `esptool merge-bin` 生成的合并镜像

## 编译烧录

```bash
cd examples/esp_target_flasher_demo
idf.py set-target esp32p4
idf.py build
idf.py -p COM18 flash monitor
```

## 关键代码解读

### 目标描述

```c
esp_tf_target_t target = {
    .label             = "C3",
    .chip              = ESP_TF_CHIP_ESP32_C3,   // 连接后自动校验
    .uart_port         = UART_NUM_1,
    .tx_pin            = GPIO_NUM_4,
    .rx_pin            = GPIO_NUM_5,
    .reset_pin         = GPIO_NUM_6,
    .boot_pin          = GPIO_NUM_7,
    .baud_connect      = 115200,
    .baud_flash        = 0,           // 0 = 不切换波特率；921600 = stub 高速
    .target_flash_size = 4 * 1024 * 1024,
};
```

### 最简烧录调用

```c
esp_tf_image_t image = ESP_TF_IMAGE_DEFAULT();
esp_tf_err_t err = esp_tf_flash_buffer(&target, &image,
                                       fw_data, fw_size,
                                       NULL, NULL, progress_cb);
```

### 进度回调

组件内部已做 10% 节流，回调直接打印即可：

```c
static void progress_cb(const char *label, size_t written,
                        size_t total, int percent) {
    ESP_LOGI(TAG, "[%s] %d%%", label, percent);
}
```

## 移植到其他芯片

修改 `main.c` 顶部引脚宏与 `s_c3_target.chip` 即可：

```c
#define TF_TX_PIN   GPIO_NUM_xx   // 你的接线
#define TF_RX_PIN   GPIO_NUM_xx
// ...
s_c3_target.chip = ESP_TF_CHIP_ESP32_C5;  // 或 C2/C6/H2/S3 等
```
