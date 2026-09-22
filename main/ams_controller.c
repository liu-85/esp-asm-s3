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

/** 快速退料指令发出去之后，等打印机做完切刀/吐料的时长 */
#define AMS_PRIME_WAIT_MS 3000

/** 进料前等热端升到目标温度的最长时间 */
#define AMS_HEAT_WAIT_MS 30000

/** 同一个"触发指纹"在这个时间窗内被当成残留上报（见 handle_report）。
 *  取 60 秒：远大于打印机状态上报的延迟（1~3 秒），又远小于切片两次
 *  换色之间的间隔 —— 所以既能挡住重复换料，又不会漏掉连续换色。 */
#define AMS_EXCHANGE_RESIDUAL_MS 60000

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

/* ---- 换料"触发指纹"与已完成时刻 ----
 * 打印机会按秒重复上报同一个状态，换料完成后它还会继续报
 * change_needed=true 好几秒。只靠 s_change_active 去重是不够的：换料一
 * 成功就得把它清零（否则后续换料永远被拒），清零后下一帧立刻又排一次
 * 命令 —— 同一个颜色换两次就是这么来的。
 * 指纹 = 触发源（哪条通路 + 哪个通道），指纹没变且在 AMS_EXCHANGE_RESIDUAL_MS
 * 窗口内，就当它是上次那次触发的残留；指纹变了立刻放行。 */
static int      s_change_fp;
static int64_t  s_change_fp_us;
static int      s_pending_fp;     /* 已排队命令对应的指纹，成功时转正为 s_change_fp */

/* ---- 热床温度记忆（对应 Top-AMS 的 bed_target_temper_max）----
 * 切片用 M140 S{next_extruder + 1} 借"目标床温"传通道号，打印机上报的
 * bed_target 于是变成 1~16。真实床温必须在被改写之前记住，换完再还回去。
 * 取"见过的最高值"：不同层/不同材料的床温不一样（PLA 首层 60、后续 55），
 * 取最高能保证恢复后不会偏低。0 = 还没有可信记录。 */
static float    s_bed_target_max;

/* 辅助送料去重：stg=8 做一次，stg=19 再做一次，stg=0 持续做（间隔 5s） */
static bool s_assist_done_stg8;         /* 本次换料 stg=8 是否已做 */
static bool s_assist_done_stg19;        /* 本次换料 stg=19 是否已做 */
static int64_t s_last_assist_time_us;   /* 上次辅助送料时间（stg=0 持续用） */
#define ASSIST_REPEAT_US (5 * 1000 * 1000)  /* 持续辅助间隔 5 秒 */
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
        .pin_bit_mask = (1ULL << (unsigned)(BOARD_PIN_LED < 64 ? BOARD_PIN_LED : 0)),
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
    ams_log("  进料：开始送料通道 %d（%s）", material_index + 1,
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
    ams_log("  退料：开始退料通道 %d（%u 次蠕动 + 等无料 + 连续退料）",
             material_index + 1, (unsigned)config_get()->creep_times);

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

    /* ---- 路径 B：无微动（C3 默认）→ 蠕动 + 等 MQTT 无料 + 连续退料 ----
     *
     * 配合 G-code 切刀段：打印机先切刀回冲刷区，再 M400 U1 通知 AMS。
     *   ① 蠕动退料 N 次（把切断的料拉松）
     *   ② 每轮等 5 秒 MQTT「无料」信号（hw_switch_state==0）：
     *      - 收到 0 → 连续退料 → 进进料
     *      - 没收到（仍是 1）→ 再蠕动一次，重复 ② 直到收到 0
     */
    uint8_t creep_n   = config_get()->creep_times;
    uint16_t creep_ms = config_get()->creep_pulse_ms;
    uint8_t creep_pct = config_get()->creep_speed_pct;
    uint16_t cont_ms  = config_get_retract_cont_ms();

    /* ① 第一次蠕动退料 */
    ams_log("  退料（蠕动）：第 1 次退料，共 %u 次", (unsigned)creep_n);
    for (uint32_t i = 0; i < creep_n; i++) {
        creep_retract_once(material_index, creep_ms, creep_pct);
    }

    /* ② 循环等 MQTT「无料」信号，每轮 5 秒，没收到就再蠕动一次 */
    uint32_t probe_ms = 5000;
    uint32_t probe_round = 0;
    bool got_signal = false;

    if (config_get()->extruder_src == EXTRUDER_SRC_MQTT) {
        uint32_t base_seq = extruder_inplace_seq();
        const uint32_t MAX_ROUNDS = 6;  /* 最多 6 轮 ≈ 30 s，超时强制兜底 */
        ams_log("  退料（等无料）：等待 MQTT 无料信号（每轮 %us，最多 %u 轮）",
                (unsigned)(probe_ms / 1000), (unsigned)MAX_ROUNDS);
        while (probe_round < MAX_ROUNDS) {
            probe_round++;
            int64_t deadline = esp_timer_get_time() + (int64_t)probe_ms * 1000;
            while (esp_timer_get_time() < deadline) {
                bool no_filament = false;
                if (xSemaphoreTake(s_report_lock, 0) == pdTRUE) {
                    if (s_last_report.extruder_inplace_hint == 0) {
                        no_filament = true;
                    }
                    xSemaphoreGive(s_report_lock);
                }
                if (no_filament) {
                    got_signal = true;
                    ams_log("  退料（等无料）：收到无料信号，料已退出，开始连续退料");
                    break;
                }
                if (extruder_inplace_seq() != base_seq) {
                    got_signal = true;
                    ams_log("  退料（等无料）：边沿检测到信号变化，开始连续退料");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (got_signal) break;
            if (probe_round >= MAX_ROUNDS) {
                ams_log("  退料（等无料）：等待超时（%u 轮），强制进连续退料",
                        (unsigned)probe_round);
                break;
            }
            /* 本轮没等到，再蠕动一次 */
            ams_log("  退料（等无料）：第 %u 轮未收到，再蠕动一次…",
                    (unsigned)probe_round);
            for (uint32_t i = 0; i < creep_n; i++) {
                creep_retract_once(material_index, creep_ms, creep_pct);
            }
        }
    } else {
        /* GPIO 模式：保持原有行为 */
        uint32_t wait_ms = config_get_retract_wait_ms();
        ams_log("  退料（等无料）：GPIO 模式，等待 %u ms", (unsigned)wait_ms);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    /* ③ 连续退料：把余料收干净 */
    ams_log("  退料（连续）：开始连续退料 %u ms…", (unsigned)cont_ms);
    bool ok = drive_channel(material_index, -1, cont_ms, false, NULL);
    if (!ok) {
        ams_log("  退料（连续）：退料失败");
        return false;
    }
    s_diag.retract_ok++;
    ams_log("  退料完成，开始进料");
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

/* --------------------------------------------------------------------------
 * ★ 为什么换料过程中要"使唤"打印机，而不是全靠我们自己的电机
 * --------------------------------------------------------------------------
 * 热端里那一段料是**熔融**的。如果只靠远端电机硬拽，会出现：打滑、拉断、
 * 拉出来的丝粗细不匀、甚至把料头拽变形导致后面进不去。
 *
 * 打印机的工具头齿轮离热端最近、扭矩也足，让它自己先把料吐到缓冲里，
 * 我们再把料收回盘 —— 两段式接力，这是 Top-AMS 实测的稳定做法。
 *
 * ⚠️ 调温必须用 M109 / M190，**不能**用 M104 / M140：
 *    A1 系列 1.04 固件下，经 MQTT 发 M104/M140 是不生效的（打印机把它
 *    当"设了就设了"但不真的执行），只有 M109/M190 会真的等温。
 *    这条是 Top-AMS 在 bambu.hpp 里专门写了注释的坑。
 * -------------------------------------------------------------------------- */

/**
 * 换料前的"快速退料"：请求打印机把热端里的料吐出来。
 *
 * @param mat_cur 当前料盘位下标，用来查它的换料温度；-1 表示未知（用默认值）
 *
 * 时序照 Top-AMS：
 *      M109 S<temp>      热端升到该料盘位的换料温度（太凉退不出来）
 *      M620 S255         进入切割流程
 *      T255              切刀动作
 *      M621 S255         退出切割流程
 *
 * ★ 调用时机：在 handle_report 一拿到通道号就发，**不要**留到 do_exchange。
 *   （旧注释把原因写成"必须在打印机还 RUNNING 时才生效" —— 2026-09-22 真机
 *   实测**否掉了这条**：那时打印机已经是 PAUSE，切刀照样执行、`M190 S76`
 *   也在 2 秒内把 bed_target_temper 从 3 改成 76。真正不能省的只有结尾的
 *   `\n`，那个已由 bambu_cmd_gcode() 统一兜住。）
 *
 *   留在这里发仍然是对的，但理由是**时序**而不是"状态门禁"：切片把 M400 U1
 *   写在 M140 后面十几行，要趁这段窗口把"还床温 + 切刀"交代给打印机；
 *   等到 do_exchange 再发，我们自己的电机已经准备收线了，打印机再切刀
 *   会跟收线抢料。
 *
 * 发完要等 AMS_PRIME_WAIT_MS —— 切刀 + 吐料是机械动作，需要时间。
 * 我们太早介入收线，会把料拽断。这段等待同时也把打印机"送到"了暂停点。
 *
 * MQTT 没连上时不报错、只警告：退料仍然可以只靠本机电机完成（只是
 * 更容易打滑），不该因为发不出这条指令就把整次换料判失败。
 */
static void exchange_prime_extrude(int mat_cur)
{
    int temp = config_get_temper(mat_cur);
    char g[96];
    int n = snprintf(g, sizeof(g),
                     "M109 S%d\n"
                     "M620 S255\n"
                     "T255\n"
                     "M621 S255\n",
                     temp);
    if (n <= 0 || n >= (int)sizeof(g)) {
        return;
    }
    if (bambu_mqtt_send_gcode(g) < 0) {
        ams_log_warn("  退料：快速退料指令没发出去（MQTT 未连接？），"
                     "只能靠自己硬退，注意别拉断");
        return;
    }
    ams_log("  退料：已请求打印机快速退料（热端 %d℃，等 %ums 让切刀跑完）",
            temp, (unsigned)AMS_PRIME_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(AMS_PRIME_WAIT_MS));
}

/**
 * 换料进料前：把热端升到新料盘位的温度，并等到温。
 *
 * 温度没到就送料，料头会在冷热端里顶住、打弯，进不去 —— 表现为"送料
 * 送不动"。Top-AMS 也是发完 M109 然后自旋等到温（它等的是
 * nozzle_target_temper >= temp - 5，这里照做）。
 *
 * 等不到就继续走：卡在这里会让打印机一直停在暂停态，比"温度差几度"
 * 严重得多。差几度顶多多退一次料。
 */
static void exchange_heat_nozzle(int mat_new)
{
    int temp = config_get_temper(mat_new);
    char g[32];
    if (snprintf(g, sizeof(g), "M109 S%d", temp) <= 0) {
        return;
    }
    if (bambu_mqtt_send_gcode(g) < 0) {
        ams_log_warn("  进料：升温指令没发出去（MQTT 未连接？），按原温度继续");
        return;
    }
    ams_log("  进料：已请求热端升到 %d℃，等到温…", temp);

    int64_t deadline = esp_timer_get_time() + (int64_t)AMS_HEAT_WAIT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        float t = -1.0f;
        if (xSemaphoreTake(s_report_lock, 0) == pdTRUE) {
            t = s_last_report.nozzle_target;
            xSemaphoreGive(s_report_lock);
        }
        if (t >= (float)temp - 5.0f) {
            ams_log("  进料：热端已到温（设定 %.0f℃）", (double)t);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ams_log_warn("  进料：等热端到温超时（%ums），继续送料（差几度顶多多退一次）",
                 (unsigned)AMS_HEAT_WAIT_MS);
}

/**
 * 等打印机"有进展"：stg_cur 或 gcode_state 变了就算。
 *
 * ★ 为什么不直接等某条特定应答：esp-mqtt 是异步回调模型，没有"等一条指定
 *   消息"的原生接口；而且拓竹不同固件版本的应答格式并不一致。
 *   Python 版的做法是 wait_msg_timeout(8000) —— "8 秒内收到任何消息就继续"，
 *   本质上也只是一个超时等待。这里做得稍好一点：等到打印机的阶段真的变了
 *   就立刻继续，最多等 AMS_PRINTER_WAIT_MS。
 */
static void __attribute__((unused)) wait_printer_progress(int prev_stg, int prev_gcode_state)
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
    ams_log("  进料（蠕动）：等待挤出机到位信号（最多 %ums）…",
            (unsigned)AMS_EXTRUDER_WAIT_MS);
    bool inplace = extruder_inplace_wait(AMS_EXTRUDER_WAIT_MS);
    if (!inplace) {
        ams_log_warn("  进料（蠕动）：等 %ums 没等到到位，料可能没咬住",
                     (unsigned)AMS_EXTRUDER_WAIT_MS);
        return false;
    }
    ams_log("  进料（蠕动）：挤出机到位");

    /* ② 到位后等 1 秒，让打印机齿轮把料头咬稳 */
    ams_log("  进料（蠕动）：间隔 %ums 后开始蠕动收尾（%u 次）",
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
    ams_log("  进料（蠕动）：5 次 × %u%% 速度",
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
    ams_log("  进料（蠕动）：蠕动收尾完成");
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

    ams_log("开始换色：目标通道 %d", printer_channel);

    int current = config_get_filament_current();
    current = probe_current_filament(current);

    /* ★ 挤出机"有没有料"这个上报（MQTT hw_switch_state）只能当**参考**，
     *   不能当权威判据：
     *   - extruder_inplace_hint == 0  → 打印机明确说无料，可以省掉电机退线这一步
     *   - extruder_inplace_hint == 1  → 只说明"工具头检测到有料"，
     *     **不代表**里面就是目标通道的料。实测 2026-09-22 那一整场打印，
     *     这个键 432 帧报告里只出现过 1 次、值恒为 1（增量报文只在值变化时
     *     才带键 = 从头到尾没变过），所以它给不出"料现在在不在"的信息。
     *   - extruder_inplace_hint == -1（还没收到过上报）→ 按"有料"处理，别省步骤
     *
     *   ★ 绝对不要再用它去决定"要不要换料"—— 那条路已经坑死过一次，
     *     见下面那段长注释。 */
    int hint = -1;
    {
        if (xSemaphoreTake(s_report_lock, 0) == pdTRUE) {
            hint = s_last_report.extruder_inplace_hint;
            xSemaphoreGive(s_report_lock);
        }
    }
    bool extruder_empty = (hint == 0);

    char cur_txt[48] = {0};
    char new_txt[48] = {0};
    ams_channel_text(current, cur_txt, sizeof(cur_txt));
    ams_channel_text(printer_channel, new_txt, sizeof(new_txt));

    if (extruder_empty) {
        ams_log("  挤出机无料，跳过退料，直接进新通道 %s", new_txt);
    } else {
        ams_log("  当前通道 %s，目标通道 %s", cur_txt, new_txt);
    }

    /* ----------------------------------------------------------------------
     * ★★ 这里原来有一条"挤出机有料 且 current==目标通道 → 无需更换，直接 resume"，
     *    2026-09-22 真机事故后**已删除**。别看它像优化，它必然把打印搞死。
     *    三条理由，条条都是实测：
     *
     *  ① 走到这里的时候，料**已经被切断、热端已经是空的**。
     *     handle_report 一拿到通道号就把 `M620 S255 / T255 / M621 S255`
     *     （切刀 + 把热端里的料吐出来）发出去了 —— 因为那条指令必须趁
     *     打印机还在 RUNNING 时发。也就是说"切刀"和"判要不要换"的顺序
     *     本来就是反的；再判成"不用换"，就变成"切了没人装回去"。
     *
     *  ② 打印机此刻就停在 stg_cur=24（**载入打印材料**），它要的正是一次进料。
     *     2026-09-22 真机时间线（打印机侧报文）：
     *       +319s  stg_cur=24 + gcode_state=PAUSE ← 换料请求
     *       +321s  bed_target_temper 3 → 76      ← 我们的 M190 生效
     *       +325s  gcode_state=RUNNING            ← resume 生效，但没进料
     *       +368s  print_error=318750723          ← 等不到料，报错
     *     打印机在"载入打印材料"这一步等的是**料**，不是 resume。
     *
     *  ③ 这条判定依赖的两个输入都不可信：
     *     · NVS 里的"当前通道"是上次手动上料留下的值，随时可能过期；
     *     · hw_switch_state 实测整场打印 432 帧里只出现 1 次、值为 1
     *       （增量报文只在"值变化"时才带这个键，说明**一次都没变过**），
     *       拿它判"挤出机有料"等于没有判据。
     *     本次事故就是两个同时踩中：NVS 记的 3 == 目标 3，但料已经被切走。
     *
     *  Top-AMS 的"同一耗材直接 resume"能成立，是因为它把判定放在发切刀
     *  **之前**（main.cpp:298，和 change_filament 互斥），压根不会出现
     *  "切了再说不换"。我们这里判定在切刀之后，只能二选一 ——
     *  **既然已经切了，就必须退料 + 进料把它装回去**。
     *
     *  结论：收到真正的换料请求，就老实走完整流程。不要再加"跳过"分支。
     * ---------------------------------------------------------------------- */
    if (current == printer_channel) {
        ams_log("  注意：目标通道 %s 与记录的当前通道相同，但切刀已经发过了 —— "
                "仍要退料 + 进料把料装回去（跳过会卡在 stg_cur=24 载入打印材料）",
                new_txt);
    }

    /* ---------- 步骤一：退料 ----------
     * 把当前通道的料从挤出机/缓冲区收回到料盘。
     * 退料由 AMS 电机完成，不需要打印机侧配合（打印机暂停中
     * G1 E-50 不会执行，发 M400 也没用）。
     *
     * ★ 走到这里"要不要换"已经不用再判了 —— 见上面那段说明：切刀都发过了，
     *   料必须在这次流程里被退出来、再把新料送进去。只有两种例外会跳过退料：
     *     · 打印机明确上报挤出机无料（hint == 0）；
     *     · NVS 里没有"当前通道"（extruder == 0），压根不知道该退哪一路。
     *   后一种必须显式告警，别让日志看着像"正常跳过"。 */
    if (!extruder_empty && current > 0) {
        int mat_cur = config_material_index_of(current);
        if (mat_cur >= 0) {
            ams_log("  退料：正在将通道 %s 的料收回料盘…", cur_txt);
            /* 打印机的快速退料（切刀 + 把热端里的料吐到缓冲）已经在
             * handle_report 里发过了 —— 那时打印机还在 RUNNING，指令发得
             * 出去；等走到这里（打印机已停在 M400 U1）再发就晚了。
             * 这里只做属于我们自己的那半件事：把料从缓冲收回到料盘。 */
            if (!do_retract(mat_cur)) {
                record_error("退料失败，换料中止");
                s_diag.exchange_fail++;
                set_state(AMS_STATE_ERROR, -1);
                return false;
            }
        } else {
            /* 通道号有效但查不到对应料盘位 —— 是配置问题，别静默跳过 */
            ams_log_warn("  退料：通道 %d 找不到对应料盘位，跳过退料", current);
        }
    } else if (current <= 0) {
        /* NVS 里 extruder == 0。Top-AMS 遇到这种情况是直接报错的
         * （"请设置当前所使用通道,否则无法退料再进料"），我们选择继续：
         * 打印机已经切了刀、正等料，报错只会更糟。但要显著告警。 */
        ams_log_warn("  退料：记录里没有当前通道（NVS extruder=0），"
                     "电机不知道退哪一路 —— 只能靠打印机刚才那次快速退料清空，"
                     "直接送新料（下次上料后请在网页把「当前通道」设对）");
    } else {
        ams_log("  退料：打印机已报挤出机无料，跳过退料步骤");
    }

    /* ---------- 步骤二：进料 + 蠕动 ----------
     * 把新通道的料送到挤出机入口，等到位后蠕动 5 次确保咬住。
     * 挤出机到位信号：C3 走 MQTT hw_switch_state，S3 走 GPIO。 */
    ams_log("  进料：正在将通道 %s 的料送入挤出机…", new_txt);

    /* ★ 先把热端升到新料的温度再送料（见 exchange_heat_nozzle）。
     *   温度不对，料头会在冷热端里顶住、打弯。 */
    exchange_heat_nozzle(mat_new);

    /* C3 无 GPIO 挤出机到位线，直接进料 */
    if (!do_load(mat_new, false)) {
        record_error("进料失败，换料中止");
        s_diag.exchange_fail++;
        set_state(AMS_STATE_ERROR, -1);
        return false;
    }

    /* 蠕动收尾（等到位 → 蠕动 5 次），即使等不到也强制执行 */
    if (!feed_until_extruder(mat_new)) {
        ams_log_warn("  未等到挤出机到位，料可能没咬住，仍尝试继续");
    }

    /* ---------- 步骤三：记录状态 + resume ----------
     * 辅助送料不在这里做——此时打印机在暂停，挤出机齿轮没转，
     * 推料没意义。正确的时机在 handle_report 的 stg=8/19/0 阶段。 */
    config_set_filament_current(printer_channel);
    s_diag.exchange_ok++;
    ams_log("  换色完成：通道 %s 已装载到挤出机，发送 resume", new_txt);
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
        ams_log("  报文未携带颜色信息，无法自动匹配");
        return -1;
    }
    int best_mat = -1;
    uint32_t dist = config_color_match((uint32_t)target_color, &best_mat);
    if (best_mat < 0) {
        ams_log_warn("自动匹配失败：所有通道都未配置颜色，退回按通道号换料");
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            uint32_t c = config_get_color(i);
            ams_log("  通道%d 颜色: %s", i + 1,
                    c ? "" : "未配置");
        }
        return -1;
    }
    int printer_ch = config_printer_channel_of(best_mat);
    if (printer_ch < 1 || printer_ch > BOARD_CHANNEL_COUNT) {
        ams_log_warn("自动匹配到料盘位 %d，但未映射到打印机通道", best_mat + 1);
        return -1;
    }
    /* 打印各通道颜色，方便排查 */
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        uint32_t c = config_get_color(i);
        ams_log("  通道%d 颜色: %s 0x%.6lX", i + 1,
                (i == best_mat) ? "(命中)" : "", (unsigned long)c);
    }
    ams_log("自动匹配：目标 0x%.6X → 通道%d（色差%lu）",
            (unsigned int)(target_color & 0xFFFFFF),
            printer_ch, (unsigned long)dist);
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
    /* ---- 热床温度记忆（必须在用它之前做）----
     * 切片用 M140 S{next_extruder + 1} 借"目标床温"传通道号，所以换色时
     * 打印机上报的 bed_target 会变成 1~16。真实床温要在被改写**之前**
     * 记住，换完再用 M190 还回去 —— 不还的话热床真的会按 M140 的设定
     * 降到 1~4℃，当前这一层直接粘不住。 */
    if (!r->has_bed_target) {
        /* ★ 这一帧压根没提床温 —— 打印机只发增量字段，**什么都别做**。
         *   2026-09-22 真机事故：以前这里把解析层的默认值 0 当成"床温真的
         *   关了"，于是每收到一帧不带该字段的报文就把刚记住的 76℃ 清掉。
         *   换色那一刻必然"没有热床温度的历史记录" → M190 从没发出去 →
         *   热床被 M140 S{next+1} 留在 3℃ 凉了一整晚。
         *   这一条就是那次卡死的根因。 */
    } else if (r->bed_channel > 0) {
        /* 这一帧的床温是通道号，不是温度 —— 别污染记忆值 */
    } else if (r->bed_target >= 17.0f) {
        /* 取见过的最高值：不同层/不同材料的床温不同（PLA 首层 60、后续 55），
         * 取最高能保证恢复后不会偏低。 */
        if (r->bed_target > s_bed_target_max) {
            s_bed_target_max = r->bed_target;
            ams_log("记住热床温度 %.0f℃（换完用它发 M190 恢复）",
                    (double)s_bed_target_max);
        }
    } else {
        /* 0（空闲 / 打印结束）或 17 以下的异常值：没有可信的床温，
         * 清掉记忆，下次开始打印重新学 —— 免得拿上一次打印的旧温度去恢复。 */
        s_bed_target_max = 0.0f;
    }

    /* ★ 只记录阶段变化，不刷屏 */
    if (r->stg_cur != s_last_seen_stg) {
        ams_log("阶段：%s", bambu_stage_text(r->stg_cur));
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

    /* ---- 辅助送料：根据打印机阶段精确控制 ----
     *
     * stg=8  （校准挤出）   → 换料后挤出机第一次拉料，做一次
     * stg=19 （流量校准）   → 动态流量校准，再做一次
     * stg=0  （打印中）     → 持续辅助，每 5s 一次（参考 Top-AMS 思路）
     *
     * 去重标志在 stg 离开目标阶段时重置（stg 从 8→19→0 正常流转，
     * 每次进入新阶段重新允许触发） */

    /* 重置标志：离开 stg=8 时重置 stg8 标志，离开 stg=19 时重置 stg19 标志 */
    if (r->stg_cur != 8)  { s_assist_done_stg8  = false; }
    if (r->stg_cur != 19) { s_assist_done_stg19 = false; }

    bool assist_do = false;
    if (config_get_assist_enabled()) {
        int cur_ch = config_get_filament_current();
        if (cur_ch > 0) {
            int mat = config_material_index_of(cur_ch);
            if (mat >= 0) {
                if (r->stg_cur == 8 && !s_assist_done_stg8) {
                    assist_do = true;
                    s_assist_done_stg8 = true;
                    ams_log("辅助送料（校准挤出）：通道%d", cur_ch);
                } else if (r->stg_cur == 19 && !s_assist_done_stg19) {
                    assist_do = true;
                    s_assist_done_stg19 = true;
                    ams_log("辅助送料（流量校准）：通道%d", cur_ch);
                } else if (r->stg_cur == 0) {
                    /* 打印中持续辅助，每 ASSIST_REPEAT_US 做一次 */
                    int64_t now = esp_timer_get_time();
                    if (now - s_last_assist_time_us >= ASSIST_REPEAT_US) {
                        assist_do = true;
                        s_last_assist_time_us = now;
                        ams_log("辅助送料（打印中）：通道%d", cur_ch);
                    }
                }
            }
        }
    }

    if (assist_do) {
        uint8_t assist_pct = config_get_assist_speed_pct();
        drive_channel_speed(
            config_material_index_of(config_get_filament_current()),
            1, assist_pct, AMS_LOAD_ASSIST_MS, false, NULL);
    }

    /* ---- 换料请求 ---- */
    if (!r->change_needed) {
        /* 打印机不再请求换料了 → 重置去重计数 */
        s_change_active = false;
        s_exchange_attempts = 0;
        return;
    }

    /* 打印机发来了换料请求，说明触发条件（mc_percent==101 或 ams_stage==1）成立
     * 默认：报文里的通道号（0 起，+1 转成 1 起） */
    int printer_ch = r->filament_next + 1;

    /* 打印触发原因（让用户知道是哪条通路触发的） */
    if (r->bed_channel > 0) {
        ams_log("收到打印机指令：热床信道 → 通道 %d"
                "（切片写了 M140 S{next_extruder+1}）", r->bed_channel);
    } else if (r->mc_percent == 101) {
        ams_log("收到打印机指令：M73 P101 换料请求（通道 %d）", r->filament_next + 1);
    } else if (r->ams_stage == 1) {
        ams_log("收到打印机指令：M400 U1 等待 AMS 换料（通道 %d）", r->filament_next + 1);
    } else {
        ams_log("收到打印机指令：换料请求（通道 %d，触发原因未知）", r->filament_next + 1);
    }

    /* ★ 自动颜色匹配：如果报文带目标颜色且本机配置了颜色，优先用匹配结果 */
    if (r->target_color >= 0) {
        ams_log("  带目标颜色 0x%.6X，尝试自动匹配…",
                (unsigned int)(r->target_color & 0xFFFFFF));
        int matched = auto_match_channel(r->target_color);
        if (matched > 0) {
            printer_ch = matched;
            ams_log("  匹配结果：通道 %d（已覆盖原始通道 %d）",
                    printer_ch, r->filament_next + 1);
        } else {
            ams_log("  自动匹配失败，退回原始通道 %d", printer_ch);
        }
    } else {
        ams_log("  报文未携带颜色信息，按通道号换料");
    }

    if (printer_ch < 1 || printer_ch > BOARD_CHANNEL_COUNT) {
        if (!s_change_active) {
            ams_log("  动作：通道 %d 超出本机 1~%d，拒绝换料",
                    printer_ch, BOARD_CHANNEL_COUNT);
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

    /* ---- 触发指纹去重（见 s_change_fp 的说明）----
     * 换料完成后打印机还会用旧状态继续上报 change_needed=true，
     * 指纹没变就当它是残留上报，直接忽略 —— 否则同一个颜色会被换两次。
     * 指纹变了（切片真的要求换到别的通道）立刻放行，连续换色不受影响。 */
    int fp = 0;
    if (r->bed_channel > 0) {
        fp = 1000 + r->bed_channel;
    } else if (r->mc_percent == 101) {
        fp = 2000 + printer_ch;
    } else if (r->ams_stage == 1) {
        fp = 3000 + printer_ch;
    }
    if (fp != 0 && fp == s_change_fp &&
        (esp_timer_get_time() - s_change_fp_us) <
            (int64_t)AMS_EXCHANGE_RESIDUAL_MS * 1000) {
        return;
    }

    /* ======================================================================
     * ★ 打印机侧预处理 —— 必须在这里做，不能留到 do_exchange
     * ======================================================================
     * 时序是这次修复里最关键的一点：
     *
     *   切片宏里 M140 S{next_extruder+1} 写在很前面，M400 U1 写在后面 ——
     *   A1 的宏是第 6 行 vs 第 25 行，仓库里那份真实切片输出（main/切片G.txt）
     *   里甚至隔了 28 行。也就是说**打印机还在 RUNNING 的时候，热床信令
     *   就已经到了**。
     *
     *   Top-AMS 正是趁这个窗口把「恢复床温 + 快速退料」发出去的。
     *
     *   （2026-09-22 补注：旧注释在这里写"必须趁 RUNNING，停机再发 G-code 流
     *   就不动了"—— 真机实测**不成立**，PAUSE 期间发的 `M190` / `M620/T255`
     *   都照常执行。窗口的真正意义是**赶在我们自己收线之前**，不是状态门禁。）
     *
     *   所以这里一拿到通道号就发两条：
     *     ① M190 S<bed>             把借走的热床温度还回去
     *     ② M109 + M620/T255/M621   让打印机自己切刀、把热端里的料吐到缓冲
     *   然后我们自己的电机再去收线（在 do_exchange 里，那时打印机已经暂停）。
     * ====================================================================== */

    /* ---- ① 归还热床温度 ----
     * 借了就必须还：不还的话热床会真的按 M140 的设定降到 1~4℃。
     * 发完 M190 之后打印机的 bed_target 立刻变回真实值，所以这段只会走一次。 */
    if (r->bed_channel > 0) {
        if (s_bed_target_max > 0.0f) {
            char g[64];   /* 留足余量：GCC 对 %.0f 的最大长度算得很宽 */
            /* ★ 结尾的 \n 不能省 —— gcode_line 的 param 必须以换行结尾，
             *   少了它这条 M190 会被打印机静默丢弃，热床就一直留在 1~4℃。
             *
             * ★ 别用 `snprintf(g, sizeof(g), "%s\n", msg)` 去拼：GCC 会算出
             *   "48 字节的 msg + \n" 可能放不下 48 字节的 g，于是
             *   -Werror=format-truncation 直接编译失败（ESP-IDF 默认开
             *   -Werror=all）。这里一次成型，日志再单独格一遍（不带 \n）。 */
            snprintf(g, sizeof(g), "M190 S%.0f\n", (double)s_bed_target_max);
            if (bambu_mqtt_send_gcode(g) >= 0) {
                ams_log("  已请求恢复热床温度：M190 S%.0f"
                        "（不恢复的话它会真的降到 1~4℃）",
                        (double)s_bed_target_max);
            } else {
                ams_log_warn("  恢复热床温度失败（MQTT 未连接？）—— "
                             "打印完请检查床温，可能已经掉了");
            }
        } else {
            ams_log_warn("  没有热床温度的历史记录，无法自动恢复 —— "
                         "如果发现热床凉了，请手动设回去");
        }
    }

    /* ---- ② 快速退料：先让打印机动手 ----
     * 挤出机已经明确上报"无料"（hint == 0）就跳过 —— 没料可退，
     * 硬发 M620 只会白换一个 HMS 报错。hint == -1 表示还没收到过这个
     * 字段，按"有料"处理（宁可多发一次）。 */
    {
        int cur_ch  = config_get_filament_current();
        int mat_cur = (cur_ch > 0) ? config_material_index_of(cur_ch) : -1;
        if (r->extruder_inplace_hint != 0) {
            exchange_prime_extrude(mat_cur);
        } else {
            ams_log("  挤出机已报无料，跳过打印机的快速退料");
        }
    }

    s_change_active = true;
    s_pending_fp = fp;

    ams_log("  动作：开始执行换料（目标通道 %d）", printer_ch);
    ams_cmd_t cmd = { .id = AMS_CMD_EXCHANGE, .channel = printer_ch };
    if (!ams_post_cmd(&cmd)) {
        ams_log("  换料命令排队失败（设备正忙），等打印机的下一次请求");
        s_change_active = false;
        s_pending_fp = 0;
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
            /* s_change_active 必须清零 —— 不清的话后续换料会被"正在处理"
             * 一直挡住。换料完成后打印机还会重复上报旧状态，那种情况由
             * 下面 s_change_fp 的指纹去重接管，不靠这个标志。 */
            s_change_active = false;
            /* ★ 把这次触发"盖章完成"：打印机在 1~3 秒内还会用旧状态
             *   继续上报 change_needed=true，指纹相同就说明是残留，忽略掉。
             *   以前这里只是清掉 s_change_active，于是下一帧立刻又排一次
             *   换料命令 —— 表现为"同一个颜色换两次"。 */
            if (s_pending_fp != 0) {
                s_change_fp = s_pending_fp;
                s_change_fp_us = esp_timer_get_time();
            }
            s_pending_fp = 0;
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
            if (boot_count == 100) ams_log("系统运行正常");
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
                handle_report(&copy);
            }
        }

        /* ---- ③ 执行命令（会阻塞，没问题 —— 只有这个任务被占住）---- */
        if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            execute_cmd(&cmd);
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
    s_last_report.extruder_inplace_hint = -1;  /* -1 = 还没收到上报 */
    s_last_error_us = UINT32_MAX;
    s_has_report = false;
    s_report_pending = false;
    s_exchange_attempts = 0;
    s_change_active = false;
    s_change_fp = -1;          /* -1 = 还没有"已完成"的触发，不会误挡 */
    s_change_fp_us = 0;
    s_pending_fp = 0;
    s_bed_target_max = 0.0f;
    s_assist_done_stg8 = false;
    s_assist_done_stg19 = false;
    s_last_assist_time_us = 0;
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
