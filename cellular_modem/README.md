# cellular_modem

USB RNDIS 蜂窝模组驱动组件（ESP-IDF / ESP32-P4）。

该组件提供：
- RNDIS 数据面接入（`esp_netif`）
- CDC AT 控制面（自动拨号、APN 识别、状态监控）
- 连接看门狗与自动恢复逻辑
- 可选数据面诊断（DNS/ICMP/TCP）

> 当前默认参数针对常见 USB 蜂窝模组场景，USB VID/PID 可通过配置注入，便于跨项目复用。

## 目录结构

```text
cellular_modem/
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── cellular_modem.c
└── include/
    └── cellular_modem.h
```

## 依赖

- ESP-IDF `>= 5.0`
- `espressif/usb`
- `espressif/iot_usbh_rndis`

`idf_component.yml` 已声明依赖，使用组件管理器时会自动拉取。

## 快速开始

### 1) 在项目中添加依赖

如果你从 ESP Component Registry 引用：

```yaml
dependencies:
  mkyx-001/cellular_modem: "^1.0.0"
```

### 2) 初始化与拨号

```c
#include "cellular_modem.h"

void app_main(void)
{
    cell_modem_hw_config_t hw = {
        .rst_gpio = GPIO_NUM_22,
        .usb_peripheral_map = BIT1,   // ESP32-P4 FS PHY 常见配置
        .usb_vid = 0,                 // 0 = 使用组件默认值
        .usb_pid_rndis = 0,           // 0 = 使用组件默认值
        .usb_pid_composite = 0,       // 0 = 使用组件默认值
    };

    cell_modem_config_t cfg = {
        .at_interface_num = 2,
        .auto_configure_rndis = false,
        .auto_activate_pdp = true,
        .apn = NULL,                  // NULL = 自动识别运营商 APN
        .enable_diag = true,
        .diag_tcp_host = NULL,        // 可选，如 "example.com"
        .diag_tcp_port = 443,
    };

    // 可选：提前硬复位，加快上电收敛
    cell_modem_early_hardware_reset(hw.rst_gpio);

    ESP_ERROR_CHECK(cell_modem_init(&hw, &cfg));

    // 阻塞等待蜂窝数据就绪
    if (cell_modem_wait_for_pdp_ready(120000)) {
        // Ready
    } else {
        // Timeout
    }
}
```

## 例程

仓库内提供最小可运行例程：
- `examples/basic`

运行方式：

```bash
cd examples/basic
idf.py set-target esp32p4
idf.py build flash monitor
```

如你的复位引脚不是 GPIO22，可在 `examples/basic/main/main.c` 中修改
`CELL_MODEM_RST_GPIO`。

## 主要 API

- `cell_modem_init()`: 初始化组件并启动后台监控
- `cell_modem_deinit()`: 反初始化并释放资源
- `cell_modem_get_state()`: 查询当前状态
- `cell_modem_get_net_info()`: 获取 RNDIS IP/网关等信息
- `cell_modem_is_connected()`: 是否已获得本地 IP
- `cell_modem_is_pdp_ready()`: 是否蜂窝数据面就绪
- `cell_modem_wait_for_pdp_ready()`: 阻塞等待就绪
- `cell_modem_register_status_callback()`: 注册状态回调
- `cell_modem_get_signal_strength()`: 获取 CSQ 信号值

详细类型与注释见 `include/cellular_modem.h`。

## Kconfig

菜单路径：
- `Cellular Modem (USB RNDIS)`

参数：
- `CELL_MODEM_DEFAULT_APN`
  - 当 IMSI/COPS 无法识别运营商时的默认 APN（默认 `3gnet`）

## 常见问题

- 启动后长时间未就绪：
  - 检查 USB 供电与数据线
  - 检查 `rst_gpio` 和模组复位时序
  - 检查 SIM 卡状态和天线
  - 按需显式设置 `cfg.apn`
- RNDIS 有 IP 但业务不通：
  - 打开 `enable_diag`
  - 配置 `diag_tcp_host` 验证业务目标连通性

## 版本发布建议

- 使用 SemVer 版本号（`major.minor.patch`）
- 每次发布前更新 `idf_component.yml` 中的 `version`
- 上传命令：

```bash
compote component upload --namespace mkyx-001 --name cellular_modem
```

## License

Apache-2.0，见 `LICENSE`。
