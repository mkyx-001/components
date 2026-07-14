/**
 * @file cellular_modem.c
 * @brief USB RNDIS 蜂窝模组驱动（AT 控制面 + RNDIS 数据面）
 *
 * 板级引脚与 USB 参数由 cell_modem_hw_config_t 注入，不依赖产品 board_config。
 */

#include "cellular_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "sdkconfig.h"
#include "driver/gpio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

/* USB 驱动头文件 */
#include "usb/usb_host.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_rndis.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "usbh_helper.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "cell_modem";

/* ======================== 常量 ======================== */

#define CELL_MODEM_USB_VID_DEFAULT           0x2ECC
#define CELL_MODEM_USB_PID_RNDIS_DEFAULT     0x3004
#define CELL_MODEM_USB_PID_COMPOSITE_DEFAULT 0x3012

#define CELL_MODEM_NETIF_NAME_DEFAULT  "cell_rndis"
#define CELL_MODEM_ROUTE_PRIORITY      50
#define CELL_MODEM_MTU_DEFAULT         1400   /* RNDIS+蜂窝链路：USB 封装(44B)+蜂窝 MTU 限制，1500 易丢大包 */

#define CELL_MODEM_AT_BUF_SIZE       512
#define CELL_MODEM_MON_INTERVAL_MS   10000

/* 开机 AT 就绪 / 拨号超时 */
#define CELL_MODEM_BOOT_MATREADY_MS      60000
#define CELL_MODEM_BOOT_CPIN_TIMEOUT_MS  5000
#define CELL_MODEM_BOOT_CPIN_RETRIES     3
#define CELL_MODEM_BOOT_CEREG_HW_RESET_MAX 2     /* CEREG 超时后硬复位重试次数（单次上电） */
#define CELL_MODEM_DIAL_MIPCALL_MS       15000   /* MIPCALL=1,1：实测 2s 内出 URC，5s 余量 */
#define CELL_MODEM_DIAL_MDIALUP_MS       30000
/* CGATT/CGACT 兜底（仅 MIPCALL+MDIALUP 主路径都失败时） */
#define CELL_MODEM_CGATT_TIMEOUT_MS      20000   /* 原 45s×2 过长；20s 一次，避免多花时间在信号/卡问题上 */
#define CELL_MODEM_CGATT_RETRY_COUNT     1
/* ① CEREG 驻网轮询（拨号前置条件，见 ml307-at-boot-sequence.md §4a） */
#define CELL_MODEM_BOOT_CEREG_POLL_MS    2000    /* 轮询间隔 */
#define CELL_MODEM_BOOT_CEREG_TOTAL_MS   60000   /* 总超时：stat=1/5 即成功；实测 ML307 5-15s 驻网 */
#define CELL_MODEM_BOOT_CEREG_KICK_MS    15000   /* stat=0 持续 15s 不动则发 CFUN=1+COPS=0 */

/* APN：config.apn 为空时按 IMSI/COPS 自动匹配；未知 PLMN 兜底 */
#define CELL_MODEM_APN_BUF_SIZE          32
#define CELL_MODEM_DEFAULT_APN_FALLBACK  CONFIG_CELL_MODEM_DEFAULT_APN

/* P3 蜂窝数据面诊断（pdp_done 后、绑定 modem netif，周期重试，仅日志） */
#define CELL_MODEM_DIAG_PING_DOMAIN      "www.baidu.com"  /* DNS + ICMP（解析后 IP） */
#define CELL_MODEM_DIAG_PING_COUNT       3
#define CELL_MODEM_DIAG_RETRY_MS         30000            /* 首次未通过时的快速重试间隔 */
#define CELL_MODEM_DIAG_REVERIFY_MS      60000            /* 通过后的慢速周期复查间隔 */
#define CELL_MODEM_DIAG_TCP_TIMEOUT_MS   5000
#define CELL_MODEM_DIAG_TCP_PORT         443              /* 云/OTA 同域 HTTPS */

/*
 * 4G 连接看门狗超时（毫秒）。
 *
 * 如果 modem 在此时间内始终未进入 GOT_IP 状态（从最后一次成功获取 IP
 * 或模块初始化算起），则强制硬件复位 modem 芯片，重新走整个
 * AT 配置 + PDP 激活 + RNDIS 拨号流程。
 *
 * 覆盖场景：基站信号丢失后模块挂死、USB 断连后无法自动恢复、
 * RNDIS 恢复机制 3 次重试耗尽后仍无连接等。
 */
#define CELL_MODEM_WATCHDOG_TIMEOUT_MS   (30 * 1000)  /* 30 秒 */

/*
 * AT 配置任务最大允许持续时间（毫秒）。
 *
 * AT 配置任务包含端口扫描（最多 ~10s）+ 多条 AT 命令（每条 3-15s 超时），
 * 正常情况下 30-60 秒内完成。如果超过此时间仍在运行，视为卡死，
 * 看门狗将强制终止 AT 任务并硬复位 modem。
 */
#define CELL_MODEM_AT_TASK_TIMEOUT_MS    (300 * 1000) /* boot CEREG 120s + 拨号兜底 */

#define CELL_MODEM_USB_FS_PHY_BIT        BIT1
#define EVENT_RNDIS_CONNECTED_BIT  BIT0
#define EVENT_RNDIS_GOT_IP_BIT     BIT1

/* ======================== 全局驱动上下文 ======================== */

static struct {
    cell_modem_hw_config_t       hw;
    cell_modem_config_t          config;
    cell_modem_state_t           state;
    cell_modem_net_info_t        net_info;
    cell_modem_status_callback_t status_cb;

    iot_eth_driver_t           *eth_driver;
    iot_eth_handle_t            eth_handle;
    esp_netif_t                *netif;
    iot_eth_netif_glue_handle_t netif_glue;
    EventGroupHandle_t          event_group;

    usb_device_match_id_t *rndis_match_id_list;

    usbh_cdc_port_handle_t  at_port;
    uint8_t                 dev_addr;
    bool                    cdc_connected;
    bool                    rndis_configured;
    bool                    rndis_ever_connected;
    bool                    pdp_done;
    bool                    at_config_in_progress;
    TaskHandle_t            at_config_task;
    TaskHandle_t            monitor_task;

    SemaphoreHandle_t       at_mutex;

    /* CSQ 信号强度缓存 */
    int                     csq_value;

    /* RNDIS 恢复机制 */
    TickType_t              rndis_config_time;  /* RNDIS 配置完成的 tick */
    int                     recovery_count;     /* RNDIS 恢复重试次数 */

    /* 4G 连接看门狗：长时间无连接则硬复位 */
    TickType_t              last_got_ip_tick;   /* 最后一次 GOT_IP 的时间（0=从未） */
    TickType_t              monitor_start_tick; /* 监控任务启动时间（看门狗基准） */
    int                     watchdog_reset_count; /* 看门狗累计复位次数 */

    /* AT 配置任务超时跟踪 */
    TickType_t              at_config_start_tick; /* AT 任务启动时间 */
} g = {0};

/* 上次成功找到 AT 端口的位置（优先重试） */
static uint8_t s_at_dev_addr = 1;
static int     s_at_itf_num  = 2;

/* RNDIS CONNECTED 时刻（诊断/日志用，驻网等待改由 boot CEREG 轮询） */
static TickType_t s_rndis_connected_tick = 0;

/* 4G 数据面诊断（GOT_IP 后周期探测，LOST_IP 停止） */
static TaskHandle_t s_data_diag_task = NULL;
static volatile bool s_data_diag_stop = false;
static bool s_data_path_ok = false;

/* 本次拨号实际使用的 APN（自动识别或强制配置） */
static char s_apn_resolved[CELL_MODEM_APN_BUF_SIZE];

/** 开机就绪阶段收集的模组信息（boot ③④ → dial 使用） */
typedef struct {
    int  csq;
    char imei[20];
    char iccid[24];
    char fw_version[32];
    char imsi_plmn[8];
    char cops_plmn[8];
    int  cops_act;
    char cell_ipv4[16];
} cell_modem_boot_info_t;

/* USB 枚举过滤器回调 */
static usb_host_enum_filter_cb_t s_enum_filter_cb = NULL;

static uint16_t cell_modem_usb_vid(void)
{
    return g.hw.usb_vid ? g.hw.usb_vid : CELL_MODEM_USB_VID_DEFAULT;
}

static uint16_t cell_modem_usb_pid_rndis(void)
{
    return g.hw.usb_pid_rndis ? g.hw.usb_pid_rndis : CELL_MODEM_USB_PID_RNDIS_DEFAULT;
}

static uint16_t cell_modem_usb_pid_composite(void)
{
    return g.hw.usb_pid_composite ? g.hw.usb_pid_composite : CELL_MODEM_USB_PID_COMPOSITE_DEFAULT;
}

/** app_main 早期复位已执行时，cell_modem_init 不再重复硬复位 */
static bool s_early_reset_done = false;
/** CEREG 超时后已执行的硬复位次数（pdp_done 成功后清零） */
static int s_cereg_hw_reset_count = 0;

/* SIM 在位状态轮询：CPIN 仅 boot 时查一次，运行期靠 monitor 周期复检。
 * 连续失败阈值避免小区切换/RF 瞬态导致的误报；任意一次 READY 立即清零。 */
#define CELL_MODEM_CPIN_FAIL_THRESHOLD  3
static int s_cpin_fail_count = 0;
static bool s_sim_present = true;       /* boot CPIN 通过即 true；运行期检测到丢失置 false */

/* 前向声明 */
static void cell_modem_update_state(cell_modem_state_t new_state);
static void cell_modem_notify_pdp_ready(void);
static void cell_modem_close_at_port(void);
static bool cell_modem_get_rndis_dev_addr(uint8_t *dev_addr_out);
static bool cell_modem_at_port_alive(void);
static bool cell_modem_ensure_at_port(void);
static esp_err_t cell_modem_run_pdp_sequence(const char *apn);
static void cell_modem_at_drain_rx(void);
static esp_err_t cell_modem_at_send_collect(const char *cmd, char *response, size_t resp_len,
                                        uint32_t timeout_ms, const char *required_substr);
static esp_err_t cell_modem_at_boot_ready(cell_modem_boot_info_t *info_out);
static esp_err_t cell_modem_rndis_dial(const char *apn, cell_modem_boot_info_t *info);
static void cell_modem_start_data_diag(void);
static void cell_modem_stop_data_diag(void);
static void cell_modem_eth_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);
static void cell_modem_ip_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data);
static void cell_modem_cdc_event_cb(usbh_cdc_device_event_t event,
                                usbh_cdc_device_event_data_t *event_data,
                                void *user_ctx);
static bool cell_modem_try_open_at_port(uint8_t dev_addr, int itf_num);
static bool cell_modem_scan_at_port(void);
static esp_err_t cell_modem_at_send(const char *cmd, char *response,
                                size_t resp_len, uint32_t timeout_ms);
static esp_err_t cell_modem_at_send_checked(const char *cmd, char *response,
                                        size_t resp_len, uint32_t timeout_ms);
static bool cell_modem_parse_cops_plmn(const char *response, char *plmn5, size_t plmn_len);
static const char *cell_modem_apn_lookup_plmn(const char *plmn5);

/** 从 +CSQ: 响应解析 RSSI（0-31 有效） */
static bool cell_modem_parse_csq_response(const char *response, int *csq_out)
{
    const char *p = strstr(response, "+CSQ:");
    if (p == NULL) {
        return false;
    }
    p += 5;
    while (*p == ' ') {
        p++;
    }
    int rssi = atoi(p);
    if (rssi >= 0 && rssi <= 31) {
        if (csq_out != NULL) {
            *csq_out = rssi;
        }
        return true;
    }
    return false;
}

/** CPIN 通过后启用 CEREG URC 上报（不干预 RF 状态）。
 *
 * 关键：不要发 AT+CFUN=1。模组默认 boot 后即 CFUN=1（RF 开），
 * 再发一次 CFUN=1 会触发 RF 子系统重新初始化，冷启动时耗时 30-159 秒，
 * 期间 CSQ=99、CEREG stat=0（RF 接收机未工作）。
 * 旧代码（refactor 867cc64 前）不发 CFUN=1，模组 2-5 秒就有 CSQ、5-15 秒驻网。
 * 同理不发 AT+COPS=0——模组默认自动搜网，COPS=0 在 RF 未就绪时发送也无用。
 */
static void cell_modem_boot_log_rf_diag(void); /* 前置声明：定义在本函数下方 */
static void cell_modem_boot_enable_rf(void)
{
    char response[256];

    /* 仅启用 CEREG URC 上报，用于被动接收注册状态变化。
     * 不发 CFUN=1、不发 COPS=0——让模组按默认流程自行初始化 RF 和搜网。 */
    ESP_LOGI(TAG, "boot: enabling CEREG URC (not touching RF — module auto-registers by default)");
    (void)cell_modem_at_send("AT+CEREG=2", response, sizeof(response), 3000);

    /* 诊断基线：CPIN 之下立刻打一次 RF 全状态，确认模组启动后是否真为默认 CFUN=1。
     * 早期 reset 后偶发会进入非默认状态（CFUN=0 / 飞行模式）时，此处可第一时间暴露，
     * 避免后续 CSQ 99、CEREG stat=0 时无法区分「RF 关断」vs「无信号」。 */
    cell_modem_boot_log_rf_diag();
}

/** 打印 CFUN? / CEREG? / COPS? / CSQ 全状态，用于区分 RF 关断 vs 无信号 vs COPS 异常。
 * 顺便解析 CSQ 回写 g.csq_value，使 boot_info 日志和心跳 sig 字段拿到真实值
 * （boot 跳过 CSQ 等待后，info->csq 仅靠此处填充）。 */
static void cell_modem_boot_log_rf_diag(void)
{
    char response[256];

    ESP_LOGW(TAG, "boot: RF diag...");
    (void)cell_modem_at_send("AT+CFUN?", response, sizeof(response), 5000);
    (void)cell_modem_at_send("AT+CEREG?", response, sizeof(response), 5000);
    (void)cell_modem_at_send("AT+COPS?", response, sizeof(response), 5000);
    memset(response, 0, sizeof(response));
    if (cell_modem_at_send("AT+CSQ", response, sizeof(response), 5000) == ESP_OK) {
        int csq = -1;
        if (cell_modem_parse_csq_response(response, &csq) && csq >= 0) {
            g.csq_value = csq;
        }
    }
}

/* ================================================================
 *  硬件复位
 * ================================================================ */

/** 对指定 GPIO 执行 modem 硬复位脉冲（低有效） */
static void cell_modem_pulse_reset_gpio(int rst_gpio)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << rst_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Hardware reset modem (GPIO%d)...", rst_gpio);

    gpio_set_level(rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(rst_gpio, 1);
    ESP_LOGI(TAG, "modem reset released, waiting for boot...");
    vTaskDelay(pdMS_TO_TICKS(2000));
}

/** 配置 RST 引脚并释放在复位态（高电平）；rst_gpio < 0 时跳过 */
static void cell_modem_rst_gpio_release(void)
{
    if (g.hw.rst_gpio < 0) {
        return;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << g.hw.rst_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(g.hw.rst_gpio, 1);
}

/**
 * modem 硬件复位（monitor 看门狗 / RNDIS 恢复用）
 */
static void cell_modem_hardware_reset(void)
{
    if (g.hw.rst_gpio < 0) {
        ESP_LOGW(TAG, "No modem RST GPIO configured, skip hardware reset");
        return;
    }

    cell_modem_pulse_reset_gpio(g.hw.rst_gpio);
}

esp_err_t cell_modem_early_hardware_reset(int rst_gpio)
{
    if (rst_gpio < 0) {
        return ESP_OK;
    }
    cell_modem_pulse_reset_gpio(rst_gpio);
    s_early_reset_done = true;
    return ESP_OK;
}

/* ================================================================
 *  状态管理
 * ================================================================ */

static void cell_modem_update_state(cell_modem_state_t new_state)
{
    if (g.state != new_state) {
        ESP_LOGI(TAG, "State: %d → %d", g.state, new_state);
        g.state = new_state;
        if (new_state == CELL_MODEM_STATE_GOT_IP) {
            /* 记录最后成功获取 IP 的时间，并清除看门狗计数 */
            g.last_got_ip_tick = xTaskGetTickCount();
            g.watchdog_reset_count = 0;
            /* 注意：不在此处置 pdp_done。RNDIS 本地 IP 仅表示 USB 链路 DHCP 完成，
             * 蜂窝数据须由 cell_modem_run_pdp_sequence() 成功后才视为就绪。 */
        }
        if (g.status_cb) {
            g.status_cb(new_state);
        }
    }
}

/** 标记 PDP 完成并在「state 已 GOT_IP 但回调会被早返回吞掉」时补发一次通知。
 *
 * 背景：RNDIS 链路先 Up 时，state 早已是 GOT_IP；随后 PDP 激活仅翻转
 * g.pdp_done，不改变 state 值。cell_modem_update_state() 在 state 不变时
 * 早返回，不会触发 g.status_cb()。订阅者（如上层 manager 缓存的
 * wan_verified）因此错过 PDP 完成事件，导致 TCP 连接被持续拦截。
 *
 * 仅在 state == GOT_IP 时补发回调——这是 bug 唯一会触发的场景。
 * 其他状态下（CONFIGURING / CONNECTED），下一次 update_state 由
 * at_config_task 或 IP 事件自然触发，回调路径完整，无需本函数重复通知。
 */
static void cell_modem_notify_pdp_ready(void)
{
    g.pdp_done = true;
    ESP_LOGI(TAG, "PDP context activated successfully");
    /* state 未变时 update_state 早返回不会触发回调；这里按需补发一次，
     * 订阅者应根据 state + cell_modem_is_pdp_ready() 联合判断就绪。 */
    if (g.status_cb && g.state == CELL_MODEM_STATE_GOT_IP) {
        g.status_cb(g.state);
    }
}

/* ================================================================
 *  AT 命令支持（通过 USB CDC 端口轮询收发）
 * ================================================================ */

static void at_recv_data_cb(usbh_cdc_port_handle_t port, void *arg)
{
    /* 空回调，实际读取在 at_send 中轮询 */
}

static void at_port_closed_cb(usbh_cdc_port_handle_t port, void *arg)
{
    ESP_LOGW(TAG, "AT port closed");
    if (g.at_port == port) {
        g.at_port = NULL;
    }
}

#define CELL_MODEM_AT_LOG_LINE_MAX 384

/** boot/dial 期间打印 AT 响应；monitor 周期 AT/CSQ 探针不刷屏 */
static bool cell_modem_at_log_enabled(const char *cmd)
{
    if (g.at_config_in_progress) {
        return true;
    }
    if (cmd != NULL && (strcmp(cmd, "AT") == 0 || strcmp(cmd, "AT+CSQ") == 0)) {
        return false;
    }
    return true;
}

/** 将 AT 响应压成单行（去 \\r\\n，截断过长内容） */
static void cell_modem_at_sanitize_line(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (in == NULL || out_len == 0) {
        return;
    }
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n') {
            if (j > 0 && out[j - 1] != ' ') {
                out[j++] = ' ';
            }
        } else if (c >= 32 && c < 127) {
            out[j++] = c;
        }
    }
    while (j > 0 && out[j - 1] == ' ') {
        j--;
    }
    out[j] = '\0';
}

static void cell_modem_at_log_tx(const char *cmd)
{
    if (cell_modem_at_log_enabled(cmd)) {
        ESP_LOGI(TAG, "AT> %s", cmd);
    }
}

static void cell_modem_at_log_rx(const char *cmd, const char *response, const char *note)
{
    if (!cell_modem_at_log_enabled(cmd)) {
        return;
    }
    char line[CELL_MODEM_AT_LOG_LINE_MAX];
    if (response != NULL && response[0] != '\0') {
        cell_modem_at_sanitize_line(response, line, sizeof(line));
        if (note != NULL) {
            ESP_LOGI(TAG, "AT< (%s) %s", note, line);
        } else {
            ESP_LOGI(TAG, "AT< %s", line);
        }
    } else if (note != NULL) {
        ESP_LOGI(TAG, "AT< (%s)", note);
    }
}

/**
 * 发送 AT 命令并轮询读取响应
 */
static esp_err_t cell_modem_at_send(const char *cmd, char *response,
                                size_t resp_len, uint32_t timeout_ms)
{
    if (g.at_port == NULL) {
        ESP_LOGE(TAG, "AT port not open");
        return ESP_ERR_INVALID_STATE;
    }

    /* 清空接收缓冲区 */
    size_t avail = 0;
    while (usbh_cdc_get_rx_buffer_size(g.at_port, &avail) == ESP_OK && avail > 0) {
        uint8_t flush[128];
        size_t flush_len = (avail < sizeof(flush)) ? avail : sizeof(flush);
        if (usbh_cdc_read_bytes(g.at_port, flush, &flush_len, pdMS_TO_TICKS(50)) != ESP_OK || flush_len == 0)
            break;
    }

    /* 发送 AT 命令 */
    char at_cmd[256];
    int len = snprintf(at_cmd, sizeof(at_cmd), "%s\r\n", cmd);
    esp_err_t ret = usbh_cdc_write_bytes(g.at_port, (uint8_t *)at_cmd, len, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AT write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    cell_modem_at_log_tx(cmd);

    /* 轮询读取响应 */
    if (response && resp_len > 0) {
        memset(response, 0, resp_len);
        int total_read = 0;
        TickType_t start = xTaskGetTickCount();

        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
            size_t avail = 0;
            ret = usbh_cdc_get_rx_buffer_size(g.at_port, &avail);
            if (ret == ESP_OK && avail > 0) {
                size_t to_read = resp_len - total_read - 1;
                if (to_read > avail) to_read = avail;
                if (to_read > 0) {
                    ret = usbh_cdc_read_bytes(g.at_port,
                                               (uint8_t *)(response + total_read),
                                               &to_read, pdMS_TO_TICKS(100));
                    if (ret == ESP_OK && to_read > 0) {
                        total_read += to_read;
                        response[total_read] = '\0';
                        if (strstr(response, "OK") || strstr(response, "ERROR") ||
                            strstr(response, "CME ERROR")) {
                            cell_modem_at_log_rx(cmd, response, NULL);
                            return ESP_OK;
                        }
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (total_read > 0) {
            cell_modem_at_log_rx(cmd, response, "partial");
            return ESP_OK;
        }
        cell_modem_at_log_rx(cmd, NULL, "timeout");
        ESP_LOGW(TAG, "AT response timeout: %s", cmd);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/**
 * 发送 AT 命令并验证响应包含 "OK" 且无 "ERROR"
 */
static esp_err_t cell_modem_at_send_checked(const char *cmd, char *response,
                                        size_t resp_len, uint32_t timeout_ms)
{
    esp_err_t ret = cell_modem_at_send(cmd, response, resp_len, timeout_ms);
    if (ret != ESP_OK) return ret;
    if (response && resp_len > 0 &&
        (strstr(response, "OK") == NULL ||
         strstr(response, "ERROR") != NULL ||
         strstr(response, "CME ERROR") != NULL)) {
        ESP_LOGW(TAG, "AT failed: %s → %s", cmd, response);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 清空 AT 口接收缓冲（开 AT 口后、MATREADY 等待前使用） */
static void cell_modem_at_drain_rx(void)
{
    if (g.at_port == NULL) {
        return;
    }
    size_t avail = 0;
    while (usbh_cdc_get_rx_buffer_size(g.at_port, &avail) == ESP_OK && avail > 0) {
        uint8_t flush[128];
        size_t flush_len = (avail < sizeof(flush)) ? avail : sizeof(flush);
        if (usbh_cdc_read_bytes(g.at_port, flush, &flush_len, pdMS_TO_TICKS(50)) != ESP_OK ||
            flush_len == 0) {
            break;
        }
    }
}

/**
 * 在已发送命令后继续读取 AT 响应（不再次 flush / 发送）
 * @return 读到的总字节数
 */
static int cell_modem_at_read_more(char *response, size_t resp_len, uint32_t timeout_ms)
{
    if (g.at_port == NULL || response == NULL || resp_len == 0) {
        return 0;
    }

    int total_read = (int)strlen(response);
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        size_t avail = 0;
        esp_err_t ret = usbh_cdc_get_rx_buffer_size(g.at_port, &avail);
        if (ret == ESP_OK && avail > 0) {
            size_t to_read = resp_len - (size_t)total_read - 1;
            if (to_read > avail) {
                to_read = avail;
            }
            if (to_read > 0) {
                ret = usbh_cdc_read_bytes(g.at_port,
                                          (uint8_t *)(response + total_read),
                                          &to_read, pdMS_TO_TICKS(100));
                if (ret == ESP_OK && to_read > 0) {
                    total_read += (int)to_read;
                    response[total_read] = '\0';
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    return total_read;
}

/**
 * 发送 AT 并收集完整响应（须含 OK；optional required_substr 如 +MDIALUP:）
 */
static esp_err_t cell_modem_at_send_collect(const char *cmd, char *response, size_t resp_len,
                                        uint32_t timeout_ms, const char *required_substr)
{
    esp_err_t ret = cell_modem_at_send(cmd, response, resp_len, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    /* MDIALUP 等 URC 可能在 OK 之后到达 */
    if (required_substr != NULL && strstr(response, required_substr) == NULL) {
        /* 响应已含 ERROR/CME ERROR 时不必再等 URC，省 ~20s 超时
         *（MDIALUP=1,1 在 PDP 已激活时回 +CME ERROR: 3 即此情形） */
        if (strstr(response, "ERROR") != NULL) {
            ESP_LOGW(TAG, "AT collect failed: %s → %s", cmd, response);
            return ESP_FAIL;
        }
        size_t before = strlen(response);
        cell_modem_at_read_more(response, resp_len, timeout_ms / 2 + 5000);
        if (strlen(response) > before) {
            cell_modem_at_log_rx(cmd, response, "collect");
        }
    }

    if (strstr(response, "OK") == NULL ||
        strstr(response, "ERROR") != NULL ||
        strstr(response, "CME ERROR") != NULL) {
        ESP_LOGW(TAG, "AT collect failed: %s → %s", cmd, response);
        return ESP_FAIL;
    }
    if (required_substr != NULL && strstr(response, required_substr) == NULL) {
        ESP_LOGW(TAG, "AT collect missing %s: %s", required_substr, response);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 解析 +COPS: 0,2,"46000",7 的 PLMN 与 act */
static bool cell_modem_parse_cops_full(const char *response, char *plmn5, size_t plmn_len, int *act_out)
{
    if (!cell_modem_parse_cops_plmn(response, plmn5, plmn_len)) {
        return false;
    }
    if (act_out != NULL) {
        const char *q2 = strrchr(response, ',');
        if (q2 != NULL) {
            *act_out = atoi(q2 + 1);
        } else {
            *act_out = -1;
        }
    }
    return plmn5[0] != '\0' && strcmp(plmn5, "0") != 0;
}

/** 校验 MDIALUP 返回的 IPv4 是否可用（非空、非 0.0.0.0） */
static bool cell_modem_is_valid_cellular_ipv4(const char *ipv4)
{
    if (ipv4 == NULL || ipv4[0] == '\0' || strcmp(ipv4, "0.0.0.0") == 0) {
        return false;
    }
    int dots = 0;
    for (const char *p = ipv4; *p != '\0'; p++) {
        if (*p == '.') {
            dots++;
        } else if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return dots == 3;
}

/** 从响应中提取指定 URC 标记（如 "+MDIALUP:" / "+MIPCALL:"）行内的首个有效 IPv4。
 * 复用于 MDIALUP 和 MIPCALL 拨号响应解析——两者 URC 格式相同：
 *   +XXX: <cid>,<state>,"<ipv4>"[,"<ipv6>"]
 * @param marker 须含冒号，如 "+MDIALUP:" / "+MIPCALL:"
 * @return true 解析到有效 IPv4（非空、非 0.0.0.0、纯点分十进制） */
static bool cell_modem_parse_urc_ipv4(const char *resp, const char *marker,
                                      char *ipv4_out, size_t ipv4_len)
{
    size_t marker_len = strlen(marker);
    const char *p = resp;
    while ((p = strstr(p, marker)) != NULL) {
        const char *q1 = strchr(p, '"');
        if (q1 == NULL) {
            p += marker_len;
            continue;
        }
        q1++;
        const char *q2 = strchr(q1, '"');
        if (q2 == NULL || (size_t)(q2 - q1) + 1 > ipv4_len) {
            p += marker_len;
            continue;
        }
        size_t len = (size_t)(q2 - q1);
        memcpy(ipv4_out, q1, len);
        ipv4_out[len] = '\0';
        if (cell_modem_is_valid_cellular_ipv4(ipv4_out)) {
            return true;
        }
        p = q2 + 1;
    }
    return false;
}

/** 从响应中提取 +MDIALUP: 行内的蜂窝 IPv4 */
static bool cell_modem_parse_mdialup_ipv4(const char *resp, char *ipv4_out, size_t ipv4_len)
{
    return cell_modem_parse_urc_ipv4(resp, "+MDIALUP:", ipv4_out, ipv4_len);
}

/** 从响应中提取 +MIPCALL: 行内的蜂窝 IPv4 */
static bool cell_modem_parse_mipcall_ipv4(const char *resp, char *ipv4_out, size_t ipv4_len)
{
    return cell_modem_parse_urc_ipv4(resp, "+MIPCALL:", ipv4_out, ipv4_len);
}

/* ================================================================
 *  AT 端口扫描
 * ================================================================ */

/**
 * 尝试在指定设备地址和接口号打开 AT 端口
 */
static bool cell_modem_try_open_at_port(uint8_t dev_addr, int itf_num)
{
    usbh_cdc_port_config_t port_config = {
        .dev_addr = dev_addr,
        .itf_num = itf_num,
        .in_transfer_buffer_size = 2048,
        .out_transfer_buffer_size = 1024,
        .in_ringbuf_size = 2048,
        .out_ringbuf_size = 1024,
        .cbs = {
            .notif_cb = NULL,
            .recv_data = at_recv_data_cb,
            .closed = at_port_closed_cb,
            .user_data = NULL,
        },
        .flags = USBH_CDC_FLAGS_DISABLE_NOTIFICATION,
    };

    esp_err_t ret = usbh_cdc_port_open(&port_config, &g.at_port);
    if (ret != ESP_OK) {
        return false;
    }

    /* 发送 AT 测试命令判断此接口是否为 AT 端口。
     * port_open 成功后接口即就绪，无需额外稳定延时。
     * AT 超时设为 2s：失败只是尝试下一个接口，快速失败更重要。 */
    char response[128];
    memset(response, 0, sizeof(response));
    ret = cell_modem_at_send("AT", response, sizeof(response), 2000);
    if (ret == ESP_OK && (strstr(response, "OK") || strstr(response, "AT"))) {
        s_at_dev_addr = dev_addr;
        s_at_itf_num = itf_num;
        g.dev_addr = dev_addr;
        ESP_LOGI(TAG, "AT port found: dev_addr=%d, interface %d", dev_addr, itf_num);
        return true;
    }

    /* 不是 AT 端口，关闭继续尝试 */
    usbh_cdc_port_close(g.at_port);
    g.at_port = NULL;
    return false;
}

static void cell_modem_close_at_port(void)
{
    if (g.at_port) {
        usbh_cdc_port_close(g.at_port);
        g.at_port = NULL;
    }
}

/**
 * 从已绑定的 RNDIS CDC 口取得 USB 设备地址（与 AT 口同设备）
 */
static bool cell_modem_get_rndis_dev_addr(uint8_t *dev_addr_out)
{
    if (g.eth_driver == NULL || dev_addr_out == NULL) {
        return false;
    }

    usbh_cdc_port_handle_t rndis_port = usb_rndis_get_cdc_port_handle(g.eth_driver);
    if (rndis_port == NULL) {
        return false;
    }

    usb_device_handle_t dev_hdl = NULL;
    if (usbh_cdc_get_dev_handle(rndis_port, &dev_hdl) != ESP_OK || dev_hdl == NULL) {
        return false;
    }

    usb_device_info_t info = {0};
    if (usb_host_device_info(dev_hdl, &info) != ESP_OK) {
        return false;
    }

    *dev_addr_out = info.dev_addr;
    return true;
}

/** AT 口是否仍可用（轻量 AT 探测） */
static bool cell_modem_at_port_alive(void)
{
    if (g.at_port == NULL) {
        return false;
    }
    char response[64];
    memset(response, 0, sizeof(response));
    return cell_modem_at_send("AT", response, sizeof(response), 2000) == ESP_OK &&
           (strstr(response, "OK") || strstr(response, "AT"));
}

/**
 * 确保 AT 口可用：已开则复用，避免重复 port_open 耗尽 HCD channel
 */
static bool cell_modem_ensure_at_port(void)
{
    const int itf = (g.config.at_interface_num >= 0) ? g.config.at_interface_num : 2;

    if (g.at_port != NULL) {
        if (cell_modem_at_port_alive()) {
            ESP_LOGI(TAG, "Reusing existing AT port (dev=%d itf=%d)", s_at_dev_addr, s_at_itf_num);
            return true;
        }
        ESP_LOGW(TAG, "AT port stale, closing before reopen");
        cell_modem_close_at_port();
    }

    uint8_t rndis_addr = 0;
    if (cell_modem_get_rndis_dev_addr(&rndis_addr)) {
        g.dev_addr = rndis_addr;
        ESP_LOGI(TAG, "Opening AT port on RNDIS device addr=%d itf=%d", rndis_addr, itf);
        if (cell_modem_try_open_at_port(rndis_addr, itf)) {
            return true;
        }
    }

    return cell_modem_scan_at_port();
}

/**
 * 扫描 USB CDC 接口查找 modem 的 AT 命令端口
 *
 * 优先级：
 *   0. 与 RNDIS 同设备（usb_rndis_get_cdc_port_handle）
 *   1. CDC 事件记录的复合模式设备（g.dev_addr）
 *   2. 上次成功位置
 *   3. 全量兜底扫描
 *
 * 注意：扫描过程中 usbh_cdc_port_open 对不存在的设备地址会产生大量
 *       ESP_LOGE 日志（"Could not open device N"）。
 *       扫描期间临时静默 USB 组件日志，结束后恢复。
 */
static bool cell_modem_scan_at_port(void)
{
    bool found = false;

    /* 静默 USB 组件日志（端口扫描对不存在设备会产生大量错误日志） */
    esp_log_level_t old_cdc_level = esp_log_level_get("USBH_CDC");
    esp_log_level_t old_host_level = esp_log_level_get("USB HOST");
    esp_log_level_set("USBH_CDC", ESP_LOG_NONE);
    esp_log_level_set("USB HOST", ESP_LOG_NONE);

    const int itf = (g.config.at_interface_num >= 0) ? g.config.at_interface_num : 2;

    /* 0. 与 RNDIS 同设备（官方示例做法） */
    uint8_t rndis_addr = 0;
    if (cell_modem_get_rndis_dev_addr(&rndis_addr)) {
        g.dev_addr = rndis_addr;
        ESP_LOGI(TAG, "Scanning AT port, RNDIS device addr=%d (itf %d first)", rndis_addr, itf);
        if (cell_modem_try_open_at_port(rndis_addr, itf)) {
            found = true;
            goto done;
        }
        for (int i = 0; i < 4; i++) {
            if (i == itf) {
                continue;
            }
            if (cell_modem_try_open_at_port(rndis_addr, i)) {
                found = true;
                goto done;
            }
        }
    }

    /* 1. CDC 事件记录的复合模式设备 */
    if (g.dev_addr > 0 && g.dev_addr != rndis_addr) {
        ESP_LOGI(TAG, "Scanning AT port, trying CDC device addr=%d (itf %d first)",
                 g.dev_addr, itf);
        if (cell_modem_try_open_at_port(g.dev_addr, itf)) {
            found = true;
            goto done;
        }
        for (int i = 0; i < 4; i++) {
            if (i == itf) {
                continue;
            }
            if (cell_modem_try_open_at_port(g.dev_addr, i)) {
                found = true;
                goto done;
            }
        }
    }

    /* 2. 上次成功的位置 */
    if (s_at_dev_addr > 0 && s_at_dev_addr != g.dev_addr && s_at_dev_addr != rndis_addr) {
        ESP_LOGI(TAG, "Scanning AT port, trying last known: dev=%d itf=%d",
                 s_at_dev_addr, s_at_itf_num);
        if (cell_modem_try_open_at_port(s_at_dev_addr, s_at_itf_num)) {
            found = true;
            goto done;
        }
    }

    /* 3. 全量兜底（跳过已试过的地址） */
    for (int attempt = 0; attempt < 2; attempt++) {
        ESP_LOGI(TAG, "Scanning AT port, attempt %d/2", attempt + 1);
        for (int dev_addr = 1; dev_addr <= 8; dev_addr++) {
            if (dev_addr == rndis_addr || dev_addr == s_at_dev_addr || dev_addr == g.dev_addr) {
                continue;
            }
            if (cell_modem_try_open_at_port(dev_addr, itf)) {
                found = true;
                goto done;
            }
            for (int i = 0; i < 4; i++) {
                if (i == itf) {
                    continue;
                }
                if (cell_modem_try_open_at_port(dev_addr, i)) {
                    found = true;
                    goto done;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGW(TAG, "AT port not found after all attempts");

done:
    /* 恢复 USB 组件日志级别 */
    esp_log_level_set("USBH_CDC", old_cdc_level);
    esp_log_level_set("USB HOST", old_host_level);
    return found;
}

/* ================================================================
 *  APN 自动识别（IMSI / COPS → 运营商 APN）
 * ================================================================ */

/** 从 AT 响应中提取前 max_digits 位数字（用于 CIMI/IMEI） */
static bool cell_modem_extract_leading_digits(const char *src, char *dst, size_t dst_len, size_t max_digits)
{
    size_t n = 0;
    for (; *src != '\0' && n < max_digits && n + 1 < dst_len; src++) {
        if (*src >= '0' && *src <= '9') {
            dst[n++] = *src;
        } else if (n > 0) {
            break;
        }
    }
    dst[n] = '\0';
    return n >= 5;
}

/** PLMN 须为 5~6 位纯数字（排除 COPS 长名如 "CHN-U"） */
static bool cell_modem_is_plmn_numeric(const char *plmn)
{
    size_t len = strlen(plmn);
    if (len < 5 || len > 6) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (plmn[i] < '0' || plmn[i] > '9') {
            return false;
        }
    }
    return true;
}

/** 从 +CCID/+MCCID 响应提取 ICCID（取最长 19~22 位连续数字段） */
static bool cell_modem_parse_iccid(const char *response, char *iccid_out, size_t iccid_len)
{
    if (response == NULL || iccid_out == NULL || iccid_len == 0) {
        return false;
    }
    iccid_out[0] = '\0';

    const char *scan = response;
    static const char *const markers[] = { "+MCCID:", "+CCID:", NULL };
    for (int i = 0; markers[i] != NULL; i++) {
        const char *p = strstr(response, markers[i]);
        if (p != NULL) {
            scan = p + strlen(markers[i]);
            break;
        }
    }

    const char *best = NULL;
    size_t best_len = 0;
    for (const char *p = scan; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            continue;
        }
        const char *start = p;
        size_t len = 0;
        while (*p >= '0' && *p <= '9') {
            len++;
            p++;
        }
        if (len >= 19 && len <= 22 && len > best_len) {
            best = start;
            best_len = len;
        }
    }
    if (best == NULL) {
        return false;
    }
    size_t copy_len = best_len;
    if (copy_len >= iccid_len) {
        copy_len = iccid_len - 1;
    }
    memcpy(iccid_out, best, copy_len);
    iccid_out[copy_len] = '\0';
    return true;
}

/** 从 +COPS: 0,2,"46001",7 解析数字 PLMN */
static bool cell_modem_parse_cops_plmn(const char *response, char *plmn_out, size_t plmn_len)
{
    const char *q1 = strchr(response, '"');
    if (q1 == NULL) {
        return false;
    }
    q1++;
    const char *q2 = strchr(q1, '"');
    if (q2 == NULL || q2 <= q1) {
        return false;
    }
    size_t oper_len = (size_t)(q2 - q1);
    if (oper_len >= plmn_len) {
        oper_len = plmn_len - 1;
    }
    memcpy(plmn_out, q1, oper_len);
    plmn_out[oper_len] = '\0';
    return cell_modem_is_plmn_numeric(plmn_out);
}

/**
 * 查询 COPS 并以数字 PLMN 解析。
 * 先 AT+COPS=3,2 令 COPS? 返回 "46006" 而非 "CHN-U"。
 */
static esp_err_t cell_modem_query_cops_numeric(char *response, size_t resp_len,
                                          char *plmn_out, size_t plmn_len, int *act_out)
{
    (void)cell_modem_at_send("AT+COPS=3,2", response, resp_len, 3000);
    memset(response, 0, resp_len);
    esp_err_t ret = cell_modem_at_send("AT+COPS?", response, resp_len, 5000);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!cell_modem_parse_cops_full(response, plmn_out, plmn_len, act_out)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const char *cell_modem_apn_lookup_plmn(const char *plmn5)
{
    if (strncmp(plmn5, "46000", 5) == 0 || strncmp(plmn5, "46002", 5) == 0 ||
        strncmp(plmn5, "46007", 5) == 0 || strncmp(plmn5, "46008", 5) == 0) {
        return "CMNET";
    }
    if (strncmp(plmn5, "46001", 5) == 0 || strncmp(plmn5, "46006", 5) == 0 ||
        strncmp(plmn5, "46009", 5) == 0) {
        return "3gnet";
    }
    if (strncmp(plmn5, "46003", 5) == 0 || strncmp(plmn5, "46005", 5) == 0 ||
        strncmp(plmn5, "46011", 5) == 0) {
        return "ctnet";
    }
    return NULL;
}

/**
 * 解析本次拨号 APN：config.apn 非空则强制；否则 info/IMSI/COPS → 兜底 3gnet
 */
static const char *cell_modem_resolve_apn_for_dial(const cell_modem_boot_info_t *info)
{
    if (g.config.apn != NULL && g.config.apn[0] != '\0') {
        snprintf(s_apn_resolved, sizeof(s_apn_resolved), "%s", g.config.apn);
        ESP_LOGI(TAG, "APN: forced %s", s_apn_resolved);
        return s_apn_resolved;
    }

    const char *apn = NULL;
    if (info != NULL && info->imsi_plmn[0] != '\0') {
        apn = cell_modem_apn_lookup_plmn(info->imsi_plmn);
        if (apn != NULL) {
            snprintf(s_apn_resolved, sizeof(s_apn_resolved), "%s", apn);
            ESP_LOGI(TAG, "boot: APN auto plmn=%s -> %s", info->imsi_plmn, s_apn_resolved);
            return s_apn_resolved;
        }
    }
    if (info != NULL && info->cops_plmn[0] != '\0') {
        apn = cell_modem_apn_lookup_plmn(info->cops_plmn);
        if (apn != NULL) {
            snprintf(s_apn_resolved, sizeof(s_apn_resolved), "%s", apn);
            ESP_LOGI(TAG, "boot: APN auto plmn=%s -> %s", info->cops_plmn, s_apn_resolved);
            return s_apn_resolved;
        }
    }

    char response[256];
    char imsi_plmn[8] = "";
    char cops_plmn[8] = "";

    if (cell_modem_at_send("AT+CIMI", response, sizeof(response), 5000) == ESP_OK) {
        if (cell_modem_extract_leading_digits(response, imsi_plmn, sizeof(imsi_plmn), 5)) {
            apn = cell_modem_apn_lookup_plmn(imsi_plmn);
            if (apn != NULL) {
                snprintf(s_apn_resolved, sizeof(s_apn_resolved), "%s", apn);
                ESP_LOGI(TAG, "APN: auto IMSI %s -> %s", imsi_plmn, s_apn_resolved);
                return s_apn_resolved;
            }
        }
    }

    if (cell_modem_query_cops_numeric(response, sizeof(response),
                                cops_plmn, sizeof(cops_plmn), NULL) == ESP_OK) {
        apn = cell_modem_apn_lookup_plmn(cops_plmn);
        if (apn != NULL) {
            snprintf(s_apn_resolved, sizeof(s_apn_resolved), "%s", apn);
            ESP_LOGI(TAG, "APN: auto COPS %s -> %s", cops_plmn, s_apn_resolved);
            return s_apn_resolved;
        }
    }

    snprintf(s_apn_resolved, sizeof(s_apn_resolved), "%s", CELL_MODEM_DEFAULT_APN_FALLBACK);
    ESP_LOGW(TAG, "APN: unknown PLMN (imsi=%s cops=%s), fallback %s",
             imsi_plmn[0] != '\0' ? imsi_plmn : "?",
             cops_plmn[0] != '\0' ? cops_plmn : "?",
             s_apn_resolved);
    return s_apn_resolved;
}

/* ================================================================
 *  开机 AT 就绪 + RNDIS 拨号（MIPCALL 主路径）
 * ================================================================ */

/** ①~④：模组就绪 / SIM / 信息 / CEREG+COPS */
static esp_err_t cell_modem_at_boot_ready(cell_modem_boot_info_t *info_out)
{
    char response[384];
    cell_modem_boot_info_t local = {0};
    cell_modem_boot_info_t *info = info_out != NULL ? info_out : &local;
    memset(info, 0, sizeof(*info));
    info->csq = -1;
    info->cops_act = -1;

    /* ① 等待模组 AT 就绪（+MATREADY 或 AT OK） */
    ESP_LOGI(TAG, "boot: waiting modem ready");
    cell_modem_at_drain_rx();
    TickType_t boot_start = xTaskGetTickCount();
    bool modem_ready = false;
    char accum[256] = "";

    while ((xTaskGetTickCount() - boot_start) < pdMS_TO_TICKS(CELL_MODEM_BOOT_MATREADY_MS)) {
        size_t avail = 0;
        if (usbh_cdc_get_rx_buffer_size(g.at_port, &avail) == ESP_OK && avail > 0) {
            size_t to_read = sizeof(accum) - strlen(accum) - 1;
            if (to_read > 0) {
                usbh_cdc_read_bytes(g.at_port, (uint8_t *)(accum + strlen(accum)),
                                    &to_read, pdMS_TO_TICKS(100));
                accum[sizeof(accum) - 1] = '\0';
            }
            if (strstr(accum, "+MATREADY") != NULL) {
                ESP_LOGI(TAG, "boot: MATREADY");
                modem_ready = true;
                break;
            }
        }

        memset(response, 0, sizeof(response));
        if (cell_modem_at_send("AT", response, sizeof(response), 3000) == ESP_OK &&
            strstr(response, "OK") != NULL) {
            ESP_LOGI(TAG, "boot: AT OK (MATREADY missed)");
            modem_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!modem_ready) {
        ESP_LOGE(TAG, "boot: modem ready timeout");
        return ESP_ERR_TIMEOUT;
    }

    cell_modem_at_send_checked("ATE0", response, sizeof(response), 3000);

    /* ② SIM 卡就绪 */
    for (int try = 0; try < CELL_MODEM_BOOT_CPIN_RETRIES; try++) {
        memset(response, 0, sizeof(response));
        esp_err_t ret = cell_modem_at_send("AT+CPIN?", response, sizeof(response),
                                      CELL_MODEM_BOOT_CPIN_TIMEOUT_MS);
        if (ret == ESP_OK && strstr(response, "READY") != NULL) {
            ESP_LOGI(TAG, "boot: CPIN READY");
            s_sim_present = true;
            s_cpin_fail_count = 0;
            break;
        }
        if (try + 1 >= CELL_MODEM_BOOT_CPIN_RETRIES) {
            ESP_LOGE(TAG, "boot: CPIN not READY: %s", response);
            /* 标记 SIM 缺失：让 need_pdp_activation 在下一轮 monitor 被抑制，
             * 看门狗就能正常触发硬复位，避免 AT 任务反复饿死看门狗。 */
            s_sim_present = false;
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* CPIN 后立即开射频，避免长期 stat=0 / CSQ 99,99 */
    cell_modem_boot_enable_rf();
    /* enable_rf 内的 RF diag 已查询 CSQ 并回写 g.csq_value；同步到 info->csq
     * 让 boot_info 日志显示真实值。 */
    if (g.csq_value >= 0) {
        info->csq = g.csq_value;
    }

    /* ③ 模组信息（非阻断） */
    if (cell_modem_at_send("AT+CGSN=1", response, sizeof(response), 5000) == ESP_OK) {
        cell_modem_extract_leading_digits(response, info->imei, sizeof(info->imei), 15);
    }
    if (cell_modem_at_send("AT+CIMI", response, sizeof(response), 5000) == ESP_OK) {
        cell_modem_extract_leading_digits(response, info->imsi_plmn, sizeof(info->imsi_plmn), 5);
    }
    if (cell_modem_at_send("AT+CCID", response, sizeof(response), 5000) != ESP_OK ||
        !cell_modem_parse_iccid(response, info->iccid, sizeof(info->iccid))) {
        memset(response, 0, sizeof(response));
        if (cell_modem_at_send("AT+MCCID", response, sizeof(response), 5000) == ESP_OK) {
            cell_modem_parse_iccid(response, info->iccid, sizeof(info->iccid));
        }
    }
    if (cell_modem_at_send("AT+CGMR", response, sizeof(response), 5000) == ESP_OK) {
        const char *ver = strstr(response, "\r\n");
        if (ver != NULL) {
            ver += 2;
            const char *end = strstr(ver, "\r\n");
            size_t n = end ? (size_t)(end - ver) : strlen(ver);
            if (n >= sizeof(info->fw_version)) {
                n = sizeof(info->fw_version) - 1;
            }
            memcpy(info->fw_version, ver, n);
            info->fw_version[n] = '\0';
        }
    }

    const char *apn_preview = cell_modem_resolve_apn_for_dial(info);
    const char *apn_tag = (g.config.apn != NULL && g.config.apn[0] != '\0') ? "forced" : "auto";
    ESP_LOGI(TAG, "boot info: CSQ=%d, IMEI=%s, ICCID=%s, IMSI=%s, APN=%s (%s)",
             info->csq, info->imei[0] ? info->imei : "?",
             info->iccid[0] ? info->iccid : "?",
             info->imsi_plmn[0] ? info->imsi_plmn : "?",
             apn_preview, apn_tag);

    /* ④ 网络注册轮询（CEREG?）——拨号前置条件
     *
     * 实测 ML307 开机后会主动搜网（非早期注释所述「不主动搜网」），通常
     * 5-15s 内 CEREG stat 置 1（本地网）或 5（漫游）。等驻网后再拨号，
     * MIPCALL/MDIALUP 首次成功率显著提升，避免进 CGATT 兜底链浪费时间。
     *
     * 轮询策略（见 ml307-at-boot-sequence.md §4a）：
     *   - stat=1/5：驻网成功，进入拨号
     *   - stat=0 持续 15s 不动：发 AT+CFUN=1 + AT+COPS=0 触发搜网
     *   - 总超时 60s：超时时仍允许尝试拨号（RNDIS 可能已自动拨通），
     *     但保留 ESP_ERR_TIMEOUT 便于上层触发硬复位重试。
     */
    ESP_LOGI(TAG, "boot: waiting for CEREG registration...");
    TickType_t cereg_start = xTaskGetTickCount();
    bool registered = false;
    bool kicked_search = false;
    TickType_t stat_zero_start = cereg_start;

    while ((xTaskGetTickCount() - cereg_start) < pdMS_TO_TICKS(CELL_MODEM_BOOT_CEREG_TOTAL_MS)) {
        memset(response, 0, sizeof(response));
        if (cell_modem_at_send("AT+CEREG?", response, sizeof(response), 5000) == ESP_OK) {
            /* +CEREG: <n>,<stat>[,...]：取前两个逗号分隔的整数为 n 和 stat */
            int cereg_n = -1, stat = -1;
            const char *cereg = strstr(response, "+CEREG:");
            if (cereg != NULL) {
                sscanf(cereg, "+CEREG: %d,%d", &cereg_n, &stat);
            }

            if (stat == 1 || stat == 5) {
                uint32_t waited_ms = (uint32_t)((xTaskGetTickCount() - cereg_start) * portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "boot: CEREG stat=%d (%s), waited %lums",
                         stat, stat == 1 ? "home" : "roaming", (unsigned long)waited_ms);
                registered = true;
                break;
            }
            if (stat == 0) {
                /* stat=0 持续 15s：发一次 CFUN=1 + COPS=0 */
                if (!kicked_search &&
                    (xTaskGetTickCount() - stat_zero_start) >= pdMS_TO_TICKS(CELL_MODEM_BOOT_CEREG_KICK_MS)) {
                    ESP_LOGW(TAG, "boot: CEREG stat=0 stuck %ds, kick search (CFUN=1, COPS=0)",
                             CELL_MODEM_BOOT_CEREG_KICK_MS / 1000);
                    (void)cell_modem_at_send("AT+CFUN=1", response, sizeof(response), 5000);
                    (void)cell_modem_at_send("AT+COPS=0", response, sizeof(response), 5000);
                    kicked_search = true;
                }
            } else {
                /* stat=2（搜网中）、3（拒绝）等：重置 stat=0 计时 */
                stat_zero_start = xTaskGetTickCount();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_BOOT_CEREG_POLL_MS));
    }

    if (!registered) {
        ESP_LOGW(TAG, "boot: CEREG not registered after %ds, attempting dial anyway "
                      "(RNDIS may have auto-dialed)", CELL_MODEM_BOOT_CEREG_TOTAL_MS / 1000);
        /* 不直接返回失败：实测模组可能已通过 RNDIS 自动拨号，
         * 由 cell_modem_rndis_dial 中的 MIPCALL? / MDIALUP? 查询兜底确认。
         * 返回 TIMEOUT 让 at_config_task 在拨号也失败时触发硬复位重试。 */
    }

    return ESP_OK;
}

/** CGATT/CGACT 兜底路径（MIPCALL+MDIALUP 主路径都失败时） */
static esp_err_t cell_modem_dial_fallback_cgatt(char *ipv4_out, size_t ipv4_len)
{
    char response[384];
    esp_err_t ret;

    ESP_LOGW(TAG, "dial: fallback CGATT/CGACT...");

    ret = cell_modem_at_send("AT+CGATT?", response, sizeof(response), 5000);
    if (ret == ESP_OK && strstr(response, "+CGATT: 1") == NULL) {
        for (int attach_try = 0; attach_try < CELL_MODEM_CGATT_RETRY_COUNT; attach_try++) {
            ret = cell_modem_at_send_checked("AT+CGATT=1", response, sizeof(response),
                                        CELL_MODEM_CGATT_TIMEOUT_MS);
            if (ret == ESP_OK) {
                break;
            }
            if (attach_try + 1 < CELL_MODEM_CGATT_RETRY_COUNT) {
                ESP_LOGW(TAG, "dial: CGATT retry (%d/%d)...",
                         attach_try + 1, CELL_MODEM_CGATT_RETRY_COUNT);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "dial: CGATT fallback failed");
            return ret;
        }
    }

    ret = cell_modem_at_send("AT+CGACT?", response, sizeof(response), 5000);
    if (ret != ESP_OK || strstr(response, "+CGACT: 1,1") == NULL) {
        ret = cell_modem_at_send_checked("AT+CGACT=1,1", response, sizeof(response), 15000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "dial: CGACT fallback failed");
            return ret;
        }
    }

    cell_modem_at_send("AT+CGPADDR=1", response, sizeof(response), 5000);

    ret = cell_modem_at_send_collect("AT+MDIALUP=1,1", response, sizeof(response),
                                CELL_MODEM_DIAL_MDIALUP_MS, "+MDIALUP:");
    if (ret != ESP_OK) {
        return ret;
    }
    if (!cell_modem_parse_mdialup_ipv4(response, ipv4_out, ipv4_len)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** ⑤ RNDIS 拨号：MIPCALL（AT 栈）+ MDIALUP（RNDIS 桥接）→ CGATT 兜底
 *
 * 路径设计：
 *   1. AT+MIPCALL=1,1   — 激活模组内部 AT socket TCP/IP 栈（开机常自动 up）
 *   2. AT+MDIALUP=1,1   — 激活 RNDIS/USB 网卡数据桥接（RNDIS 网口真实数据面）
 *   3. CGATT/CGACT       — GPRS 附着兜底（信号弱/未驻网时，最慢）
 *
 * 关键：MIPCALL 与 MDIALUP 是两条独立语义——MIPCALL 仅激活 AT 栈，不必然激活
 * RNDIS 桥接。模组开机自动 MIPCALL 时，RNDIS 网口能 DHCP、本地(网关 IP)能应答 DNS，
 * 但若 MDIALUP 未激活，TCP connect/ICMP 会全部超时（SYN 无响应）。因此 MIPCALL 与
 * MDIALUP 必须「都」激活才视为数据面就绪；MDIALUP 幂等失败（已被 MIPCALL 覆盖）不致命。 */
static esp_err_t cell_modem_rndis_dial(const char *apn_unused, cell_modem_boot_info_t *info)
{
    (void)apn_unused;
    char response[512];
    char ipv4[16] = "";

    const char *apn = cell_modem_resolve_apn_for_dial(info);

    /* 5.1 CGDCONT? / 必要时写入 */
    if (cell_modem_at_send("AT+CGDCONT?", response, sizeof(response), 5000) == ESP_OK) {
        if (apn[0] != '\0' && strstr(response, apn) == NULL) {
            char cmd[160];
            snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IPV4V6\",\"%s\"", apn);
            if (cell_modem_at_send_checked(cmd, response, sizeof(response), 5000) != ESP_OK) {
                ESP_LOGW(TAG, "dial: CGDCONT set failed, continuing...");
            } else {
                ESP_LOGI(TAG, "dial: CGDCONT %s IPV4V6", apn);
            }
        } else {
            ESP_LOGI(TAG, "dial: CGDCONT already has %s", apn);
        }
    }

    /* 5.2 COPS?（数字 PLMN，日志） */
    {
        char plmn[8] = "";
        int act = -1;
        if (cell_modem_query_cops_numeric(response, sizeof(response),
                                    plmn, sizeof(plmn), &act) == ESP_OK) {
            ESP_LOGI(TAG, "dial: COPS %s act=%d", plmn, act);
        }
    }

    /* 5.3 MIPCALL? — 先查是否已拨号（模组开机自动拨号时直接命中），等待 */
    bool mipcall_up = false;
    if (cell_modem_at_send("AT+MIPCALL?", response, sizeof(response), 10000) == ESP_OK &&
        cell_modem_parse_mipcall_ipv4(response, ipv4, sizeof(ipv4))) {
        ESP_LOGI(TAG, "dial: MIPCALL? already up, ipv4=%s", ipv4);
        mipcall_up = true;
    } else {
        /* 5.4 MIPCALL=1,1 — 快速主路径（实测 2s 出 URC） */
        ESP_LOGI(TAG, "dial: MIPCALL? not up, dialing via MIPCALL...");
        memset(response, 0, sizeof(response));
        esp_err_t ret = cell_modem_at_send_collect("AT+MIPCALL=1,1", response, sizeof(response),
                                              CELL_MODEM_DIAL_MIPCALL_MS, "+MIPCALL:");
        if (ret == ESP_OK && cell_modem_parse_mipcall_ipv4(response, ipv4, sizeof(ipv4))) {
            ESP_LOGI(TAG, "dial: MIPCALL OK, ipv4=%s", ipv4);
            mipcall_up = true;
        } else {
            ESP_LOGW(TAG, "dial: MIPCALL fail, trying MDIALUP");
        }
    }

    /*
     * 5.5 确保 RNDIS 网桥拨号（MDIALUP）激活——关键修复点。
     *
     * ML307C 的两条「拨号」语义不同：
     *   - MIPCALL：激活模组内部 AT socket TCP/IP 栈（供 AT+MIPOPEN 使用）；
     *   - MDIALUP：激活 RNDIS/USB 网卡数据桥接（把 RNDIS 网口的包转发到蜂窝侧）。
     * 模组开机自动 MIPCALL 时，RNDIS 网桥未必同时激活——表现为 RNDIS 链路 Up、
     * ESP 拿到本地 IP、模组本地(192.168.0.1)能应答 DNS，但 TCP connect/ICMP 全部
     * 超时（SYN 发出无响应）。因此无论 MIPCALL 是否已 up，都必须显式激活/确认
     * MDIALUP。MDIALUP 已激活时再发 =1,1 通常回 +CME ERROR（幂等），按已 up 处理。
     */
    {
        char mdialup_ipv4[16] = "";
        /* 先查 MDIALUP? 是否已 up */
        if (cell_modem_at_send("AT+MDIALUP?", response, sizeof(response), 10000) == ESP_OK &&
            cell_modem_parse_mdialup_ipv4(response, mdialup_ipv4, sizeof(mdialup_ipv4))) {
            ESP_LOGI(TAG, "dial: MDIALUP already up, ipv4=%s (RNDIS bridge ready)",
                     mdialup_ipv4);
        } else {
            ESP_LOGI(TAG, "dial: MDIALUP not up, activating RNDIS bridge...");
            memset(response, 0, sizeof(response));
            esp_err_t ret = cell_modem_at_send_collect("AT+MDIALUP=1,1", response, sizeof(response),
                                                  CELL_MODEM_DIAL_MDIALUP_MS, "+MDIALUP:");
            if (ret == ESP_OK && cell_modem_parse_mdialup_ipv4(response, mdialup_ipv4, sizeof(mdialup_ipv4))) {
                ESP_LOGI(TAG, "dial: MDIALUP OK, ipv4=%s (RNDIS bridge activated)",
                         mdialup_ipv4);
            } else {
                /* MDIALUP 激活失败：某些固件 MIPCALL 已涵盖 RNDIS 桥接，
                 * 此时 MDIALUP 报 CME ERROR 仍可继续；真正无数据面由后续 diag 揭示 */
                ESP_LOGW(TAG, "dial: MDIALUP activate returned %s (may be covered by MIPCALL)",
                         esp_err_to_name(ret));
            }
        }
        /* 优先用 MDIALUP 返回的蜂窝 IP（数据面真实地址），回落 MIPCALL IP */
        if (mdialup_ipv4[0] != '\0') {
            snprintf(ipv4, sizeof(ipv4), "%s", mdialup_ipv4);
        }
    }

    if (mipcall_up || ipv4[0] != '\0') {
        if (info != NULL) {
            snprintf(info->cell_ipv4, sizeof(info->cell_ipv4), "%s", ipv4);
        }
        ESP_LOGI(TAG, "dial: done, ipv4=%s", ipv4);
        cell_modem_notify_pdp_ready();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "dial: MIPCALL+MDIALUP both failed, trying CGATT fallback");

    /* 5.6 CGATT/CGACT 兜底（信号弱/未驻网时） */
    ipv4[0] = '\0';
    esp_err_t ret = cell_modem_dial_fallback_cgatt(ipv4, sizeof(ipv4));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "dial: fallback OK, ipv4=%s", ipv4);
        if (info != NULL) {
            snprintf(info->cell_ipv4, sizeof(info->cell_ipv4), "%s", ipv4);
        }
        cell_modem_notify_pdp_ready();
        return ESP_OK;
    }

    ESP_LOGE(TAG, "dial: all paths failed");
    g.pdp_done = false;
    return ret != ESP_OK ? ret : ESP_FAIL;
}

/** boot + dial 完整序列（替代原 cell_modem_activate_pdp） */
static esp_err_t cell_modem_run_pdp_sequence(const char *apn)
{
    cell_modem_boot_info_t info = {0};
    esp_err_t boot_ret = cell_modem_at_boot_ready(&info);

    if (boot_ret != ESP_OK && boot_ret != ESP_ERR_TIMEOUT) {
        return boot_ret;
    }
    if (boot_ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "boot: CEREG timeout, try MDIALUP anyway (RNDIS may already be up)...");
    }

    esp_err_t dial_ret = cell_modem_rndis_dial(apn, &info);
    if (dial_ret == ESP_OK) {
        if (boot_ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "dial: OK despite CEREG timeout");
        }
        return ESP_OK;
    }
    /* 拨号失败时保留 CEREG 超时，便于触发硬复位 */
    return (boot_ret == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : dial_ret;
}

/* ================================================================
 *  P3 蜂窝数据面诊断（ICMP IP / DNS+域名 / TCP，周期重试，仅日志）
 * ================================================================ */

typedef struct {
    const char *label;
    uint32_t    replies;
    SemaphoreHandle_t done;
} cell_modem_diag_ping_ctx_t;

static void cell_modem_diag_ping_success(esp_ping_handle_t hdl, void *args)
{
    cell_modem_diag_ping_ctx_t *ctx = (cell_modem_diag_ping_ctx_t *)args;
    uint32_t elapsed_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    ctx->replies++;
    ESP_LOGI(TAG, "4G data diag: ICMP reply (%s), %lu ms",
             ctx->label, (unsigned long)elapsed_ms);
}

static void cell_modem_diag_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    cell_modem_diag_ping_ctx_t *ctx = (cell_modem_diag_ping_ctx_t *)args;
    ESP_LOGD(TAG, "4G data diag: ICMP timeout (%s)", ctx->label);
}

static void cell_modem_diag_ping_end(esp_ping_handle_t hdl, void *args)
{
    cell_modem_diag_ping_ctx_t *ctx = (cell_modem_diag_ping_ctx_t *)args;
    esp_ping_delete_session(hdl);
    if (ctx->done != NULL) {
        xSemaphoreGive(ctx->done);
    }
}

/** 绑定 modem netif 发起 ICMP，返回收到 reply 数 */
static uint32_t cell_modem_diag_run_icmp(const char *label, const ip_addr_t *target, int if_idx)
{
    cell_modem_diag_ping_ctx_t ctx = {
        .label = label,
        .replies = 0,
        .done = xSemaphoreCreateBinary(),
    };
    if (ctx.done == NULL) {
        return 0;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = CELL_MODEM_DIAG_PING_COUNT;
    config.interval_ms = 1000;
    config.timeout_ms = 3000;
    config.interface = (uint32_t)if_idx;
    config.target_addr = *target;

    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = cell_modem_diag_ping_success,
        .on_ping_timeout = cell_modem_diag_ping_timeout,
        .on_ping_end = cell_modem_diag_ping_end,
    };

    esp_ping_handle_t ping = NULL;
    if (esp_ping_new_session(&config, &cbs, &ping) != ESP_OK || ping == NULL) {
        ESP_LOGW(TAG, "4G data diag: ICMP session fail (%s)", label);
        vSemaphoreDelete(ctx.done);
        return 0;
    }

    ESP_LOGI(TAG, "4G data diag: ICMP %s via cellular modem (idx=%d)...", label, if_idx);
    if (esp_ping_start(ping) != ESP_OK) {
        esp_ping_delete_session(ping);
        vSemaphoreDelete(ctx.done);
        return 0;
    }

    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(15000));
    vSemaphoreDelete(ctx.done);
    return ctx.replies;
}

/** 将 modem 设为默认网口以便 DNS 走 4G */
static void cell_modem_diag_use_modem_netif(void)
{
    if (g.netif != NULL) {
        esp_netif_set_default_netif(g.netif);
    }
}

/** DNS 解析（须先 cell_modem_diag_use_modem_netif） */
static bool cell_modem_diag_resolve_v4(const char *hostname, ip_addr_t *out, char *ip_str, size_t ip_str_len)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    int ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || res == NULL) {
        ESP_LOGW(TAG, "4G data diag: DNS fail %s (err=%d)", hostname, ret);
        return false;
    }

    struct sockaddr_in *addr4 = (struct sockaddr_in *)res->ai_addr;
    ip4_addr_t ip4 = { .addr = addr4->sin_addr.s_addr };
    ip_addr_copy_from_ip4(*out, ip4);
    if (ip_str != NULL && ip_str_len > 0) {
        inet_ntoa_r(addr4->sin_addr, ip_str, ip_str_len);
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "4G data diag: DNS %s -> %s", hostname, ip_str ? ip_str : "?");
    return true;
}

/** TCP connect 探测（bind 本地 modem IP，验证真实数据面） */
static bool cell_modem_diag_tcp_probe(const char *hostname, uint16_t port, const char *label)
{
    if (!g.net_info.connected || g.net_info.ip[0] == '\0') {
        return false;
    }

    cell_modem_diag_use_modem_netif();

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || res == NULL) {
        ESP_LOGW(TAG, "4G data diag: TCP DNS fail %s (%s)", hostname, label);
        return false;
    }

    char remote_ip[16] = {0};
    struct sockaddr_in *remote = (struct sockaddr_in *)res->ai_addr;
    inet_ntoa_r(remote->sin_addr, remote_ip, sizeof(remote_ip));

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGW(TAG, "4G data diag: TCP socket fail (%s)", label);
        freeaddrinfo(res);
        return false;
    }

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    if (inet_aton(g.net_info.ip, &local.sin_addr) == 0) {
        close(sock);
        freeaddrinfo(res);
        return false;
    }
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGW(TAG, "4G data diag: TCP bind %s fail errno=%d (%s)",
                 g.net_info.ip, errno, label);
        close(sock);
        freeaddrinfo(res);
        return false;
    }

    struct timeval tv = {
        .tv_sec = CELL_MODEM_DIAG_TCP_TIMEOUT_MS / 1000,
        .tv_usec = (CELL_MODEM_DIAG_TCP_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    remote->sin_port = htons(port);
    bool ok = (connect(sock, (struct sockaddr *)remote, sizeof(*remote)) == 0);
    if (ok) {
        ESP_LOGI(TAG, "4G data diag: TCP %s OK (%s:%u, local=%s, pdp_done=%d)",
                 label, remote_ip, (unsigned)port, g.net_info.ip, g.pdp_done);
    } else {
        ESP_LOGW(TAG, "4G data diag: TCP %s FAIL (%s:%u errno=%d, pdp_done=%d)",
                 label, remote_ip, (unsigned)port, errno, g.pdp_done);
    }

    close(sock);
    freeaddrinfo(res);
    return ok;
}

static bool cell_modem_diag_run_round(int if_idx)
{
    ip_addr_t target = {0};
    char resolved_ip[16] = {0};
    uint32_t icmp_dom_replies = 0;
    bool dns_domain_ok = false;
    bool tcp_cloud_ok = false;

    cell_modem_diag_use_modem_netif();

    /* 1. 业务路径：TCP 云域名（主探针，host 由 cell_modem_config_t 注入） */
    if (g.config.diag_tcp_host != NULL && g.config.diag_tcp_host[0] != '\0') {
        uint16_t port = g.config.diag_tcp_port ? g.config.diag_tcp_port : CELL_MODEM_DIAG_TCP_PORT;
        tcp_cloud_ok = cell_modem_diag_tcp_probe(g.config.diag_tcp_host, port, "cloud_https");
    }

    /* 2. DNS + ICMP 域名（验证模组 DNS，辅探针） */
    if (cell_modem_diag_resolve_v4(CELL_MODEM_DIAG_PING_DOMAIN, &target, resolved_ip, sizeof(resolved_ip))) {
        dns_domain_ok = true;
        char label[48];
        snprintf(label, sizeof(label), "%s(%s)", CELL_MODEM_DIAG_PING_DOMAIN, resolved_ip);
        icmp_dom_replies = cell_modem_diag_run_icmp(label, &target, if_idx);
    }

    ESP_LOGI(TAG,
             "4G data diag round: tcp_cloud=%s dns_%s=%s icmp_dom=%lu/%d pdp_done=%d",
             tcp_cloud_ok ? "OK" : "FAIL",
             CELL_MODEM_DIAG_PING_DOMAIN, dns_domain_ok ? "OK" : "FAIL",
             (unsigned long)icmp_dom_replies, CELL_MODEM_DIAG_PING_COUNT, g.pdp_done);

    if (tcp_cloud_ok) {
        if (!s_data_path_ok) {
            ESP_LOGI(TAG, "4G data diag: data path verified via TCP cloud");
        }
        s_data_path_ok = true;
    } else if (icmp_dom_replies > 0) {
        s_data_path_ok = true;
    }

    if (!tcp_cloud_ok && !dns_domain_ok) {
        ESP_LOGW(TAG, "4G data diag: DNS and TCP both fail — check PDP/APN or wait next round");
    } else if (dns_domain_ok && !tcp_cloud_ok) {
        ESP_LOGW(TAG, "4G data diag: DNS OK but TCP cloud fail — data may not be fully up");
    } else if (tcp_cloud_ok && icmp_dom_replies == 0) {
        ESP_LOGI(TAG, "4G data diag: TCP OK, ICMP domain 0/%d (ICMP optional)",
                 CELL_MODEM_DIAG_PING_COUNT);
    }

    if (s_data_path_ok && !g.pdp_done) {
        ESP_LOGW(TAG, "4G data diag: path OK but pdp_done=0 (modem may auto-dial before AT)");
    }

    return tcp_cloud_ok;
}

static void cell_modem_data_diag_task(void *arg)
{
    bool ever_passed = false;

    while (!s_data_diag_stop && g.netif != NULL && g.state == CELL_MODEM_STATE_GOT_IP) {
        /* 须等 AT PDP 完成后再测，避免 GOT_IP 误报 */
        while (!s_data_diag_stop && g.state == CELL_MODEM_STATE_GOT_IP && !g.pdp_done) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (s_data_diag_stop || g.state != CELL_MODEM_STATE_GOT_IP || !g.pdp_done) {
            break;
        }

        int if_idx = esp_netif_get_netif_impl_index(g.netif);
        bool round_ok = false;
        if (if_idx <= 0) {
            ESP_LOGW(TAG, "4G data diag: skip round, bad netif idx=%d", if_idx);
        } else {
            round_ok = cell_modem_diag_run_round(if_idx);
        }

        if (round_ok) {
            /* 通过：切到慢速周期复查模式，持续守门数据面真实可达。
             * 不再像旧版那样「通过一次就退出」——否则信号变差/RNDIS 链路
             * 未断时数据面已丢，s_data_path_ok 仍恒 true，应用层误判正常。 */
            if (!ever_passed) {
                ESP_LOGI(TAG, "4G data diag: passed, switch to slow re-verify (%ds)",
                         (int)(CELL_MODEM_DIAG_REVERIFY_MS / 1000));
                ever_passed = true;
            }
            for (uint32_t waited = 0; waited < CELL_MODEM_DIAG_REVERIFY_MS && !s_data_diag_stop; waited += 1000) {
                if (g.state != CELL_MODEM_STATE_GOT_IP) break;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            /* 失败：若之前曾通过，说明数据面在「链路有 IP」前提下丢失
             * （典型场景：信号差到 PDP 掉但 RNDIS 链路未断、SIM 拔插、
             *  运营商侧路由失效）。降级状态 → 4G 看门狗 30s 后硬复位 →
             *  信号恢复后自动重拨。 */
            if (ever_passed) {
                ESP_LOGE(TAG, "4G data diag: data path LOST after previous success — "
                              "invalidating PDP/data_path, dropping to CONNECTED "
                              "(watchdog will hard-reset to reconnect)");
                cell_modem_stop_data_diag();   /* 清 s_data_path_ok + 置 stop */
                g.pdp_done = false;
                cell_modem_update_state(CELL_MODEM_STATE_CONNECTED);
                break;
            }
            /* 从未通过：继续快速重试（原逻辑） */
            for (uint32_t waited = 0; waited < CELL_MODEM_DIAG_RETRY_MS && !s_data_diag_stop; waited += 1000) {
                if (g.state != CELL_MODEM_STATE_GOT_IP) break;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    s_data_diag_task = NULL;
    vTaskDelete(NULL);
}

static void cell_modem_start_data_diag(void)
{
    if (!g.config.enable_diag) {
        return;
    }
    if (s_data_diag_task != NULL) {
        return;
    }
    s_data_diag_stop = false;
    s_data_path_ok = false;
    if (xTaskCreate(cell_modem_data_diag_task, "cell_modem_diag", 4096, NULL, 2, &s_data_diag_task) != pdPASS) {
        ESP_LOGW(TAG, "4G data diag: failed to create task");
        s_data_diag_task = NULL;
    }
}

static void cell_modem_stop_data_diag(void)
{
    s_data_diag_stop = true;
    s_data_path_ok = false;
}

/* ================================================================
 *  RNDIS 模式配置
 * ================================================================ */

/**
 * 配置 modem 为 RNDIS 模式（先激活 PDP，再发 RNDIS 切换命令）
 */
static esp_err_t cell_modem_configure_rndis_mode(void)
{
    char response[256];

    ESP_LOGI(TAG, "Configuring modem to RNDIS mode...");

    esp_err_t ret = cell_modem_run_pdp_sequence(g.config.apn);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to activate PDP before RNDIS switch");
        return ret;
    }

    /* AT+MDIALUPCFG="mode",0 → 0=RNDIS, 1=ECM */
    ret = cell_modem_at_send_checked("AT+MDIALUPCFG=\"mode\",0", response, sizeof(response), 5000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send RNDIS config command");
        return ret;
    }

    ESP_LOGI(TAG, "RNDIS mode configured, device will re-enumerate...");

    /* 关闭 AT 端口，设备将重新枚举 */
    vTaskDelay(pdMS_TO_TICKS(500));
    cell_modem_close_at_port();

    g.rndis_configured = true;
    g.rndis_config_time = xTaskGetTickCount();  /* 记录配置时刻，用于恢复超时检测 */
    ESP_LOGI(TAG, "Waiting for RNDIS re-enumeration (up to 20s for link up, then retry)...");
    return ESP_OK;
}

/* ================================================================
 *  AT 配置任务
 * ================================================================ */

static void at_config_task(void *arg)
{
    ESP_LOGI(TAG, "AT config task started");

    bool need_rndis_config = g.config.auto_configure_rndis && !g.rndis_configured;
    bool need_pdp_activation = g.config.auto_activate_pdp && !g.pdp_done;

    if (!need_rndis_config && !need_pdp_activation) {
        ESP_LOGI(TAG, "No AT-side modem setup needed");
        g.at_config_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    if (need_rndis_config) {
        cell_modem_update_state(CELL_MODEM_STATE_CONFIGURING);
    } else if (need_pdp_activation && g.state < CELL_MODEM_STATE_CONNECTED) {
        /* RNDIS 已 Up 时勿回退到 CONNECTING，避免日志 State: 3→2 误导 */
        cell_modem_update_state(CELL_MODEM_STATE_CONNECTING);
    }

    if (g.at_mutex != NULL) {
        xSemaphoreTake(g.at_mutex, portMAX_DELAY);
    }

    /* 等待 USB 设备稳定（仅在需要 RNDIS 配置时；PDP 激活可直接进行） */
    if (need_rndis_config) {
        ESP_LOGI(TAG, "Waiting for device to stabilize (cdc_connected=%d, dev_addr=%d)...",
                 g.cdc_connected, g.dev_addr);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* 扫描 / 复用 AT 端口（持 mutex，避免 monitor CSQ 抢占） */
    bool found = cell_modem_ensure_at_port();

    if (found) {
        esp_err_t ret = ESP_OK;

        if (need_rndis_config) {
            ESP_LOGI(TAG, "Configuring RNDIS mode...");
            ret = cell_modem_configure_rndis_mode();
        } else if (need_pdp_activation) {
            ESP_LOGI(TAG, "Activating PDP/dialup only...");
            ret = cell_modem_run_pdp_sequence(g.config.apn);
        }

        if (ret != ESP_OK) {
            /* CEREG/ boot 超时：硬复位模组后由 monitor 下一轮重试（限次数） */
            if (ret == ESP_ERR_TIMEOUT && need_pdp_activation && !need_rndis_config &&
                s_cereg_hw_reset_count < CELL_MODEM_BOOT_CEREG_HW_RESET_MAX) {
                s_cereg_hw_reset_count++;
                ESP_LOGW(TAG, "Boot timeout, modem hardware reset (%d/%d)...",
                         s_cereg_hw_reset_count, CELL_MODEM_BOOT_CEREG_HW_RESET_MAX);
                cell_modem_close_at_port();
                cell_modem_hardware_reset();
            }
            /* 配置失败：如果 RNDIS 链路已建立（CONNECTED/GOT_IP），不降级到 ERROR。
             * RNDIS 模式下 PDP 由模组内部处理，AT 激活失败不代表 4G 数据不通。
             * 只有从未连上时才标记错误状态。 */
            if (g.state >= CELL_MODEM_STATE_CONNECTED) {
                ESP_LOGW(TAG, "PDP/RNDIS config failed but link is up (state=%d), ignoring", g.state);
            } else {
                cell_modem_update_state(CELL_MODEM_STATE_ERROR);
            }
        } else if (!need_rndis_config && g.pdp_done) {
            s_cereg_hw_reset_count = 0;
            /* 拨号成功，保持 RNDIS 链路状态（GOT_IP 不降级） */
            if (g.state < CELL_MODEM_STATE_CONNECTED) {
                cell_modem_update_state(g.rndis_ever_connected ? CELL_MODEM_STATE_CONNECTED : CELL_MODEM_STATE_CONNECTING);
            }
            ESP_LOGI(TAG, "PDP/dialup done, state=%d, pdp_done=1", g.state);
        }

        /* 关闭 AT 端口 — 仅在 RNDIS 模式切换时需要（设备会重新枚举，AT 接口消失）。
         * PDP-only 模式下保持 AT 端口打开，用于监控任务周期性 CSQ 查询。 */
        if (need_rndis_config && g.at_port) {
            usbh_cdc_port_close(g.at_port);
            g.at_port = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Failed to find modem AT port");
        if (!g.rndis_ever_connected) {
            cell_modem_update_state(CELL_MODEM_STATE_CONNECTING);
        }
    }

    if (g.at_mutex != NULL) {
        xSemaphoreGive(g.at_mutex);
    }

    g.at_config_in_progress = false;
    g.at_config_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  监控任务
 * ================================================================ */

static void cell_modem_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Monitor task started");

    /* 短暂等待让 USB 枚举/事件回调把 dev_addr、cdc_connected 写入；
     * 实测复位释放后 ~750ms 设备枚举完成，1.5s 足够覆盖 */
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* 记录监控任务启动时间，作为看门狗基准（last_got_ip_tick == 0 时使用） */
    g.monitor_start_tick = xTaskGetTickCount();

    while (g.event_group != NULL) {
        bool need_rndis_config = g.config.auto_configure_rndis && !g.rndis_configured;
        /* SIM 不在位时不再尝试 PDP 软重拨（CPIN 必然失败，纯属浪费 AT 口），
         * 交由看门狗硬复位模组来重新识别 SIM 热插拔。 */
        bool need_pdp_activation = g.config.auto_activate_pdp && !g.pdp_done && s_sim_present;

        /* RNDIS 链路已连接时不需要重新配置 USB Config。
         * 但 PDP 激活仍然需要——modem 直接以 RNDIS 模式启动时，
         * RNDIS DHCP 分配了本地 IP，但 4G 数据拨号可能尚未执行。 */
        if (g.state >= CELL_MODEM_STATE_CONNECTED) {
            need_rndis_config = false;
        }

        if ((need_rndis_config || need_pdp_activation) && !g.at_config_in_progress) {
            /* 仅 PDP：须 RNDIS 链路 Up；驻网等待由 boot CEREG 轮询完成 */
            if (need_pdp_activation && !need_rndis_config) {
                if (g.state < CELL_MODEM_STATE_CONNECTED) {
                    goto monitor_sleep;
                }
            }
            ESP_LOGI(TAG, "Starting AT config task from monitor...");
            g.at_config_in_progress = true;
            g.at_config_start_tick = xTaskGetTickCount();
            BaseType_t ret = xTaskCreate(at_config_task, "cell_modem_atcfg",
                                          6144, NULL, 5, &g.at_config_task);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create AT config task");
                g.at_config_in_progress = false;
                g.at_config_task = NULL;
            }
        }

        /* ============================================================
         *  RNDIS 恢复机制
         *
         *  问题场景：AT 配置发送 AT+MDIALUPCFG="mode",0 后，modem
         *  重新枚举为 RNDIS 设备。但 ESP32-P4 的 RNDIS 驱动可能因为
         *  时序问题未能检测到重新枚举的设备，导致链路永远不建立。
         *
         *  恢复策略：如果 RNDIS 已配置 + PDP 已激活，但链路在 20s 内
         *  仍未建立（state < CONNECTED），则硬复位 modem 重新走整个
         *  流程。最多重试 3 次。
         *
         *  注意：AT 配置任务正在运行时跳过（避免打断正在使用的 USB 端口）。
         * ============================================================ */
        if (g.rndis_configured && g.pdp_done &&
            g.state < CELL_MODEM_STATE_CONNECTED &&
            g.recovery_count < 3 &&
            !g.at_config_in_progress &&
            (xTaskGetTickCount() - g.rndis_config_time) > pdMS_TO_TICKS(20000)) {

            g.recovery_count++;
            ESP_LOGW(TAG, "RNDIS link not up after 20s (state=%d, cdc_connected=%d), "
                          "re-resetting modem (attempt %d/3)",
                     g.state, g.cdc_connected, g.recovery_count);

            /* 清除标志，让 monitor 重新触发 AT 配置 */
            g.rndis_configured = false;
            g.pdp_done = false;
            g.cdc_connected = false;
            cell_modem_close_at_port();
            cell_modem_update_state(CELL_MODEM_STATE_DISCONNECTED);

            /* 硬复位 modem，强制 USB 重新枚举 */
            cell_modem_hardware_reset();

            /* 复位后立即重新检查（跳过 10s 延迟），尽快启动 AT 配置 */
            continue;
        }

        /* ============================================================
         *  4G 连接看门狗
         *
         *  问题场景：modem 可能因基站信号丢失后模块挂死、USB 断连后
         *  无法自动恢复、RNDIS 恢复机制 3 次重试耗尽等情况，长时间
         *  停留在非 GOT_IP 状态。此时需要强制硬复位 modem 芯片，
         *  重新走整个 AT 配置 + PDP 激活 + RNDIS 拨号流程。
         *
         *  策略：从最后一次成功获取 IP（或监控任务启动）算起，
         *  超过 CELL_MODEM_WATCHDOG_TIMEOUT_MS 仍未进入 GOT_IP 状态则复位。
         *
         *  注意：AT 配置任务正在运行时跳过（避免打断正在进行的配置）。
         *  但如果 AT 任务自身超时（> CELL_MODEM_AT_TASK_TIMEOUT_MS），视为
         *  卡死，强制终止并触发硬复位。
         * ============================================================ */
        bool at_task_stuck = g.at_config_in_progress &&
                             (xTaskGetTickCount() - g.at_config_start_tick) > pdMS_TO_TICKS(CELL_MODEM_AT_TASK_TIMEOUT_MS);
        if (g.state != CELL_MODEM_STATE_GOT_IP && (!g.at_config_in_progress || at_task_stuck)) {
            TickType_t ref_tick = g.last_got_ip_tick;
            if (ref_tick == 0) {
                ref_tick = g.monitor_start_tick;  /* 从未连上，以监控启动时间为基准 */
            }
            TickType_t elapsed = xTaskGetTickCount() - ref_tick;
            if (elapsed > pdMS_TO_TICKS(CELL_MODEM_WATCHDOG_TIMEOUT_MS)) {
                if (at_task_stuck) {
                    ESP_LOGW(TAG, "AT config task stuck for %ds, killing it...",
                             (int)((xTaskGetTickCount() - g.at_config_start_tick) * portTICK_PERIOD_MS / 1000));
                    if (g.at_config_task) {
                        vTaskDelete(g.at_config_task);
                        g.at_config_task = NULL;
                    }
                    cell_modem_close_at_port();
                }
                g.watchdog_reset_count++;
                ESP_LOGW(TAG, "4G watchdog: no connection for %ds (state=%d, cdc=%d, resets=%d), "
                              "forcing hardware reset...",
                         CELL_MODEM_WATCHDOG_TIMEOUT_MS / 1000, g.state,
                         g.cdc_connected, g.watchdog_reset_count);

                /* 重置所有内部标志，让 monitor 的正常流程重新配置 */
                g.recovery_count = 0;
                g.rndis_configured = false;
                g.pdp_done = false;
                g.cdc_connected = false;
                g.at_config_in_progress = false;
                cell_modem_close_at_port();
                memset(&g.net_info, 0, sizeof(g.net_info));

                /* 硬复位后重新走 boot 序列验证 CPIN，不要继承 SIM 缺失状态 —
                 * 否则 need_pdp_activation 会被抑制，导致新一轮看门狗也不会触发。 */
                s_sim_present = true;
                s_cpin_fail_count = 0;

                cell_modem_update_state(CELL_MODEM_STATE_DISCONNECTED);

                /* 硬复位 modem，强制 USB 重新枚举 */
                cell_modem_hardware_reset();

                /* 重置看门狗基准时间，避免复位后立即再次触发 */
                g.monitor_start_tick = xTaskGetTickCount();
                g.last_got_ip_tick = 0;

                continue;  /* 跳过本次循环，让正常流程接管重新配置 */
            }
        }

        /* 已获取 IP 时定期查询 CSQ + CPIN（PDP/AT 配置未完成时不抢 AT 口） */
        if (g.state == CELL_MODEM_STATE_GOT_IP && !g.at_config_in_progress &&
            !(g.config.auto_activate_pdp && !g.pdp_done)) {
            g.recovery_count = 0;  /* 连接成功，清除恢复计数 */
            /* AT 端口在 PDP 激活后保持打开（未发生 RNDIS 模式切换时），
             * 直接用于周期性 CSQ / CPIN 查询。如果发生了 RNDIS 模式切换
             * （复合模式→纯 RNDIS），AT 端口已关闭，此处跳过——
             * 纯 RNDIS 模式下不支持 SIM 热插拔检测与 CSQ 周期更新。 */
            if (g.at_port != NULL && xSemaphoreTake(g.at_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                char response[128];

                /* CSQ：信号强度周期刷新（rssi==99 不更新，保留上次有效值） */
                if (cell_modem_at_send("AT+CSQ", response, sizeof(response), 3000) == ESP_OK) {
                    char *p = strstr(response, "+CSQ:");
                    if (p) {
                        p += 5;
                        while (*p == ' ') p++;
                        int rssi = atoi(p);
                        if (rssi >= 0 && rssi <= 31) {
                            g.csq_value = rssi;
                        }
                    }
                }

                /* CPIN：SIM 在位周期复检。连续 N 次 NOT READY/ERROR 才认定 SIM 丢失，
                 * 避免小区切换/RF 重配瞬态误报。检测到丢失后仅降状态 + 清标志，
                 * 不主动硬复位——复位策略交由上层或看门狗决定。
                 * SIM 重新插入后 monitor 的 need_pdp_activation 路径会自动重拨。 */
                if (cell_modem_at_send("AT+CPIN?", response, sizeof(response), 3000) == ESP_OK) {
                    if (strstr(response, "READY") != NULL) {
                        if (s_cpin_fail_count > 0 || !s_sim_present) {
                            ESP_LOGI(TAG, "CPIN: READY (recovered, was fail_count=%d)",
                                     s_cpin_fail_count);
                        }
                        s_cpin_fail_count = 0;
                        s_sim_present = true;
                    } else {
                        s_cpin_fail_count++;
                        ESP_LOGW(TAG, "CPIN: not READY (%d/%d): %s",
                                 s_cpin_fail_count, CELL_MODEM_CPIN_FAIL_THRESHOLD,
                                 response[0] ? response : "(empty)");
                        if (s_cpin_fail_count >= CELL_MODEM_CPIN_FAIL_THRESHOLD &&
                            s_sim_present) {
                            s_sim_present = false;
                            ESP_LOGE(TAG, "SIM removed/lost after %d consecutive CPIN failures — "
                                          "invalidating PDP/data_path state",
                                     s_cpin_fail_count);
                            g.pdp_done = false;
                            cell_modem_stop_data_diag();  /* 清 s_data_path_ok */
                            /* 降到 CONNECTED（链路在但数据不通），让 4G 看门狗
                             * 30s 后硬复位 → SIM 重新插入后自动重拨。
                             * 不在这里直接复位，避免用户临时换卡也被强制重启。 */
                            cell_modem_update_state(CELL_MODEM_STATE_CONNECTED);
                        }
                    }
                }

                xSemaphoreGive(g.at_mutex);
            }
        }

monitor_sleep:
        vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_MON_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Monitor task stopped");
    g.monitor_task = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 *  USB 枚举过滤器
 * ================================================================ */

/**
 * USB 设备枚举过滤器：选择 Config 1（RNDIS 模式）
 * modem 有多个 USB 配置：Config 1=RNDIS, Config 2=ECM
 */
static bool usb_host_enum_filter_cb(const usb_device_desc_t *dev_desc,
                                     uint8_t *bConfigurationValue)
{
    ESP_LOGI(TAG, "USB dev: VID=%04X PID=%04X class=%02X configs=%d",
             dev_desc->idVendor, dev_desc->idProduct,
             dev_desc->bDeviceClass, dev_desc->bNumConfigurations);

    *bConfigurationValue = 1;  /* Config 1 = RNDIS */
    ESP_LOGI(TAG, "USB selected config %d", *bConfigurationValue);

    /* 检测 modem 设备 */
    if (dev_desc->idVendor == cell_modem_usb_vid()) {
        ESP_LOGI(TAG, "USB cellular modem detected");
        /* 纯 RNDIS PID 与 RNDIS+AT 复合 PID 均已有 RNDIS 接口（Config 1 = RNDIS），
         * 无需再发 AT+MDIALUPCFG 切换模式。
         *
         * 关键：若对复合模式执行模式切换，设备会重新枚举为
         * 纯 RNDIS，AT 接口消失，无法查询 CSQ。
         * 保持复合模式 = RNDIS 数据 + AT 指令共存。 */
        if (dev_desc->idProduct == cell_modem_usb_pid_rndis() ||
            dev_desc->idProduct == cell_modem_usb_pid_composite()) {
            ESP_LOGI(TAG, "RNDIS already available (PID:%04X), skip mode switch",
                     dev_desc->idProduct);
            g.rndis_configured = true;
            /* 必须同时记录配置时刻，否则恢复机制会因
             * (tick - 0) > 20s 永远为真而误触发硬复位 */
            g.rndis_config_time = xTaskGetTickCount();
        }
    }

    return true;
}

/* ================================================================
 *  CDC 设备事件回调
 * ================================================================ */

static void cell_modem_cdc_event_cb(usbh_cdc_device_event_t event,
                                usbh_cdc_device_event_data_t *event_data,
                                void *user_ctx)
{
    if (event == CDC_HOST_DEVICE_EVENT_CONNECTED) {
        const usb_device_desc_t *dev_desc = event_data->new_dev.device_desc;
        if (dev_desc->idVendor == cell_modem_usb_vid()) {
            const char *mode = (dev_desc->idProduct == cell_modem_usb_pid_rndis()) ? "RNDIS" : "AT/other";
            ESP_LOGI(TAG, "CDC device connected: VID:%04X PID:%04X (%s), addr=%d",
                     dev_desc->idVendor, dev_desc->idProduct, mode,
                     event_data->new_dev.dev_addr);
            /* 仅复合模式带 AT 口；纯 RNDIS 幽灵枚举会浪费 EP0 channel */
            if (dev_desc->idProduct == cell_modem_usb_pid_composite()) {
                g.dev_addr = event_data->new_dev.dev_addr;
                g.cdc_connected = true;
            }
        }
    } else if (event == CDC_HOST_DEVICE_EVENT_DISCONNECTED) {
        if (event_data->dev_gone.dev_addr == g.dev_addr) {
            ESP_LOGW(TAG, "modem CDC device disconnected");
            g.cdc_connected = false;
            if (g.at_port) {
                g.at_port = NULL;
            }
        }
    }
}

/* ================================================================
 *  事件处理器
 * ================================================================ */

static void cell_modem_eth_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    switch (event_id) {
    case IOT_ETH_EVENT_CONNECTED:
        ESP_LOGI(TAG, "cellular modem Link Up (RNDIS interface ready)");
        xEventGroupSetBits(g.event_group, EVENT_RNDIS_CONNECTED_BIT);
        cell_modem_update_state(CELL_MODEM_STATE_CONNECTED);
        s_rndis_connected_tick = xTaskGetTickCount();
        /* RNDIS 链路已连接，说明 USB Config 正确 */
        g.rndis_configured = true;
        /* 不在 RNDIS 连接时触发 AT 配置任务——RNDIS 模式下没有 AT 端口。
         * 如果 PDP 未激活，监控任务的恢复机制（硬复位）会处理。 */
        g.rndis_ever_connected = true;
        break;

    case IOT_ETH_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "cellular modem Link Down");
        xEventGroupClearBits(g.event_group, EVENT_RNDIS_CONNECTED_BIT | EVENT_RNDIS_GOT_IP_BIT);
        cell_modem_update_state(CELL_MODEM_STATE_DISCONNECTED);
        s_rndis_connected_tick = 0;
        cell_modem_stop_data_diag();
        memset(&g.net_info, 0, sizeof(g.net_info));
        /* 注意：不清除 g.pdp_done。PDP 上下文存储在 modem 模组固件中，
         * 不会因 USB 断开/重新枚举而丢失。仅在硬件复位时才需重新激活。 */
        break;

    default:
        break;
    }
}

static void cell_modem_ip_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        /* 只处理 modem netif 的 IP 事件（防止 CH182D 等其他网口事件串入） */
        if (event->esp_netif != g.netif) {
            return;
        }

        snprintf(g.net_info.ip, sizeof(g.net_info.ip),
                 IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(g.net_info.netmask, sizeof(g.net_info.netmask),
                 IPSTR, IP2STR(&event->ip_info.netmask));
        snprintf(g.net_info.gateway, sizeof(g.net_info.gateway),
                 IPSTR, IP2STR(&event->ip_info.gw));
        g.net_info.connected = true;

        ESP_LOGI(TAG, "Got IP: %s, Netmask: %s, Gateway: %s (netif=%s)",
                 g.net_info.ip, g.net_info.netmask, g.net_info.gateway,
                 esp_netif_get_ifkey(event->esp_netif) ?: "?");

        /* 打印 DHCP 下发的 DNS 服务器（用于诊断 DNS 解析失败问题） */
        esp_netif_dns_info_t dns_main = {0}, dns_backup = {0};
        if (esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK &&
            dns_main.ip.u_addr.ip4.addr != 0) {
            ESP_LOGI(TAG, "DNS (main):    " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "DNS (main):    NOT SET (DHCP didn't provide DNS!) -> DNS resolution will fail!");
        }
        if (esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK &&
            dns_backup.ip.u_addr.ip4.addr != 0) {
            ESP_LOGI(TAG, "DNS (backup):  " IPSTR, IP2STR(&dns_backup.ip.u_addr.ip4));
        }

        xEventGroupSetBits(g.event_group, EVENT_RNDIS_GOT_IP_BIT);

        /* RNDIS 接口已获取 IP = 数据连接可用，直接进入 GOT_IP 状态。
         * 注意：不能在这里触发 AT 配置任务，因为 RNDIS 模式下没有 AT 端口可用。 */
        cell_modem_update_state(CELL_MODEM_STATE_GOT_IP);
        cell_modem_start_data_diag();
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event->esp_netif != g.netif) {
            return;
        }
        ESP_LOGW(TAG, "Lost IP");
        cell_modem_stop_data_diag();
        xEventGroupClearBits(g.event_group, EVENT_RNDIS_GOT_IP_BIT);
        g.net_info.connected = false;
        cell_modem_update_state(CELL_MODEM_STATE_CONNECTED);
    }
}

/* ================================================================
 *  USB Host 库后台任务
 * ================================================================ */

static void usb_lib_task(void *arg)
{
    ESP_LOGI(TAG, "Installing USB Host Library (peripheral_map=0x%x)...",
             (unsigned)g.hw.usb_peripheral_map);

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = s_enum_filter_cb,
    };
    if (g.hw.usb_peripheral_map != 0) {
        host_config.peripheral_map = g.hw.usb_peripheral_map;
    }

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    if (g.hw.usb_peripheral_map & CELL_MODEM_USB_FS_PHY_BIT) {
        ESP_LOGI(TAG, "USB Host Library installed (FS PHY: GPIO26/27)");
    } else {
        ESP_LOGI(TAG, "USB Host Library installed");
    }

    /* 通知调用者 USB Host 已就绪 */
    xTaskNotifyGive((TaskHandle_t)arg);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients) {
        uint32_t event_flags;
        esp_err_t ev_ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ev_ret != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(ev_ret));
            break;
        }
        if (event_flags) {
            ESP_LOGI(TAG, "USB Host event flags: 0x%08lx", (unsigned long)event_flags);
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            if (ESP_OK == usb_host_device_free_all()) {
                ESP_LOGI(TAG, "USB Host: all clients gone, no devices");
                has_clients = false;
            } else {
                ESP_LOGI(TAG, "USB Host: clients gone but devices still connected");
                has_devices = true;
            }
        }
        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
            ESP_LOGI(TAG, "USB Host: all devices freed");
            has_clients = false;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    usb_host_uninstall();
    vTaskDelete(NULL);
}

/* ================================================================
 *  RNDIS 驱动安装
 * ================================================================ */

static esp_err_t cell_modem_install_rndis(void)
{
    esp_err_t ret;

    /* VID-only 匹配，兼容 RNDIS/ECM 两种 PID */
    usb_device_match_id_t *dev_match_id = calloc(2, sizeof(usb_device_match_id_t));
    ESP_RETURN_ON_FALSE(dev_match_id, ESP_ERR_NO_MEM, TAG, "Failed to alloc match_id");

    dev_match_id[0].match_flags = USB_DEVICE_ID_MATCH_VENDOR;
    dev_match_id[0].idVendor = cell_modem_usb_vid();
    memset(&dev_match_id[1], 0, sizeof(usb_device_match_id_t));
    g.rndis_match_id_list = dev_match_id;

    iot_usbh_rndis_config_t rndis_config = {
        .match_id_list = dev_match_id,
    };
    ret = iot_eth_new_usb_rndis(&rndis_config, &g.eth_driver);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RNDIS driver: %s", esp_err_to_name(ret));
        free(dev_match_id);
        g.rndis_match_id_list = NULL;
        return ret;
    }

    iot_eth_config_t eth_config = {
        .driver = g.eth_driver,
    };
    ret = iot_eth_install(&eth_config, &g.eth_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to install ETH driver");

    /* 创建 modem 专用 netif（名称/路由优先级由 config 注入；未配置时用内置默认值）
 *
 * if_key 与 if_desc 同时取自 netif_name：
 *   - if_key  决定内核侧 netif 名字（"cell_rndis" → "cell_rndis0"）
 *   - if_desc 是路由表 / esp_netif list 里的可读描述
 * 若调用方同时挂 eth_ch182d / Wi-Fi，请确保 netif_name 与之不冲突。 */
    esp_netif_inherent_config_t netif_base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    const char *netif_name = (g.config.netif_name != NULL && g.config.netif_name[0] != '\0')
                            ? g.config.netif_name : CELL_MODEM_NETIF_NAME_DEFAULT;
    netif_base.if_key  = netif_name;
    netif_base.if_desc = netif_name;
    netif_base.route_prio = (g.config.route_priority > 0)
                        ? g.config.route_priority : CELL_MODEM_ROUTE_PRIORITY;
    /* RNDIS 蜂窝链路 MTU：USB RNDIS 封装每包加 44B 头，且蜂窝侧 MTU 通常 <1500，
     * 用默认 1500 会导致大包（TCP 数据段/ICMP/分片）被模组丢弃——表现为
     * DNS(小UDP)通但 TCP connect/ICMP 失败(errno=113)。统一设 1400 兜底。 */
    netif_base.mtu = (g.config.mtu > 0) ? g.config.mtu : CELL_MODEM_MTU_DEFAULT;

    esp_netif_config_t netif_config = {
        .base = &netif_base,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    g.netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(g.netif, ESP_ERR_NO_MEM, TAG, "Failed to create netif");

    g.netif_glue = iot_eth_new_netif_glue(g.eth_handle);
    ESP_RETURN_ON_FALSE(g.netif_glue, ESP_ERR_NO_MEM, TAG, "Failed to create netif glue");
    esp_netif_attach(g.netif, g.netif_glue);

    ret = iot_eth_start(g.eth_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to start ETH driver");

    return ESP_OK;
}

/* ================================================================
 *  公开 API
 * ================================================================ */

esp_err_t cell_modem_init(const cell_modem_hw_config_t *hw, const cell_modem_config_t *config)
{
    esp_err_t ret;

    if (hw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (g.event_group != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing cellular modem module...");

    /* 确保默认事件循环与 netif 已就绪。两者是 esp_event_handler_register /
     * esp_netif_new 的前置依赖；若调用方已自行创建，此处忽略 INVALID_STATE。 */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create default event loop");
    }
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize netif");
    }

    g.hw = *hw;

    /* 保存配置 */
    if (config) {
        g.config = *config;
    } else {
        g.config = (cell_modem_config_t){
            .at_interface_num     = 2,
            .auto_configure_rndis   = false,
            .auto_activate_pdp      = true,
            .apn                    = NULL,
            .enable_diag            = true,
            .diag_tcp_host          = NULL,
            .diag_tcp_port          = CELL_MODEM_DIAG_TCP_PORT,
            .netif_name             = NULL,    /* NULL=用内置 "cell_rndis" */
            .route_priority         = 0,       /* 0=用内置默认 CELL_MODEM_ROUTE_PRIORITY */
        };
    }
    if (g.config.at_interface_num < 0) {
        g.config.at_interface_num = 2;
    }
    if (g.config.diag_tcp_port == 0) {
        g.config.diag_tcp_port = CELL_MODEM_DIAG_TCP_PORT;
    }

    /* CSQ 初始值 -1 表示「未知」，与 deinit 时的值保持一致。
     * 避免零初始化导致 CSQ 在首次有效查询前被误报为 0（=极弱信号）。 */
    g.csq_value = -1;

    /* 创建事件组 + 互斥锁 */
    g.event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(g.event_group, ESP_ERR_NO_MEM, TAG, "Failed to create event group");
    g.at_mutex = xSemaphoreCreateMutex();

    /* 注册事件处理器 */
    ESP_ERROR_CHECK(esp_event_handler_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID,
                                                &cell_modem_eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                &cell_modem_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                                &cell_modem_ip_event_handler, NULL));

    /* 设置 USB 枚举过滤器 */
    s_enum_filter_cb = usb_host_enum_filter_cb;

    /* 创建 USB Host 库后台任务 */
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        usb_lib_task, "usb_lib", 4096,
        xTaskGetCurrentTaskHandle(), configMAX_PRIORITIES - 1, NULL, 1);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB lib task");
        return ESP_FAIL;
    }

    /* 等待 USB Host 库安装完成 */
    uint32_t notify_value = ulTaskNotifyTake(false, pdMS_TO_TICKS(2000));
    if (notify_value == 0) {
        ESP_LOGE(TAG, "USB host library not installed");
        return ESP_ERR_TIMEOUT;
    }

    /* 安装 CDC 驱动（skip_init=true，因为 USB Host 已由上面安装） */
    usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = configMAX_PRIORITIES - 2,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    ret = usbh_cdc_driver_install(&cdc_config);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to install CDC driver");

    /* 注册 CDC 设备事件回调（VID-only 匹配） */
    static usb_device_match_id_t cdc_match_ids[2];
    cdc_match_ids[0].match_flags = USB_DEVICE_ID_MATCH_VENDOR;
    cdc_match_ids[0].idVendor = cell_modem_usb_vid();
    memset(&cdc_match_ids[1], 0, sizeof(usb_device_match_id_t));
    usbh_cdc_register_dev_event_cb(cdc_match_ids, cell_modem_cdc_event_cb, NULL);

    /* 安装 RNDIS 驱动——须在 USB 枚举前完成，否则错过复合模式设备的 RNDIS 绑定。 */
    ret = cell_modem_install_rndis();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install RNDIS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 硬复位：优先使用 app_main 早期复位；否则 init 内复位（RNDIS 已安装） */
    if (s_early_reset_done) {
        cell_modem_rst_gpio_release();
        ESP_LOGI(TAG, "skip init reset (early reset at boot)");
    } else {
        cell_modem_hardware_reset();
    }

    cell_modem_update_state(CELL_MODEM_STATE_CONNECTING);

    /* 启动监控任务（后台自动扫描 AT 端口并配置 RNDIS/PDP） */
    xTaskCreate(cell_modem_monitor_task, "cell_modem_mon", 4096, NULL, 3, &g.monitor_task);

    ESP_LOGI(TAG, "initialized (auto-config: %s, auto-PDP: %s)",
             g.config.auto_configure_rndis ? "ON" : "OFF",
             g.config.auto_activate_pdp ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t cell_modem_deinit(void)
{
    /* 停止监控任务 */
    if (g.monitor_task) {
        g.event_group = NULL;  /* 让监控任务退出循环 */
        vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_MON_INTERVAL_MS + 1000));
        g.monitor_task = NULL;
    }

    /* 停止 AT 配置任务 */
    if (g.at_config_task) {
        vTaskDelete(g.at_config_task);
        g.at_config_task = NULL;
    }
    g.at_config_in_progress = false;

    /* 关闭 AT 端口 */
    cell_modem_close_at_port();

    cell_modem_stop_data_diag();

    /* 注销事件处理器 */
    esp_event_handler_unregister(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, &cell_modem_eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &cell_modem_ip_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, &cell_modem_ip_event_handler);

    /* 停止并卸载以太网驱动 */
    if (g.eth_handle) {
        iot_eth_stop(g.eth_handle);
        iot_eth_uninstall(g.eth_handle);
        g.eth_handle = NULL;
        g.eth_driver = NULL;
    }

    /* 释放 netif glue */
    if (g.netif_glue) {
        iot_eth_del_netif_glue(g.netif_glue);
        g.netif_glue = NULL;
    }

    /* 删除网络接口 */
    if (g.netif) {
        esp_netif_destroy(g.netif);
        g.netif = NULL;
    }

    /* 卸载 CDC 驱动 */
    usbh_cdc_driver_uninstall();

    /* 释放匹配 ID */
    if (g.rndis_match_id_list) {
        free(g.rndis_match_id_list);
        g.rndis_match_id_list = NULL;
    }

    /* 删除同步原语 */
    if (g.at_mutex) {
        vSemaphoreDelete(g.at_mutex);
        g.at_mutex = NULL;
    }

    /* 重置状态 */
    g.state = CELL_MODEM_STATE_DISCONNECTED;
    memset(&g.net_info, 0, sizeof(g.net_info));
    g.rndis_configured = false;
    g.rndis_ever_connected = false;
    g.pdp_done = false;
    g.cdc_connected = false;
    g.dev_addr = 0;
    g.csq_value = -1;
    s_sim_present = true;
    s_cpin_fail_count = 0;

    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

cell_modem_state_t cell_modem_get_state(void)
{
    return g.state;
}

esp_err_t cell_modem_get_net_info(cell_modem_net_info_t *info)
{
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    *info = g.net_info;
    return ESP_OK;
}

void cell_modem_register_status_callback(cell_modem_status_callback_t callback)
{
    g.status_cb = callback;
}

esp_netif_t *cell_modem_get_netif(void)
{
    return g.netif;
}

bool cell_modem_is_connected(void)
{
    return g.state == CELL_MODEM_STATE_GOT_IP;
}

bool cell_modem_is_pdp_ready(void)
{
    return g.state == CELL_MODEM_STATE_GOT_IP && g.pdp_done && !g.at_config_in_progress;
}

bool cell_modem_is_data_path_ok(void)
{
    /* 数据面探针通过即认为真实可达；附加 pdp_ready 兜底，
     * 防止 s_data_path_ok 在 LOST_IP/重连间隙未及时清零时的误报。 */
    return s_data_path_ok && cell_modem_is_pdp_ready();
}

bool cell_modem_wait_for_pdp_ready(uint32_t timeout_ms)
{
    const uint32_t poll_ms = 500;
    uint32_t elapsed = 0;
    TickType_t last_log_tick = xTaskGetTickCount();

    while (!cell_modem_is_pdp_ready()) {
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            ESP_LOGW(TAG, "modem PDP wait timeout (%lums)", (unsigned long)timeout_ms);
            return false;
        }
        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(10000)) {
            ESP_LOGI(TAG, "Waiting for modem PDP (state=%d, pdp_done=%d, at_cfg=%d)...",
                     g.state, g.pdp_done, g.at_config_in_progress);
            last_log_tick = now;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        if (timeout_ms > 0) {
            elapsed += poll_ms;
        }
    }

    ESP_LOGI(TAG, "modem PDP ready (waited %lums)", (unsigned long)elapsed);
    return true;
}

int cell_modem_get_signal_strength(void)
{
    return g.csq_value;
}
