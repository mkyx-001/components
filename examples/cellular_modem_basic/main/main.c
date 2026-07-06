#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cellular_modem.h"

/*
 * 板级配置：产品必须显式注入 USB VID/PID、USB PHY 映射、复位引脚。
 * cellular_modem 组件仅在字段为 0 时使用内置默认值兜底，
 * 但例程作为产品模板，应显式给出所有板级参数，便于移植时一目了然。
 *
 * 以下数值针对 ML307 + ESP32-P4 USB FS PHY 板子：
 *   - VID           0x2ECC     ML307 USB Vendor ID
 *   - PID_COMPOSITE 0x3012     RNDIS + AT CDC 复合模式
 *   - PID_RNDIS     0x3004     纯 RNDIS 模式（mode switch 后）
 *   - USB_PERIPHERAL BIT1       P4 FS DWC（D+ = GPIO27, D- = GPIO26）
 *   - RST_GPIO      54         ML307 RST（低有效）
 */
#ifndef CELL_MODEM_USB_VID
#define CELL_MODEM_USB_VID           0x2ECC
#endif
#ifndef CELL_MODEM_USB_PID_RNDIS
#define CELL_MODEM_USB_PID_RNDIS     0x3004
#endif
#ifndef CELL_MODEM_USB_PID_COMPOSITE
#define CELL_MODEM_USB_PID_COMPOSITE 0x3012
#endif
#ifndef CELL_MODEM_USB_PERIPHERAL
#define CELL_MODEM_USB_PERIPHERAL    (1U << 1)
#endif
#ifndef CELL_MODEM_RST_GPIO
#define CELL_MODEM_RST_GPIO          54
#endif

/*
 * 数据面探针目标：组件的 enable_diag 会在 GOT_IP 后用此 host:port 跑 TCP 连通探针，
 * 配合 cell_modem_is_data_path_ok() 区分「RNDIS 有 IP」与「真实可上网」。
 * 选一个业务实际要访问的云端域名效果最好（同域 HTTPS/OTA 走的就是这条路径）。
 */
#ifndef CELL_MODEM_DIAG_TCP_HOST
#define CELL_MODEM_DIAG_TCP_HOST     "www.baidu.com"
#endif

static const char *TAG = "cell_modem_example";

/* 状态枚举可读化，便于日志判读与上层业务分支 */
static const char *state_name(cell_modem_state_t s)
{
    switch (s) {
    case CELL_MODEM_STATE_DISCONNECTED: return "DISCONNECTED";
    case CELL_MODEM_STATE_CONFIGURING:  return "CONFIGURING";
    case CELL_MODEM_STATE_CONNECTING:   return "CONNECTING";
    case CELL_MODEM_STATE_CONNECTED:    return "CONNECTED";
    case CELL_MODEM_STATE_GOT_IP:       return "GOT_IP";
    case CELL_MODEM_STATE_ERROR:        return "ERROR";
    default:                            return "?";
    }
}

static void modem_state_cb(cell_modem_state_t state)
{
    ESP_LOGI(TAG, "state -> %s(%d)", state_name(state), (int)state);
}

void app_main(void)
{
    cell_modem_hw_config_t hw = {
        .rst_gpio           = CELL_MODEM_RST_GPIO,
        .usb_peripheral_map = CELL_MODEM_USB_PERIPHERAL,
        .usb_vid            = CELL_MODEM_USB_VID,
        .usb_pid_rndis      = CELL_MODEM_USB_PID_RNDIS,
        .usb_pid_composite  = CELL_MODEM_USB_PID_COMPOSITE,
    };

    cell_modem_config_t cfg = {
        .at_interface_num = 2,
        .auto_configure_rndis = false,
        .auto_activate_pdp = true,
        .apn = NULL,
        .enable_diag = true,
        .diag_tcp_host = CELL_MODEM_DIAG_TCP_HOST,
        .diag_tcp_port = 443,
    };

    ESP_LOGI(TAG, "cellular modem basic example start");

    ESP_ERROR_CHECK(cell_modem_early_hardware_reset(hw.rst_gpio));
    cell_modem_register_status_callback(modem_state_cb);
    ESP_ERROR_CHECK(cell_modem_init(&hw, &cfg));

    if (!cell_modem_wait_for_pdp_ready(120000)) {
        ESP_LOGW(TAG, "PDP not ready within timeout");
    }

    /* 三层「网络正常」判读模型：
     *   1) is_connected()   —— RNDIS 拿到本地 IP（USB 链路 Up，不代表能上网）
     *   2) is_pdp_ready()   —— AT 拨号成功 + AT 任务空闲（运营商 PDP 上下文已建）
     *   3) is_data_path_ok()—— TCP 云探针 / ICMP 实测可达（真实上下行通）
     * 例程主循环逐层打印，演示如何分层判读，便于上层业务做差异化响应。
     */
    while (1) {
        cell_modem_net_info_t info = {0};
        (void)cell_modem_get_net_info(&info);

        ESP_LOGI(TAG,
                 "link=%s pdp=%s datapath=%s | state=%s ip=%s gw=%s csq=%d",
                 cell_modem_is_connected()      ? "UP"   : "DOWN",
                 cell_modem_is_pdp_ready()      ? "OK"   : "WAIT",
                 cell_modem_is_data_path_ok()   ? "OK"   : "UNVERIFIED",
                 state_name(cell_modem_get_state()),
                 info.connected ? info.ip       : "-",
                 info.connected ? info.gateway  : "-",
                 cell_modem_get_signal_strength());

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
