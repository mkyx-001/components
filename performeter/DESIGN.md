# Performeter — ESP32 性能监视器组件 实现文档

> 组件库目录：`D:\works\components`
> 组件名：`performeter`
> 目标芯片：ESP32（双核 Xtensa），框架 ESP-IDF v5.x
> 文档版本：v1.0 ｜ 日期：2026-07-01

---

## 1. 背景与目标

在嵌入式开发中，CPU 是否跑满、哪个任务吃掉算力、堆内存是否在缓慢泄漏，是定位卡顿与稳定性问题的三把钥匙。`performeter` 作为组件库的第一个组件，目标是：

- 提供 ESP32 上**周期性**的性能快照，覆盖：
  1. **每核 CPU 总利用率**（Core0 / Core1）
  2. **逐任务 CPU 占比排行**（找出热点任务）
  3. **内存 / 堆 / 栈水位**（排查泄漏与栈溢出风险）
- 以**周期日志输出**为主要用法（`ESP_LOGI` 打印到串口），开箱即用。
- 作为 **ESP-IDF managed component** 发布，可被任意工程通过 `idf_component.yml` 引用。

### 非目标（v1 不做）
- 不做上云 / 写屏 / Web 可视化（未来可通过新增回调扩展，见 §9）。
- 不做指令级 profiling（那需要 `perfmon` / GCOV，属另一类工具）。
- 不做高频示波器式采样（采样周期最低建议 200ms，见 §4.3）。

---

## 2. 技术路线选型

### 2.1 候选方案对比

| 方案 | 原理 | 优点 | 缺点 | 是否采用 |
|---|---|---|---|---|
| **A. FreeRTOS Run-Time Stats** | 内核为每个任务维护一个"已运行时间"计数器，时基用 `esp_timer` | 官方原生、配置简单、含 IDLE 任务可直接反推空闲度 | 计数器为累计值，需自己做"窗口差分"才能得到实时占比 | ✅ **采用** |
| B. 性能计数器 `perfmon` | 直接读 Xtensa CCOUNT / 自定义 PC 采样 | 指令级精度 | 与 RTOS 任务语义脱节，需汇编，跨核复杂 | ❌ |
| C. IDLE Hook 自计数 | 在 IDLE 钩子里累加计数器反推空闲 | 实现极简 | 只能得到总空闲，无法分任务；钩子可能被抢占不准 | ❌ |
| D. 采样 PC 寄存器 | 定时器中断里采样当前 PC 统计调用栈 | 类 Linux perf，可做火焰图 | 实现复杂，需自写中断处理 | ❌（未来可选） |

**结论**：采用方案 A，即官方 [real_time_stats 示例](https://github.com/espressif/esp-idf/blob/master/examples/system/freertos/real_time_stats/README.md) 的思路 —— **快照 → 等待采样窗口 → 再快照 → 差分**，得到"窗口内"占比而非自上电起的累计平均。

### 2.2 为什么能从 IDLE 任务反推每核利用率

ESP-IDF 的 SMP FreeRTOS 为每个核各创建一个 IDLE 任务（`IDLE0`、`IDLE1`）。当一个核上没有就绪任务时，调度器运行该核的 IDLE 任务，其 run-time 计数器随之累加。

> **核心公式**：`某核CPU利用率 ≈ 100% − 该核IDLE任务在该窗口的占比`

⚠️ 近似性说明（必须如实认知）：
- ESP-IDF SMP 的 run-time 计数器实现为**全局原子累加**，`vTaskGetRunTimeStats()` 给出的 IDLE0/IDLE1 百分比是相对**整机总运行时间**的。直接相减会引入双核叠加误差。
- 本组件采用**逐核独立快照**策略（§4.2）：在采样窗口内，用 `ulTaskGetRunTimeCounter` 分别取 IDLE0、IDLE1 的计数，再结合窗口长度（由 `esp_timer_get_time()` 给出微秒级绝对时间）独立核算每核利用率，规避全局归一化带来的误差。
- 误差来源还包括：采样窗口边界处正在运行的任务、`esp_timer` 回调本身的微小开销。±2~3% 的精度对"是否跑满 / 谁是热点"这类判断完全够用。

### 2.3 关键依赖配置

为使 run-time stats 可用，工程 `sdkconfig` 需开启（组件通过 `Kconfig`/`idf_component.yml` 的 `dependencies` 提示，但**最终开关在工程 sdkconfig**，组件无法强制覆盖）：

| 配置项 | 作用 | 默认 |
|---|---|---|
| `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` | 启用运行时统计计数 | IDF v5 默认开 |
| `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` | 启用 `uxTaskGetSystemState` 等遍历 API | 需确认 |
| `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y` | 启用 `vTaskGetRunTimeStats` 文本格式化 | 需确认 |
| `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS_NOWARN=y` | 自定义宏时不报警告 | 可选 |

> 在组件启动时调用 `esp_log` 提示用户：若检测到上述未开启，打印警告并列出所需 Kconfig 名。

---

## 3. 目录结构（组件库组织约定）

`D:\works\components` 作为**组件库根目录**，其下每个子目录是一个 managed component。`performeter` 将由当前"工程模板"重构为标准组件结构：

```
D:\works\components\
├── performeter\                        # ← 组件本体（managed component）
│   ├── idf_component.yml              # 组件元数据：name/version/dependencies/targets
│   ├── CMakeLists.txt                 # idf_component_register(...)
│   ├── Kconfig.performeter            # 组件级配置项（采样周期、使能开关等）
│   ├── LICENSE
│   ├── README.md
│   ├── include\                       # 对外公共头文件（仅放这里的内容算公开 API）
│   │   └── performeter.h
│   ├── src\                           # 组件私有实现
│   │   ├── performeter.c              # 采集核心逻辑
│   │   └── performeter_print.c        # 日志格式化输出
│   └── example\                       # 测试工程：可直接 idf.py build/flash/monitor
│       ├── CMakeLists.txt             # project() 形式
│       ├── main\
│       │   ├── CMakeLists.txt
│       │   └── main.c                 # app_main 调用 performeter 启动 API
│       └── sdkconfig.defaults         # 预置上述关键配置为 y，开箱可用
│
└── (未来其他组件)\                     # 如 memory_debugger, wifi_helper ...
```

### 3.1 现有 performeter 工程的改造步骤

当前 `performeter/` 是工程模板（顶层 `CMakeLists.txt` 走 `project.cmake`、含 `main/main.c`、`.devcontainer`、`.vscode`）。改造动作：

1. **删除** 顶层 `CMakeLists.txt`（project 形式）、`main/`、`.clangd`、`.devcontainer/`、`.vscode/`。
2. **新建** `idf_component.yml`、组件级 `CMakeLists.txt`、`include/`、`src/`、`Kconfig.performeter`、`example/`。
3. 将原 `.gitignore` 收敛进组件根（保留）。
4. example 测试工程内的 `.vscode`、`.devcontainer`、`.clangd` 可在 example 下按需重建。

> 说明：本文件是**实现文档**，以上改造在后续编码阶段执行；本文档先定结构。

---

## 4. 采集原理详解

### 4.1 数据来源

| 指标 | API / 数据源 | 单位 |
|---|---|---|
| 每任务累计运行时间 | `ulTaskGetRunTimeCounter(TaskHandle_t)` 或 `vTaskGetRunTimeStats()` | ticks（时基计数） |
| 任务列表 | `uxTaskGetSystemState(arr, len, &total)` | 结构体数组 |
| 绝对时间窗 | `esp_timer_get_time()` | 微秒（int64） |
| 空闲堆 | `esp_get_free_heap_size()` | 字节 |
| 历史最低空闲堆 | `esp_get_minimum_free_heap_size()` | 字节 |
| 任务栈高水位 | `uxTaskGetStackHighWaterMark(TaskHandle_t)` | 字节（剩余） |
| 任务所在核 | `xTaskGetCoreID(TaskHandle_t)` | 0/1 |
| 任务数 | `uxTaskGetNumberOfTasks()` | 个 |

### 4.2 采样窗口算法（窗口内实时占比）

官方 `vTaskGetRunTimeStats()` 返回自上电起累计值，跑久了百分比几乎不变。本组件做"差分"：

```
1. t0 = esp_timer_get_time()
2. 对所有任务快照 run_counter_start[i]   （遍历 uxTaskGetSystemState）
3. vTaskDelay(采样窗口)                  （如 1000ms）
4. t1 = esp_timer_get_time()
5. 再次快照 run_counter_end[i]
6. window_us = t1 - t0                   （注意：≈窗口，但非严格相等，用实测 t1-t0）
7. 对每个任务 i：
      delta[i] = run_counter_end[i] - run_counter_start[i]
      pct[i]   = delta[i] * 100 / Σ(delta[*])      ← 逐任务占比（归一化到全部任务）
8. 每核利用率：
      对核 c ∈ {0,1}：
        idle_delta_c = IDLEc 的 delta
        core_total_c = Σ(本窗口内 coreID==c 的任务 delta) + IDLEc 的 delta
        core_util_c  = 100 - (idle_delta_c * 100 / core_total_c)
```

> **双核核算关键**：步骤 8 用"该核任务 delta 之和 + 该核 IDLE delta"作为该核分母，而非全局总和，从而规避 §2.2 提到的 SMP 全局归一化误差。这是本组件相对官方示例的改进点；若实测 `core_total_c` 仍不准，则回退为"用 window_us × 单核频率"作为分母的估算。

### 4.3 采样窗口与精度取舍

- 窗口越长，统计越平滑但响应越慢；窗口越短，越能抓瞬时尖峰但 IDLE 离散误差大。
- **默认 1000ms**，可配；建议范围 200ms ~ 5000ms。
- 窗口内必须让 IDLE 任务有机会运行，否则空闲度恒为 0 —— 这通常自动满足。

### 4.4 内存 / 栈采集

无需差分，直接读 API 即可，与 CPU 采样同周期输出。栈高水位遍历所有任务打印 Top-N（最危险的几个）。

---

## 5. 公共 API 设计（`include/performeter.h`）

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单次采样快照（不含 IDLE 反推后的派生量，仅原始差分结果）。 */
typedef struct {
    uint32_t  sample_ms;        /* 本次采样窗口长度 */
    uint8_t   core_util[2];     /* Core0/Core1 利用率 (%)，0..100 */
    uint32_t  free_heap;        /* esp_get_free_heap_size */
    uint32_t  min_free_heap;    /* esp_get_minimum_free_heap_size */
    uint16_t  task_count;       /* 本窗口任务数 */
} performeter_snapshot_t;

/**
 * 初始化并启动后台监视任务。
 * period_ms: 采样周期，0 表示用 Kconfig 默认值。
 * 返回 ESP_OK / ESP_ERR_INVALID_STATE（重复启动）/ ESP_ERR_NOT_SUPPORTED（配置未开）。
 */
esp_err_t performeter_start(uint32_t period_ms);

/** 停止并释放后台任务。 */
esp_err_t performeter_stop(void);

/** 是否正在运行。 */
bool performeter_is_running(void);

/** 主动触发一次采样并填充 snapshot（阻塞，耗时≈period_ms）。 */
esp_err_t performeter_sample_once(performeter_snapshot_t *out);

/** 注册 / 注销单次采样完成回调（用于未来扩展上云/上屏）。 */
typedef void (*performeter_cb_t)(const performeter_snapshot_t *snap);
esp_err_t performeter_register_cb(performeter_cb_t cb);
esp_err_t performeter_unregister_cb(performeter_cb_t cb);

#ifdef __cplusplus
}
#endif
```

> v1 仅实现 `performeter_start/stop/is_running/sample_once`，回调留接口空实现以稳定 ABI。

---

## 6. 组件级 Kconfig（`Kconfig.performeter`）

```
menu "Performeter"
    config PERFORMETER_ENABLE
        bool "Enable Performeter background task"
        default y
    config PERFORMETER_PERIOD_MS
        int "Default sampling period (ms)"
        default 1000
        range 200 5000
    config PERFORMETER_LOG_TASKS_TOPN
        int "How many top CPU tasks to print per sample"
        default 8
        range 1 32
    config PERFORMETER_TASK_STACK
        int "Background task stack size (bytes)"
        default 4096
endmenu
```

---

## 7. 关键实现要点（`src/performeter.c`）

1. **后台任务**：`xTaskCreate` 创建 `performeter_task`，优先级设为 **1**（低于业务，避免抢算力影响被测对象），绑定 `tskNO_AFFINITY`（不固定核，以免干扰测量）。
2. **任务列表缓冲**：`uxTaskGetNumberOfTasks()` 取数量，动态 `malloc` 一倍容量 `TaskStatus_t` 数组（防止窗口间新增任务），采样后 `free`。或静态定长数组 + 溢出告警。
3. **快照两次 `uxTaskGetSystemState`** 之间用 `vTaskDelay(period / portTICK_PERIOD_MS)`。注意 `uxTaskGetSystemState` 内部会短暂挂起调度器，开销可控。
4. **匹配两次快照里的同一任务**：用 `xTaskHandle`（`TaskStatus_t::xHandle`）做 key 匹配，而非任务名（可能重名）。
5. **利用率计算**：按 §4.2 步骤 8，分核求和。对 `delta<0`（任务在窗口内被删除/计数回绕）做保护：跳过该任务。
6. **配置自检**：`performeter_start` 内用编译期宏（`#if !configGENERATE_RUN_TIME_STATS`）+ 运行期特征（采样后所有 delta 全 0 则判定未开启）双重检测，打印明确警告。
7. **栈高水位 TopN**：用 `uxTaskGetStackHighWaterMark`，排序后打印最小的 N 个（最危险优先）。
8. **线程安全**：snapshot 通过后台任务独占写入，回调在后台任务上下文调用，用户回调内禁止阻塞；`stop` 时先置停止标志再 `vTaskDelay` 等其退出。

---

## 8. 测试方案（`example/` 测试工程）

### 8.1 example 工程职责
- 在 `app_main` 中调用 `performeter_start(0)`（用默认周期）。
- **不创建任何负载任务**，仅观察自然状态下的系统 CPU 占用（系统任务 + IDLE）。
- 这是组件正确性的基线验证：空载下应看到 IDLE 占绝大部分、core util 很低，
  说明采集、窗口差分、分核计算、格式化输出整条链路都正常工作。
- 串口 `idf.py monitor` 观察周期日志。
- 若需验证"负载下能否正确反映 CPU 升高"，可在自己的工程里按需添加负载任务
  （注意长时间不让出的任务需自行 `esp_task_wdt_reset()` 喂狗）。

### 8.2 期望日志样例（空载示意）

```
I (21000) performeter: ===== sample 1000ms | core0 util: 0.80%  core1 util: 0.20% | heap free: 286432  min: 263312 =====
I (21001) performeter:   Top 8 tasks (CPU%):
I (21001) performeter:    IDLE0             CORE0  99.10%
I (21001) performeter:    IDLE1             CORE1  99.70%
I (21001) performeter:    ipc0              CORE0   0.20%
I (21001) performeter:    main              CORE0   ...
```

### 8.3 验证清单（人工对照）

| # | 场景 | 预期 | 判定方法 |
|---|---|---|---|
| T1 | example 空载启动 | 两个核 util 均 < 10%，IDLE 占比 > 90% | 看日志 Core util |
| T2 | 周期输出稳定 | 每秒一行，数值在空载基线附近小幅波动 | 观察日志节奏 |
| T3 | 采样窗口切换为 200ms / 2000ms | 日志节奏随之变化，数值稳定 | 改 Kconfig 重测 |
| T4 | min_free_heap 单调不回升 | 反映历史最低，不随释放回升 | 制造一次大 malloc/free |
| T5 | 关闭 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` | 启动时打印明确警告，不崩溃 | menuconfig 改后重测 |
| T6 | 反复 start/stop | 无内存泄漏、无重复创建任务 | 多次循环后 heap 稳定 |

### 8.4 自动化测试边界
ESP-IDF 组件常宿主单测（host test）用于纯逻辑，但本组件强依赖 FreeRTOS/双核/esp_timer，**不适合 host mock**。因此以 example 工程上板验证为主；仅对"百分比计算函数"（纯函数，输入 delta 数组输出 util）抽离为可单测单元，未来用 `idf.py create-component-test` 做宿主单测。

---

## 9. 扩展性预留（v2+）

- **回调上云**：§5 已留 `performeter_register_cb`，v2 接 MQTT/HTTP 上报。
- **环形缓冲历史**：保留最近 N 次采样供 `performeter_get_history()` 拉取。
- **阈值告警**：util > 90% 或 stack low-water < 阈值时触发用户回调（看门狗式预警）。
- **PC 采样火焰图**：§2.1 方案 D，作为可选高性能 profiling 模块。

---

## 10. 参考资料

- ESP-IDF FreeRTOS（含 SMP 说明）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html
- 官方 real_time_stats 示例：https://github.com/espressif/esp-idf/blob/master/examples/system/freertos/real_time_stats/README.md
- FreeRTOS Run-time Statistics：https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/08-Run-time-statistics
- esp_timer 高精度定时器：https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html
- ESP32 论坛：双核任务统计讨论：https://esp32.com/viewtopic.php?t=36763

---

## 附：交付物清单（实现阶段产出）

- [ ] `performeter/idf_component.yml`
- [ ] `performeter/CMakeLists.txt`
- [ ] `performeter/Kconfig.performeter`
- [ ] `performeter/include/performeter.h`
- [ ] `performeter/src/performeter.c`、`performeter_print.c`
- [ ] `performeter/example/`（含 sdkconfig.defaults 预置配置）
- [ ] `performeter/README.md`（面向使用者，简版使用说明）
