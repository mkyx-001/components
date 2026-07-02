/*
 * Performeter 采集核心实现。
 *
 * 机制：两次 uxTaskGetSystemState() 快照 + esp_timer_get_time() 构成采样窗口，
 * 对每个任务的 ulRunTimeCounter 做差分，得到窗口内的实时 CPU 占比（详见 DESIGN.md §4）。
 * 分核利用率 = 100% - 该核 IDLE 任务占比（IDLE0 / IDLE1）。
 */
#include "performeter.h"
#include "performeter_internal.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "performeter";

/* ---------------------------------------------------------------- *
 *  配置宏（Kconfig 未提供时给默认值，保证可独立编译）
 * ---------------------------------------------------------------- */
#ifndef CONFIG_PERFORMETER_PERIOD_MS
#define CONFIG_PERFORMETER_PERIOD_MS      1000
#endif
#ifndef CONFIG_PERFORMETER_TASK_STACK
#define CONFIG_PERFORMETER_TASK_STACK     4096
#endif
#ifndef CONFIG_PERFORMETER_LOG_TASKS_TOPN
#define CONFIG_PERFORMETER_LOG_TASKS_TOPN 8
#endif
#ifndef CONFIG_PERFORMETER_ENABLE
#define CONFIG_PERFORMETER_ENABLE         1
#endif

#define PERFORMETER_DEFAULT_PERIOD_MS     CONFIG_PERFORMETER_PERIOD_MS
#define PERFORMETER_MIN_PERIOD_MS         200
#define PERFORMETER_MAX_PERIOD_MS         5000

/* 单核 SoC 没有 IDLE1；用宏判定是否双核。 */
#if CONFIG_FREERTOS_UNICORE
#define PERFORMETER_NUM_CORES 1
#else
#define PERFORMETER_NUM_CORES 2
#endif

/* 单核 FreeRTOS 的 TaskStatus_t 不含 xCoreID，统一经此读取。 */
#if CONFIG_FREERTOS_UNICORE
static inline int performeter_task_core_id(const TaskStatus_t *st)
{
    (void)st;
    return 0;
}
#else
static inline int performeter_task_core_id(const TaskStatus_t *st)
{
    return (int)st->xCoreID;
}
#endif

/* ESP-IDF 空闲任务命名：单核 "IDLE" 或 "IDLE0"，双核 "IDLE0"/"IDLE1"。 */
static bool performeter_is_idle_task(const char *nm)
{
    if (nm == NULL || nm[0] != 'I' || nm[1] != 'D' || nm[2] != 'L' || nm[3] != 'E') {
        return false;
    }
    return nm[4] == '\0'
        || (nm[4] == '0' && nm[5] == '\0')
        || (nm[4] == '1' && nm[5] == '\0');
}

/* ---------------------------------------------------------------- *
 *  运行期状态
 * ---------------------------------------------------------------- */
static TaskHandle_t       s_task_handle   = NULL;
static uint32_t           s_period_ms     = 0;
static performeter_cb_t   s_callback      = NULL;
static volatile bool      s_stop_request  = false;

/* 快照输出缓冲（后台任务专用），柔性数组需手工分配。 */
static performeter_snapshot_t *s_snap = NULL;
#define PERFORMETER_SNAP_CAPACITY  24   /* 同时追踪的任务数上限 */

/* ---------------------------------------------------------------- *
 *  前置声明
 * ---------------------------------------------------------------- */
static void performeter_task(void *arg);
static esp_err_t collect_into(performeter_snapshot_t *out, uint32_t period_ms);
static void clamp_period(uint32_t *period_ms);

/* ---------------------------------------------------------------- *
 *  公共 API
 * ---------------------------------------------------------------- */

esp_err_t performeter_start(uint32_t period_ms)
{
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* —— 配置自检 ——
     * 编译期判定：若上游工程未开启 run-time stats，计数恒为 0，
     * 在 start() 给出明确告警，并在运行期进一步兜底（见 collect_into）。 */
#if !defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) || (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS == 0)
    ESP_LOGE(TAG, "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is disabled in sdkconfig. "
                  "Enable it via menuconfig -> Component config -> FreeRTOS -> "
                  "Generate Run Time Stats.");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    clamp_period(&period_ms);
    s_period_ms     = period_ms;
    s_stop_request  = false;

    /* 分配快照缓冲：柔性数组需一次性 malloc。 */
    if (s_snap == NULL) {
        size_t sz = sizeof(performeter_snapshot_t)
                  + PERFORMETER_SNAP_CAPACITY * sizeof(performeter_task_info_t);
        s_snap = (performeter_snapshot_t *)malloc(sz);
        if (s_snap == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_snap->task_capacity = PERFORMETER_SNAP_CAPACITY;
    }

    /* 后台任务：优先级 1（最低，避免干扰被测对象），不绑核。 */
    BaseType_t ok = xTaskCreate(performeter_task, TAG,
                                CONFIG_PERFORMETER_TASK_STACK,
                                NULL, 1, &s_task_handle);
    if (ok != pdPASS) {
        free(s_snap);
        s_snap = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started: period=%ums, cores=%d, capacity=%d",
             (unsigned)period_ms, PERFORMETER_NUM_CORES, PERFORMETER_SNAP_CAPACITY);
    return ESP_OK;
}

esp_err_t performeter_stop(void)
{
    if (s_task_handle == NULL) {
        return ESP_OK;
    }
    s_stop_request = true;
    /* 不在 stop 路径上等任务退出，避免业务侧误调用导致死锁；
     * 任务在下个循环边界自行 delete 并回收。 */
    return ESP_OK;
}

bool performeter_is_running(void)
{
    return (s_task_handle != NULL) && (s_stop_request == false);
}

esp_err_t performeter_sample_once(performeter_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t period = (s_period_ms != 0) ? s_period_ms : PERFORMETER_DEFAULT_PERIOD_MS;
    return collect_into(out, period);
}

esp_err_t performeter_register_cb(performeter_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callback = cb;
    return ESP_OK;
}

esp_err_t performeter_unregister_cb(performeter_cb_t cb)
{
    (void)cb;
    s_callback = NULL;
    return ESP_OK;
}

/* ---------------------------------------------------------------- *
 *  后台任务
 * ---------------------------------------------------------------- */
static void performeter_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "background task running on core %d", xPortGetCoreID());

    while (!s_stop_request) {
        esp_err_t ret = collect_into(s_snap, s_period_ms);
        if (ret == ESP_OK) {
            performeter_print(s_snap);   /* 日志输出（实现于 performeter_print.c） */
            if (s_callback) {
                s_callback(s_snap);      /* 用户回调（禁止阻塞） */
            }
        } else if (ret == ESP_ERR_NOT_SUPPORTED) {
            /* run-time stats 未开：停止后续采样，避免刷屏。 */
            break;
        } else {
            ESP_LOGW(TAG, "sample failed: %s", esp_err_to_name(ret));
        }
    }

    /* 自我回收。 */
    TaskHandle_t self = s_task_handle;
    s_task_handle = NULL;
    free(s_snap);
    s_snap = NULL;
    ESP_LOGI(TAG, "background task exiting");
    vTaskDelete(self);
}

/* ---------------------------------------------------------------- *
 *  采样核心
 * ---------------------------------------------------------------- */

/**
 * 在采样窗口两端各调用一次 uxTaskGetSystemState，差分计算。
 * out 由调用方分配，柔性数组容量由 out->task_capacity 指定。
 */
static esp_err_t collect_into(performeter_snapshot_t *out, uint32_t period_ms)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 任务数动态变化，分配 2 倍容量缓冲（防止窗口内新增任务导致截断）。 */
    UBaseType_t n_tasks = uxTaskGetNumberOfTasks();
    size_t arr_cap = (size_t)n_tasks * 2 + 4;
    if (arr_cap < 16) {
        arr_cap = 16;
    }

    TaskStatus_t *arr_a = malloc(arr_cap * sizeof(TaskStatus_t));
    TaskStatus_t *arr_b = malloc(arr_cap * sizeof(TaskStatus_t));
    if (arr_a == NULL || arr_b == NULL) {
        free(arr_a);
        free(arr_b);
        return ESP_ERR_NO_MEM;
    }

    /* —— 第一次快照 —— */
    UBaseType_t cnt_a = uxTaskGetSystemState(arr_a, (UBaseType_t)arr_cap, NULL);
    if (cnt_a == 0) {
        free(arr_a);
        free(arr_b);
        ESP_LOGE(TAG, "uxTaskGetSystemState returned 0; check "
                      "CONFIG_FREERTOS_USE_TRACE_FACILITY.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    int64_t t0_us = esp_timer_get_time();

    /* —— 采样窗口 —— */
    vTaskDelay(pdMS_TO_TICKS(period_ms));

    /* —— 第二次快照 —— */
    UBaseType_t cnt_b = uxTaskGetSystemState(arr_b, (UBaseType_t)arr_cap, NULL);
    if (cnt_b == 0) {
        free(arr_a);
        free(arr_b);
        return ESP_FAIL;
    }
    int64_t t1_us = esp_timer_get_time();

    uint32_t window_us = (uint32_t)(t1_us - t0_us);

    /* —— 配置兜底：若所有 ulRunTimeCounter 均为 0，说明未真正开启 run-time stats。 —— */
    bool any_nonzero = false;
    for (UBaseType_t i = 0; i < cnt_b; i++) {
        if (arr_b[i].ulRunTimeCounter != 0) {
            any_nonzero = true;
            break;
        }
    }
    if (!any_nonzero) {
        free(arr_a);
        free(arr_b);
        ESP_LOGE(TAG, "All runtime counters are zero. Enable "
                      "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS in sdkconfig.");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* —— 差分：用 xHandle 在两次快照间匹配同一任务。 —— */
    /* 先统计每核的“运行计数增量之和”作为该核分母（DESIGN.md §4.2 步骤 8）。 */
    uint64_t core_total[PERFORMETER_NUM_CORES] = {0};
    uint64_t idle_delta[PERFORMETER_NUM_CORES] = {0};

    /* 先遍历 b：累加每个任务到其所在核的 total。 */
    for (UBaseType_t j = 0; j < cnt_b; j++) {
        int core = performeter_task_core_id(&arr_b[j]);
        if (core < 0 || core >= PERFORMETER_NUM_CORES) {
            /* tskNO_AFFINITY 或异常值：暂不计入分核，避免分母失真。 */
            continue;
        }
        /* 找到对应的 start 快照做差分。 */
        uint64_t start_rt = 0;
        bool found = false;
        for (UBaseType_t i = 0; i < cnt_a; i++) {
            if (arr_a[i].xHandle == arr_b[j].xHandle) {
                start_rt = arr_a[i].ulRunTimeCounter;
                found = true;
                break;
            }
        }
        uint64_t end_rt = arr_b[j].ulRunTimeCounter;
        /* 处理回绕/任务窗口内创建（start>end 或没找到）：取 end 作为近似增量。 */
        uint64_t delta = (found && end_rt >= start_rt) ? (end_rt - start_rt) : end_rt;

        core_total[core] += delta;

        /* IDLE 任务命名：IDLE0 / IDLE1（ESP-IDF SMP），单核为 IDLE 或 IDLE0。 */
        const char *nm = arr_b[j].pcTaskName;
        if (performeter_is_idle_task(nm)) {
            idle_delta[core] += delta;
        }
    }

    /* —— 计算每核利用率 = 100% - idle% —— */
    for (int c = 0; c < PERFORMETER_NUM_CORES; c++) {
        if (core_total[c] == 0) {
            out->core_util[c] = 0;
            continue;
        }
        /* 0..10000 定点：idle% = idle*10000/total；util = 10000 - idle%。 */
        uint32_t idle_pct = (uint32_t)(idle_delta[c] * 10000ull / core_total[c]);
        out->core_util[c] = (idle_pct > 10000u) ? 0 : (10000u - idle_pct);
    }
    /* 单核 SoC：core_util[1] 清零。 */
    if (PERFORMETER_NUM_CORES < PERFORMETER_MAX_CORES) {
        for (int c = PERFORMETER_NUM_CORES; c < PERFORMETER_MAX_CORES; c++) {
            out->core_util[c] = 0;
        }
    }

    /* —— 堆内存 —— */
    out->free_heap     = esp_get_free_heap_size();
    out->min_free_heap = esp_get_minimum_free_heap_size();
    out->sample_ms     = window_us / 1000u;

    /* —— 构建逐任务排行（差分后按 delta 降序） —— */
    /* 用 out->tasks 柔性数组接收结果，受 task_capacity 限制。 */
    uint16_t cap = out->task_capacity;
    uint16_t filled = 0;

    /* 临时构建一个 (delta, TaskStatus_b) 视图，按 delta 排序后取 Top cap。 */
    /* 为避免再分配，用就地方式：遍历 b，对每个任务算 delta 后做插入排序入 out->tasks。 */
    for (UBaseType_t j = 0; j < cnt_b; j++) {
        uint64_t start_rt = 0;
        bool found = false;
        for (UBaseType_t i = 0; i < cnt_a; i++) {
            if (arr_a[i].xHandle == arr_b[j].xHandle) {
                start_rt = arr_a[i].ulRunTimeCounter;
                found = true;
                break;
            }
        }
        uint64_t end_rt = arr_b[j].ulRunTimeCounter;
        uint64_t delta = (found && end_rt >= start_rt) ? (end_rt - start_rt) : end_rt;

        /* 全局百分比（相对所有任务总运行时间）。 */
        uint64_t grand_total = 0;
        for (int c = 0; c < PERFORMETER_NUM_CORES; c++) {
            grand_total += core_total[c];
        }
        uint32_t pct_x100 = 0;
        if (grand_total > 0) {
            pct_x100 = (uint32_t)(delta * 10000ull / grand_total);
        }

        /* 插入排序：维护 out->tasks[0..filled] 按 pct_x100 降序。 */
        if (filled < cap) {
            /* 还有空位：找到插入位置后整体后移。 */
            uint16_t k = filled;
            while (k > 0 && out->tasks[k - 1].cpu_percent < pct_x100) {
                out->tasks[k] = out->tasks[k - 1];
                k--;
            }
            strncpy(out->tasks[k].name, arr_b[j].pcTaskName, PERFORMETER_TASK_NAME_LEN - 1);
            out->tasks[k].name[PERFORMETER_TASK_NAME_LEN - 1] = '\0';
            out->tasks[k].cpu_percent     = pct_x100;
            out->tasks[k].core_id         = (int8_t)performeter_task_core_id(&arr_b[j]);
            out->tasks[k].stack_remaining = (uint32_t)arr_b[j].usStackHighWaterMark * sizeof(StackType_t);
            filled++;
        } else {
            /* 已满：仅当比末位大才替换末位并重新插入。 */
            if (pct_x100 > out->tasks[cap - 1].cpu_percent) {
                uint16_t k = cap - 1;
                while (k > 0 && out->tasks[k - 1].cpu_percent < pct_x100) {
                    out->tasks[k] = out->tasks[k - 1];
                    k--;
                }
                strncpy(out->tasks[k].name, arr_b[j].pcTaskName, PERFORMETER_TASK_NAME_LEN - 1);
                out->tasks[k].name[PERFORMETER_TASK_NAME_LEN - 1] = '\0';
                out->tasks[k].cpu_percent     = pct_x100;
                out->tasks[k].core_id         = (int8_t)performeter_task_core_id(&arr_b[j]);
                out->tasks[k].stack_remaining = (uint32_t)arr_b[j].usStackHighWaterMark * sizeof(StackType_t);
            }
        }
    }
    out->task_count = filled;

    free(arr_a);
    free(arr_b);
    return ESP_OK;
}

/* ---------------------------------------------------------------- *
 *  辅助
 * ---------------------------------------------------------------- */
static void clamp_period(uint32_t *period_ms)
{
    if (*period_ms == 0) {
        *period_ms = PERFORMETER_DEFAULT_PERIOD_MS;
    }
    if (*period_ms < PERFORMETER_MIN_PERIOD_MS) {
        *period_ms = PERFORMETER_MIN_PERIOD_MS;
    }
    if (*period_ms > PERFORMETER_MAX_PERIOD_MS) {
        *period_ms = PERFORMETER_MAX_PERIOD_MS;
    }
}
