/**
 * ams_controller.c —— AMS 业务状态机的实现
 * ============================================================================
 *
 * 这个文件把三样东西接在一起：
 *
 *     打印机（MQTT 上报 / 命令）
 *          ↕
 *     ams_controller  ←── 命令队列 ←── 网页 / 微动触发
 *          ↕
 *     电机 / 电磁离合 / 微动（GPIO）
 *
 * ----------------------------------------------------------------------------
 * 任务模型
 * ----------------------------------------------------------------------------
 * 只有一个 FreeRTOS 任务 `ams_task`：
 *
 *     while (1) {
 *         ① 离合体检（防两路同时吸合）
 *         ② 有新的打印机上报？→ 处理（可能触发换料）
 *         ③ 有命令？→ 取出来执行（**阻塞**执行，几秒也照跑）
 *         ④ 检查"开始送料 / 自吸"微动是否被触发
 *         ⑤ 更新状态灯
 *         ⑥ 睡 20ms
 *     }
 *
 * 第 ③ 步会阻塞这个任务好几秒（送料、等挤出机、蠕动……），这完全没有问题 ——
 * Web 服务在 httpd 的任务里，MQTT 在 esp-mqtt 的任务里，都不受影响。
 * 这一点是相对 MicroPython 版最大的结构性简化：Python 版为了不让事件循环
 * 被按住，把每个动作都拆成了 `await` 片段，代码读起来非常绕。
 *
 * ----------------------------------------------------------------------------
 * 安全约定（**改这个文件时请务必保持**）
 * ----------------------------------------------------------------------------
 *   1. 任何"吸合离合 + 转电机"的组合，都必须走下面的 drive_channel() /
 *      creep_feed()，它们内部保证：无论正常返回还是出错，最后一定是
 *      「电机停 + 全部离合断开」。
 *   2. 不允许在别处直接调 clutch_engage() —— 那会绕过 drive_channel 的收尾。
 *   3. 主循环每个周期都调 clutch_assert_single()，发现多路吸合立即全部断开。
 */

#include "ams_controller.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bambu_mqtt.h"
#include "bambu_proto.h"
#include "clutch.h"
#include "config.h"
#include "filament_sensor.h"
#include "log_buffer.h"
#include "motor.h"

/* ==========================================================================
 * 常量
 * ========================================================================== */
#define AMS_CMD_QUEUE_LEN 6
#define AMS_TASK_STACK    8192
#define AMS_TASK_PRIO     5

/** 给打印机发完一条命令后，等它"有进展"的最长时间 */
#define AMS_PRINTER_WAIT_MS 8000

/** 打印机重复上报换料请求时，最多重试几次（超过就停下来报错，别无限折腾） */
#define AMS_EXCHANGE_MAX_RETRY 5

/* ==========================================================================
 * 内部状态
 * ========================================================================== */

static QueueHandle_t      s_cmd_queue;
static SemaphoreHandle_t  s_report_lock;
static SemaphoreHandle_t  s_state_lock;
static TaskHandle_t       s_task;

static ams_state_t s_state = AMS_STATE_IDLE;
static int         s_active_material = -1;
static ams_diag_t  s_diag;
static uint32_t    s_last_error_us = UINT32_MAX;

/* 最近一次打印机上报（网页用，也用于"等打印机有进展"） */
static bambu_report_t s_last_report;
static bool           s_has_report;
static volatile bool  s_report_pending;

/* 换料请求去重 */
static int  s_exchange_attempts;
static bool s_change_active;
/* 流量校准阶段辅助送料去重：同一轮换料只做一次 */
static bool s_assist_done_for_this_exchange;
/* 上次看到的 stg_cur，用于避免每次上报都打印日志 */
static int  s_last_seen_stg = -1;

/* 状态灯 */
static bool     s_led_on;
static uint32_t s_led_tick;

/* 微动触发去重：记录上一次看到的自吸/开始微动序号 */
static uint32_t s_last_autoload_seq[BOARD_CHANNEL_COUNT];
static uint32_t s_last_start_seq[BOARD_CHANNEL_COUNT];

/* ==========================================================================
 * 一、状态读写
 * ========================================================================== */

static void set_state(ams_state_t st, int material)
{
    if (s_state_lock &&
        xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state = st;
        s_active_material = material;
        xSemaphoreGive(s_state_lock);
    } else {
        s_state = st;
        s_active_material = material;
    }
}

ams_state_t ams_get_state(void)
{
    return s_state;
}

int ams_active_material(void)
{
    return s_active_material;
}

bool ams_is_busy(void)
{
    return s_state != AMS_STATE_IDLE;
}

const char *ams_state_text(void)
{
    switch (s_state) {
    case AMS_STATE_IDLE:     return "空闲";
    case AMS_STATE_JOG:      return "手动点动";
    case AMS_STATE_RETRACT:  return "退料中";
    case AMS_STATE_LOAD:     return "送料中";
    case AMS_STATE_ASSIST:   return "辅助送料中";
    case AMS_STATE_CREEP:    return "蠕动送料中";
    case AMS_STATE_AUTOLOAD: return "自吸上料中";
    case AMS_STATE_EXCHANGE: return "换料中";
    case AMS_STATE_ERROR:    return "出错";
    default:                 return "未知";
    }
}

int ams_current_printer_channel(void)
{
    return config_get_filament_current();
}

int ams_channel_text(int printer_channel, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return 0;
    }
    if (printer_channel <= 0) {
        return snprintf(buf, buflen, "未知");
    }
    int mat = config_material_index_of(printer_channel);
    if (mat < 0) {
        return snprintf(buf, buflen, "通道 %d（未映射）", printer_channel);
    }
    return snprintf(buf, buflen, "通道 %d（料盘位 %d）",
                    printer_channel, mat + 1);
}

static void record_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[80];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    strncpy(s_diag.last_error, msg, sizeof(s_diag.last_error) - 1);
    s_diag.last_error_ms = 0;
    s_last_error_us = esp_timer_get_time();
    ams_log_err("%s", msg);
}

/* ==========================================================================
 * 二、状态灯
 * ========================================================================== */

static void led_init(void)
{
    if (BOARD_PIN_LED < 0) {
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (unsigned)BOARD_PIN_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)BOARD_PIN_LED, !BOARD_LED_ACTIVE_LEVEL);
    s_led_on = false;
}

static void led_set(bool on)
{
    if (BOARD_PIN_LED < 0) {
        return;
    }
    gpio_set_level((gpio_num_t)BOARD_PIN_LED, on ? BOARD_LED_ACTIVE_LEVEL
                                                : !BOARD_LED_ACTIVE_LEVEL);
    s_led_on = on;
}

/**
 * 状态灯语义（和 Python 版一致）：
 *     空闲       慢闪（1Hz）—— 一切正常，等指令
 *     动作中     常亮       —— 正在动，别碰机构
 *     出错       快闪（5Hz）—— 需要人看一眼
 */
static void led_update(void)
{
    if (BOARD_PIN_LED < 0) {
        return;
    }
    s_led_tick++;

    uint32_t period;
    if (s_state == AMS_STATE_ERROR) {
        period = 10;      /* 10 × 20ms = 200ms 一个相位 → 5Hz */
    } else if (s_state != AMS_STATE_IDLE) {
        led_set(true);
        return;
    } else {
        period = 25;      /* 25 × 20ms = 500ms 一个相位 → 1Hz */
    }

    if (s_led_tick % period == 0) {
        led_set(!s_led_on);
    }
}

/* ==========================================================================
 * 三、总线守护：所有复合动作都从这里走
 * ========================================================================== */

/**
 * 吸合通道 → 按方向转 ms 毫秒（可选：中途查停止微动）→ 停 → 断开全部离合。
 *
 * @param material_index  料盘位下标（0 起）
 * @param direction       1 进料 / -1 退料
 * @param max_ms          最长转多久
 * @param stop_on_sensor  true 时每 AMS_FILAMENT_STEP_MS 查一次「停止送料微动」，
 *                        触发就提前结束（有微动的通道才有意义）
 * @param out_triggered   非 NULL 时，返回是否是被微动提前结束的
 * @return true 动作正常完成（不代表"送到了"，只代表没出错）
 */
static bool drive_channel_speed(int material_index, int direction,
                                uint8_t speed_pct, uint32_t max_ms,
                                bool stop_on_sensor, bool *out_triggered);

static bool drive_channel(int material_index, int direction, uint32_t max_ms,
                          bool stop_on_sensor, bool *out_triggered)
{
    return drive_channel_speed(material_index, direction, 100, max_ms,
                               stop_on_sensor, out_triggered);
}

/**
 * 同 drive_channel，但指定电机速度（PWM 占空比百分比）。
 * 用于辅助送料等需要非全速跑的场景。
 */
static bool drive_channel_speed(int material_index, int direction,
                                uint8_t speed_pct, uint32_t max_ms,
                                bool stop_on_sensor, bool *out_triggered)
{
    if (out_triggered) {
        *out_triggered = false;
    }
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        record_error("送料失败：料盘位 %d 不存在", material_index + 1);
        return false;
    }

    esp_err_t err = clutch_engage(material_index + 1);
    if (err != ESP_OK) {
        record_error("料盘位%d 离合吸合失败（%s）", material_index + 1,
                     esp_err_to_name(err));
        clutch_release_all();
        return false;
    }
    clutch_set_busy(true);

    bool triggered = false;
    bool use_sensor = stop_on_sensor && config_sensor_enabled(material_index) &&
                      sensor_pin_present(material_index, SENSOR_STOP);

    if (use_sensor) {
        uint32_t elapsed = 0;
        while (elapsed < max_ms) {
            uint32_t run = AMS_FILAMENT_STEP_MS;
            if (elapsed + run > max_ms) {
                run = max_ms - elapsed;
            }
            motor_run((motor_dir_t)direction, run);
            elapsed += run;

            if (sensor_triggered(material_index, SENSOR_STOP)) {
                triggered = true;
                break;
            }
        }
        motor_stop();
    } else {
        /* ★ 关键：direction 是 int（±1），必须 cast 成 motor_dir_t。
         * 不 cast 的话 C 编译器可能把 -1 当 0（STOP）处理，
         * 退料方向就会丢，电机不转（之前 bug 的根因） */
        motor_run_speed((motor_dir_t)direction, max_ms, speed_pct);
        motor_stop();
    }

    if (out_triggered) {
        *out_triggered = triggered;
    }

    /* ★ 收尾：无论如何都要回到安全状态。
     *   这里不用 try/finally（C 没法那么写），但保证只有这一条返回路径 ——
     *   上面所有分支最后都落到这里。 */
    clutch_release_all_settled();
    return true;
}

/**
 * 蠕动间隔：慢速短脉冲之间留 150ms，让挤出机齿轮转到位再拱下一下。
 * 没有间隔就变成连续慢速送料，齿轮还没转到位料又顶上来，容易打折。
 */
#define CREEP_GAP_MS 150

/* ==========================================================================
 * 四、基本动作：送料 / 退料
 * ========================================================================== */

/** 是否有任何一路装了微动 */
static bool has_any_sensor(void)
{
    return sensor_enabled_mask() != 0;
}

/**
 * 送料：把指定料盘位的料往挤出机方向推。
 *
 * 有微动：分步推进，每步之后查「停止送料微动」，触发即停。
 * 无微动：按时间推进，总时长被 AMS_NO_LIMIT_LOAD_MS 封顶（**必须实测调整**）。
 *
 * @param wait_extruder 是否在送完之后等挤出机到位信号（自吸流程要，普通送料不要）
 */
static bool feed_until_extruder(int mat_new);  /* 前向声明（do_load 早于定义用到） */
static bool do_load(int material_index, bool wait_extruder)
{
    set_state(AMS_STATE_LOAD, material_index);
    ams_log("开始送料：料盘位%d（%s）", material_index + 1,
            config_sensor_enabled(material_index) ? "按微动反馈" : "按时间推进");

    uint32_t max_ms;
    if (config_sensor_enabled(material_index)) {
        max_ms = (uint32_t)AMS_LOAD_RETRY_TIMES * AMS_FILAMENT_STEP_MS;
        if (max_ms > AMS_NO_LIMIT_LOAD_MS) {
            max_ms = AMS_NO_LIMIT_LOAD_MS;
        }
    } else {
        max_ms = AMS_NO_LIMIT_LOAD_MS;
    }

    bool triggered = false;
    if (!drive_channel(material_index, 1, max_ms, true, &triggered)) {
        return false;
    }
    s_diag.load_ok++;
    if (triggered) {
        ams_log("停止送料微动触发，送料结束");
    } else {
        ams_log_warn("送料 %ums 结束，但停止送料微动没触发 —— "
                     "可能是料没到位、或者 AMS_NO_LIMIT_LOAD_MS 设小了",
                     (unsigned)max_ms);
    }

    /* ---- 等挤出机到位 → 蠕动收尾（自吸流程要，普通送料不要）---- */
    if (wait_extruder) {
        if (!feed_until_extruder(material_index)) {
            ams_log_warn("等 %ums 没等到挤出机到位，跳过蠕动收尾。"
                         "若机器没有这根线，请把挤出机信号来源改成 MQTT",
                         (unsigned)AMS_EXTRUDER_WAIT_MS);
        }
    }
    return true;
}

/** 蠕动退料一次（短脉冲反向推一小段，需要吸合离合） */
static void creep_retract_once(int material_index, uint16_t pulse_ms,
                                uint8_t speed_pct)
{
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        return;
    }
    esp_err_t err = clutch_engage(material_index + 1);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(CLUTCH_ENGAGE_MS));
        motor_run_speed((motor_dir_t)-1, pulse_ms, speed_pct);
        clutch_release(material_index + 1);
        vTaskDelay(pdMS_TO_TICKS(CLUTCH_RELEASE_MS));
    }
}

/**
 * 退料：把料从挤出机/缓冲区收回到料盘。
 *
 * ★ 重写版（C3 无微动降级模式）：
 *   1. 蠕动退料 N 次（短脉冲反向推）
 *   2. 等待挤出机 MQTT 信号：hw_switch_state 变为 0（"没料了"）
 *   3. 收到"没料"信号 → 连续退料 retract_cont_ms（把余料收干净）
 *   4. 超时没收到 → 直接连续退料 retract_cont_ms 兜底
 *
 * 有微动时仍走原有的"分步推、微动触发即停"路径。
 */
static bool do_retract(int material_index)
{
    set_state(AMS_STATE_RETRACT, material_index);
    ams_log("开始退料：料盘位%d", material_index + 1);

    /* ---- 路径 A：有微动 → 分步推，微动触发即停 ---- */
    if (config_sensor_enabled(material_index) &&
        sensor_pin_present(material_index, SENSOR_STOP)) {
        uint32_t max_ms = (uint32_t)AMS_RETRACT_STEPS * AMS_FILAMENT_STEP_MS;
        bool triggered = false;
        bool ok = drive_channel(material_index, -1, max_ms, true, &triggered);
        if (!ok) {
            return false;
        }
        s_diag.retract_ok++;
        ams_log("退料结束（%s）", triggered ? "微动触发" : "到达时长上限");
        return true;
    }

    /* ---- 路径 B：无微动（C3 默认）→ 蠕动 + 等 MQTT + 连续退料 ---- */
    uint8_t creep_n   = config_get()->creep_times;
    uint16_t creep_ms = config_get()->creep_pulse_ms;
    uint8_t creep_pct = config_get()->creep_speed_pct;

    /* ① 蠕动退料 N 次 */
    ams_log("蠕动退料 %u 次 × %ums @%u%%", (unsigned)creep_n,
            (unsigned)creep_ms, (unsigned)creep_pct);
    for (uint32_t i = 0; i < creep_n; i++) {
        creep_retract_once(material_index, creep_ms, creep_pct);
    }

    /* ② 等挤出机 MQTT 信号："没料"（hw_switch_state == 0） */
    uint32_t wait_ms = config_get_retract_wait_ms();
    ams_log("等待挤出机 MQTT 信号（最多 %ums）…", (unsigned)wait_ms);
    bool got_signal = false;

    if (config_get()->extruder_src == EXTRUDER_SRC_MQTT) {
        uint32_t base_seq = extruder_inplace_seq();
        int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
        while (esp_timer_get_time() < deadline) {
            /* 安全读取最新报告的 hw_switch_state */
            bool no_filament = false;
            if (xSemaphoreTake(s_report_lock, 0) == pdTRUE) {
                if (s_last_report.extruder_inplace_hint == 0) {
                    no_filament = true;
                }
                xSemaphoreGive(s_report_lock);
            }
            if (no_filament) {
                got_signal = true;
                ams_log("收到挤出机「无料」MQTT 信号（hw_switch_state=0）");
                break;
            }
            /* 兜底：如果 MQTT 通知已把 extruder_inplace_seq 推上去了
             * （旧通知残留），也算收到信号 */
            if (extruder_inplace_seq() != base_seq) {
                got_signal = true;
                ams_log("检测到挤出机到位边沿变化（兜底）");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    /* ③ 连续退料：把余料收干净 */
    uint16_t cont_ms = config_get_retract_cont_ms();
    if (got_signal) {
        ams_log("连续退料 %ums（MQTT 确认无料）", (unsigned)cont_ms);
    } else {
        ams_log("等 MQTT 超时，直接连续退料 %ums（兜底）", (unsigned)cont_ms);
    }
    bool ok = drive_channel(material_index, -1, cont_ms, false, NULL);
    if (!ok) {
        return false;
    }
    s_diag.retract_ok++;
    ams_log("退料结束（%s）", got_signal ? "MQTT 确认" : "超时兜底");
    return true;
}

/* ==========================================================================
 * 五、★ 自吸上料（你这次要求的核心流程）
 * ========================================================================== */

/**
 * 自吸微动触发 → 全自动上料。
 *
 * 完整流程：
 *
 *     ① 清掉挤出机到位状态
 *        ★ 这一步不能省。上一轮动作结束时如果信号还是"到位"，不清的话
 *          第 ③ 步会瞬间通过，蠕动收尾就白做了。
 *
 *     ② 自动送料
 *        直到「停止送料微动」触发（或到达时长上限）
 *
 *     ③ 等挤出机到位信号
 *        GPIO 模式等那根线；MQTT 模式等打印机上报
 *
 *     ④ 蠕动送料 3 次（次数/时长/速度都在 config 里，网页可改）
 *
 *     ⑤ 收尾：停电机、断开离合、记录当前料盘
 *
 * 全过程不需要人再按任何按钮 —— 把料插进料盘位，碰到自吸微动就行。
 */
static bool do_autoload(int material_index)
{
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        record_error("自吸失败：料盘位 %d 不存在", material_index + 1);
        return false;
    }

    set_state(AMS_STATE_AUTOLOAD, material_index);
    ams_log("===== 自吸上料开始：料盘位%d =====", material_index + 1);

    bool ok = do_load(material_index, true);

    if (!ok) {
        s_diag.autoload_fail++;
        record_error("自吸上料失败：料盘位%d", material_index + 1);
        set_state(AMS_STATE_ERROR, -1);
        return false;
    }

    /* 记录成"当前料盘"：自吸完成意味着这一路已经上料，可以把打印机通道
     * 也指过来（映射关系由 access_list 决定） */
    int printer_ch = config_printer_channel_of(material_index);
    if (printer_ch > 0) {
        config_set_filament_current(printer_ch);
    }

    s_diag.autoload_ok++;
    ams_log("===== 自吸上料完成：料盘位%d =====", material_index + 1);
    set_state(AMS_STATE_IDLE, -1);
    return true;
}

/* ==========================================================================
 * 六、探测当前料盘
 * ========================================================================== */

/**
 * 探测当前正在使用的是哪一路。
 *
 * 原理（和上游一致）：当前有料的通道反向转动时，料线会绷紧并把送料臂推到
 * 限位，微动触发；空料通道反转时轮子空转，微动不动。
 *
 * ⚠️ 本版本用「停止送料微动」当反馈，属于**近似**：如果机械结构上反转
 *    根本就压不到那只微动，探测就会全部走完一遍返回"未知"。
 *    那种情况下程序会退回使用 NVS 里记录的 filament_current ——
 *    也就是"未安装微动"时的行为，不会出错，只是不能自动识别。
 *
 * 探测到之后会反向转同样的时长把料退回去，避免长期绷紧。
 */
static int probe_current_filament(int fallback)
{
    if (!has_any_sensor()) {
        ams_log("未启用微动，跳过料盘探测，沿用记录值: %d", fallback);
        return fallback;
    }

    /* 探测顺序：先试上次用的通道，再试其余通道 */
    int order[BOARD_CHANNEL_COUNT];
    int n = 0;
    int fallback_mat = config_material_index_of(fallback);
    if (fallback_mat >= 0) {
        order[n++] = fallback_mat;
    }
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        if (i != fallback_mat) {
            order[n++] = i;
        }
    }

    const uint32_t step_ms = 500;
    const uint32_t max_steps = AMS_NO_LIMIT_PROBE_MS / step_ms;

    for (int idx = 0; idx < n; idx++) {
        int mat = order[idx];
        if (!config_sensor_enabled(mat)) {
            continue;
        }

        ams_log("探测料盘位%d 是否有料…", mat + 1);
        set_state(AMS_STATE_RETRACT, mat);

        if (clutch_engage(mat + 1) != ESP_OK) {
            clutch_release_all();
            continue;
        }
        clutch_set_busy(true);

        bool found = false;
        uint32_t steps = 0;
        for (steps = 0; steps < max_steps; steps++) {
            motor_run(MOTOR_DIR_RETRACT, step_ms);
            if (sensor_triggered(mat, SENSOR_STOP)) {
                found = true;
                break;
            }
        }
        motor_stop();

        if (found) {
            /* 反向回位：把刚才拉紧的那一段料推回去 */
            motor_run(MOTOR_DIR_FEED, (steps + 1) * step_ms);
            motor_stop();
            clutch_release_all_settled();
            int ch = config_printer_channel_of(mat);
            ams_log("探测到当前料盘: %d（料盘位%d）", ch, mat + 1);
            return ch;
        }
        clutch_release_all_settled();
    }

    ams_log("探测未命中，沿用记录值: %d", fallback);
    return fallback;
}

/* ==========================================================================
 * 七、换料主流程
 * ========================================================================== */

/**
 * 等打印机"有进展"：stg_cur 或 gcode_state 变了就算。
 *
 * ★ 为什么不直接等某条特定应答：esp-mqtt 是异步回调模型，没有"等一条指定
 *   消息"的原生接口；而且拓竹不同固件版本的应答格式并不一致。
 *   Python 版的做法是 wait_msg_timeout(8000) —— "8 秒内收到任何消息就继续"，
 *   本质上也只是一个超时等待。这里做得稍好一点：等到打印机的阶段真的变了
 *   就立刻继续，最多等 AMS_PRINTER_WAIT_MS。
 */
static void wait_printer_progress(int prev_stg, int prev_gcode_state)
{
    int64_t deadline = esp_timer_get_time() +
                       (int64_t)AMS_PRINTER_WAIT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_has_report &&
            (s_last_report.stg_cur != prev_stg ||
             s_last_report.gcode_state != prev_gcode_state)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ams_log_warn("等打印机回应超时（%ums），继续下一步",
                 (unsigned)AMS_PRINTER_WAIT_MS);
}

/**
 * 换料主流程里的蠕动收尾：
 *   ① 等挤出机到位信号（硬件 GPIO 或 MQTT 事件）
 *   ② 到位后等 CREEP_DELAY_MS（1 秒，让打印机把料头咬稳、齿轮复位）
 *   ③ 蠕动 CREEP_EXCHANGE_TIMES（5）次
 *
 * 返回 true = 到位且蠕动完成；false = 等不到位（料没咬住，打印机会报缺料）
 */
#define CREEP_DELAY_MS       1000
#define CREEP_EXCHANGE_TIMES 5

static bool feed_until_extruder(int mat_new)
{
    /* ① 等挤出机到位 */
    extruder_inplace_clear();
    ams_log("等待挤出机到位信号（最多 %ums）…", (unsigned)AMS_EXTRUDER_WAIT_MS);
    bool inplace = extruder_inplace_wait(AMS_EXTRUDER_WAIT_MS);
    if (!inplace) {
        ams_log_warn("等 %ums 没等到挤出机到位，料可能没咬住",
                     (unsigned)AMS_EXTRUDER_WAIT_MS);
        return false;
    }
    ams_log("挤出机到位");

    /* ② 到位后等 1 秒，让打印机齿轮把料头咬稳 */
    ams_log("间隔 %ums 后开始蠕动收尾（%u 次）",
            (unsigned)CREEP_DELAY_MS, (unsigned)CREEP_EXCHANGE_TIMES);
    vTaskDelay(pdMS_TO_TICKS(CREEP_DELAY_MS));

    /* ③ 蠕动 5 次 */
    set_state(AMS_STATE_CREEP, mat_new);
    esp_err_t err = clutch_engage(mat_new + 1);
    if (err != ESP_OK) {
        record_error("换料蠕动：料盘位%d 离合吸合失败", mat_new + 1);
        clutch_release_all();
        return false;
    }
    clutch_set_busy(true);
    ams_log("开始换料蠕动：5 次 × %u%% 速度（用默认脉冲时长）",
            (unsigned)config_get()->creep_speed_pct);
    for (int i = 0; i < CREEP_EXCHANGE_TIMES; i++) {
        motor_run_speed(MOTOR_DIR_FEED, config_get()->creep_pulse_ms,
                        config_get()->creep_speed_pct);
        motor_stop();
        if (i + 1 < CREEP_EXCHANGE_TIMES) {
            vTaskDelay(pdMS_TO_TICKS(CREEP_GAP_MS));
        }
    }
    clutch_release_all_settled();
    ams_log("换料蠕动收尾完成");
    return true;
}

/**
 * 换料主流程（重写版）。
 *
 * 时序：
 *   1. 探测当前料盘（有微动时）
 *   2. 退料：先把当前通道的料从挤出机/缓冲区收回到料盘
 *   3. 进料：把新通道的料送到挤出机入口，等到位后蠕动收尾
 *   4. 辅助送料：帮打印机把新料咬住
 *   5. 发 resume 让打印机继续 G-code（冲刷、流量校准等由打印机完成）
 *
 * ★ 关键改动（修复暂停后卡死问题）：
 *   - 原来在暂停状态下发 M400/M83 是无效的（暂停中 G-code 排队不执行），
 *     导致 wait_printer_progress 超时，整个流程卡住 ~50s 不发 resume。
 *   - 现在改为：退料/进料全程不做打印机同步等待，只做 AMS 侧动作。
 *     完成后发 resume，打印机自己继续执行 M73 P101 之后的冲刷 + 流量校准。
 *   - 流量校准阶段（stg=19）如果 AMS 被再次唤醒，同步做一次辅助送料。
 */
static bool do_exchange(int printer_channel)
{
    int mat_new = config_material_index_of(printer_channel);
    if (mat_new < 0) {
        record_error("换料失败：打印机通道 %d 没有对应的料盘位", printer_channel);
        return false;
    }

    set_state(AMS_STATE_EXCHANGE, mat_new);

    int current = config_get_filament_current();
    current = probe_current_filament(current);

    char cur_txt[48] = {0};
    char new_txt[48] = {0};
    ams_channel_text(current, cur_txt, sizeof(cur_txt));
    ams_channel_text(printer_channel, new_txt, sizeof(new_txt));
    ams_log("换料请求：当前 %s → 目标 %s", cur_txt, new_txt);

    if (current == printer_channel) {
        ams_log("当前已经在该通道，无需换料，直接 resume");
        set_state(AMS_STATE_IDLE, -1);
        /* 打印机在等 AMS 完成，即使不换也要 resume */
        bambu_mqtt_send_resume();
        return true;
    }

    /* ---------- 步骤一：退料 ----------
     * 把当前通道的料从挤出机/缓冲区收回到料盘。
     * 退料由 AMS 电机完成，不需要打印机侧配合（打印机暂停中
     * G1 E-50 不会执行，发 M400 也没用）。 */
    if (current > 0) {
        int mat_cur = config_material_index_of(current);
        if (mat_cur >= 0) {
            ams_log("退料：料盘位%d（通道%d）", mat_cur + 1, current);
            if (!do_retract(mat_cur)) {
                record_error("退料失败，换料中止");
                s_diag.exchange_fail++;
                set_state(AMS_STATE_ERROR, -1);
                return false;
            }
        }
    }

    /* ---------- 步骤二：进料 + 蠕动 ----------
     * 把新通道的料送到挤出机入口，等到位后蠕动 5 次确保咬住。
     * 挤出机到位信号：C3 走 MQTT hw_switch_state，S3 走 GPIO。 */
    ams_log("进料：料盘位%d（通道%d）", mat_new + 1, printer_channel);

    /* C3 无 GPIO 挤出机到位线，直接进料 */
    if (!do_load(mat_new, false)) {
        record_error("进料失败，换料中止");
        s_diag.exchange_fail++;
        set_state(AMS_STATE_ERROR, -1);
        return false;
    }

    /* 蠕动收尾（等到位 → 蠕动 5 次），即使等不到也强制执行 */
    if (!feed_until_extruder(mat_new)) {
        ams_log_warn("换料后未等到挤出机到位，料可能没咬住，仍然尝试 resume");
    }

    /* ---------- 步骤三：辅助送料（可配置 PWM + 开关） ----------
     * 帮打印机把新料咬住（AMS 侧多推一小段）。
     * 这一步在 AMS 侧完成，不需要打印机配合。
     * PWM 占空比和开关均可在网页「硬件调试」面板配置。 */
    if (config_get_assist_enabled()) {
        uint8_t assist_pct = config_get_assist_speed_pct();
        ams_log("辅助送料：料盘位%d @%u%%", mat_new + 1, (unsigned)assist_pct);
        drive_channel_speed(mat_new, 1, assist_pct, AMS_LOAD_ASSIST_MS, false, NULL);
    } else {
        ams_log("辅助送料已关闭，跳过");
    }

    /* ---------- 步骤四：记录状态 + resume ---------- */
    config_set_filament_current(printer_channel);
    s_diag.exchange_ok++;
    ams_log("换料完成：%s，发送 resume 让打印机继续", new_txt);
    set_state(AMS_STATE_IDLE, -1);

    /* resume 让打印机的 G-code 从 M73 P101 之后继续执行：
     * 冲刷（M73 P102/P103）→ 流量校准（M620/M621）→ 恢复打印 */
    bambu_mqtt_send_resume();
    return true;
}

/* ==========================================================================
 * 八、打印机上报处理
 * ========================================================================== */

/**
 * MQTT 回调（跑在 esp-mqtt 的任务里）。
 *
 * ★ 这里只做"搬数据"，绝不做耗时操作 —— 具体处理交给 ams_task。
 *   用互斥锁而不是队列，因为上报是**累积状态**：连来 3 条的话，
 *   最新的那条已经包含前面所有信息，中间的直接覆盖掉即可，不需要排队。
 */
static void on_mqtt_report(const bambu_report_t *report, void *user)
{
    (void)user;
    if (report == NULL) {
        return;
    }
    if (s_report_lock &&
        xSemaphoreTake(s_report_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_last_report = *report;
        s_has_report = true;
        s_report_pending = true;
        xSemaphoreGive(s_report_lock);
    }
}

/**
 * 自动颜色匹配辅助函数。
 * 输入目标颜色（0xRRGGBB），输出最接近的打印机通道号（1 起）。
 * 如果 4 个通道都没配颜色（全 0），返回 -1。
 */
static int auto_match_channel(int target_color)
{
    if (target_color < 0) {
        return -1;
    }
    int best_mat = -1;
    uint32_t dist = config_color_match((uint32_t)target_color, &best_mat);
    if (best_mat < 0) {
        ams_log_warn("自动匹配失败：所有通道都未配置颜色，退回按通道号换料");
        return -1;
    }
    int printer_ch = config_printer_channel_of(best_mat);
    if (printer_ch < 1 || printer_ch > BOARD_CHANNEL_COUNT) {
        ams_log_warn("自动匹配到料盘位 %d，但未映射到打印机通道", best_mat + 1);
        return -1;
    }
    ams_log("自动匹配：目标 #%.6X → 料盘位 %d（通道 %d），距离 %lu",
            (unsigned int)(target_color & 0xFFFFFF),
            best_mat + 1, printer_ch, (unsigned long)dist);
    return printer_ch;
}

/**
 * 处理一条打印机上报（在 ams_task 里调用，可以阻塞，几秒无所谓）。
 *
 * 三件事：
 *   1. 如果报文带目标颜色，用自动匹配覆盖默认的通道号
 *   2. 如果打印机请求换料，把换料命令排进队列（带去重，一秒只排一次）
 *   3. 如果打印机进入流量校准阶段（stg=8 或 19）且处于打印中，
 *      同步做一次辅助送料，帮打印机把新料咬住
 */
static void handle_report(const bambu_report_t *r)
{
    /* ★ 调试：每次收到上报都打印 stg_cur / is_printing / change_needed，
     * 方便确认实际固件在流量校准阶段上报的 stg_cur 值是多少。
     * 稳定后可删除或降为 warn 级别。 */
    if (r->stg_cur != s_last_seen_stg || r->change_needed || r->is_printing) {
        ams_log("上报 stg_cur=%d is_printing=%d change_needed=%d ams_stage=%d",
                r->stg_cur, (int)r->is_printing, (int)r->change_needed,
                r->ams_stage);
        s_last_seen_stg = r->stg_cur;
    }

    /* ---- 挤出机到位（MQTT 来源）---- */
    if (config_get()->extruder_src == EXTRUDER_SRC_MQTT &&
        r->extruder_inplace_hint == 1 &&
        !extruder_inplace_triggered()) {
        ams_log("打印机上报耗材已到挤出机");
        extruder_inplace_notify_from_mqtt();
    }
    /* hw_switch_state == 0 表示"无料"—— 不需要同步更新 extruder_inplace
     * 状态（那只在 do_retract 里通过 s_last_report 直接读取） */

    /* ---- 流量校准阶段：同步辅助送料 ----
     * 放宽条件：stg_cur >= 8 即触发（覆盖不同固件的值），
     * 不强制要求 is_printing（刚 resume 时可能还没变 RUNNING）。
     * 用 s_assist_done_for_this_exchange 去重，同一轮换料只做一次。 */
    if (r->stg_cur >= 8) {
        int cur_ch = config_get_filament_current();
        ams_log("stg_cur=%d is_printing=%d cur_ch=%d assist_enabled=%d assist_done=%d",
                r->stg_cur, (int)r->is_printing, cur_ch,
                (int)config_get_assist_enabled(), (int)s_assist_done_for_this_exchange);
        if (cur_ch > 0) {
            int mat = config_material_index_of(cur_ch);
            if (mat >= 0 && !s_assist_done_for_this_exchange &&
                config_get_assist_enabled()) {
                uint8_t assist_pct = config_get_assist_speed_pct();
                ams_log("同步辅助送料：料盘位%d @%u%%", mat + 1, (unsigned)assist_pct);
                drive_channel_speed(mat, 1, assist_pct, AMS_LOAD_ASSIST_MS,
                                     false, NULL);
                s_assist_done_for_this_exchange = true;
            }
        }
    } else {
        /* 不在校准阶段了，重置标志（下一轮换料重新做） */
        s_assist_done_for_this_exchange = false;
    }

    /* ---- 换料请求 ---- */
    if (!r->change_needed) {
        /* 打印机不再请求换料了 → 重置去重计数 */
        s_change_active = false;
        s_exchange_attempts = 0;
        return;
    }

    /* 默认：报文里的通道号（0 起，+1 转成 1 起） */
    int printer_ch = r->filament_next + 1;

    /* ★ 自动颜色匹配：如果报文带目标颜色且本机配置了颜色，优先用匹配结果 */
    if (r->target_color >= 0) {
        int matched = auto_match_channel(r->target_color);
        if (matched > 0) {
            printer_ch = matched;
        }
    }

    if (printer_ch < 1 || printer_ch > BOARD_CHANNEL_COUNT) {
        if (!s_change_active) {
            record_error("打印机请求通道 %d，超出本机支持的 1~%d",
                         printer_ch, BOARD_CHANNEL_COUNT);
            s_change_active = true;
        }
        return;
    }

    /* 已经在处理这个请求了（打印机每秒都会重复上报）→ 不重复排队 */
    if (s_change_active) {
        return;
    }
    s_change_active = true;

    ams_log("打印机请求换料到通道 %d", printer_ch);

    ams_cmd_t cmd = { .id = AMS_CMD_EXCHANGE, .channel = printer_ch };
    if (!ams_post_cmd(&cmd)) {
        ams_log_err("换料命令排队失败（设备正忙），等打印机的下一次请求");
        s_change_active = false;
    }
}

/* ==========================================================================
 * 九、微动触发的自动动作
 * ========================================================================== */

/**
 * 检查「开始送料微动」和「自吸微动」。
 *
 * 用"序号变了"判断有没有新的触发，而不是看电平 —— 电平是持续状态，
 * 用它在每次循环里都会判定成"触发了"，会无限重复动作。
 */
static void poll_trigger_switches(void)
{
    if (ams_is_busy()) {
        /* 有动作在跑时不响应新触发。但仍然要同步序号，
         * 否则动作结束后会"补做"一次刚刚错过的触发。 */
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            s_last_autoload_seq[i] = sensor_trigger_seq(i, SENSOR_AUTOLOAD);
            s_last_start_seq[i] = sensor_trigger_seq(i, SENSOR_START);
        }
        return;
    }

    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        uint32_t al = sensor_trigger_seq(i, SENSOR_AUTOLOAD);
        uint32_t st = sensor_trigger_seq(i, SENSOR_START);

        bool autoload_new = (al != s_last_autoload_seq[i]);
        bool start_new = (st != s_last_start_seq[i]);
        s_last_autoload_seq[i] = al;
        s_last_start_seq[i] = st;

        if (!config_sensor_enabled(i)) {
            continue;
        }

        if (autoload_new) {
            ams_log("料盘位%d 自吸微动触发 → 开始全自动上料", i + 1);
            ams_cmd_t cmd = { .id = AMS_CMD_AUTOLOAD, .channel = i };
            ams_post_cmd(&cmd);
            return;   /* 一次只处理一个，避免同时排多个动作 */
        }
        if (start_new) {
            ams_log("料盘位%d 开始送料微动触发 → 开始送料", i + 1);
            ams_cmd_t cmd = { .id = AMS_CMD_LOAD, .channel = i };
            ams_post_cmd(&cmd);
            return;
        }
    }
}

/* ==========================================================================
 * 十、命令执行
 * ========================================================================== */

static void do_jog(int material_index, int direction, int ms)
{
    if (ms <= 0) {
        ms = config_get_jog_ms();
    }
    int dir = (direction < 0) ? -1 : 1;
    set_state(AMS_STATE_JOG, material_index);
    ams_log("手动点动：料盘位%d %s %dms", material_index + 1,
            dir > 0 ? "进料" : "退料", ms);

    /* 点动不做微动提前停止 —— 用户在手动调试，就是要它转满设定的时长 */
    drive_channel(material_index, dir, (uint32_t)ms, false, NULL);
    s_diag.jog_count++;
    set_state(AMS_STATE_IDLE, -1);
}

static void execute_cmd(const ams_cmd_t *cmd)
{
    switch (cmd->id) {
    case AMS_CMD_JOG:
        do_jog(cmd->channel, cmd->arg, cmd->ms);
        break;

    case AMS_CMD_LOAD:
        if (do_load(cmd->channel, false)) {
            set_state(AMS_STATE_IDLE, -1);
        } else {
            set_state(AMS_STATE_ERROR, -1);
        }
        break;

    case AMS_CMD_RETRACT:
        if (do_retract(cmd->channel)) {
            set_state(AMS_STATE_IDLE, -1);
        } else {
            set_state(AMS_STATE_ERROR, -1);
        }
        break;

    case AMS_CMD_AUTOLOAD:
        do_autoload(cmd->channel);
        break;

    case AMS_CMD_EXCHANGE: {
        if (do_exchange(cmd->channel)) {
            s_exchange_attempts = 0;
            s_change_active = false;
            /* do_exchange 内部已发 resume，这里不重复发 */
        } else {
            s_exchange_attempts++;
            if (s_exchange_attempts >= AMS_EXCHANGE_MAX_RETRY) {
                ams_log_err("换料连续失败 %d 次，AMS 已停止重试，"
                            "请检查机构后手动处理",
                            s_exchange_attempts);
                set_state(AMS_STATE_ERROR, -1);
                s_change_active = true;   /* 不再重试 */
                /* 即使失败也发 resume，别让打印机一直卡在暂停 */
                bambu_mqtt_send_resume();
            } else {
                ams_log_warn("换料失败（第 %d 次），等打印机的下一次请求再试",
                             s_exchange_attempts);
                s_change_active = false;
            }
        }
        break;
    }

    case AMS_CMD_STOP:
        ams_log("收到停止指令");
        clutch_release_all();
        set_state(AMS_STATE_IDLE, -1);
        break;

    default:
        break;
    }
}

/* ==========================================================================
 * 十一、对外接口
 * ========================================================================== */

bool ams_post_cmd(const ams_cmd_t *cmd)
{
    if (!cmd || s_cmd_queue == NULL) {
        return false;
    }
    /* 不等待：队列满说明设备正忙，直接拒绝比让网页请求挂在那里好 */
    return xQueueSend(s_cmd_queue, cmd, 0) == pdTRUE;
}

bool ams_post_jog(int material_index, int direction, int ms)
{
    ams_cmd_t cmd = { .id = AMS_CMD_JOG, .channel = material_index,
                      .arg = direction, .ms = ms };
    return ams_post_cmd(&cmd);
}

bool ams_post_stop(void)
{
    ams_cmd_t cmd = { .id = AMS_CMD_STOP };
    /* 停止指令优先级最高：先把队列里排着的命令清掉，再塞进去。
     * 不能用 xQueueReset() —— 它要求调用方必须是队列的 owner 任务，
     * 这里是从 httpd 任务调过来的，跨任务 reset 是 FreeRTOS 明确
     * 禁止的行为（会破坏队列内部结构）。安全的清法是循环收空。 */
    if (s_cmd_queue) {
        ams_cmd_t drain;
        while (xQueueReceive(s_cmd_queue, &drain, 0) == pdTRUE) {
            /* 丢弃排队中的命令 */
        }
    }
    return ams_post_cmd(&cmd);
}

bool ams_post_load(int material_index)
{
    ams_cmd_t cmd = { .id = AMS_CMD_LOAD, .channel = material_index };
    return ams_post_cmd(&cmd);
}

bool ams_post_retract(int material_index)
{
    ams_cmd_t cmd = { .id = AMS_CMD_RETRACT, .channel = material_index };
    return ams_post_cmd(&cmd);
}

bool ams_post_autoload(int material_index)
{
    ams_cmd_t cmd = { .id = AMS_CMD_AUTOLOAD, .channel = material_index };
    return ams_post_cmd(&cmd);
}

bool ams_post_exchange(int printer_channel)
{
    ams_cmd_t cmd = { .id = AMS_CMD_EXCHANGE, .channel = printer_channel };
    return ams_post_cmd(&cmd);
}

void ams_get_diag(ams_diag_t *out)
{
    if (!out) {
        return;
    }
    *out = s_diag;
    if (s_last_error_us != UINT32_MAX) {
        int64_t delta = esp_timer_get_time() - (int64_t)s_last_error_us;
        out->last_error_ms = delta > 0 ? (uint32_t)(delta / 1000) : 0;
    } else {
        out->last_error_ms = UINT32_MAX;
    }
}

void ams_reset_diag(void)
{
    memset(&s_diag, 0, sizeof(s_diag));
    s_last_error_us = UINT32_MAX;
    clutch_reset_conflict_count();
    ams_log("诊断统计已清零");
}

int ams_describe_hardware_json(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return 0;
    }

    /* 先让 clutch 写主体，再补微动和业务状态 */
    int off = clutch_describe_json(buf, buflen, (int)motor_get_dir(),
                                   ams_is_busy(), clutch_get_owner());
    if (off <= 0 || (size_t)off >= buflen - 8) {
        return off;
    }

    /* 去掉结尾的 '}'，追加字段后再补回来 */
    if (buf[off - 1] == '}') {
        off--;
    }

    off += snprintf(buf + off, buflen - off, ",\"limits\":[");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        off += snprintf(buf + off, buflen - off, "%s%d", i ? "," : "",
                        config_sensor_enabled(i) ? 1 : 0);
    }
    off += snprintf(buf + off, buflen - off,
                    "],\"state\":%d,\"state_text\":\"%s\","
                    "\"active_material\":%d,\"motor_speed\":%d}",
                    (int)s_state, ams_state_text(), s_active_material,
                    motor_get_speed_pct());
    return off;
}

/* ==========================================================================
 * 十二、任务
 * ========================================================================== */

static void ams_task(void *arg)
{
    (void)arg;
    ams_cmd_t cmd;
    int boot_count = 0;

    ams_log("AMS 主任务已启动");

    while (1) {
        /* 每 100 轮打印一次心跳，用于诊断是否卡住 */
        if (++boot_count % 100 == 0) {
            ams_log("ams_task 心跳 #%d，状态=%d", boot_count, s_state);
        }

        /* ---- ① 离合体检 ---- */
        clutch_assert_single(true);

        /* ---- ② 处理打印机上报 ---- */
        if (s_report_pending) {
            bambu_report_t copy;
            bool got = false;
            if (xSemaphoreTake(s_report_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                copy = s_last_report;
                s_report_pending = false;
                got = true;
                xSemaphoreGive(s_report_lock);
            }
            if (got) {
                ams_log("handle_report 进入 ams_stage=%d", copy.ams_stage);
                handle_report(&copy);
                ams_log("handle_report 离开");
            }
        }

        /* ---- ③ 执行命令（会阻塞，没问题 —— 只有这个任务被占住）---- */
        if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            ams_log("execute_cmd 开始");
            execute_cmd(&cmd);
            ams_log("execute_cmd 结束");
            continue;
        }

        /* ---- ④ 微动触发的自动动作 ---- */
        poll_trigger_switches();

        /* ---- ⑤ 状态灯 ---- */
        led_update();

        /* ---- ⑥ 空闲时把时钟让出去 ---- */
        vTaskDelay(pdMS_TO_TICKS(AMS_TASK_POLL_MS));
    }
}

esp_err_t ams_init(void)
{
    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(AMS_CMD_QUEUE_LEN, sizeof(ams_cmd_t));
        if (!s_cmd_queue) {
            ams_log_err("创建命令队列失败（内存不足？）");
            return ESP_FAIL;
        }
    }
    if (s_report_lock == NULL) {
        s_report_lock = xSemaphoreCreateMutex();
    }
    if (s_state_lock == NULL) {
        s_state_lock = xSemaphoreCreateMutex();
    }

    memset(&s_diag, 0, sizeof(s_diag));
    memset(&s_last_report, 0, sizeof(s_last_report));
    s_last_error_us = UINT32_MAX;
    s_has_report = false;
    s_report_pending = false;
    s_exchange_attempts = 0;
    s_change_active = false;
    s_state = AMS_STATE_IDLE;
    s_active_material = -1;

    /* 微动序号先取当前值，避免开机瞬间把"已经触发着"当成一次新触发 */
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        s_last_autoload_seq[i] = sensor_trigger_seq(i, SENSOR_AUTOLOAD);
        s_last_start_seq[i] = sensor_trigger_seq(i, SENSOR_START);
    }

    led_init();

    ams_log("AMS 控制器已初始化（MQTT 将在 WiFi 起来后再连）");
    return ESP_OK;
}

/**
 * 在 WiFi 连接完成后调用，启动 MQTT 客户端。
 * 这样避免 esp_mqtt_client_start() 在 WiFi 未就绪时阻塞。
 */
esp_err_t ams_connect_printer(void)
{
    esp_err_t err = bambu_mqtt_init(on_mqtt_report, NULL);
    if (err == ESP_OK) {
        ams_log("打印机 MQTT 客户端已就绪");
    }
    return err;
}

esp_err_t ams_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(ams_task, "ams_task", AMS_TASK_STACK, NULL,
                                AMS_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ams_log_err("创建 AMS 任务失败（栈 %d 字节不够？）", AMS_TASK_STACK);
        return ESP_FAIL;
    }
    return ESP_OK;
}
