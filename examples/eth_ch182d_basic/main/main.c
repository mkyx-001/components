#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "eth_ch182d.h"

/*
 * 板级配置：产品必须显式给出 RMII / SMI 引脚、PHY 地址、复位引脚与接口名字。
 * eth_ch182d 组件仅在调用方未覆盖时使用 ETH_CH182D_CONFIG_DEFAULT() 兜底，
 * 但例程作为产品模板，应显式列出所有板级参数，便于移植时一目了然。
 *
 * 以下数值针对 ESP32-P4 + CH182D 参考板（与 rid/v4pro/p4 的 board_config.h 一致）：
 *   - REFCLK 由 CH182D 输出 50MHz，接到 P4 GPIO44（组件内固定为 EXT_IN 模式）
 *   - PHY 地址 3（由 CH182D 的 PA0/PA1 决定）
 *   - PHY 硬件复位：GPIO30（低有效）
 *   - 接口名字 "ETH0"，路由优先级 60
 */
#ifndef ETH_CH182D_IF_NAME
#define ETH_CH182D_IF_NAME      "ETH0"
#endif
#ifndef ETH_CH182D_RMII_TXD0
#define ETH_CH182D_RMII_TXD0    GPIO_NUM_41
#endif
#ifndef ETH_CH182D_RMII_TXD1
#define ETH_CH182D_RMII_TXD1    GPIO_NUM_42
#endif
#ifndef ETH_CH182D_RMII_TX_EN
#define ETH_CH182D_RMII_TX_EN   GPIO_NUM_40
#endif
#ifndef ETH_CH182D_RMII_RXD0
#define ETH_CH182D_RMII_RXD0    GPIO_NUM_46
#endif
#ifndef ETH_CH182D_RMII_RXD1
#define ETH_CH182D_RMII_RXD1    GPIO_NUM_47
#endif
#ifndef ETH_CH182D_RMII_CRS_DV
#define ETH_CH182D_RMII_CRS_DV  GPIO_NUM_45
#endif
#ifndef ETH_CH182D_RMII_CLK
#define ETH_CH182D_RMII_CLK     GPIO_NUM_44
#endif
#ifndef ETH_CH182D_MDC
#define ETH_CH182D_MDC          GPIO_NUM_52
#endif
#ifndef ETH_CH182D_MDIO
#define ETH_CH182D_MDIO         GPIO_NUM_53
#endif
#ifndef ETH_CH182D_PHY_RESET
#define ETH_CH182D_PHY_RESET    GPIO_NUM_30
#endif
#ifndef ETH_CH182D_PHY_ADDR
#define ETH_CH182D_PHY_ADDR     3
#endif
#ifndef ETH_CH182D_ROUTE_PRIO
#define ETH_CH182D_ROUTE_PRIO   60
#endif

/*
 * 静态 IP（仅当通过 menuconfig 开启 CONFIG_ETH_CH182D_USE_STATIC_IP 时生效）。
 * 默认 DHCP 模式下不会用到。
 */
#ifndef ETH_CH182D_STATIC_IP
#define ETH_CH182D_STATIC_IP        "192.168.1.100"
#endif
#ifndef ETH_CH182D_STATIC_NETMASK
#define ETH_CH182D_STATIC_NETMASK   "255.255.255.0"
#endif
#ifndef ETH_CH182D_STATIC_GATEWAY
#define ETH_CH182D_STATIC_GATEWAY   "192.168.1.1"
#endif

static const char *TAG = "eth_ch182d_example";

void app_main(void)
{
    /* 显式构造板级配置（不依赖组件默认宏），产品模板一目了然 */
    eth_ch182d_config_t cfg = {
        .name        = ETH_CH182D_IF_NAME,
        .rmii_txd0   = ETH_CH182D_RMII_TXD0,
        .rmii_txd1   = ETH_CH182D_RMII_TXD1,
        .rmii_tx_en  = ETH_CH182D_RMII_TX_EN,
        .rmii_rxd0   = ETH_CH182D_RMII_RXD0,
        .rmii_rxd1   = ETH_CH182D_RMII_RXD1,
        .rmii_crs_dv = ETH_CH182D_RMII_CRS_DV,
        .rmii_clk    = ETH_CH182D_RMII_CLK,
        .mdc         = ETH_CH182D_MDC,
        .mdio        = ETH_CH182D_MDIO,
        .phy_reset   = ETH_CH182D_PHY_RESET,
        .phy_addr    = ETH_CH182D_PHY_ADDR,
        .route_prio  = ETH_CH182D_ROUTE_PRIO,
    };

    ESP_LOGI(TAG, "eth_ch182d basic example start (PHY addr=%d, route_prio=%d)",
             cfg.phy_addr, cfg.route_prio);

    /* 初始化以太网：内部已安装驱动 + 创建 netif + 启动 DHCP（默认） */
    esp_err_t ret = eth_ch182d_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CH182D Ethernet init failed: %s (PHY may not be present)",
                 esp_err_to_name(ret));
        /* 例程不 abort：允许在没接 PHY 的板子上 build/启动做冒烟验证 */
    }

#ifdef CONFIG_ETH_CH182D_USE_STATIC_IP
    /* 启动时切静态 IP（默认 DHCP 模式不会进入此分支） */
    if (eth_ch182d_apply_static_ip(ETH_CH182D_STATIC_IP,
                                   ETH_CH182D_STATIC_NETMASK,
                                   ETH_CH182D_STATIC_GATEWAY) != ESP_OK) {
        ESP_LOGW(TAG, "apply static IP failed, fallback to DHCP");
        eth_ch182d_enable_dhcp();
    }
#endif

    /* 主循环：周期打印链路 / IP / DHCP 状态，演示分层判读 */
    while (1) {
        esp_netif_t *netif = eth_ch182d_get_netif();
        esp_netif_ip_info_t ip = {0};
        bool has_ip = (netif != NULL) &&
                      (esp_netif_get_ip_info(netif, &ip) == ESP_OK) &&
                      (ip.ip.addr != 0);

        ESP_LOGI(TAG, "link=%s ip=%s dhcp=%s",
                 eth_ch182d_is_connected() ? "UP" : "DOWN",
                 has_ip ? ip4addr_ntoa((const ip4_addr_t *)&ip.ip) : "-",
                 eth_ch182d_is_dhcp_enabled() ? "ON" : "OFF");

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
