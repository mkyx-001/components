# eth_ch182d

CH182D 以太网驱动组件（ESP-IDF / ESP32 · ESP32-P4）。

该组件在 ESP32 / ESP32-P4 内置 EMAC（RMII 模式）上驱动 **CH182D** PHY 收发器，提供：

- 一行 `eth_ch182d_init(&cfg)` 拉起完整以太网网口（EMAC + PHY + `esp_netif` + 事件）
- DHCP / 静态 IP 运行时切换
- 链路 / IP 就绪状态查询
- 引脚、PHY 地址、接口名字（`ETH0`/`ETH1`…）全部运行时可配置，组件不依赖任何外部 `board_config.h`

> 底层 PHY 寄存器级驱动由上游组件 [`sergeykharenko/ch182`](https://components.espressif.com/components/sergeykharenko/ch182) 提供（提供 `esp_eth_phy_new_ch182()`），本组件通过 `idf_component.yml` 自动拉取。

## 硬件拓扑

```text
CH182D PHY ──RMII──┐
   │               │
   │  50MHz REFCLK ├──► ESP32 / ESP32-P4 EMAC（内置，RMII）
   │               │
   └── SMI(MDC/MDIO)──►
```

- **MAC**：ESP32 / ESP32-P4 内置 EMAC，RMII 模式，REFCLK 由 CH182D 输出（外部时钟输入 `EMAC_CLK_EXT_IN`）。
- **PHY**：CH182D，PHY 地址由其 PA0/PA1 等引脚决定。
- **⚠ ESP32（原始）限制**：外部 REFCLK 必须固定在 GPIO0，其余 RMII 引脚也需按板子调整。

## 目录结构

```text
eth_ch182d/
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── eth_ch182d.c
├── LICENSE
└── include/
    └── eth_ch182d.h
```

## 依赖

- ESP-IDF `>= 5.0`
- `sergeykharenko/ch182`（CH182 PHY 驱动）

`idf_component.yml` 已声明依赖，使用组件管理器时会自动拉取。

## 配置

所有引脚 / PHY 地址 / 接口名字均通过 `eth_ch182d_config_t` 在**运行时**传入：

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | `const char *` | 接口名字，如 `"ETH0"` / `"ETH1"`。同时作为 `esp_netif` 的 `if_key` / `if_desc` 和日志 TAG |
| `rmii_txd0` / `rmii_txd1` | `gpio_num_t` | RMII 发送数据 |
| `rmii_tx_en` | `gpio_num_t` | RMII 发送使能 |
| `rmii_rxd0` / `rmii_rxd1` | `gpio_num_t` | RMII 接收数据 |
| `rmii_crs_dv` | `gpio_num_t` | RMII CRS_DV |
| `rmii_clk` | `gpio_num_t` | RMII REFCLK 输入引脚（CH182D 输出 50MHz） |
| `mdc` / `mdio` | `gpio_num_t` | SMI 管理接口 |
| `phy_reset` | `gpio_num_t` | PHY 硬件复位引脚；填 `-1` 表示不使用硬件复位 |
| `phy_addr` | `uint32_t` | PHY 地址（由 PHY 的 PA0/PA1 等引脚决定） |
| `route_prio` | `uint32_t` | `esp_netif` 路由优先级（如需让以太网优先于 4G，设高于 4G 的值） |

便利宏 `ETH_CH182D_CONFIG_DEFAULT()` 提供一块参考板的默认值（ESP32-P4 + CH182D，PHY addr=3，route_prio=60），调用方一般只需覆盖 `name`。

## 快速开始

### 1) 在项目中添加依赖

如果你从 ESP Component Registry 引用：

```yaml
dependencies:
  mkyx-001/eth_ch182d: "^0.1.0"
```

或本地路径引用：

```yaml
dependencies:
  eth_ch182d:
    path: ../components/packages/eth_ch182d
```

### 2) 初始化与使用

```c
#include "eth_ch182d.h"

void app_main(void)
{
    eth_ch182d_config_t cfg = ETH_CH182D_CONFIG_DEFAULT();
    cfg.name = "ETH0";   /* 接口名字 / 日志 TAG */

    ESP_ERROR_CHECK(eth_ch182d_init(&cfg));   /* 内部已启动 DHCP */

    /* 运行时切换 IP 模式（典型用于配置下发后） */
    /* eth_ch182d_enable_dhcp(); */
    /* eth_ch182d_apply_static_ip("192.168.1.100", "255.255.255.0", "192.168.1.1"); */
}
```

## 例程

仓库根目录提供最小可运行例程：

- `examples/eth_ch182d_basic`

运行方式：

```bash
cd examples/eth_ch182d_basic
idf.py set-target esp32p4
idf.py build flash monitor
```

如你的引脚与默认值不同，可在 `examples/eth_ch182d_basic/main/main.c` 中覆盖
`ETH_CH182D_*` 宏。

## 主要 API

- `eth_ch182d_init()`: 按 config 初始化以太网（EMAC + PHY + netif + 事件 + 启动 DHCP）
- `eth_ch182d_get_netif()`: 获取底层 `esp_netif` 句柄
- `eth_ch182d_get_handle()`: 获取以太网驱动句柄
- `eth_ch182d_enable_dhcp()`: 切换到 DHCP 模式
- `eth_ch182d_is_dhcp_enabled()`: 查询当前 DHCP 是否实际启动（运行时状态）
- `eth_ch182d_apply_static_ip()`: 应用静态 IP
- `eth_ch182d_is_connected()`: 是否已获取 IP

详细类型与注释见 `include/eth_ch182d.h`。

## Kconfig

菜单路径：`CH182D Ethernet`

- `ETH_CH182D_USE_STATIC_IP`：启动用静态 IP 而非 DHCP（默认 n）
- `ETH_CH182D_STATIC_IP` / `ETH_CH182D_STATIC_NETMASK` / `ETH_CH182D_STATIC_GATEWAY`：
  静态 IP 参数（仅当 `ETH_CH182D_USE_STATIC_IP=y` 出现）

> 这些 Kconfig 项主要为 example 工程便利；产品代码一般直接调用
> `eth_ch182d_apply_static_ip()` 在运行时下发配置。

## 设计说明

- **单实例**：组件内部使用静态全局状态，一套固件只支持一个 CH182D 接口。`name` 字段用于命名单实例（如 `"ETH0"`），便于在多网口（以太网 + 4G RNDIS）固件中区分日志与 netif。若同一固件需两个独立 CH182D 接口（真 ETH0 + ETH1），需另行做 handle 化重构。
- **IP 事件过滤**：`ip_event_handler` 仅处理本接口 netif 的 `IP_EVENT_ETH_GOT_IP`，忽略其他网口（如 4G RNDIS），避免误标记连接就绪。
- **REFCLK**：固定为外部输入模式（`EMAC_CLK_EXT_IN`），即由 CH182D 向 ESP32-P4 提供 50MHz 时钟，无需软件配置 PLL。

## 移植来源

源自 `mbox/firmware components/base/src/ch390lib.c` 的 `eth2_init_task()`，后经 `rid/v4pro/p4` 演化，去除了项目专属的 `board_config.h` 依赖。

## License

Apache-2.0，见 `LICENSE`。
