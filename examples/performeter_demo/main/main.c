/*
 * Performeter example —— 启动性能监视器并观察其周期输出。
 *
 * 本 example 不创建任何负载任务，只验证组件本身能正常工作。
 * 空载状态下应看到：
 *   - core util 很低（绝大部分时间在 IDLE）
 *   - IDLE0 / IDLE1（双核）占据 Top 榜前列
 *   - 系统任务（main、ipc、wifi 等若有）占比很小
 * 这是组件正确性的基线验证：能正常采集、差分、分核计算、格式化输出。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "performeter.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Performeter example start");

    /* 启动性能监视器：周期用默认值 (CONFIG_PERFORMETER_PERIOD_MS=1000)。
     * 不创建任何负载任务，仅观察自然状态下的系统 CPU 占用。 */
    esp_err_t err = performeter_start(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "performeter_start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "monitor running. Watch serial output.");
    /* 主任务结束；后台监视器继续运行。 */
}
