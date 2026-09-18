/**
 * filament_sensor.h —— 微动开关（每路 3 个）+ 挤出机到位信号
 * ============================================================================
 *
 * ★ 本次新增的模块，对应你提的新接线要求 ★
 *
 * ----------------------------------------------------------------------------
 * 每路（每个料盘位）三个微动
 * ----------------------------------------------------------------------------
 *     SENSOR_STOP      停止送料微动
 *                      触发 → 立刻停电机。料到位 / 料尽 / 机构顶到头都会触发它。
 *
 *     SENSOR_START     开始送料微动
 *                      触发 → 按常规速度送料，直到"停止送料微动"触发或超时。
 *
 *     SENSOR_AUTOLOAD  自吸微动
 *                      触发 → **全自动上料**，这是最省事的一个：
 *                        ① 自动送料（直到停止送料微动触发 / 超时）
 *                        ② 等挤出机到位信号
 *                        ③ 蠕动送料 3 次收尾，确保耗材被挤出机齿轮咬住
 *                      整个流程不需要人再按任何按钮。
 *
 * ----------------------------------------------------------------------------
 * 一个全局的「挤出机到位信号」
 * ----------------------------------------------------------------------------
 *     自吸流程送到位后靠它确认"耗材真的进了挤出机"。
 *
 *     来源两种（config_get()->extruder_src）：
 *       EXTRUDER_SRC_GPIO  走 board_pins.h 里的 BOARD_PIN_EXTRUDER_INPLACE
 *       EXTRUDER_SRC_MQTT  由打印机上报的事件驱动，调
 *                          extruder_inplace_notify_from_mqtt()
 *                          （有些机器没有空闲 IO 能引出来，就用这个）
 *
 * ----------------------------------------------------------------------------
 * 去抖是怎么做的
 * ----------------------------------------------------------------------------
 *     一个 5ms 的 esp_timer 周期回调把 12 个引脚全读一遍，每个通道独立计数：
 *     只有"连续 N 次读数都和当前稳定值不同"才真的翻转状态。
 *
 *     为什么不用 GPIO 中断：微动是机械触点，按下和松开的瞬间会弹跳几十次
 *     （几十微秒内），中断会被触发上百次，既浪费 CPU 又要在 ISR 里做去抖。
 *     5ms 轮询 + 计数去抖更简单，而且 3 次 × 5ms = 15ms 的响应延迟对这种
 *     送料机构完全够用。
 *
 * ----------------------------------------------------------------------------
 * 等待边沿：用序号，不用标志位
 * ----------------------------------------------------------------------------
 *     每个 (通道, 类型) 都维护两个递增序号：trigger_seq / release_seq。
 *     等一个边沿的标准写法是：
 *
 *         uint32_t s = sensor_trigger_seq(ch, SENSOR_STOP);
 *         while (sensor_trigger_seq(ch, SENSOR_STOP) == s) {
 *             if (超时) break;
 *             vTaskDelay(...);
 *         }
 *
 *     这样不会有"事件在我开始等之前就发生了"的丢失问题（这正是标志位方案的
 *     经典 bug：置位 → 消费 → 清位，中间任何一次丢步都会少一次动作）。
 */

#ifndef FILAMENT_SENSOR_H
#define FILAMENT_SENSOR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 类型
 * ========================================================================== */

/** 微动的三种用途。数值用于数组下标，不要改顺序。 */
typedef enum {
    SENSOR_STOP     = 0,  /**< 停止送料微动 */
    SENSOR_START    = 1,  /**< 开始送料微动 */
    SENSOR_AUTOLOAD = 2,  /**< 自吸微动（触发即全自动上料） */
    SENSOR_KIND_COUNT = 3,
} sensor_kind_t;

/** 去抖参数 */
#define SENSOR_POLL_MS      5    /**< 轮询周期 */
#define SENSOR_DEBOUNCE_N   3    /**< 连续多少次读数一致才认可（3 × 5ms = 15ms） */

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

/**
 * 初始化：
 *   · 把 12 个微动引脚（+ 挤出机到位脚）配成"内部上拉输入"
 *   · 启动 5ms 周期去抖定时器
 *
 * 未安装的通道（config_get()->sensor_enabled_mask 对应位为 0）依然会读引脚，
 * 但 `sensor_channel_enabled()` 返回 false，上层据此走降级模式。
 */
esp_err_t sensor_init(void);

/** 停止去抖定时器（基本只用于测试 / 重启前收尾） */
void sensor_deinit(void);

/* ==========================================================================
 * 读数
 * ========================================================================== */

/**
 * 去抖后的状态：微动是否处于"触发"状态。
 * 通道号用 0 起的**物理料盘位下标**（和 board_pins.h 里数组下标一致）。
 * 越界或未安装时返回 false。
 */
bool sensor_triggered(int channel, sensor_kind_t kind);

/** 原始电平（不去抖），排查接线问题时用 */
int sensor_raw_level(int channel, sensor_kind_t kind);

/** 该 (通道, 类型) 是否配置了引脚 */
bool sensor_pin_present(int channel, sensor_kind_t kind);

/** 该通道是否启用了微动（config 里的 mask，决定走正常模式还是降级模式） */
bool sensor_channel_enabled(int channel);

/** 取消 / 恢复某通道的微动（运行期改配置用，会同时写 config 并保存） */
esp_err_t sensor_set_channel_enabled(int channel, bool enabled);

/** 当前启用掩码（bit0 = 料盘位1） */
uint8_t sensor_enabled_mask(void);

/* ==========================================================================
 * 边沿序号与等待
 * ========================================================================== */

/** 「未触发 → 触发」发生的累计次数 */
uint32_t sensor_trigger_seq(int channel, sensor_kind_t kind);

/** 「触发 → 未触发」发生的累计次数 */
uint32_t sensor_release_seq(int channel, sensor_kind_t kind);

/** 最近一次触发距现在多少毫秒；从未触发过返回 UINT32_MAX */
uint32_t sensor_ms_since_trigger(int channel, sensor_kind_t kind);

/**
 * 等指定通道的微动**被触发**。
 *
 * @param timeout_ms 0 表示只查当前状态、不等待
 * @return true = 在超时前等到了触发
 *
 * 如果一开始就已经处于触发状态，立即返回 true（不需要等一次新的边沿）。
 * 这一点很重要：料本来就压着微动时，"开始送料"应该马上动作，而不是死等。
 */
bool sensor_wait_trigger(int channel, sensor_kind_t kind, uint32_t timeout_ms);

/**
 * 等指定通道的微动**被松开**（离开触发状态）。
 * 用途：确认微动确实回位了，避免机构卡在触发位置导致下一轮动作误判。
 */
bool sensor_wait_release(int channel, sensor_kind_t kind, uint32_t timeout_ms);

/* ==========================================================================
 * 挤出机到位信号
 * ========================================================================== */

/** 信号是否处于"到位"状态 */
bool extruder_inplace_triggered(void);

/** 「到位」发生的累计次数 */
uint32_t extruder_inplace_seq(void);

/** 挤出机到位信号的原始电平；未接 GPIO（走 MQTT）时返回 -1 */
int extruder_inplace_raw_level(void);

/** 最近一次「到位」距现在多少毫秒；从未到位过返回 UINT32_MAX */
uint32_t extruder_inplace_ms_since(void);

/** 清掉到位状态（下一轮自吸流程开始前调） */
void extruder_inplace_clear(void);

/**
 * 由打印机 MQTT 事件驱动时，收到"挤出机到位"上报就调这个。
 * 内部把它记成一次"到位"边沿，效果与 GPIO 触发完全一样，
 * 所以自吸流程不需要区分信号是从哪来的。
 */
void extruder_inplace_notify_from_mqtt(void);

/**
 * 等挤出机到位信号。
 *
 * @param timeout_ms 0 表示只查当前状态
 * @return true = 在超时前等到
 *
 * ⚠️ 注意语义：**已经处于到位状态时立即返回 true**。
 *    如果希望"必须等到一次新的到位事件"，请自己先 snapshot
 *    extruder_inplace_seq() 再轮询它 —— 自吸流程里就是这么做的（先
 *    extruder_inplace_clear() 再等），否则上一轮残留的到位状态会让这一轮
 *    的等待瞬间通过，蠕动收尾就白做了。
 */
bool extruder_inplace_wait(uint32_t timeout_ms);

/* ==========================================================================
 * 状态描述（给网页）
 * ========================================================================== */

/**
 * 生成 JSON（12 个微动的实时状态 + 启用掩码 + 挤出机到位）。
 * 例：
 *   {"enabled":15,"extruder":0,"ch":[[0,0,0],[1,0,0],[0,0,0],[0,0,0]]}
 *   ch[i] 是 [停止, 开始, 自吸] 三个 0/1
 */
int sensor_describe_json(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* FILAMENT_SENSOR_H */
