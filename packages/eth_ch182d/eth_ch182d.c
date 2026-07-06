/**
 * @file eth_ch182d.c
 * @brief CH182D 以太网驱动实现（ESP32-P4 内置 EMAC + CH182D PHY，RMII）
 *
 * CH182D 输出 50MHz REFCLK → ESP32-P4 RMII 时钟输入。
 * 使用 sergeykharenko/ch182 组件的 PHY 驱动。
 *
 * 所有引脚 / PHY 地址 / 接口名字均由调用方通过 eth_ch182d_config_t 提供。
 */

#include "eth_ch182d.h"

#include "esp_log.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy_ch182.h"
#include "esp_event.h"
#include "esp_netif.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <arpa/inet.h>
#include <sys/socket.h>

/* ======================== 常量 ======================== */

#define ETH_START_DONE_BIT      (1 << 0)
#define ETH_GOT_IP_BIT          (1 << 1)

/* ======================== 模块状态 ======================== */

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static EventGroupHandle_t s_eth_event_group = NULL;
static bool s_eth_connected = false;
/* 运行时日志 TAG，由 eth_ch182d_init() 用配置中的 name 设置 */
static const char *s_eth_tag = "ETH_CH182D";

/* ======================== 网络事件回调 ======================== */

/**
 * @brief 以太网事件处理函数
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(s_eth_tag, "Ethernet link up — MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(s_eth_tag, "Ethernet link down");
        s_eth_connected = false;
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(s_eth_tag, "Ethernet driver started");
        if (s_eth_event_group) {
            xEventGroupSetBits(s_eth_event_group, ETH_START_DONE_BIT);
        }
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(s_eth_tag, "Ethernet driver stopped");
        break;

    default:
        break;
    }
}

/**
 * @brief IP 事件处理函数 — 获取 IP 后标记连接就绪
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    /* 只处理 CH182D 自身 netif 的 IP 事件，忽略其他网口（如 4G RNDIS） */
    if (s_eth_netif && event->esp_netif != s_eth_netif) {
        return;
    }

    ESP_LOGI(s_eth_tag, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(s_eth_tag, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    ESP_LOGI(s_eth_tag, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    s_eth_connected = true;
    if (s_eth_event_group) {
        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }
}

/* ======================== 公共接口 ======================== */

esp_err_t eth_ch182d_init(const eth_ch182d_config_t *cfg)
{
    if (cfg == NULL || cfg->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_eth_tag = cfg->name;

    /* 创建事件组 */
    s_eth_event_group = xEventGroupCreate();

    /* 注册事件处理器 */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                &ip_event_handler, NULL));

    /* ---- 配置 ESP32-P4 内置 EMAC（RMII 引脚） ---- */
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    // SMI 管理接口
    emac_config.smi_gpio.mdc_num = cfg->mdc;
    emac_config.smi_gpio.mdio_num = cfg->mdio;

    // RMII 数据信号
    emac_config.emac_dataif_gpio.rmii.tx_en_num = cfg->rmii_tx_en;
    emac_config.emac_dataif_gpio.rmii.txd0_num  = cfg->rmii_txd0;
    emac_config.emac_dataif_gpio.rmii.txd1_num  = cfg->rmii_txd1;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = cfg->rmii_crs_dv;
    emac_config.emac_dataif_gpio.rmii.rxd0_num  = cfg->rmii_rxd0;
    emac_config.emac_dataif_gpio.rmii.rxd1_num  = cfg->rmii_rxd1;

    // RMII 时钟：CH182D 输出 50MHz REFCLK
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = cfg->rmii_clk;

    /* ---- 配置 MAC ---- */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    /* ---- 配置 PHY ---- */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = cfg->phy_addr;
    phy_config.reset_gpio_num = cfg->phy_reset;
    phy_config.reset_timeout_ms = 1000;
    phy_config.autonego_timeout_ms = 5000;
    phy_config.post_hw_reset_delay_ms = 50;

    /* ---- 创建 CH182D PHY 实例（CH182D 输出 REFCLK） ---- */
    esp_eth_phy_t *phy = esp_eth_phy_new_ch182(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(s_eth_tag, "Failed to create CH182D PHY");
        return ESP_FAIL;
    }

    /* ---- 创建 MAC 实例 ---- */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(s_eth_tag, "Failed to create ESP32 EMAC");
        phy->del(phy);
        return ESP_FAIL;
    }

    /* ---- 安装以太网驱动 ---- */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(s_eth_tag, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        mac->del(mac);
        phy->del(phy);
        return ret;
    }

    /* 启用流量控制 */
    bool flow_ctrl = true;
    esp_eth_ioctl(s_eth_handle, ETH_CMD_S_FLOW_CTRL, &flow_ctrl);

    /* ---- 创建 esp_netif ---- */
    esp_netif_inherent_config_t netif_base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    netif_base.if_key = cfg->name;
    netif_base.if_desc = cfg->name;
    netif_base.route_prio = cfg->route_prio;

    esp_netif_config_t netif_cfg = {
        .base = &netif_base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_eth_netif = esp_netif_new(&netif_cfg);
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));

    /* ---- 启动以太网 ---- */
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(s_eth_tag, "Ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(s_eth_tag, "CH182D Ethernet initialized (name=%s, PHY addr=%d, route_prio=%d)",
             cfg->name, cfg->phy_addr, cfg->route_prio);
    return ESP_OK;
}

esp_netif_t *eth_ch182d_get_netif(void)
{
    return s_eth_netif;
}

esp_eth_handle_t eth_ch182d_get_handle(void)
{
    return s_eth_handle;
}

esp_err_t eth_ch182d_enable_dhcp(void)
{
    if (s_eth_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(s_eth_tag, "Switching to DHCP mode");
    /* 静态 IP 切换回 DHCP 须先 stop 再 start，否则 esp_netif 可能仍为 STOPPED */
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_err_t ret = esp_netif_dhcpc_start(s_eth_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(s_eth_tag, "DHCP client start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

bool eth_ch182d_is_dhcp_enabled(void)
{
    if (s_eth_netif == NULL) {
        return false;
    }
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    if (esp_netif_dhcpc_get_status(s_eth_netif, &status) != ESP_OK) {
        return false;
    }
    return status == ESP_NETIF_DHCP_STARTED;
}

esp_err_t eth_ch182d_apply_static_ip(const char *ip, const char *netmask, const char *gateway)
{
    if (s_eth_netif == NULL || ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(s_eth_tag, "Applying static IP: %s/%s gw %s", ip, netmask, gateway);

    /* 先停止 DHCP */
    esp_netif_dhcpc_stop(s_eth_netif);

    esp_netif_ip_info_t ip_info = {0};
    if (inet_pton(AF_INET, ip, &ip_info.ip.addr) != 1) {
        ESP_LOGE(s_eth_tag, "Invalid IP address: %s", ip);
        return ESP_ERR_INVALID_ARG;
    }
    if (netmask && inet_pton(AF_INET, netmask, &ip_info.netmask.addr) != 1) {
        ESP_LOGE(s_eth_tag, "Invalid netmask: %s", netmask);
        return ESP_ERR_INVALID_ARG;
    }
    if (gateway && inet_pton(AF_INET, gateway, &ip_info.gw.addr) != 1) {
        ESP_LOGE(s_eth_tag, "Invalid gateway: %s", gateway);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_eth_netif, &ip_info));
    return ESP_OK;
}

bool eth_ch182d_is_connected(void)
{
    return s_eth_connected;
}
