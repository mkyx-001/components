# Performeter — ESP32 性能监视器组件

ESP32 上的周期性性能监视器，基于 FreeRTOS 运行时统计（run-time stats）采集 **每核 CPU 利用率**、**逐任务 CPU 占比排行** 与 **堆/栈水位**，通过串口日志周期输出。作为 ESP-IDF managed component 提供。

> 实现细节与原理见 [`DESIGN.md`](./DESIGN.md)。

## 采集能力

| 指标 | 说明 |
|---|---|
| 每核 CPU 总利用率 | `100% − IDLE 任务占比`，Core0 / Core1 独立计算 |
| 逐任务 CPU 占比 | 采样窗口内的实时占比（非累计平均），降序排行 |
| 空闲堆 / 历史最低空闲堆 | `esp_get_free_heap_size` / `esp_get_minimum_free_heap_size` |
| 栈高水位告警 | 接近溢出（< 256 字节）的任务打 ⚠ 标记 |

## 目录结构

```
performeter/
├── idf_component.yml          # 组件元数据
├── CMakeLists.txt             # 组件构建脚本
├── Kconfig.performeter        # 组件配置项
├── include/performeter.h      # 公共 API
├── src/
│   ├── performeter.c          # 采集核心
│   └── performeter_print.c    # 日志格式化
└── example/                   # 测试工程（含双核负载任务）
```

## 快速使用

### 1. 引用组件

**方式 A — 同一组件库内的工程**（如 `example/`）：在工程顶层 `CMakeLists.txt` 把父目录设为额外组件目录：

```cmake
set(EXTRA_COMPONENT_DIRS "../")
```

**方式 B — 任意工程**：在工程的 `main/idf_component.yml` 中添加：

```yaml
dependencies:
  performeter:
    path: D:/works/components/performeter   # 或相对路径
```

### 2. 开启前置配置

Performeter 依赖 FreeRTOS 运行时统计，需在工程 `sdkconfig` 中开启（组件无法强制覆盖，详见 `DESIGN.md §2.3`）：

```
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_USE_TRACE_FACILITY=y
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y
```

> `example/sdkconfig.defaults` 已预置这些项，可作模板。

### 3. 调用 API

```c
#include "performeter.h"

void app_main(void) {
    // 启动后台监视器，周期用 Kconfig 默认值（1000ms）
    performeter_start(0);
    // 之后每秒在串口看到一行概览 + Top-N 任务
}
```

主动单次采样（阻塞约一个周期）：

```c
size_t cap = 16;
performeter_snapshot_t *snap = malloc(sizeof(performeter_snapshot_t)
                                    + cap * sizeof(performeter_task_info_t));
snap->task_capacity = cap;
performeter_sample_once(snap);
printf("core0 util: %u%%\n", snap->core_util[0] / 100);
free(snap);
```

## 配置项（menuconfig → Performeter）

| 配置项 | 默认 | 说明 |
|---|---|---|
| `CONFIG_PERFORMETER_ENABLE` | y | 编译期总开关，关闭则不构建后台任务 |
| `CONFIG_PERFORMETER_PERIOD_MS` | 1000 | 采样周期，范围 200–5000 ms |
| `CONFIG_PERFORMETER_LOG_TASKS_TOPN` | 8 | 每次打印 Top-N 任务 |
| `CONFIG_PERFORMETER_TASK_STACK` | 4096 | 后台任务栈大小（字节） |

## 运行 example

```bash
cd performeter/example
idf.py set-target esp32      # 或 esp32c3 / esp32c6 / esp32s3 等
idf.py build flash monitor
```

example 不创建任何负载任务，仅启动监视器观察自然状态下的系统 CPU 占用，作为组件
正确性的基线验证。预期串口输出（双核 ESP32 空载示意）：

```
I (21000) performeter: ===== sample 1000ms | core0 util: 0.80%  core1 util: 0.20% | heap free: 286432  min: 263312 =====
I (21001) performeter:   Top 8 tasks (CPU%):
I (21001) performeter:    IDLE0             CORE0  99.10%
I (21001) performeter:    IDLE1             CORE1  99.70%
I (21001) performeter:    ipc0              CORE0   0.20%
I (21001) performeter:    main              CORE0   ...
```

空载下 IDLE 占绝大部分、core util 很低，即说明采集与差分计算正确。

> ⚠ 若你在自己的工程里编写会长时间不让出 CPU 的负载任务，需在该任务内调用
> `esp_task_wdt_reset()` 主动喂狗，否则会触发 Task Watchdog 并复位。

## 验证清单

见 [`DESIGN.md §8.3`](./DESIGN.md)，含 T1–T6 共 6 项人工对照测试。

## 限制

- **精度**：基于 IDLE 任务反推，双核场景存在 ±2–3% 近似误差，用于趋势判断而非绝对计量。
- **采样周期下限**：建议 ≥ 200ms，过短时 IDLE 离散误差显著。
- **测量干扰**：后台任务优先级为 1 且不绑核，仍会带来极小开销。
- **单核 SoC**（esp32c3/c6 等）：`core_util[1]` 恒为 0 且无 IDLE1。

## 许可证

Apache-2.0，见 [`LICENSE`](./LICENSE)。
