/**
 * motor.h —— 共享直流电机（H 桥 IN1/IN2）
 * ============================================================================
 *
 * 和 MicroPython 版 motor_clutch.HBridgeMotor 的语义完全一致：
 *     direction =  1  → 进料（正转）
 *     direction = -1  → 退料（反转）
 *     direction =  0  → 停止
 *
 * 换向时先停止、等死区时间再反向，避免 H 桥上下管直通（直通会瞬间烧毁驱动
 * 芯片），同时减小对机械结构的冲击。
 *
 * ----------------------------------------------------------------------------
 * 相比 MicroPython 版新增：**PWM 调速**
 * ----------------------------------------------------------------------------
 *   Python 版只有"转 / 不转"两种状态（GPIO 高低电平）。这次要做的「蠕动送料」
 *   （自吸流程收尾那 3 下）需要**慢速**，所以这里把 IN1/IN2 接到 LEDC 的 PWM
 *   通道上：
 *
 *       进料： IN1 = duty(速度)   IN2 = 0
 *       退料： IN1 = 0            IN2 = duty(速度)
 *       停止： IN1 = 0            IN2 = 0
 *
 *   AT8236 这类 H 桥的 IN 脚本身就是 PWM 输入（真值表里 IN1=PWM / IN2=0 就是
 *   正转调速），所以不需要额外电路，直接把原来的 GPIO 换成 PWM 输出即可。
 *
 *   PWM 频率取 20kHz —— 高于人耳可听范围，电机不会发出"滋滋"声。占空比
 *   0~255 对应 0~100%。
 *
 *   ⚠️ 全速运行时占空比是 255，也就是恒高电平，和原来纯 GPIO 驱动的效果
 *      一模一样 —— 所以这次改动不会影响原有的换料节奏。
 */

#ifndef MOTOR_H
#define MOTOR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 全速（百分比）。LEDC 的 8 位分辨率（0~255）由 motor.c 内部换算，这里只表示"百分之百" */
#define MOTOR_SPEED_FULL 100

/** 方向。数值直接乘在 PWM 上，所以必须是 ±1 / 0。 */
typedef enum {
    MOTOR_DIR_RETRACT = -1,  /**< 退料（反转） */
    MOTOR_DIR_STOP    =  0,  /**< 停止 */
    MOTOR_DIR_FEED    =  1,  /**< 进料（正转） */
} motor_dir_t;

/**
 * 初始化电机：配置两个 LEDC 通道，并把输出清零（电机静止）。
 * 对应 Python 版 build_motor_bus() 里"上电先确保不动"的那一步。
 */
esp_err_t motor_init(void);

/** 立即停止（两路 PWM 都写 0），并记录停止时刻 */
void motor_stop(void);

/**
 * 设置方向并立即启动，**不阻塞**。速度用上一次设定的值（默认全速）。
 * 同方向重复调用是幂等的，不会做无谓的"停-启"（避免机构抖动）。
 */
esp_err_t motor_set_dir(motor_dir_t dir);

/**
 * 设置方向 + 速度并立即启动，不阻塞。
 * @param speed_pct 0~100，0 等同于停止；超过 100 会被夹到 100
 */
esp_err_t motor_set_dir_speed(motor_dir_t dir, int speed_pct);

/** 阻塞运行 ms 毫秒（跑完**不会**自动停，调用方自己决定） */
esp_err_t motor_run(motor_dir_t dir, uint32_t ms);

/** 阻塞运行 ms 毫秒 + 指定速度。蠕动送料用这个。 */
esp_err_t motor_run_speed(motor_dir_t dir, uint32_t ms, int speed_pct);

/** 阻塞运行 ms 毫秒后自动停止 */
esp_err_t motor_run_then_stop(motor_dir_t dir, uint32_t ms);

/** 当前方向（0 = 停） */
motor_dir_t motor_get_dir(void);

/** 当前速度百分比 */
int motor_get_speed_pct(void);

/** 当前方向的持续时长（毫秒）；停止时返回 0 */
uint32_t motor_elapsed_ms(void);

/** 换向死区时间（毫秒），初始化时从配置取 */
void motor_set_dead_time_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_H */
