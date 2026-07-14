/**
 * @file io_expander.h
 * @brief IO Expander GPIO 驱动组件
 *
 * 基于 TCA9539/TCA9555 的 IO 扩展器驱动，使用 I2C 接口
 * SCL: GPIO20, SDA: GPIO21
 */

#ifndef IO_EXPANDER_H
#define IO_EXPANDER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IO Expander 引脚定义 (0-15)
 */
typedef enum {
    IO_EXP_PIN_0  = 0,  /*!< IO Expander Pin 0 */
    IO_EXP_PIN_1  = 1,  /*!< IO Expander Pin 1 */
    IO_EXP_PIN_2  = 2,  /*!< IO Expander Pin 2 */
    IO_EXP_PIN_3  = 3,  /*!< IO Expander Pin 3 */
    IO_EXP_PIN_4  = 4,  /*!< IO Expander Pin 4 */
    IO_EXP_PIN_5  = 5,  /*!< IO Expander Pin 5 */
    IO_EXP_PIN_6  = 6,  /*!< IO Expander Pin 6 */
    IO_EXP_PIN_7  = 7,  /*!< IO Expander Pin 7 */
    IO_EXP_PIN_8  = 8,  /*!< IO Expander Pin 8 */
    IO_EXP_PIN_9  = 9,  /*!< IO Expander Pin 9 */
    IO_EXP_PIN_10 = 10, /*!< IO Expander Pin 10 */
    IO_EXP_PIN_11 = 11, /*!< IO Expander Pin 11 */
    IO_EXP_PIN_12 = 12, /*!< IO Expander Pin 12 */
    IO_EXP_PIN_13 = 13, /*!< IO Expander Pin 13 */
    IO_EXP_PIN_14 = 14, /*!< IO Expander Pin 14 */
    IO_EXP_PIN_15 = 15, /*!< IO Expander Pin 15 */
} io_exp_pin_t;

/**
 * @brief IO 方向
 */
typedef enum {
    IO_EXP_DIR_INPUT  = 0,  /*!< 输入模式 */
    IO_EXP_DIR_OUTPUT = 1,  /*!< 输出模式 */
} io_exp_direction_t;

/**
 * @brief IO 电平
 */
typedef enum {
    IO_EXP_LEVEL_LOW  = 0,  /*!< 低电平 */
    IO_EXP_LEVEL_HIGH = 1,  /*!< 高电平 */
} io_exp_level_t;

/**
 * @brief IO Expander 配置结构
 */
typedef struct {
    uint8_t i2c_addr;          /*!< I2C 地址 (7位地址，默认 0x74) */
    bool use_gpio_wrapper;     /*!< 是否使用 GPIO API 包装器 */
} io_exp_config_t;

/**
 * @brief 初始化 IO Expander
 *
 * 使用默认配置初始化 IO Expander (GPIO20 SCL, GPIO21 SDA)
 *
 * @return ESP_OK 成功, ESP_ERR_xxx 失败
 */
esp_err_t io_expander_init(void);

/**
 * @brief 使用自定义配置初始化 IO Expander
 *
 * @param config 配置参数
 * @return ESP_OK 成功, ESP_ERR_xxx 失败
 */
esp_err_t io_expander_init_with_config(const io_exp_config_t *config);

/**
 * @brief 反初始化 IO Expander
 *
 * @return ESP_OK 成功, ESP_ERR_xxx 失败
 */
esp_err_t io_expander_deinit(void);

/**
 * @brief 设置引脚方向
 *
 * @param pin 引脚号 (0-15)
 * @param direction 方向 (输入/输出)
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t io_expander_set_dir(uint8_t pin, io_exp_direction_t direction);

/**
 * @brief 批量设置引脚方向
 *
 * @param pin_mask 引脚掩码 (bit0=pin0, bit1=pin1, ...)
 * @param direction 方向 (输入/输出)
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t io_expander_set_dir_mask(uint16_t pin_mask, io_exp_direction_t direction);

/**
 * @brief 设置输出引脚电平
 *
 * @param pin 引脚号 (0-15)
 * @param level 电平 (高/低)
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t io_expander_set_level(uint8_t pin, io_exp_level_t level);

/**
 * @brief 批量设置输出引脚电平
 *
 * @param pin_mask 引脚掩码 (bit0=pin0, bit1=pin1, ...)
 * @param level 电平 (高/低)
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t io_expander_set_level_mask(uint16_t pin_mask, io_exp_level_t level);

/**
 * @brief 获取输入引脚电平
 *
 * @param pin 引脚号 (0-15)
 * @param level 输出电平值
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t io_expander_get_level(uint8_t pin, io_exp_level_t *level);

/**
 * @brief 批量获取输入引脚电平
 *
 * @param pin_mask 引脚掩码 (bit0=pin0, bit1=pin1, ...)
 * @param level_mask 输出电平掩码
 * @return ESP_OK 成功, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t io_expander_get_level_mask(uint16_t pin_mask, uint16_t *level_mask);

/**
 * @brief 打印所有引脚状态
 *
 * @return ESP_OK 成功
 */
esp_err_t io_expander_print_state(void);

/**
 * @brief 检查 IO Expander 是否已初始化
 *
 * @return true 已初始化, false 未初始化
 */
bool io_expander_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // IO_EXPANDER_H
