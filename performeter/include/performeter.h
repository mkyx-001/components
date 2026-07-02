/*
 * Performeter — ESP32 性能监视器组件（公共 API）
 *
 * 通过 FreeRTOS 运行时统计（run-time stats）周期性采集：
 *   - 每核 CPU 总利用率（Core0 / Core1）
 *   - 逐任务 CPU 占比排行
 *   - 堆内存与任务栈水位
 *
 * 详见 DESIGN.md。使用约束见各函数注释。
 */
#ifndef PERFORMETER_H
#define PERFORMETER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOSConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ *
 *  常量
 * ============================================================ */

/** 支持的最大 CPU 核数（ESP32 双核；单核 SoC 仅使用 [0]）。 */
#define PERFORMETER_MAX_CORES       2

/** 任务名最大长度（与 FreeRTOS configMAX_TASK_NAME_LEN 对齐）。 */
#define PERFORMETER_TASK_NAME_LEN   16

/* ============================================================ *
 *  数据结构
 * ============================================================ */

/** 单个任务的一次采样结果。 */
typedef struct {
    char     name[PERFORMETER_TASK_NAME_LEN]; /* 任务名（截断） */
    uint32_t cpu_percent;                     /* 窗口内 CPU 占比，0..10000 (= 0.00%..100.00%，放大100倍) */
    int8_t   core_id;                         /* 任务当前所在核：0 / 1 / -1（tskNO_AFFINITY 或不固定） */
    uint32_t stack_remaining;                 /* 栈高水位，剩余字节；0 表示不可用 */
} performeter_task_info_t;

/**
 * 单次采样快照。
 * 字段 cpu_percent 使用 0..10000 的定点表示（×100），便于无浮点环境下保留两位小数。
 */
typedef struct {
    /* —— 窗口元信息 —— */
    uint32_t sample_ms;            /* 本次采样窗口实际长度（毫秒） */

    /* —— 每核 CPU 总利用率 —— */
    uint16_t core_util[PERFORMETER_MAX_CORES]; /* 每核利用率，0..10000 (×100) */

    /* —— 堆内存 —— */
    uint32_t free_heap;            /* 当前空闲堆，字节 */
    uint32_t min_free_heap;        /* 历史最低空闲堆，字节（单调不回升） */

    /* —— 任务排行 —— */
    uint16_t task_count;           /* 数组中有效条目数 */
    uint16_t task_capacity;        /* 数组容量（调用方填充时用于边界检查） */
    performeter_task_info_t tasks[]; /* 按 cpu_percent 降序排列的柔性数组 */
} performeter_snapshot_t;

/** 单次采样完成回调函数原型。在 Performeter 后台任务上下文中调用。 */
typedef void (*performeter_cb_t)(const performeter_snapshot_t *snapshot);

/* ============================================================ *
 *  生命周期
 * ============================================================ */

/**
 * @brief 初始化并启动后台监视任务。
 *
 * @param period_ms  采样周期（毫秒）。传 0 使用 Kconfig 默认值
 *                   (CONFIG_PERFORMETER_PERIOD_MS)。范围 200..5000，越界自动夹紧。
 * @return ESP_OK                 成功
 *         ESP_ERR_INVALID_STATE  已在运行，需先 stop
 *         ESP_ERR_NOT_SUPPORTED  编译期未开启 run-time stats（见下文“前置配置”）
 *         ESP_ERR_NO_MEM         任务列表缓冲分配失败
 *
 * 前置配置（必须在工程 sdkconfig 中开启，组件无法强制覆盖）：
 *   - CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
 *   - CONFIG_FREERTOS_USE_TRACE_FACILITY=y
 *   - CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y
 * 本函数会在未开启时检测并返回 ESP_ERR_NOT_SUPPORTED（详见 DESIGN.md §2.3）。
 */
esp_err_t performeter_start(uint32_t period_ms);

/**
 * @brief 停止后台监视任务。
 *
 * 置停止标志后等待当前采样窗口结束、回收资源。不会在已停止时返回错误。
 *
 * @return ESP_OK 总是返回成功（与 ESP-IDF 常见 stop 语义一致）。
 */
esp_err_t performeter_stop(void);

/** @brief 后台任务是否正在运行。 */
bool performeter_is_running(void);

/* ============================================================ *
 *  主动采样
 * ============================================================ */

/**
 * @brief 主动触发一次采样（阻塞）。
 *
 * 阻塞时长约等于一个采样周期。调用方需自行分配快照缓冲：
 *
 *   size_t cap = 16;
 *   size_t sz  = sizeof(performeter_snapshot_t)
 *              + cap * sizeof(performeter_task_info_t);
 *   performeter_snapshot_t *snap = malloc(sz);
 *   snap->task_capacity = cap;
 *   performeter_sample_once(snap);
 *   ...
 *   free(snap);
 *
 * @param out  输出快照。task_capacity 由调用方先填好；函数写入 task_count ≤ task_capacity。
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_FAIL（采集失败，详见日志）
 *
 * 注意：本函数可在 start() 启动后台任务之外独立使用（“一次性采集”模式）。
 */
esp_err_t performeter_sample_once(performeter_snapshot_t *out);

/* ============================================================ *
 *  回调（v1 留接口，稳定 ABI）
 * ============================================================ */

/**
 * @brief 注册采样完成回调。每次后台采样结束触发。
 *
 * 回调在 Performeter 后台任务上下文中执行，禁止阻塞、禁止调用
 * performeter_stop()。v1 支持单回调；重复注册覆盖前次。
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG（cb == NULL）
 */
esp_err_t performeter_register_cb(performeter_cb_t cb);

/**
 * @brief 注销回调。传 NULL 等效于取消所有回调。
 */
esp_err_t performeter_unregister_cb(performeter_cb_t cb);

/* ============================================================ *
 *  工具：百分比定点值 <-> 整数显示
 * ============================================================ */

/** 将 0..10000 的定点百分比转为整数部分（0..100）。 */
static inline uint8_t performeter_pct_int(uint32_t pct_x100)
{
    return (uint8_t)(pct_x100 / 100u);
}

/** 将 0..10000 的定点百分比转为小数部分（0..99）。 */
static inline uint8_t performeter_pct_frac(uint32_t pct_x100)
{
    return (uint8_t)(pct_x100 % 100u);
}

#ifdef __cplusplus
}
#endif

#endif /* PERFORMETER_H */
