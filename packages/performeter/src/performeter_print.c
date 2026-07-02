/*
 * Performeter 日志格式化输出。
 *
 * 将一次采样快照格式化为人类可读的多行日志。
 * 后台任务每个周期调用一次 performeter_print()。
 */
#include "performeter.h"
#include "performeter_internal.h"

#include <stdio.h>
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_PERFORMETER_LOG_TASKS_TOPN
#define CONFIG_PERFORMETER_LOG_TASKS_TOPN 8
#endif

/* 栈高水位告警阈值：剩余字节低于此值打 ⚠ 标记。 */
#define PERFORMETER_STACK_WARN_BYTES  256

static const char *TAG = "performeter";

void performeter_print(const performeter_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    /* —— 概览行 —— */
#if CONFIG_FREERTOS_UNICORE
    ESP_LOGI(TAG, "===== sample %ums | core0 util: %u.%02u%% | "
                  "heap free: %u  min: %u =====",
             (unsigned)snap->sample_ms,
             performeter_pct_int(snap->core_util[0]),
             performeter_pct_frac(snap->core_util[0]),
             (unsigned)snap->free_heap,
             (unsigned)snap->min_free_heap);
#else
    ESP_LOGI(TAG, "===== sample %ums | core0 util: %u.%02u%%  core1 util: %u.%02u%% | "
                  "heap free: %u  min: %u =====",
             (unsigned)snap->sample_ms,
             performeter_pct_int(snap->core_util[0]),
             performeter_pct_frac(snap->core_util[0]),
             performeter_pct_int(snap->core_util[1]),
             performeter_pct_frac(snap->core_util[1]),
             (unsigned)snap->free_heap,
             (unsigned)snap->min_free_heap);
#endif

    /* —— 任务 CPU 排行（已按降序排列，取 Top-N） —— */
    uint16_t topn = CONFIG_PERFORMETER_LOG_TASKS_TOPN;
    if (topn > snap->task_count) {
        topn = snap->task_count;
    }
    if (topn == 0) {
        ESP_LOGI(TAG, "  (no tasks)");
        return;
    }

    ESP_LOGI(TAG, "  Top %u tasks (CPU%%):", (unsigned)topn);
    for (uint16_t i = 0; i < topn; i++) {
        const performeter_task_info_t *t = &snap->tasks[i];
        /* core_id: -1 表示 tskNO_AFFINITY（不固定核），显示为 '*'。 */
        int core = t->core_id;
        char core_tag = (core == 0) ? '0' : (core == 1) ? '1' : '*';
        ESP_LOGI(TAG, "    %-16s CORE%c  %2u.%02u%%",
                 t->name, core_tag,
                 performeter_pct_int(t->cpu_percent),
                 performeter_pct_frac(t->cpu_percent));
    }

    /* —— 栈水位告警（仅打印接近溢出的任务，避免日志过长） —— */
    bool header_printed = false;
    for (uint16_t i = 0; i < snap->task_count; i++) {
        const performeter_task_info_t *t = &snap->tasks[i];
        if (t->stack_remaining == 0) {
            continue;   /* 不可用 */
        }
        if (t->stack_remaining < PERFORMETER_STACK_WARN_BYTES) {
            if (!header_printed) {
                ESP_LOGW(TAG, "  Low-water stack warning:");
                header_printed = true;
            }
            ESP_LOGW(TAG, "    %-16s %u bytes remaining  (near overflow)",
                     t->name, (unsigned)t->stack_remaining);
        }
    }
}
