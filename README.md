# 🧩 components

ESP-IDF 可复用组件集合，用于集中维护、开发与发布自研 managed component。

---

## 📦 组件列表

| 组件 | 说明 | 文档 |
|:---:|:---|:---|
| 📡 [`cellular_modem`](./packages/cellular_modem/) | USB RNDIS 蜂窝模组驱动（AT 控制面 + RNDIS 数据面，面向 ESP32-P4 等 USB Host 场景） | 📖 [README](./packages/cellular_modem/README.md) |
| 📈 [`performeter`](./packages/performeter/) | 基于 FreeRTOS run-time stats 的性能监视器（CPU 利用率、任务排行、堆/栈水位） | 📖 [README](./packages/performeter/README.md) · 📝 [DESIGN](./packages/performeter/DESIGN.md) |

---

## 📁 仓库结构

```text
components/
├── 📄 README.md
├── 🗂️  components.code-workspace   # VS Code / Cursor 多根工作区
├── packages/
│   ├── 📡 cellular_modem/          # 蜂窝模组组件（库）
│   └── 📈 performeter/             # 性能监视器组件（库）
└── examples/
    ├── cellular_modem_basic/       # cellular_modem 最小例程
    └── performeter_demo/           # performeter 演示工程
```

> 💡 推荐用 `components.code-workspace` 打开本仓库，可同时浏览总览、组件包与示例工程。

---

## 🔌 在工程中使用

### 📂 方式 A — 本地路径引用

在目标工程的 `main/idf_component.yml` 中声明依赖：

```yaml
dependencies:
  cellular_modem:
    path: ../components/packages/cellular_modem
  performeter:
    path: ../components/packages/performeter
```

路径按你的目录布局调整即可。

### 🌐 方式 B — 组件管理器（Registry）

`cellular_modem` 已按 Registry 命名空间发布，可在工程中直接引用：

```yaml
dependencies:
  mkyx-001/cellular_modem: "^1.0.0"
```

### 🧪 方式 C — 同仓库内的 example 工程

在 example 顶层 `CMakeLists.txt` 中把 `packages/` 设为额外组件目录，例如：

```cmake
set(EXTRA_COMPONENT_DIRS "../../packages/cellular_modem")
# 或 performeter: set(EXTRA_COMPONENT_DIRS "../../packages/performeter")
```

`examples/cellular_modem_basic` 与 `examples/performeter_demo` 即采用此方式。

---

## ⚙️ 环境要求

- 🛠️ [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) `>= 5.0`
- 📦 ESP-IDF 组件管理器（`idf.py` 构建时自动处理 `idf_component.yml`）

各组件的目标芯片、额外依赖与 Kconfig 项见对应子目录 README。

---

## 🚀 开发与验证

```bash
# 📡 cellular_modem 例程
cd examples/cellular_modem_basic
idf.py set-target esp32p4
idf.py build flash monitor

# 📈 performeter 例程
cd examples/performeter_demo
idf.py set-target esp32
idf.py build flash monitor
```

---

## 📄 许可证

各组件均采用 Apache-2.0，详见各目录下的 `LICENSE`。

---