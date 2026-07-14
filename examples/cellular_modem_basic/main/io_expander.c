/**
 * @file io_expander.c
 * @brief IO Expander GPIO 驱动组件实现
 */

#include "io_expander.h"
#include "esp_log.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "IO_EXPANDER";

// I2C 配置
#define I2C_SCL_PIN         GPIO_NUM_20  // SCL 引脚
#define I2C_SDA_PIN         GPIO_NUM_21  // SDA 引脚
#define I2C_CLOCK_SPEED     100000       // I2C 时钟频率 100kHz
#define I2C_PORT            I2C_NUM_0    // I2C 端口

// 默认 I2C 地址 (TCA9555 A0=0, A1=0)
#define DEFAULT_I2C_ADDR    ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000

// IO Expander 句柄
static esp_io_expander_handle_t g_io_expander_handle = NULL;
static i2c_master_bus_handle_t g_i2c_bus_handle = NULL;
static bool g_initialized = false;

/**
 * @brief 使用默认配置初始化 IO Expander
 *
 * 使用默认配置初始化 IO Expander，I2C 地址为 0x20 (TCA9555 A0=0, A1=0)
 * I2C 引脚配置为：GPIO20 (SCL), GPIO21 (SDA)
 *
 * @return ESP_OK 成功, ESP_ERR_xxx 失败
 */
esp_err_t io_expander_init(void)
{
    io_exp_config_t config = {
        .i2c_addr = DEFAULT_I2C_ADDR,
        .use_gpio_wrapper = false
    };
    return io_expander_init_with_config(&config);
}

/**
 * @brief 使用自定义配置初始化 IO Expander
 *
 * 使用用户提供的配置初始化 IO Expander 和 I2C 总线
 * I2C 引脚固定为：GPIO20 (SCL), GPIO21 (SDA)
 *
 * @param config 配置参数，包含 I2C 地址等
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数为空, ESP_ERR_xxx I2C/设备初始化失败
 */
esp_err_t io_expander_init_with_config(const io_exp_config_t *config)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "IO Expander already initialized");
        return ESP_OK;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing IO Expander...");
    ESP_LOGI(TAG, "  I2C SCL: GPIO%d", I2C_SCL_PIN);
    ESP_LOGI(TAG, "  I2C SDA: GPIO%d", I2C_SDA_PIN);
    ESP_LOGI(TAG, "  I2C Address: 0x%02X", config->i2c_addr);

    // 1. 创建 I2C 总线
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &g_i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C bus created successfully");

    // 2. 创建 IO Expander
    ret = esp_io_expander_new_i2c_tca95xx_16bit(g_i2c_bus_handle, config->i2c_addr, &g_io_expander_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create IO expander: %s", esp_err_to_name(ret));
        i2c_del_master_bus(g_i2c_bus_handle);
        g_i2c_bus_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "IO Expander created successfully");

    // 3. 重置 IO Expander 到默认状态
    ret = esp_io_expander_reset(g_io_expander_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reset IO expander: %s", esp_err_to_name(ret));
    }

    g_initialized = true;
    ESP_LOGI(TAG, "IO Expander initialized successfully");

    return ESP_OK;
}

/**
 * @brief 反初始化 IO Expander
 *
 * 释放 IO Expander 和 I2C 总线资源，将句柄置空
 *
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t io_expander_deinit(void)
{
    if (!g_initialized) {
        ESP_LOGW(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    // 删除 IO Expander
    if (g_io_expander_handle != NULL) {
        ret = esp_io_expander_del(g_io_expander_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete IO expander: %s", esp_err_to_name(ret));
        }
        g_io_expander_handle = NULL;
    }

    // 删除 I2C 总线
    if (g_i2c_bus_handle != NULL) {
        ret = i2c_del_master_bus(g_i2c_bus_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete I2C bus: %s", esp_err_to_name(ret));
        }
        g_i2c_bus_handle = NULL;
    }

    g_initialized = false;
    ESP_LOGI(TAG, "IO Expander deinitialized");

    return ESP_OK;
}

/**
 * @brief 设置单个引脚的输入/输出方向
 *
 * 将指定引脚配置为输入或输出模式
 *
 * @param pin 引脚号 (0-15)
 * @param direction 方向 (IO_EXP_DIR_INPUT/IO_EXP_DIR_OUTPUT)
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_ERR_INVALID_ARG 引脚号无效
 */
esp_err_t io_expander_set_dir(uint8_t pin, io_exp_direction_t direction)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pin >= 16) {
        ESP_LOGE(TAG, "Invalid pin number: %d (must be 0-15)", pin);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pin_mask = (1U << pin);
    esp_io_expander_dir_t exp_dir = (direction == IO_EXP_DIR_OUTPUT) ?
                                     IO_EXPANDER_OUTPUT : IO_EXPANDER_INPUT;

    esp_err_t ret = esp_io_expander_set_dir(g_io_expander_handle, pin_mask, exp_dir);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pin %d direction: %s", pin, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Set pin %d direction to %s", pin,
             (direction == IO_EXP_DIR_OUTPUT) ? "OUTPUT" : "INPUT");
    return ESP_OK;
}

/**
 * @brief 批量设置多个引脚的输入/输出方向
 *
 * 使用位掩码同时设置多个引脚的方向，bit0 对应 Pin0，bit1 对应 Pin1，以此类推
 *
 * @param pin_mask 引脚掩码 (例如 0x000F 表示 Pin 0-3)
 * @param direction 方向 (IO_EXP_DIR_INPUT/IO_EXP_DIR_OUTPUT)
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_ERR_INVALID_ARG 掩码为空
 */
esp_err_t io_expander_set_dir_mask(uint16_t pin_mask, io_exp_direction_t direction)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pin_mask == 0) {
        ESP_LOGE(TAG, "Pin mask is empty");
        return ESP_ERR_INVALID_ARG;
    }

    esp_io_expander_dir_t exp_dir = (direction == IO_EXP_DIR_OUTPUT) ?
                                     IO_EXPANDER_OUTPUT : IO_EXPANDER_INPUT;

    esp_err_t ret = esp_io_expander_set_dir(g_io_expander_handle, pin_mask, exp_dir);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pin mask 0x%04X direction: %s", pin_mask, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Set pin mask 0x%04X direction to %s", pin_mask,
             (direction == IO_EXP_DIR_OUTPUT) ? "OUTPUT" : "INPUT");
    return ESP_OK;
}

/**
 * @brief 设置单个输出引脚的电平
 *
 * 设置指定引脚输出高电平或低电平，引脚必须先配置为输出模式
 *
 * @param pin 引脚号 (0-15)
 * @param level 电平 (IO_EXP_LEVEL_LOW/IO_EXP_LEVEL_HIGH)
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_ERR_INVALID_ARG 引脚号无效
 */
esp_err_t io_expander_set_level(uint8_t pin, io_exp_level_t level)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pin >= 16) {
        ESP_LOGE(TAG, "Invalid pin number: %d (must be 0-15)", pin);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pin_mask = (1U << pin);

    esp_err_t ret = esp_io_expander_set_level(g_io_expander_handle, pin_mask, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pin %d level: %s", pin, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Set pin %d level to %s", pin,
             (level == IO_EXP_LEVEL_HIGH) ? "HIGH" : "LOW");
    return ESP_OK;
}

/**
 * @brief 批量设置多个输出引脚的电平
 *
 * 使用位掩码同时设置多个引脚输出相同的电平，bit0 对应 Pin0，bit1 对应 Pin1，以此类推
 *
 * @param pin_mask 引脚掩码 (例如 0x000F 表示 Pin 0-3)
 * @param level 电平 (IO_EXP_LEVEL_LOW/IO_EXP_LEVEL_HIGH)
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_ERR_INVALID_ARG 掩码为空
 */
esp_err_t io_expander_set_level_mask(uint16_t pin_mask, io_exp_level_t level)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pin_mask == 0) {
        ESP_LOGE(TAG, "Pin mask is empty");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_io_expander_set_level(g_io_expander_handle, pin_mask, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pin mask 0x%04X level: %s", pin_mask, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Set pin mask 0x%04X level to %s", pin_mask,
             (level == IO_EXP_LEVEL_HIGH) ? "HIGH" : "LOW");
    return ESP_OK;
}

/**
 * @brief 获取单个引脚的输入电平
 *
 * 读取指定引脚的当前电平状态，引脚可以是输入或输出模式
 *
 * @param pin 引脚号 (0-15)
 * @param level 输出电平值指针
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_ERR_INVALID_ARG 引脚号或指针无效
 */
esp_err_t io_expander_get_level(uint8_t pin, io_exp_level_t *level)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pin >= 16) {
        ESP_LOGE(TAG, "Invalid pin number: %d (must be 0-15)", pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (level == NULL) {
        ESP_LOGE(TAG, "Level pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pin_mask = (1U << pin);
    uint32_t level_mask = 0;

    esp_err_t ret = esp_io_expander_get_level(g_io_expander_handle, pin_mask, &level_mask);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get pin %d level: %s", pin, esp_err_to_name(ret));
        return ret;
    }

    *level = (level_mask & pin_mask) ? IO_EXP_LEVEL_HIGH : IO_EXP_LEVEL_LOW;

    ESP_LOGD(TAG, "Get pin %d level: %s", pin,
             (*level == IO_EXP_LEVEL_HIGH) ? "HIGH" : "LOW");
    return ESP_OK;
}

/**
 * @brief 批量获取多个引脚的输入电平
 *
 * 使用位掩码同时读取多个引脚的电平状态，结果也以位掩码形式返回
 *
 * @param pin_mask 引脚掩码 (例如 0x00F0 表示 Pin 4-7)
 * @param level_mask 输出电平掩码指针，返回对应引脚的电平状态
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_ERR_INVALID_ARG 掩码或指针无效
 */
esp_err_t io_expander_get_level_mask(uint16_t pin_mask, uint16_t *level_mask)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (pin_mask == 0) {
        ESP_LOGE(TAG, "Pin mask is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (level_mask == NULL) {
        ESP_LOGE(TAG, "Level mask pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t mask = 0;
    esp_err_t ret = esp_io_expander_get_level(g_io_expander_handle, pin_mask, &mask);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get pin mask 0x%04X level: %s", pin_mask, esp_err_to_name(ret));
        return ret;
    }

    *level_mask = (uint16_t)mask;

    ESP_LOGD(TAG, "Get pin mask 0x%04X level: 0x%04X", pin_mask, *level_mask);
    return ESP_OK;
}

/**
 * @brief 打印所有引脚的当前状态
 *
 * 在日志中输出所有 16 个引脚的方向、输入电平和输出电平信息
 *
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t io_expander_print_state(void)
{
    if (!g_initialized || g_io_expander_handle == NULL) {
        ESP_LOGE(TAG, "IO Expander not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return esp_io_expander_print_state(g_io_expander_handle);
}

/**
 * @brief 检查 IO Expander 是否已初始化
 *
 * 查询 IO Expander 组件的初始化状态
 *
 * @return true 已初始化, false 未初始化
 */
bool io_expander_is_initialized(void)
{
    return g_initialized;
}
