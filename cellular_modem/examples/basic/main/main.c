#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cellular_modem.h"

/*
 * Board-specific pin mapping for the demo.
 * Adjust CELL_MODEM_RST_GPIO and usb_peripheral_map for your hardware.
 */
#ifndef CELL_MODEM_RST_GPIO
#define CELL_MODEM_RST_GPIO 22
#endif

static const char *TAG = "cell_modem_example";

static void modem_state_cb(cell_modem_state_t state)
{
    ESP_LOGI(TAG, "state changed -> %d", state);
}

void app_main(void)
{
    cell_modem_hw_config_t hw = {
        .rst_gpio = CELL_MODEM_RST_GPIO,
        .usb_peripheral_map = BIT1,
        .usb_vid = 0,
        .usb_pid_rndis = 0,
        .usb_pid_composite = 0,
    };

    cell_modem_config_t cfg = {
        .at_interface_num = 2,
        .auto_configure_rndis = false,
        .auto_activate_pdp = true,
        .apn = NULL,
        .enable_diag = true,
        .diag_tcp_host = NULL,
        .diag_tcp_port = 443,
    };

    ESP_LOGI(TAG, "cellular modem basic example start");

    ESP_ERROR_CHECK(cell_modem_early_hardware_reset(hw.rst_gpio));
    cell_modem_register_status_callback(modem_state_cb);
    ESP_ERROR_CHECK(cell_modem_init(&hw, &cfg));

    if (!cell_modem_wait_for_pdp_ready(120000)) {
        ESP_LOGW(TAG, "PDP not ready within timeout");
    }

    while (1) {
        cell_modem_net_info_t info = {0};
        if (cell_modem_get_net_info(&info) == ESP_OK && info.connected) {
            ESP_LOGI(TAG,
                     "net ok ip=%s mask=%s gw=%s csq=%d",
                     info.ip,
                     info.netmask,
                     info.gateway,
                     cell_modem_get_signal_strength());
        } else {
            ESP_LOGI(TAG,
                     "net not ready, state=%d csq=%d",
                     cell_modem_get_state(),
                     cell_modem_get_signal_strength());
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
