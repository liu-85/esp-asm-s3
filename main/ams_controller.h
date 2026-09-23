/**
 * ams_controller.h —— AMS 业务状态机
 * ============================================================================
 *
 * 对应 MicroPython 版的 AMS_MODEL.py + device_processing.py 里的 material 门面
 * 对象。它是唯一一个"既碰硬件、又碰打印机协议"的模块：
 *
 *     打印机 MQTT 上报 ──┐
 *                        ├──> ams_controller ──> motor / clutch / sensor
 *     网页按钮 / 微动触发 ─┘
 *
 * ----------------------------------------------------------------------------
 * ★ 为什么这里能写得比 Python 版直白得多
 * ----------------------------------------------------------------------------
 * Python 版所有动作都必须写成异步的、不能阻塞的，否则整个 uasyncio 事件循环
 * 会被按住，网页就卡死 —— 换料流程里到处是 `await asyncio.sleep_ms()` 和
 * "两段式 begin()/finish()" 的拆分，就是为了绕开这个限制。
 *
 * ESP-IDF 下 Web 服务在 httpd 自己的任务里，MQTT 在 esp-mqtt 的任务里，
 * 所以 AMS 状态机可以是一个**老老实实的阻塞任务**：
 *
 *     while (1) {
 *         取命令 → （可能阻塞几秒地）执行 → 更新状态
 *     }
 *
 * 送料时 vTaskDelay() 阻塞的是这个任务自己，网页照常响应。这一类"为了不阻塞
 * 而把流程拆得七零八落"的复杂度，从架构上就不存在了。
 *
 * ----------------------------------------------------------------------------
 * ★ 硬件约束（从 Python 版原样继承）
 * ----------------------------------------------------------------------------
 * 任何时刻最多 1 路电磁离合吸合 → 由 clutch.c 强制。
 * 本模块加一层：**所有**吸合/动作都走 ams_bus_guard_* 系列，保证异常路径
 * 也一定停电机 + 断开全部离合（对应 Python 版到处写的 try/finally）。
 *
 * ----------------------------------------------------------------------------
 * ★ 三只微动各自的语义
 * ----------------------------------------------------------------------------
 *   SENSOR_STOP      触发 → 立刻停电机（送料到底了）
 *   SENSOR_START     触发 → 按常规速度送料（人把料插进去、碰到这只就自动走）
 *   SENSOR_AUTOLOAD  触发 → 全自动上料：送料 → 等挤出机到位 → 蠕动 3 次
 *
 * 未装微动的通道自动走**降级模式**：按时间推进送料，时长由
 * AMS_NO_LIMIT_LOAD_MS / AMS_NO_LIMIT_RETRACT_MS 封顶 —— 和 Python 版行为一致。
 */

#ifndef AMS_CONTROLLER_H
#define AMS_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 动作参数（对应 Python 版 hardware_config.py 的第四节）
 * ========================================================================== */

/** 单步推送时长（有微动反馈时每一步走这么久，然后查一次开关） */
#define AMS_FILAMENT_STEP_MS     500
/** 退料最多推几步（有到位开关时触发即停） */
#define AMS_RETRACT_STEPS        15
/** 进料最多重试几轮（有到位开关时到位即停） */
#define AMS_LOAD_RETRY_TIMES     10
/** 打印机拉料时的辅助送料时长 —— ⚠️ 已搬到 config（config_get_assist_ms()，
 *  网页「硬件调试」可改）。这里保留只是为了兼容老引用，新代码别再用它：
 *  写死时长导致现场想调一次就得重烧固件。 */
#define AMS_LOAD_ASSIST_MS       1000

/**
 * 降级模式（未安装微动）下的动作时长上限，单位 ms
 *
 * ⚠️ 必须按你的送料机构实测调整：
 *   - 太小 → 料没送到/收回，看起来"没动"
 *   - 太大 → 料被顶弯、缓冲堆积、甚至顶坏挤出机
 *
 * 退料（retract）：把料从挤出机/缓冲区收回到料盘，通常 10~15 秒。
 * 进料（load）  ：把料推到挤出机入口，通常 5~10 秒。
 */
#define AMS_NO_LIMIT_RETRACT_MS  12000
#define AMS_NO_LIMIT_LOAD_MS     8000
#define AMS_NO_LIMIT_PROBE_MS    4000

/** 自吸流程里等挤出机到位信号的最长时间 */
#define AMS_EXTRUDER_WAIT_MS     15000

/** 主循环节奏 */
#define AMS_TASK_POLL_MS         20

/* ==========================================================================
 * 状态与命令
 * ========================================================================== */

/** 当前正在做什么。网页上会显示成中文。 */
typedef enum {
    AMS_STATE_IDLE = 0,   /**< 空闲 */
    AMS_STATE_JOG,        /**< 手动点动 */
    AMS_STATE_RETRACT,    /**< 退料 */
    AMS_STATE_LOAD,       /**< 送料 */
    AMS_STATE_ASSIST,     /**< 打印机拉料，辅助送料 */
    AMS_STATE_CREEP,      /**< 蠕动送料收尾 */
    AMS_STATE_AUTOLOAD,   /**< 自吸上料全流程 */
    AMS_STATE_EXCHANGE,   /**< 换料 */
    AMS_STATE_ERROR,      /**< 出错（等下一次指令恢复） */
} ams_state_t;

/** 命令类型 */
typedef enum {
    AMS_CMD_NONE = 0,
    AMS_CMD_JOG,          /**< 手动点动：channel=料盘位下标, arg=方向, ms=时长 */
    AMS_CMD_LOAD,         /**< 送料到挤出机 */
    AMS_CMD_RETRACT,      /**< 退料回料盘 */
    AMS_CMD_AUTOLOAD,     /**< 自吸上料（送料 + 挤出机到位 + 蠕动 3 次） */
    AMS_CMD_EXCHANGE,     /**< 换料：channel = **打印机通道号**（1 起） */
    AMS_CMD_ASSIST_TEST,  /**< 辅助送料自检：用当前网页配置的占空比/时长
                           *   手动跑**一次**辅助送料（channel = 料盘位下标，
                           *   -1 = 用当前通道）。见 ams_post_assist_test() */
    AMS_CMD_STOP,         /**< 立即停止所有动作 */
} ams_cmd_id_t;

typedef struct {
    ams_cmd_id_t id;
    int channel;   /**< 料盘位下标（0 起）；AMS_CMD_EXCHANGE 时是打印机通道号 */
    int arg;       /**< 方向（1 进料 / -1 退料）等附加参数 */
    int ms;        /**< 时长（毫秒）；0 表示用默认值 */
} ams_cmd_t;

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

/**
 * 创建命令队列与同步对象。必须在 ams_start() 之前调用。
 * 硬件（motor / clutch / sensor）由 main.c 先初始化好。
 */
esp_err_t ams_init(void);

/** 启动 AMS 任务（FreeRTOS task） */
esp_err_t ams_start(void);

/**
 * 在 WiFi 连接完成后调用，启动 MQTT 客户端连接打印机。
 * 必须在 ams_init() 之后、ams_start() 之后调用。
 */
esp_err_t ams_connect_printer(void);

/* ==========================================================================
 * 投递命令（线程安全，可以从 Web / MQTT / 任意任务调用）
 * ========================================================================== */

/**
 * 投递一条命令。
 *
 * @return true 已入队；false 队列满（说明设备正忙，调用方应该提示用户稍后再试）
 *
 * ★ 队列满时**不等待**，直接返回 false —— 这正好对应 Python 版
 *   "总线忙的时候直接拒绝并说明，而不是让用户排长队等" 的做法。
 */
bool ams_post_cmd(const ams_cmd_t *cmd);

/** 便捷封装：手动点动某个料盘位 */
bool ams_post_jog(int material_index, int direction, int ms);

/** 便捷封装：立即停止 */
bool ams_post_stop(void);

/** 便捷封装：送料（跑到挤出机） */
bool ams_post_load(int material_index);

/** 便捷封装：退料 */
bool ams_post_retract(int material_index);

/** 便捷封装：自吸上料 */
bool ams_post_autoload(int material_index);

/** 便捷封装：换料到指定打印机通道（1 起） */
bool ams_post_exchange(int printer_channel);

/**
 * 便捷封装：**辅助送料自检** —— 立刻按网页上的「辅助送料」配置
 * （占空比 / 时长）驱动一次当前通道的电机，并用完整回执把硬件回读打出来。
 *
 * ★ 为什么要有这个按钮（2026-09-23 现场反馈）：
 *   用户报"辅助送料没作用，电磁吸合了但电机不转"。而辅助送料是**由打印机
 *   阶段驱动**的 —— 要验证它，正常得等下一轮打印、等到校准阶段，一次十几
 *   分钟，还不一定复现。这个自检把"验证一次辅助送料"变成点一下按钮：
 *     · 手动点动（duty 255）转、辅助送料（自检，duty 随网页设置）不转
 *       → 就是占空比不够，把网页上的速度往上调；
 *     · 两个都不转 → 硬件链路（H 桥使能、离合齿轮、接线），不是软件。
 *
 * @param material_index 料盘位下标（0 起）；-1 = 用当前记录的通道
 */
bool ams_post_assist_test(int material_index);

/* ==========================================================================
 * 状态查询（线程安全，网页可随便调）
 * ========================================================================== */

ams_state_t ams_get_state(void);

/** 当前状态的中文说明（静态字符串） */
const char *ams_state_text(void);

/** 是否有动作正在执行（等价于 state != IDLE） */
bool ams_is_busy(void);

/** 当前正在操作的料盘位下标；-1 表示无 */
int ams_active_material(void);

/** 当前记录在用的打印机通道号（0 = 未知） */
int ams_current_printer_channel(void);

/** 把一个打印机通道号翻成中文描述，如 "通道 2（料盘位 2）" */
int ams_channel_text(int printer_channel, char *buf, size_t buflen);

/* ==========================================================================
 * 诊断 / 状态输出
 * ========================================================================== */

/**
 * 一次驱动动作的「执行回执」—— 现场诊断用。
 *
 * ★ 为什么必须有它（2026-09-23 现场反馈）：
 *   用户报"日志显示在辅助送料，但电机没动作、电磁离合也没吸合"。
 *   复查流程时发现一处致命的信息丢失：**日志是在调用 drive_channel_speed()
 *   之前打的**，而那个函数的返回值被 `(void)` 丢掉了。于是离合吸合失败时
 *   （吸合前复核发现残留吸合 → ESP_ERR_CLUTCH_CONFLICT，或 3 秒拿不到锁 →
 *   ESP_ERR_TIMEOUT），它只 record_error 再 return false，现场看到的
 *   **仍然**是"辅助送料（校准挤出）…"那一行 —— "指令发出去了"和
 *   "硬件真的动了"从此没法区分。
 *
 *   回执把"指令到底有没有落到硬件上"固定下来：离合是否真吸合、实际写进
 *   LEDC 的 duty 原值、电机真正通了多久电。除跟随日志打印外，/status 的
 *   `drive` 字段也暴露它，所以远程就能判断，不必再抓串口日志。
 */
typedef struct {
    bool     clutch_ok;   /**< 离合是否真的吸合成功；false = 本次完全没驱动电机 */
    bool     skipped;     /**< true = 电机层幂等短路（同方向同速度已在转），
                           *   没有重新驱动 —— 这也是"车不动"的一条可能路径 */
    int8_t   dir;         /**< 本次驱动方向：1 进料 / -1 退料 / 0 未驱动 */
    uint8_t  speed_pct;   /**< 本次请求的占空比（%） */
    uint16_t duty_raw;    /**< 请求对应的 LEDC 原值（0~255）—— **算出来的**，
                           *   只说明"我们打算写多少"。手动点动是全速 255，
                           *   辅助送料 60% 只有 153。 */
    uint32_t ran_ms;      /**< 电机实际通电时长（ms） */

    /* ---- ★ 2026-09-23 新增：硬件回读（不是记账，是真读寄存器/引脚）----
     *
     * 为什么必须补这一组：用户报"辅助送料没作用、电磁吸合了但电机没信号"。
     * 当时回执里的 duty_raw 是 motor_pct_to_duty() 算出来的，它只能证明
     * "我们打算写 153"，证明不了"寄存器里真的是 153、IN1 真的在翻转"。
     * 现场看到 duty=153 却仍不转时，就没法把"软件没写进去"和"写了但带不动"
     * 分开 —— 这正是这个项目反复踩的那类"不可证伪"的坑。
     *
     * 有这组数之后，一次判断就够：
     *   dutys[] == 153/0 且 levels[] == 1/0 → 信号确实出去了，不转 = 占空比/机械
     *   dutys[] 全 0                        → 根本没写到硬件，是软件路径问题 */
    uint32_t duty_in1;    /**< 回读：IN1 的 LEDC 寄存器原值（0~255） */
    uint32_t duty_in2;    /**< 回读：IN2 的 LEDC 寄存器原值（0~255） */
    int8_t   level_in1;   /**< 回读：IN1 引脚电平 0/1；-1 = 读不到 */
    int8_t   level_in2;   /**< 回读：IN2 引脚电平 0/1；-1 = 读不到 */
} drive_receipt_t;

/** 读最近一次驱动动作的执行回执（任何一次送料/退料/点动都会刷新它） */
const drive_receipt_t *ams_last_drive_receipt(void);

/** 累计换料次数 / 失败次数 / 自吸次数，给网页的诊断面板用 */
typedef struct {
    uint32_t exchange_ok;
    uint32_t exchange_fail;
    uint32_t autoload_ok;
    uint32_t autoload_fail;
    uint32_t load_ok;
    uint32_t retract_ok;
    uint32_t jog_count;
    uint32_t last_error_ms;   /**< 最近一次出错距今多少毫秒；UINT32_MAX 表示没出过错 */
    char     last_error[80];
} ams_diag_t;

void ams_get_diag(ams_diag_t *out);

/** 清零统计（网页"重置诊断"用） */
void ams_reset_diag(void);

/**
 * 生成硬件 + 业务状态的 JSON（给网页，避免前端发多个请求）。
 * 形如：
 *   {"active":2,"engaged":[2],"conflicts":0,"motor_direction":1,
 *    "channels":[1,2,3,4],"busy":false,"owner":"","limits":[...]}
 */
int ams_describe_hardware_json(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* AMS_CONTROLLER_H */
