/**
 * @file cellular_modem.h
 * @brief USB RNDIS 蜂窝模组驱动（RNDIS 数据面 + CDC AT 控制面）
 *
 * ESP32-P4 USB FS Host 连接蜂窝模组，RNDIS 承载数据、CDC 承载 AT 拨号。
 * 板级引脚、USB PHY 与 USB VID/PID 由产品在 init 时通过 cell_modem_hw_config_t 注入。
 */

#ifndef CELLULAR_MODEM_H
#define CELLULAR_MODEM_H

#include "esp_err.h"
#include "esp_netif_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 状态枚举 ======================== */

typedef enum {
    CELL_MODEM_STATE_DISCONNECTED,   /**< 未连接 */
    CELL_MODEM_STATE_CONFIGURING,    /**< AT 指令配置中 */
    CELL_MODEM_STATE_CONNECTING,     /**< 拨号/连接中 */
    CELL_MODEM_STATE_CONNECTED,      /**< RNDIS 链路 Up */
    CELL_MODEM_STATE_GOT_IP,         /**< RNDIS 本地 IP（≠ 蜂窝数据就绪） */
    CELL_MODEM_STATE_ERROR,          /**< 错误状态 */
} cell_modem_state_t;

/* ======================== 网络信息 ======================== */

typedef struct {
    bool    connected;
    char    ip[16];
    char    netmask[16];
    char    gateway[16];
} cell_modem_net_info_t;

/* ======================== 板级硬件配置（产品注入） ======================== */

typedef struct {
    int         rst_gpio;              /**< 模组复位 GPIO；-1=无硬件复位 */
    uint32_t    usb_peripheral_map;    /**< USB Host peripheral_map，P4 FS PHY 通常为 BIT1 */
    uint16_t    usb_vid;               /**< USB Vendor ID；0=使用组件内置默认值 */
    uint16_t    usb_pid_rndis;         /**< 纯 RNDIS 模式 PID；0=使用组件内置默认值 */
    uint16_t    usb_pid_composite;     /**< RNDIS+AT 复合模式 PID；0=使用组件内置默认值 */
} cell_modem_hw_config_t;

/* ======================== 模组行为配置 ======================== */

typedef struct {
    int         at_interface_num;       /**< AT CDC 接口号；-1=自动扫描 */
    bool        auto_configure_rndis;     /**< 自动发 MDIALUPCFG 切 RNDIS（量产通常 false） */
    bool        auto_activate_pdp;        /**< 自动 boot + 拨号激活 PDP */
    const char *apn;                      /**< NULL/空=IMSI/COPS 自动；非空=强制 APN */
    bool        enable_diag;              /**< GOT_IP 且 pdp_done 后 TCP/ICMP 诊断 */
    const char *diag_tcp_host;            /**< 诊断 TCP 主机；NULL=跳过 TCP 探针 */
    uint16_t    diag_tcp_port;            /**< 诊断 TCP 端口，默认 443 */
    /* ======================== netif 接入 ======================== */
    const char *netif_name;             /**< esp_netif 名称（同时作为 if_key 与 if_desc）；NULL=使用 "cell_rndis" */
    int         route_priority;         /**< 默认路由优先级；0=使用内置默认 50，越大越优先 */
} cell_modem_config_t;

/* ======================== 回调 ======================== */

typedef void (*cell_modem_status_callback_t)(cell_modem_state_t state);

/* ======================== API ======================== */

/**
 * @brief 设备上电早期硬复位模组（须在 cell_modem_init 之前调用）
 *
 * 建议在 app_main 的 gpio_init 之后、USB 初始化之前调用，
 * 与以太网 PHY 启动并行，为模组争取 RF/USB 就绪时间。
 * 若已调用本函数，cell_modem_init 内不再重复硬复位。
 *
 * @param rst_gpio 复位引脚；<0 跳过
 */
esp_err_t cell_modem_early_hardware_reset(int rst_gpio);

/**
 * @brief 初始化蜂窝模组（非阻塞，后台 monitor 完成拨号）
 * @param hw  板级硬件配置（不可为 NULL）
 * @param cfg 行为配置（NULL=使用默认值）
 * @return ESP_OK 成功
 */
esp_err_t cell_modem_init(const cell_modem_hw_config_t *hw, const cell_modem_config_t *cfg);

/**
 * @brief 反初始化蜂窝模组
 */
esp_err_t cell_modem_deinit(void);

/**
 * @brief 获取当前状态
 */
cell_modem_state_t cell_modem_get_state(void);

/**
 * @brief 获取 RNDIS 网络信息
 */
esp_err_t cell_modem_get_net_info(cell_modem_net_info_t *info);

/**
 * @brief 注册状态变更回调
 *
 * 回调时机：
 *   - cell_modem_state_t 发生状态转移时（如 CONNECTING → GOT_IP）；
 *   - 蜂窝 PDP 上下文激活完成时，即便 state 未变（RNDIS 链路先 Up 时常见），
 *     回调会被再次触发，以便上层订阅者重新评估就绪状态。
 *
 * 因此订阅者**不应**假设「回调触发 = state 变了」；正确做法是同时
 * 查询 cell_modem_is_pdp_ready() / cell_modem_is_data_path_ok()，以
 * (state, pdp_ready, data_path_ok) 三元组判断就绪。
 */
void cell_modem_register_status_callback(cell_modem_status_callback_t callback);

/**
 * @brief 获取蜂窝模组的 esp_netif 句柄
 */
esp_netif_t *cell_modem_get_netif(void);

/**
 * @brief RNDIS 本地 IP 是否已分配（不代表蜂窝数据可用）
 */
bool cell_modem_is_connected(void);

/**
 * @brief 蜂窝数据是否就绪（RNDIS IP + 拨号成功 + AT 任务未占用）
 */
bool cell_modem_is_pdp_ready(void);

/**
 * @brief 数据面是否已验证可用（TCP 云探针或 ICMP ping 通过）
 *
 * 与 cell_modem_is_pdp_ready() 的区别：后者只确认 AT 拨号成功，
 * 本函数确认真实上下行可达（DNS 解析 + TCP 连云或 ICMP 收到回包）。
 * 需在 cell_modem_config_t 中启用 enable_diag 并配置 diag_tcp_host
 * 才会触发探针；未启用诊断时本函数恒为 false。
 */
bool cell_modem_is_data_path_ok(void);

/**
 * @brief 阻塞等待蜂窝数据就绪
 * @param timeout_ms 最大等待毫秒；0=无限等待
 * @return true 就绪；false 超时（仅 timeout_ms>0）
 */
bool cell_modem_wait_for_pdp_ready(uint32_t timeout_ms);

/**
 * @brief 获取 CSQ 信号强度
 * @return 0-31，-1 表示未知
 */
int cell_modem_get_signal_strength(void);

#ifdef __cplusplus
}
#endif

#endif // CELLULAR_MODEM_H
