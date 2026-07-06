/**
 * @file eth_ch182d.h
 * @brief CH182D 以太网驱动接口（ESP32-P4 内置 EMAC + CH182D PHY，RMII）
 *
 * 引脚 / PHY 地址 / 接口名字均通过 eth_ch182d_config_t 在运行时配置，
 * 组件不依赖任何外部 board_config.h，可直接被多个项目共享。
 */

#ifndef ETH_CH182D_H
#define ETH_CH182D_H

#include "esp_err.h"
#include "esp_eth_driver.h"
#include "esp_netif_types.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CH182D 以太网配置
 *
 * 由调用方填充后传入 eth_ch182d_init()。可使用
 * ETH_CH182D_CONFIG_DEFAULT() 获取 ESP32-P4 当前板子的便利默认值，
 * 再按需覆盖个别字段（如 name）。
 */
typedef struct {
    const char *name;        /*!< 接口名字，如 "ETH0"/"ETH1"
                                  → esp_netif 的 if_key / if_desc，亦作日志 TAG */

    /* RMII 数据信号 */
    gpio_num_t rmii_txd0;    /*!< RMII TXD0 */
    gpio_num_t rmii_txd1;    /*!< RMII TXD1 */
    gpio_num_t rmii_tx_en;   /*!< RMII TX_EN */
    gpio_num_t rmii_rxd0;    /*!< RMII RXD0 */
    gpio_num_t rmii_rxd1;    /*!< RMII RXD1 */
    gpio_num_t rmii_crs_dv;  /*!< RMII CRS_DV */
    gpio_num_t rmii_clk;     /*!< RMII REFCLK 输入（CH182D 输出 50MHz） */

    /* SMI 管理接口 */
    gpio_num_t mdc;          /*!< MDC */
    gpio_num_t mdio;         /*!< MDIO */

    /* PHY */
    gpio_num_t phy_reset;    /*!< PHY 复位引脚，设为 -1 表示不使用硬件复位 */
    uint32_t    phy_addr;    /*!< PHY 地址（由 PHY 的 PA0/PA1 等引脚决定） */
    uint32_t    route_prio;  /*!< esp_netif 路由优先级 */
} eth_ch182d_config_t;

/**
 * @brief 便利默认配置
 *
 * 默认值对应 ESP32-P4 + CH182D 的某块参考板。其他板子请按需覆盖，例如：
 *
 * @code
 * eth_ch182d_config_t cfg = ETH_CH182D_CONFIG_DEFAULT();
 * cfg.name = "ETH1";
 * cfg.phy_addr = 1;
 * eth_ch182d_init(&cfg);
 * @endcode
 */
#define ETH_CH182D_CONFIG_DEFAULT() { \
    .name       = "ETH0",            \
    .rmii_txd0  = GPIO_NUM_41,       \
    .rmii_txd1  = GPIO_NUM_42,       \
    .rmii_tx_en = GPIO_NUM_40,       \
    .rmii_rxd0  = GPIO_NUM_46,       \
    .rmii_rxd1  = GPIO_NUM_47,       \
    .rmii_crs_dv= GPIO_NUM_45,       \
    .rmii_clk   = GPIO_NUM_44,       \
    .mdc        = GPIO_NUM_52,       \
    .mdio       = GPIO_NUM_53,       \
    .phy_reset  = GPIO_NUM_30,       \
    .phy_addr   = 3,                 \
    .route_prio = 60 }

/**
 * @brief 初始化 CH182D 以太网（RMII + 内置 EMAC）
 *
 * 流程：
 *   1. 按 cfg 配置 RMII / SMI 引脚和 PHY 地址
 *   2. 创建内置 EMAC + CH182D PHY
 *   3. 安装以太网驱动
 *   4. 创建 esp_netif，路由优先级取自 cfg->route_prio
 *   5. 注册网络事件处理
 *   6. 启动 DHCP（默认）
 *
 * @param cfg 配置结构体，字段见 eth_ch182d_config_t
 * @return ESP_OK 成功；其他表示失败
 */
esp_err_t eth_ch182d_init(const eth_ch182d_config_t *cfg);

/**
 * @brief 获取 CH182D 以太网的 esp_netif 句柄
 * @return netif 句柄，未初始化返回 NULL
 */
esp_netif_t *eth_ch182d_get_netif(void);

/**
 * @brief 获取以太网驱动句柄
 * @return 以太网驱动句柄，未初始化返回 NULL
 */
esp_eth_handle_t eth_ch182d_get_handle(void);

/**
 * @brief 启用 DHCP 模式
 * @return ESP_OK 成功
 */
esp_err_t eth_ch182d_enable_dhcp(void);

/**
 * @brief 查询以太网接口当前 DHCP 是否启用（运行时实际状态，非配置意图）
 *
 * 供心跳上报 net.dhcp 字段使用——上报设备此刻实际生效的 DHCP 模式，
 * 而非调用方配置意图（二者在切换瞬间可能不同步）。
 *
 * @return true DHCP 已启动；false 已停止或 netif 未就绪
 */
bool eth_ch182d_is_dhcp_enabled(void);

/**
 * @brief 应用静态 IP 配置
 * @param ip      IP 地址字符串（如 "192.168.1.100"）
 * @param netmask 子网掩码字符串
 * @param gateway 网关字符串
 * @return ESP_OK 成功
 */
esp_err_t eth_ch182d_apply_static_ip(const char *ip, const char *netmask, const char *gateway);

/**
 * @brief 检查以太网是否已获取 IP
 * @return true 已获取 IP
 */
bool eth_ch182d_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // ETH_CH182D_H
