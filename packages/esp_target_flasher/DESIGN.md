# esp_target_flasher — ESP 主机给 ESP 从机烧录 共享组件设计

> 组件库目录：`D:\components\packages\esp_target_flasher`  
> 底层依赖：官方 [`espressif/esp-serial-flasher`](https://components.espressif.com/components/espressif/esp-serial-flasher)  
> 文档版本：v1.0 ｜ 日期：2026-07-04  
> 范围：**仅 ESP32 系列主机 → ESP32 系列目标**，UART 烧录；**不含 Lua / ymodem / PC esptool 封装**

---

## 1. 背景：各项目现状调研

### 1.1 已实现的 C 代码（可抽取来源）

| 项目 | 路径 | 主机 | 目标 | 状态 |
|------|------|------|------|------|
| **V4PRO / rid** | `D:\my\rid\v4pro\p4\main\flasher\` | ESP32-P4 | 4×ESP32-C2 + 1×ESP32-C5 | **已量产验证** |
| **V4PRO / RD** | `D:\my\RD\V4PRO\V4PRO\main\flasher\` | 同上 | 同上 | 与 rid 同源副本 |

核心文件：

```
flasher/
├── flasher_common.h/.c   # esp-serial-flasher 封装：互斥锁、RAM/文件流式烧录、WDT 禁用
├── c2_flasher.h/.c       # 4 路 C2：下载模式时序、UART 分时复用、逐路烧录
├── c5_flasher.h/.c       # C5：同上 + USB 桥接兜底模式
└── ota_manager.h/.c      # 项目级 OTA 编排（HTTP/LittleFS/版本对齐）—— 不纳入本组件
```

**应抽取到共享组件**：`flasher_common.*` 中的通用逻辑 + 可参数化的「进入下载模式 / 复位目标」模式。  
**保留在各工程**：`c2_flasher` / `c5_flasher` 的引脚表、`board_config.h`、`uart_c2/uart_c5` 驱动卸载、`ota_manager`。

### 1.2 仅有设计文档（尚未写 C）

| 项目 | 路径 | 说明 |
|------|------|------|
| **vehicleDetector M1** | `D:\work\vehicleDetector\docs\M1固件升级方案.md` | S3 主控 + 模拟开关选路，给 S1(S3)/S2(S3)/S3(C5) 升级；规划组件 `fw_updater` |
| **trafficflow** | `D:\work\trafficflow\doc\design\共享组件与多节点部署方案.md` | P4 给 C2/C5 烧录在方案层提及，**无 flasher 源码** |

M1 方案与 V4PRO 共性：UART + 共享 BOOT + 独立 RST + stub 高速烧录 + 先下载校验再烧录。差异在 **UART 模拟开关选路**（M1 特有，通过 Kconfig/回调注入组件）。

### 1.3 明确排除（非 ESP→ESP C 方案）

| 项目 | 原因 |
|------|------|
| **NBrbox** `Lua/fota/updata.lua` | Air8000(Lua) 自定义 SLIP，非 ESP-IDF C |
| **mbox** 烧录包 / ESP32FlashTool | PC 端生产烧录工具链 |
| **shoulder/c3 OTA** | 蓝牙 OTA 给 C3 自身升级，非主机给子 ESP 烧录 |

---

## 2. 官方方案：esp-serial-flasher

### 2.1 定位

[esp-serial-flasher](https://github.com/espressif/esp-serial-flasher) 是 Espressif 官方的嵌入式烧录库，作用类似 PC 上的 [esptool](https://github.com/espressif/esptool)，但运行在 **STM32 / ESP32 / Linux / Pico** 等主机上，通过 SLIP 协议与目标 ROM Bootloader（或 stub）通信。

**Host（主机）**：运行本库、发起烧录的 MCU（如 ESP32-P4、ESP32-S3）。  
**Target（目标）**：被烧录的 Espressif SoC（如 ESP32-C2/C5/S3）。

### 2.2 ESP32 作 Host 的支持情况（Registry v2.0.0）

| 接口 | ESP32-P4 Host | 说明 |
|------|---------------|------|
| UART | ✅ | V4PRO / M1 方案均用此接口 |
| USB CDC ACM | ✅ | v1.10+ 支持 P4 作 Host；可用于 USB 直连目标 |
| SPI | 🚧 | 仅 RAM download |
| SDIO | ✅ | v1.9+ 实验性；v1.11.0 起支持 C5 target + 1-bit SDIO |

目标芯片 UART 支持：ESP8266、ESP32、S2/S3、C2/C3/C5/C6/C61、H2、P4 等（见官方 README 矩阵）。

### 2.3 推荐连接模式

| 模式 | API | 速度 | Flash 开销 | 适用 |
|------|-----|------|------------|------|
| **Stub** | `esp_loader_connect_with_stub()` | 可提 baud（如 921600） | +~87KB（全部 stub 链入） | **推荐**：>2MB 镜像、MD5 校验、批量写 |
| **ROM only** | `esp_loader_connect()` | 115200，擦除慢 | ~0KB | 内存/Flash 紧张；C5 大镜像需分块擦写（V4PRO 已实现） |

Stub 来自 [`espressif/esp-flasher-stub`](https://github.com/espressif/esp-flasher-stub)；v2.0.0 已迁移到新 stub 体系，**所有支持芯片均内置 stub**。

### 2.4 v2.0.0 公共 API 要点（与 v1 差异）

当前 Registry 最新稳定版为 **2.0.0**（2026-06-04）。V4PRO 已使用 v2 API（`esp_loader_t` 上下文、`esp32_port_t` 结构体）。

**基本烧录流程**：

```c
#include "esp_loader.h"
#include "esp32_port.h"

esp32_port_t port = {
    .port.ops    = &esp32_uart_ops,
    .baud_rate   = 115200,
    .uart_port   = UART_NUM_1,
    .uart_tx_pin = GPIO_NUM_4,
    .uart_rx_pin = GPIO_NUM_5,
    .reset_pin   = GPIO_NUM_25,
    .boot_pin    = GPIO_NUM_26,
};

esp_loader_t loader;
esp_loader_init_serial(&loader, &port.port);

esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
esp_loader_connect_with_stub(&loader, &args);   // 或 esp_loader_connect()

esp_loader_flash_cfg_t cfg = {
    .offset     = 0x10000,
    .image_size = size,
    .block_size = 4096,
};
esp_loader_flash_start(&loader, &cfg);
// 循环 esp_loader_flash_write(&loader, &cfg, buf, chunk)
esp_loader_flash_finish(&loader, &cfg);   // v2: 必须调用，内含 MD5 校验

esp_loader_deinit(&loader);
```

**v1 → v2 迁移要点**（[Migration Guide](https://github.com/espressif/esp-serial-flasher/blob/master/docs/migration-v1-to-v2.md)）：

- 端口配置：`reset_trigger_pin` → `reset_pin`，`gpio0_trigger_pin` → `boot_pin`
- 全局 `loader_port_*_init()` 已移除，改用 `esp32_port_t` + `esp_loader_init_serial()`
- `esp_loader_flash_verify()` 已移除，MD5 由 `esp_loader_flash_finish()` 完成
- 需 **ESP-IDF ≥ 5.5**（v2 不再支持 5.4 及以下）
- `esp_loader_change_transmission_rate_stub()` 已移除，连接 stub 时直接指定目标波特率

**目标芯片型号**：官方 API **不在 connect 参数里传 chip**，连接时 ROM 回 magic 值，库写入 `loader._target`，可通过 `esp_loader_get_target()` 读取。`target_chip_t` 枚举定义在 `esp_loader.h`（`ESP32C2_CHIP`、`ESP32C5_CHIP` 等）。  
本共享组件在 `esp_tf_target_t.chip` 中增加 **期望型号** 字段，连接成功后与 `esp_loader_get_target()` 比对——这是工程层安全需求，不是官方 API 的缺口。

### 2.5 v1.11.0 Changelog 与我们的关系

参考：[esp-serial-flasher v1.11.0 changelog](https://components.espressif.com/components/espressif/esp-serial-flasher/versions/1.11.0/changelog?language=en)

| 版本变更 | 对本组件的意义 |
|----------|----------------|
| **v1.11.0** SDIO for ESP32-C5 + 1-bit SDIO | 若未来 P4↔C5 走 SDIO 硬件，可换接口；当前 V4PRO 用 UART |
| **v1.11.0** flash boundary check 修复 | 读/校验边界更安全 |
| **v1.10.0** P4 作 Host 的 USB CDC ACM | 可选：USB 直连烧录子板 |
| **v1.9.0** ESP32-P4 Host、C5 无 stub 支持、erase API | P4 作主机的基础能力 |
| **v1.8.0** P4 Host INVALID_ARG 修复 | P4 项目必用 ≥1.8 |
| **v1.4.0** stub 支持 | 高速烧录基础 |

**版本建议**：

- 新工程：**pin `espressif/esp-serial-flasher: "~2.0.0"`**（与 V4PRO 一致）
- 若 IDF 暂无法升到 5.5：可暂用 `^1.11.0`，但 API 与 v2 不兼容，需按 v1 写法封装

M1 文档曾提到 v1.11.0 不含 C5 stub——**v2.0.0 已包含完整 stub**，该问题已解决。

### 2.6 Kconfig / sdkconfig 必配项

```ini
# ESP-IDF 工程 sdkconfig.defaults
CONFIG_SERIAL_FLASHER_PORT_UART=y
CONFIG_SERIAL_FLASHER_RESET_INVERT=y   # 若 RST 经反相器（V4PRO）
CONFIG_SERIAL_FLASHER_BOOT_INVERT=y    # 若 BOOT 经反相器（V4PRO）
```

引脚极性也可在 `esp32_port_t` 层配合 `CONFIG_SERIAL_FLASHER_*_INVERT`，与 `board_config.h` 中 `PIN_LVL_*` 宏保持一致。

---

## 3. 组件分层架构

```
┌─────────────────────────────────────────────────────────────┐
│  工程层（各产品 main/）                                        │
│  c2_flasher / c5_flasher / fw_updater / ota_manager        │
│  - 引脚表、模拟开关、HTTP 下载、版本策略、UART 驱动生命周期    │
└───────────────────────────┬─────────────────────────────────┘
                            │ 调用
┌───────────────────────────▼─────────────────────────────────┐
│  esp_target_flasher（本共享组件）                             │
│  - 目标描述 esp_tf_target_t                                   │
│  - 会话：connect → flash → finish → reset                   │
│  - 互斥锁（共享 BOOT）                                        │
│  - RAM / 文件 / 回调 三种数据源                               │
│  - WDT 禁用、flash size 兜底、进度回调                        │
│  - hooks：before/after_uart_handover                          │
└───────────────────────────┬─────────────────────────────────┘
                            │ 依赖
┌───────────────────────────▼─────────────────────────────────┐
│  espressif/esp-serial-flasher v2                            │
│  SLIP 协议 / stub / MD5 / esp32_port UART                   │
└─────────────────────────────────────────────────────────────┘
```

**设计原则**：

1. **不把 V4PRO 引脚写死在组件里**——通过 `esp_tf_target_t` 注入。
2. **不把 OTA/HTTP/LittleFS 放进组件**——只接受内存指针、文件路径或 read 回调。
3. **UART 驱动卸载由 hook 完成**——组件不依赖 `uart_c2.h` 等项目头文件。
4. **复用 V4PRO 已验证的 workaround**——目标 WDT 禁用、flash size 强制 2MB、C5 ROM 分块模式。

---

## 4. 目录结构（目标）

```
packages/esp_target_flasher/
├── idf_component.yml          # 依赖 espressif/esp-serial-flasher ~2.0.0
├── CMakeLists.txt
├── Kconfig.esp_target_flasher
├── LICENSE
├── README.md
├── DESIGN.md                  # 本文档
├── include/
│   └── esp_target_flasher.h   # 唯一公共头文件
└── src/
    ├── esp_target_flasher.c   # 公共 API 实现
    └── esp_tf_internal.h      # WDT、进度等内部 helper（不对外）
```

---

## 5. 公共数据类型

### 5.1 目标芯片型号 `esp_tf_chip_t`

官方 `esp-serial-flasher` 在 `esp_loader_connect*()` 时通过 **chip magic 自动识别**目标型号，连接后可调用 `esp_loader_get_target()` 读取。  
`esp_loader_connect_args_t` **不含** chip 字段——v2 起 Secure Download Mode 也不再要求预先指定型号。

但工程侧（V4PRO 4 路 C2 + 1 路 C5、M1 模拟开关选 S1/S2/S3）**必须显式声明期望型号**，用于：

1. **安全校验**：连接后比对 detected vs expected，防止 C5 固件误刷到 C2
2. **芯片相关 workaround**：WDT 寄存器路径、ROM 分块策略、stub 选择日志
3. **可读性**：日志与 OTA 策略按型号分支

本组件 **直接复用** 官方枚举（避免重复维护），在公共头文件中 typedef 别名：

```c
#include "esp_loader.h"

/** 与 esp-serial-flasher 的 target_chip_t 一一对应 */
typedef target_chip_t esp_tf_chip_t;

/* 常用别名（值来自 esp_loader.h） */
#define ESP_TF_CHIP_AUTO        ESP_UNKNOWN_CHIP   /* 仅信任自动检测，不做期望校验 */
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
```

| 工程 | 期望 `chip` 取值 |
|------|------------------|
| V4PRO C2×4 | `ESP_TF_CHIP_ESP32_C2` |
| V4PRO C5 | `ESP_TF_CHIP_ESP32_C5` |
| M1 S1 | `ESP_TF_CHIP_ESP32_S3` |
| M1 S2 / S3 | `ESP_TF_CHIP_ESP32_C5` |
| 调试/未知板 | `ESP_TF_CHIP_AUTO`（不推荐量产） |

### 5.2 目标硬件描述 `esp_tf_target_t`

```c
typedef struct {
    const char     *label;           /* 日志标签，如 "C2#1" */
    esp_tf_chip_t   chip;            /* 期望目标型号；ESP_TF_CHIP_AUTO = 不校验 */
    uint32_t        uart_port;       /* UART_NUM_x */
    gpio_num_t      tx_pin;
    gpio_num_t      rx_pin;
    gpio_num_t      reset_pin;       /* GPIO_NUM_NC 表示由调用方手动控制 */
    gpio_num_t      boot_pin;        /* GPIO_NUM_NC 同上 */
    uint32_t        baud_connect;    /* 连接波特率，通常 115200 */
    uint32_t        baud_flash;      /* stub 连接后的目标波特率；0=不切换 */
    uint32_t        target_flash_size; /* 0=自动检测；检测失败时的兜底（如 2MB） */
} esp_tf_target_t;

/** 常用默认值：C2 2MB Flash、115200 连接、期望 ESP32-C2 */
#define ESP_TF_TARGET_C2_DEFAULT(uart, tx, rx, rst, boot, lbl) { \
    .label = (lbl), .chip = ESP_TF_CHIP_ESP32_C2, \
    .uart_port = (uart), .tx_pin = (tx), .rx_pin = (rx), \
    .reset_pin = (rst), .boot_pin = (boot), \
    .baud_connect = 115200, .baud_flash = 0, \
    .target_flash_size = 2 * 1024 * 1024, \
}
```

连接成功后的校验逻辑（组件内部）：

```
detected = esp_loader_get_target(&loader)
if (target->chip != ESP_TF_CHIP_AUTO && detected != target->chip)
    return ESP_TF_ERR_CHIP_MISMATCH   /* 日志打印 expected vs detected */
```

### 5.3 烧录参数 `esp_tf_image_t`

```c
typedef struct {
    uint32_t offset;             /* Flash 偏移，合并 bin 通常为 0x0 */
    uint32_t block_size;         /* 写块大小，默认 4096 */
    bool     use_stub;           /* true: 先 stub 再写；false: ROM only */
    bool     rom_chunked;        /* ROM 模式下按块 erase+write（大镜像/C5） */
    bool     skip_md5;           /* 跳过 finish 时 MD5（ROM 分块模式常用） */
} esp_tf_image_t;

#define ESP_TF_IMAGE_DEFAULT() { \
    .offset = 0, .block_size = 4096, \
    .use_stub = true, .rom_chunked = false, .skip_md5 = false \
}
```

### 5.4 进度回调

```c
typedef void (*esp_tf_progress_cb_t)(const char *label,
                                     size_t written, size_t total, int percent);
```

### 5.5 UART 生命周期钩子（分时复用）

```c
typedef esp_err_t (*esp_tf_uart_prepare_cb_t)(uint32_t uart_port, void *ctx);
typedef esp_err_t (*esp_tf_uart_restore_cb_t)(uint32_t uart_port, void *ctx);

typedef struct {
    esp_tf_uart_prepare_cb_t prepare;  /* 烧录前：停 RX 任务、uart_driver_delete */
    esp_tf_uart_restore_cb_t restore;  /* 烧录后：uart_driver_install、恢复业务 */
    void *ctx;
} esp_tf_hooks_t;
```

### 5.6 自定义下载模式时序（可选）

若不用 esp-serial-flasher 内置 `reset_pin`/`boot_pin` 触发（V4PRO `flasher_do_flash` 即如此），提供：

```c
typedef void (*esp_tf_enter_download_cb_t)(void *ctx);
typedef void (*esp_tf_reset_target_cb_t)(void *ctx);

typedef struct {
    esp_tf_enter_download_cb_t enter_download;
    esp_tf_reset_target_cb_t   reset_target;
    void *ctx;
} esp_tf_gpio_ops_t;
```

---

## 6. 公共 API

### 6.1 全局

```c
/** 初始化组件（创建互斥锁等），建议在 app_main 早期调用一次 */
esp_err_t esp_tf_init(void);

/** 共享 BOOT 引脚场景的全局互斥锁 */
void esp_tf_lock(void);
void esp_tf_unlock(void);
```

### 6.2 从内存缓冲烧录

```c
/**
 * 一次性 RAM 镜像烧录（固件已全部在内存中）。
 * 适用于 M1 PSRAM 缓冲、或小体积 C2 固件。
 */
esp_err_t esp_tf_flash_buffer(const esp_tf_target_t *target,
                              const esp_tf_image_t *image,
                              const uint8_t *data, size_t size,
                              const esp_tf_hooks_t *hooks,
                              const esp_tf_gpio_ops_t *gpio_ops,
                              esp_tf_progress_cb_t progress_cb);
```

### 6.3 从文件流式烧录

```c
/**
 * 按 block_size 从文件读取并烧录，峰值 RAM ≈ 一块（默认 4KB）。
 * 适用于 V4PRO C2 ~600KB / C5 ~838KB 大于空闲堆的场景。
 */
esp_err_t esp_tf_flash_file(const esp_tf_target_t *target,
                            const esp_tf_image_t *image,
                            const char *path,
                            const esp_tf_hooks_t *hooks,
                            esp_tf_gpio_ops_t *gpio_ops,  /* 可为 NULL：用 loader 内置 trigger */
                            esp_tf_progress_cb_t progress_cb);
```

### 6.4 从回调流式烧录（无文件系统）

```c
typedef esp_err_t (*esp_tf_read_cb_t)(size_t offset, uint8_t *buf,
                                      size_t max_len, size_t *out_len, void *ctx);

esp_err_t esp_tf_flash_stream(const esp_tf_target_t *target,
                              const esp_tf_image_t *image,
                              size_t total_size,
                              esp_tf_read_cb_t read_cb, void *read_ctx,
                              const esp_tf_hooks_t *hooks,
                              const esp_tf_gpio_ops_t *gpio_ops,
                              esp_tf_progress_cb_t progress_cb);
```

### 6.5 低级会话 API（高级用法）

供需要「连接后读 MAC / 改 baud / 多段写入」的场景：

```c
typedef struct esp_tf_session esp_tf_session_t;

esp_err_t esp_tf_session_open(esp_tf_session_t **out,
                              const esp_tf_target_t *target,
                              const esp_tf_hooks_t *hooks,
                              const esp_tf_gpio_ops_t *gpio_ops,
                              bool use_stub);

esp_err_t esp_tf_session_flash(esp_tf_session_t *sess,
                               const esp_tf_image_t *image,
                               esp_tf_read_cb_t read_cb, void *read_ctx,
                               size_t total_size,
                               esp_tf_progress_cb_t progress_cb);

esp_err_t esp_tf_session_close(esp_tf_session_t *sess, bool reset_target);

/** 会话已连接时，读取 loader 自动识别到的芯片型号 */
esp_tf_chip_t esp_tf_session_get_detected_chip(esp_tf_session_t *sess);
```

### 6.6 错误码

```c
typedef enum {
    ESP_TF_OK = 0,
    ESP_TF_ERR_INVALID_ARG,
    ESP_TF_ERR_NO_MEM,
    ESP_TF_ERR_NOT_FOUND,      /* 文件不存在 */
    ESP_TF_ERR_CONNECT,        /* esp_loader_connect* 失败 */
    ESP_TF_ERR_CHIP_MISMATCH,  /* 期望 chip != esp_loader_get_target() 检测结果 */
    ESP_TF_ERR_FLASH_START,
    ESP_TF_ERR_FLASH_WRITE,
    ESP_TF_ERR_FLASH_FINISH,   /* 含 MD5 失败 */
    ESP_TF_ERR_UART_HOOK,
} esp_tf_err_t;

const char *esp_tf_err_to_str(esp_tf_err_t err);
```

---

## 7. 内部实现要点（自 V4PRO 沉淀）

以下逻辑从 `flasher_common.c` 提炼，作为组件 `src/esp_target_flasher.c` 的实现规范：

### 7.1 连接策略

```
esp_loader_init_serial()
    ↓
if (use_stub)
    esp_loader_connect_with_stub() ──失败──→ esp_loader_connect() [ROM fallback]
else
    esp_loader_connect()
    ↓
detected = esp_loader_get_target()
if (target->chip != ESP_TF_CHIP_AUTO && detected != target->chip)
    → ESP_TF_ERR_CHIP_MISMATCH
    ↓
disable_target_watchdogs(detected)  // 按 detected 选 WDT 寄存器表（C2/C3/C5/C6 等同族）
    ↓
if (detected_flash_size < fallback) force fallback   // V4PRO: 2MB
    ↓
[可选] esp_loader_change_transmission_rate()  // baud_flash > 0
```

### 7.2 目标 WDT 禁用（按 `detected` 芯片分支）

V4PRO 实测：长时间 `FLASH_BEGIN` 擦除会触发目标 RTC WDT 复位，导致主机侧超时。  
组件内置 `disable_target_watchdogs(esp_tf_chip_t chip)`，根据 **连接后识别到的型号** 选择寄存器表（C5/C6/C3 等同族共用 LP WDT 基址 `0x600B1C00`；ESP32 经典款走不同路径），与 esptool `_post_connect()` 行为对齐。

> 若 `chip` 期望为 C2 但 detected 为 C5，应在上一步已返回 `ESP_TF_ERR_CHIP_MISMATCH`，不会执行 WDT 操作。

### 7.3 ROM 分块模式（`rom_chunked = true`）

针对 C5 大镜像 + ROM-only 或 stub flash_write 异常时的兜底：

- 每块：`flash_start(单块) → flash_write → flash_finish(skip_verify)`
- 避免 ROM 一次性擦除整片镜像超时
- 此模式 **不做 MD5**（`skip_md5 = true`）

### 7.4 MD5 上下文生命周期（stub 模式）

**关键**：`esp_loader_flash_cfg_t` 必须在 `flash_start → flash_write* → flash_finish` 全程复用**同一变量**，否则 `_md5_context` 丢失，finish 误报 `ESP_LOADER_ERROR_INVALID_MD5`（V4PRO 已踩坑修复）。

### 7.5 UART 分时复用约定

1. `hooks.prepare()` — 停止 RX 任务、`uart_driver_delete`
2. `esp_tf_lock()` — 若多目标共享 BOOT
3. 进入下载模式（gpio_ops 或 loader trigger pin）
4. 烧录
5. `gpio_ops.reset_target()` — 确保 BOOT 回到运行电平再 RST 脉冲
6. `esp_tf_unlock()`
7. 仅成功时 `hooks.restore()`；失败则提示需断电恢复

### 7.6 进度回调节流

每跨越 10% 或到达 100% 触发一次，避免串口日志洪泛（与 V4PRO `report_progress` 一致）。

---

## 8. 工程层集成示例

### 8.1 V4PRO：单路 C2 流式烧录

```c
#include "esp_target_flasher.h"
#include "board_config.h"
#include "uart_c2.h"

static esp_err_t c2_prepare(uint32_t port, void *ctx)
{
    uint8_t id = (uint8_t)(uintptr_t)ctx;
    return uart_c2_deinit(id);
}

static esp_err_t c2_restore(uint32_t port, void *ctx)
{
    uint8_t id = (uint8_t)(uintptr_t)ctx;
    return uart_c2_reinit(id);
}

static void c2_enter_download(void *ctx)
{
    uint8_t id = (uint8_t)(uintptr_t)ctx;
    /* enter_download_mode() 自 c2_flasher.c 移入 */
}

esp_err_t my_c2_flash_from_file(uint8_t c2_id, const char *path)
{
    esp_tf_target_t target = {
        .label = "C2A",
        .chip = ESP_TF_CHIP_ESP32_C2,   /* 连接后校验，防错刷 */
        .uart_port = c2_uart_ports[c2_id],
        .tx_pin = c2_tx_pins[c2_id],
        .rx_pin = c2_rx_pins[c2_id],
        .reset_pin = c2_rst_pins[c2_id],
        .boot_pin = SHARED_BOOT_PIN,
        .baud_connect = 115200,
        .baud_flash = 0,
        .target_flash_size = 2 * 1024 * 1024,
    };
    esp_tf_image_t image = ESP_TF_IMAGE_DEFAULT();
    esp_tf_hooks_t hooks = {
        .prepare = c2_prepare,
        .restore = c2_restore,
        .ctx = (void *)(uintptr_t)c2_id,
    };
    esp_tf_gpio_ops_t gpio = {
        .enter_download = c2_enter_download,
        .reset_target = c2_reset_one,
        .ctx = (void *)(uintptr_t)c2_id,
    };
    return esp_tf_flash_file(&target, &image, path, &hooks, &gpio, my_progress);
}
```

### 8.2 M1：模拟开关 + 高速 stub

```c
esp_tf_target_t target = {
    .label = "S3",
    .chip = ESP_TF_CHIP_ESP32_C5,   /* M1 的 S3 从设备为 ESP32-C5 */
    .uart_port = UART_NUM_0,
    .tx_pin = GPIO_NUM_43,
    .rx_pin = GPIO_NUM_44,
    .reset_pin = GPIO_NUM_41,  /* 按目标切换 */
    .boot_pin = GPIO_NUM_6,
    .baud_connect = 115200,
    .baud_flash = 921600,
    .target_flash_size = 0,
};
/* prepare 内：gpio_set_level(A0/A1) 选通模拟开关 + uart_driver_delete */
```

### 8.3 idf_component.yml

```yaml
dependencies:
  espressif/esp-serial-flasher: "~2.0.0"
```

组件自身 `idf_component.yml`：

```yaml
version: "0.1.0"
description: "ESP host-to-target firmware flasher wrapper over esp-serial-flasher"
dependencies:
  espressif/esp-serial-flasher: "~2.0.0"
```

---

## 9. 与 OTA / 云升级的边界

| 能力 | 归属 |
|------|------|
| SLIP 连接、擦写、MD5、复位目标 | **esp_target_flasher** |
| HTTP/HTTPS 下载、SHA256 校验 | 工程 `fw_updater` / `ota_manager` |
| LittleFS 路径约定 `/lfs/c2_fw.bin` | 工程 |
| 版本比对、多路顺序烧录策略 | 工程 |
| 云命令协议 | 工程 |
| PC esptool 透明桥接（`c5_bridge_mode`） | 工程可选工具，非组件核心 |

---

## 10. 实施步骤

1. **脚手架**：在 `packages/esp_target_flasher/` 创建 CMakeLists、`idf_component.yml`、空实现。
2. **迁移 `flasher_common.c`**：改名为 `esp_target_flasher.c`，API 对齐 §6，去掉硬编码。
3. **V4PRO 适配**：`c2_flasher.c` / `c5_flasher.c` 改为薄封装，调用 `esp_tf_*`。
4. **example**：`examples/esp_target_flasher_demo/` — P4/S3 Host 给一块 C3 开发板烧 merged-bin（最小引脚配置）。
5. **M1 接入**：实现 `fw_updater` 时直接依赖本组件 + 模拟开关 hook。
6. **文档**：README 补充 Kconfig、依赖 IDF 版本、常见故障（MD5 误报、WDT 超时、BOOT 极性）。

---

## 11. 验证清单

- [ ] **chip 校验**：期望 C2 但 UART 接到 C5 时返回 `ESP_TF_ERR_CHIP_MISMATCH`，不写入 Flash
- [ ] stub 模式：C2 merged-bin ~600KB，MD5 通过，目标正常启动
- [ ] stub 模式：C5 merged-bin ~838KB，文件流式，空闲堆 < 400KB 仍可烧录
- [ ] ROM 分块模式：C5 强制 `rom_chunked=true` 可完成（无 MD5）
- [ ] 共享 BOOT：两路并发调用时互斥锁生效
- [ ] RST/BOOT 反相：`CONFIG_SERIAL_FLASHER_*_INVERT=y` 与手动 gpio_ops 一致
- [ ] 高速 baud：M1 场景 921600 stub 烧录 S3/C5
- [ ] 失败恢复：烧录失败后不 restore UART，日志提示需断电

---

## 12. 参考链接

- [ESP Component Registry — esp-serial-flasher](https://components.espressif.com/components/espressif/esp-serial-flasher)
- [esp-serial-flasher v1.11.0 Changelog](https://components.espressif.com/components/espressif/esp-serial-flasher/versions/1.11.0/changelog?language=en)
- [esp-serial-flasher GitHub](https://github.com/espressif/esp-serial-flasher)
- [esp-serial-flasher v2 Migration Guide](https://github.com/espressif/esp-serial-flasher/blob/master/docs/migration-v1-to-v2.md)
- [esptool 文档（协议原理）](https://docs.espressif.com/projects/esptool/en/latest/esp32/)
- V4PRO 实机源码：`D:\my\rid\v4pro\p4\main\flasher\`
- M1 方案：`D:\work\vehicleDetector\docs\M1固件升级方案.md`
