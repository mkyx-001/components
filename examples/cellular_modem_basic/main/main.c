#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cellular_modem.h"
#include "io_expander.h"

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

/*
 * 蜂窝 netif 接入：netif_name 同时作为 esp_netif 的 if_key（内核侧 netif 名字，如
 * "cell_rndis" → "cell_rndis0"）与 if_desc（路由表/日志里的可读描述）。
 * route_priority 决定蜂窝作为默认路由的优先级——数值越大越优先，可与以太网/Wi-Fi 抢占。
 * 蜂窝作为唯一外网时保持 50 即可；若与有线/无线并存，建议设为更高（如 60）使其成为首选。
 */
#ifndef CELL_MODEM_NETIF_NAME
#define CELL_MODEM_NETIF_NAME        "cell_rndis"
#endif
#ifndef CELL_MODEM_ROUTE_PRIORITY
#define CELL_MODEM_ROUTE_PRIORITY    50
#endif

/*
 * TCP 连接测试：周期连接指控服务器，验证 4G 到业务端口的真实可达性。
 * 故意不依赖 cell_modem_is_data_path_ok()——直接发包，原始 errno
 * 一旦非 0 就能立刻看到（典型表现：errno=113 EHOSTUNREACH，
 * 表示 modem 网关回了 ICMP 不可达；errno=110 表示 SYN 无响应）。
 *
 * 当前现场测试目标为指控服务器 218.94.126.124:9999。
 */
#ifndef CELL_MODEM_TCP_TEST_HOST
#define CELL_MODEM_TCP_TEST_HOST     "218.94.126.124"
#endif
#ifndef CELL_MODEM_TCP_TEST_PORT
#define CELL_MODEM_TCP_TEST_PORT     9999
#endif
#ifndef CELL_MODEM_TCP_TEST_INTERVAL_MS
#define CELL_MODEM_TCP_TEST_INTERVAL_MS  30000   /* 默认 30s 一次 */
#endif
#ifndef CELL_MODEM_TCP_TEST_RECV_TIMEOUT_MS
#define CELL_MODEM_TCP_TEST_RECV_TIMEOUT_MS  5000
#endif

static const char *TAG = "cell_modem_example";

/* IO Expander 隔离测试：拉高 Pin 3 并保持（latch） */
static void open_io_exp_pin3(void);

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

/*
 * TCP 连通性测试任务：
 *   1) 等 cell_modem_is_pdp_ready() 真为 true
 *   2) 解析目标地址
 *   3) bind 到 cell_rndis 的本地 IP（不调用 set_default_netif，避免污染
 *      其他 netif 的默认路由）
 *   4) connect() 并计时，成功后立即关闭，不向业务服务器发送测试负载
 *
 * 失败时打印原始 errno，便于区分 EHOSTUNREACH、ETIMEDOUT 和
 * ECONNREFUSED；成功则证明 4G 到业务端口的 TCP 三次握手完整可达。
 */
static void tcp_test_task(void *arg)
{
    int round = 0;

    ESP_LOGI(TAG, "tcp_test: target=%s:%d interval=%dms recv_to=%dms",
             CELL_MODEM_TCP_TEST_HOST, CELL_MODEM_TCP_TEST_PORT,
             CELL_MODEM_TCP_TEST_INTERVAL_MS,
             CELL_MODEM_TCP_TEST_RECV_TIMEOUT_MS);

    while (1) {
        round++;

        /* 等 PDP 就绪再测——但也兼容 PDP 长期 false 的故障情形，输出原始错误 */
        if (!cell_modem_is_pdp_ready()) {
            ESP_LOGW(TAG, "tcp_test #%d: PDP not ready (state=%s), retry in 5s",
                     round, state_name(cell_modem_get_state()));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        cell_modem_net_info_t info = {0};
        cell_modem_get_net_info(&info);
        ESP_LOGI(TAG, "=========== tcp_test #%d ===========", round);
        ESP_LOGI(TAG, "local=%s gw=%s", info.ip, info.gateway);

        /* 1) DNS 解析 */
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        struct addrinfo *res = NULL;
        int gret = getaddrinfo(CELL_MODEM_TCP_TEST_HOST, NULL, &hints, &res);
        if (gret != 0 || res == NULL) {
            /* getaddrinfo 错误码为 EAI_*，与 errno 不一定互通，仅打印数值
             * 常见值：EAI_NONAME(-2) 名称无解析；EAI_AGAIN(-3) 临时失败；EAI_FAIL(-4) */
            ESP_LOGE(TAG, "DNS %s fail: ret=%d (EAI_*)",
                     CELL_MODEM_TCP_TEST_HOST, gret);
            ESP_LOGI(TAG, "===================================");
            vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_TCP_TEST_INTERVAL_MS));
            continue;
        }

        char remote_ip[16] = {0};
        struct sockaddr_in *remote = (struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(remote->sin_addr, remote_ip, sizeof(remote_ip));
        remote->sin_port = htons(CELL_MODEM_TCP_TEST_PORT);
        ESP_LOGI(TAG, "remote=%s:%d", remote_ip, CELL_MODEM_TCP_TEST_PORT);

        /* 2) socket */
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() fail: errno=%d (%s)", errno, strerror(errno));
            freeaddrinfo(res);
            ESP_LOGI(TAG, "===================================");
            vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_TCP_TEST_INTERVAL_MS));
            continue;
        }

        /* 3) bind 到 cell_rndis 本地 IP（锁定出口接口） */
        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_port = htons(0);
        if (inet_aton(info.ip, &local.sin_addr) == 0) {
            ESP_LOGE(TAG, "inet_aton(%s) fail", info.ip);
            close(sock);
            freeaddrinfo(res);
            ESP_LOGI(TAG, "===================================");
            vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_TCP_TEST_INTERVAL_MS));
            continue;
        }
        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            ESP_LOGE(TAG, "bind(%s) fail: errno=%d (%s)",
                     info.ip, errno, strerror(errno));
            close(sock);
            freeaddrinfo(res);
            ESP_LOGI(TAG, "===================================");
            vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_TCP_TEST_INTERVAL_MS));
            continue;
        }
        ESP_LOGI(TAG, "bound to %s", info.ip);

        /* 4) 收发超时 */
        struct timeval tv = {
            .tv_sec  = CELL_MODEM_TCP_TEST_RECV_TIMEOUT_MS / 1000,
            .tv_usec = (CELL_MODEM_TCP_TEST_RECV_TIMEOUT_MS % 1000) * 1000,
        };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* 5) connect（计 connect 时延） */
        TickType_t t0 = xTaskGetTickCount();
        int cret = connect(sock, (struct sockaddr *)remote, sizeof(*remote));
        uint32_t connect_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
        if (cret != 0) {
            ESP_LOGE(TAG, "connect() FAIL errno=%d (%s) after %ums",
                     errno, strerror(errno), (unsigned)connect_ms);
            close(sock);
            freeaddrinfo(res);
            ESP_LOGI(TAG, "===================================");
            vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_TCP_TEST_INTERVAL_MS));
            continue;
        }
        ESP_LOGI(TAG, "TCP connected in %ums", (unsigned)connect_ms);
        ESP_LOGI(TAG, "tcp_test #%d: business port reachable ✓", round);

        close(sock);
        freeaddrinfo(res);
        ESP_LOGI(TAG, "===================================");
        vTaskDelay(pdMS_TO_TICKS(CELL_MODEM_TCP_TEST_INTERVAL_MS));
    }
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
        .netif_name = CELL_MODEM_NETIF_NAME,
        .route_priority = CELL_MODEM_ROUTE_PRIORITY,
    };

    ESP_LOGI(TAG, "cellular modem basic example start");

    ESP_ERROR_CHECK(cell_modem_early_hardware_reset(hw.rst_gpio));
    cell_modem_register_status_callback(modem_state_cb);
    ESP_ERROR_CHECK(cell_modem_init(&hw, &cfg));

    /* 启动 TCP 连通性测试（独立任务，PDP 阻塞等待由 cell_modem_wait_for_pdp_ready 完成） */
    BaseType_t tcp_task_ret = xTaskCreate(tcp_test_task, "tcp_test",
                                          4096, NULL, 4, NULL);
    if (tcp_task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tcp_test task");
    }

    if (!cell_modem_wait_for_pdp_ready(120000)) {
        ESP_LOGW(TAG, "PDP not ready within timeout");
    }

    /* 拓展 IO 测试：拉高 IO Expander Pin 3 并保持 */
    open_io_exp_pin3();

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

/*
 * IO Expander 隔离测试入口 —— 拉高 Pin 3 并保持。
 * 默认接线：GPIO20 SCL / GPIO21 SDA / TCA9555 地址 0x20。
 * 失败仅打日志，不影响蜂窝主流程。
 */
static void open_io_exp_pin3(void)
{
    const char *TAG_IO = "IO_EXP";
    esp_err_t ret = io_expander_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_IO, "io_expander_init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG_IO, "check I2C wiring (GPIO20 SCL / GPIO21 SDA) and TCA9555 address");
        return;
    }

    ESP_ERROR_CHECK(io_expander_set_dir(IO_EXP_PIN_3, IO_EXP_DIR_OUTPUT));
    ESP_ERROR_CHECK(io_expander_set_level(IO_EXP_PIN_3, IO_EXP_LEVEL_HIGH));
    ESP_LOGI(TAG_IO, "IO Expander Pin 3 set HIGH (kept latched)");
}
