# cellular_modem

USB RNDIS 蜂窝模组驱动组件（ESP-IDF / ESP32-S2 · S3 · P4）。

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
  mkyx-001/cellular_modem: "^1.1.0"
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
        .netif_name = "cell_rndis",   // esp_netif 名称（同时作为 if_key 和 if_desc）
        .route_priority = 50,         // 默认路由优先级，越大越优先
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

仓库根目录提供最小可运行例程：

- `examples/cellular_modem_basic`

运行方式：

```bash
cd examples/cellular_modem_basic
idf.py set-target esp32p4
idf.py build flash monitor
```

如你的复位引脚不是 GPIO22，可在 `examples/cellular_modem_basic/main/main.c` 中修改
`CELL_MODEM_RST_GPIO`。

## 主要 API

- `cell_modem_init()`: 初始化组件并启动后台监控
- `cell_modem_deinit()`: 反初始化并释放资源
- `cell_modem_get_state()`: 查询当前状态
- `cell_modem_get_net_info()`: 获取 RNDIS IP/网关等信息
- `cell_modem_is_connected()`: 是否已获得本地 IP
- `cell_modem_is_pdp_ready()`: 是否蜂窝数据面就绪
- `cell_modem_wait_for_pdp_ready()`: 阻塞等待就绪
- `cell_modem_register_status_callback()`: 注册状态回调。回调会在状态变化 **或** PDP 激活完成（state 未变也会触发）时被调用，订阅者应结合 `cell_modem_is_pdp_ready()` 综合判断就绪。
- `cell_modem_get_signal_strength()`: 获取 CSQ 信号值
- `cell_modem_is_data_path_ok()`: 数据面是否已验证可达（TCP/ICMP 探针通过，区别于 `is_pdp_ready()` 的 AT 拨号成功）

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
  - 用 `cell_modem_is_data_path_ok()` 判断真实上下行可达（区别于 `cell_modem_is_pdp_ready()`）

## 已知限制（Known Limitations）

- **SIM 卡热插拔检测仅复合模式支持**
  - `auto_configure_rndis=false`（复合模式，RNDIS + AT CDC 共存）时，monitor 任务会周期发 `AT+CPIN?` 复检 SIM 在位状态，连续 3 次失败才认定 SIM 丢失（避免小区切换瞬态误报）。
  - `auto_configure_rndis=true`（纯 RNDIS 模式）时，模式切换后 AT CDC 接口消失，CPIN 与 CSQ 周期查询均不可用。此模式下 SIM 拔插需通过硬复位或上电重启感知。
- **信号丢失但 RNDIS 链路未断时的自愈**
  - 数据面诊断（`enable_diag=true` + `diag_tcp_host` 配置）启用后，diag 任务会在首次通过后以 60s 周期复查数据面；复查失败时降级状态并触发 4G 看门狗硬复位，信号恢复后自动重拨。
  - 未启用数据面诊断时，信号差但 RNDIS 链路未断且 DHCP 租约仍存的场景下，组件无法感知数据面失效，需外部重启或开启诊断。
- **看门狗触发硬复位会断开当前所有 TCP 连接**：30s 无连接即复位模组，量产场景需评估上层业务的断连重连能力。

## 版本发布建议

- 使用 SemVer 版本号（`major.minor.patch`）
- 每次发布前更新 `idf_component.yml` 中的 `version`
- 上传命令：

```bash
compote component upload --namespace mkyx-001 --name cellular_modem
```

## License

Apache-2.0，见 `LICENSE`。
