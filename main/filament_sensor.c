/**
 * filament_sensor.c —— 微动开关的实现（5ms 轮询去抖 + 边沿序号）
 * ============================================================================
 *
 * 数据结构很直白：每个 (通道, 类型) 一格，记四样东西
 *
 *     raw         最近一次读到的电平（0/1）
 *     stable      去抖之后的稳定电平（0/1）
 *     debounce    连续读到"与 stable 不同"的次数，达到 SENSOR_DEBOUNCE_N 就翻转
 *     trig_seq    累计触发次数
 *     rel_seq     累计松开次数
 *     last_trig_us 最近一次触发的时刻（esp_timer 微秒），用于算"多久没动了"
 *
 * 去抖定时器是 esp_timer 的周期回调，跑在 esp_timer 任务里（独立于
 * ams_controller 那个任务），所以即使主循环正忙在一次长动作里，微动状态也在
 * 被持续更新 —— 等会儿一查序号就知道期间发生过什么，不会漏事件。
 */

#include "filament_sensor.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "log_buffer.h"

/* ==========================================================================
 * 引脚表
 * ==========================================================================
 * 顺序必须和 sensor_kind_t 一致：[停止, 开始, 自吸]，每行是 4 个料盘位。
 * 三个宏来自 board_pins.h，各自展开成 4 项的初始化列表。 */
static const int s_kind_pins[SENSOR_KIND_COUNT][BOARD_CHANNEL_COUNT] = {
    BOARD_PIN_SENSOR_STOP,
    BOARD_PIN_SENSOR_START,
    BOARD_PIN_SENSOR_AUTOLOAD,
};

/* ==========================================================================
 * 状态
 * ========================================================================== */
typedef struct {
    int      pin;             /* -1 表示未配置 */
    int      raw;
    int      stable;
    int      debounce;
    uint32_t trig_seq;
    uint32_t rel_seq;
    int64_t  last_trig_us;
    int64_t  last_rel_us;
} sensor_slot_t;

static sensor_slot_t s_slot[SENSOR_KIND_COUNT][BOARD_CHANNEL_COUNT];

/* 挤出机到位信号 */
static int      s_extruder_pin = -1;
static int      s_extruder_raw;
static int      s_extruder_stable;
static int      s_extruder_debounce;
static uint32_t s_extruder_seq;
static int64_t  s_extruder_last_us;

static esp_timer_handle_t s_timer;

/** 有效电平：BOARD_SENSOR_ACTIVE_LEVEL 默认 0（低电平触发） */
#define ACTIVE_LEVEL BOARD_SENSOR_ACTIVE_LEVEL

static inline int level_is_active(int level)
{
    return level == ACTIVE_LEVEL ? 1 : 0;
}

/* ==========================================================================
 * 去抖核心
 * ========================================================================== */

/** 处理一格：读引脚 → 去抖 → 翻转时更新序号 */
static void debounce_slot(sensor_slot_t *s)
{
    if (s->pin < 0) {
        return;
    }
    int level = gpio_get_level((gpio_num_t)s->pin);
    s->raw = level;
    int active = level_is_active(level);

    if (active == s->stable) {
        s->debounce = 0;
        return;
    }

    /* 读数与稳定值不同 → 累计；连着 N 次都不同才认可这次翻转 */
    if (++s->debounce < SENSOR_DEBOUNCE_N) {
        return;
    }
    s->debounce = 0;
    s->stable = active;

    int64_t now = esp_timer_get_time();
    if (active) {
        s->trig_seq++;
        s->last_trig_us = now;
    } else {
        s->rel_seq++;
        s->last_rel_us = now;
    }
}

static void sensor_timer_cb(void *arg)
{
    (void)arg;
    for (int k = 0; k < SENSOR_KIND_COUNT; k++) {
        for (int c = 0; c < BOARD_CHANNEL_COUNT; c++) {
            debounce_slot(&s_slot[k][c]);
        }
    }

    /* 挤出机到位信号：GPIO 模式才需要读引脚；MQTT 模式由
     * extruder_inplace_notify_from_mqtt() 直接推进序号 */
    if (s_extruder_pin >= 0) {
        int level = gpio_get_level((gpio_num_t)s_extruder_pin);
        s_extruder_raw = level;
        int active = level_is_active(level);
        if (active != s_extruder_stable) {
            if (++s_extruder_debounce >= SENSOR_DEBOUNCE_N) {
                s_extruder_debounce = 0;
                s_extruder_stable = active;
                if (active) {
                    s_extruder_seq++;
                    s_extruder_last_us = esp_timer_get_time();
                }
            }
        } else {
            s_extruder_debounce = 0;
        }
    }
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */

esp_err_t sensor_init(void)
{
    memset(s_slot, 0, sizeof(s_slot));
    for (int k = 0; k < SENSOR_KIND_COUNT; k++) {
        for (int c = 0; c < BOARD_CHANNEL_COUNT; c++) {
            s_slot[k][c].pin = s_kind_pins[k][c];
            s_slot[k][c].last_trig_us = -1;
            s_slot[k][c].last_rel_us = -1;
        }
    }

    /* ---- 配置 12 个微动引脚 ---- */
    uint64_t mask = 0;
    int configured = 0;
    for (int k = 0; k < SENSOR_KIND_COUNT; k++) {
        for (int c = 0; c < BOARD_CHANNEL_COUNT; c++) {
            int pin = s_slot[k][c].pin;
            if (pin < 0) {
                continue;
            }
            mask |= (1ULL << pin);
            configured++;
        }
    }

    if (configured > 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = mask,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = BOARD_SENSOR_PULL_UP ? GPIO_PULLUP_ENABLE
                                                 : GPIO_PULLUP_DISABLE,
            .pull_down_en = BOARD_SENSOR_PULL_UP ? GPIO_PULLDOWN_DISABLE
                                                 : GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ams_log_err("微动引脚配置失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* ---- 挤出机到位信号 ---- */
    if (!BOARD_EXTRUDER_INPLACE_UNUSED && BOARD_PIN_EXTRUDER_INPLACE >= 0) {
        s_extruder_pin = BOARD_PIN_EXTRUDER_INPLACE;
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << s_extruder_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = BOARD_SENSOR_PULL_UP ? GPIO_PULLUP_ENABLE
                                                 : GPIO_PULLUP_DISABLE,
            .pull_down_en = BOARD_SENSOR_PULL_UP ? GPIO_PULLDOWN_DISABLE
                                                 : GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ams_log_err("挤出机到位信号(GPIO%d)配置失败: %s",
                        s_extruder_pin, esp_err_to_name(err));
            s_extruder_pin = -1;
        }
    } else {
        s_extruder_pin = -1;
    }

    /* ---- 读一遍初值，让 stable 反映真实状态，而不是停留在 0 ---- */
    for (int k = 0; k < SENSOR_KIND_COUNT; k++) {
        for (int c = 0; c < BOARD_CHANNEL_COUNT; c++) {
            if (s_slot[k][c].pin >= 0) {
                int level = gpio_get_level((gpio_num_t)s_slot[k][c].pin);
                s_slot[k][c].raw = level;
                s_slot[k][c].stable = level_is_active(level);
            }
        }
    }
    if (s_extruder_pin >= 0) {
        int level = gpio_get_level((gpio_num_t)s_extruder_pin);
        s_extruder_raw = level;
        s_extruder_stable = level_is_active(level);
    }

    /* ---- 启动去抖定时器 ---- */
    if (s_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback        = sensor_timer_cb,
            .arg             = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "ams_sensor",
        };
        esp_err_t err = esp_timer_create(&args, &s_timer);
        if (err != ESP_OK) {
            ams_log_err("创建微动去抖定时器失败: %s", esp_err_to_name(err));
            return err;
        }
    }
    esp_timer_start_periodic(s_timer, SENSOR_POLL_MS * 1000);

    /* ---- 打一份接线摘要 ---- */
    ams_log("%d 个微动已初始化（%s触发，去抖 %d×%dms）",
            configured, ACTIVE_LEVEL ? "高电平" : "低电平",
            SENSOR_DEBOUNCE_N, SENSOR_POLL_MS);
    for (int c = 0; c < BOARD_CHANNEL_COUNT; c++) {
        ams_log("  料盘位%d: 停止=GPIO%d 开始=GPIO%d 自吸=GPIO%d  %s",
                c + 1,
                s_slot[SENSOR_STOP][c].pin,
                s_slot[SENSOR_START][c].pin,
                s_slot[SENSOR_AUTOLOAD][c].pin,
                sensor_channel_enabled(c) ? "（已启用）" : "（未启用，走降级模式）");
    }
    if (s_extruder_pin >= 0) {
        ams_log("  挤出机到位信号: GPIO%d", s_extruder_pin);
    } else {
        ams_log("  挤出机到位信号: 不接 GPIO，等打印机 MQTT 事件");
    }

    return ESP_OK;
}

void sensor_deinit(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
}

/* ==========================================================================
 * 读数
 * ========================================================================== */

static inline const sensor_slot_t *slot_of(int channel, sensor_kind_t kind)
{
    if (channel < 0 || channel >= BOARD_CHANNEL_COUNT) {
        return NULL;
    }
    if (kind < 0 || kind >= SENSOR_KIND_COUNT) {
        return NULL;
    }
    return &s_slot[kind][channel];
}

bool sensor_triggered(int channel, sensor_kind_t kind)
{
    const sensor_slot_t *s = slot_of(channel, kind);
    if (s == NULL || s->pin < 0) {
        return false;
    }
    return s->stable != 0;
}

int sensor_raw_level(int channel, sensor_kind_t kind)
{
    const sensor_slot_t *s = slot_of(channel, kind);
    if (s == NULL || s->pin < 0) {
        return -1;
    }
    return s->raw;
}

bool sensor_pin_present(int channel, sensor_kind_t kind)
{
    const sensor_slot_t *s = slot_of(channel, kind);
    return (s != NULL) && (s->pin >= 0);
}

bool sensor_channel_enabled(int channel)
{
    return config_sensor_enabled(channel);
}

uint8_t sensor_enabled_mask(void)
{
    return config_get()->sensor_enabled_mask;
}

esp_err_t sensor_set_channel_enabled(int channel, bool enabled)
{
    if (channel < 0 || channel >= BOARD_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    ams_config_t *cfg = config_get();
    uint8_t before = cfg->sensor_enabled_mask;
    if (enabled) {
        cfg->sensor_enabled_mask |= (uint8_t)(1u << channel);
    } else {
        cfg->sensor_enabled_mask &= (uint8_t)~(1u << channel);
    }
    if (cfg->sensor_enabled_mask != before) {
        ams_log("料盘位%d 微动%s，该路%s",
                channel + 1, enabled ? "已启用" : "已停用",
                enabled ? "按微动反馈动作" : "改为按时间推进（降级模式）");
        return config_save();
    }
    return ESP_OK;
}

/* ==========================================================================
 * 边沿序号与等待
 * ========================================================================== */

uint32_t sensor_trigger_seq(int channel, sensor_kind_t kind)
{
    const sensor_slot_t *s = slot_of(channel, kind);
    return s ? s->trig_seq : 0;
}

uint32_t sensor_release_seq(int channel, sensor_kind_t kind)
{
    const sensor_slot_t *s = slot_of(channel, kind);
    return s ? s->rel_seq : 0;
}

uint32_t sensor_ms_since_trigger(int channel, sensor_kind_t kind)
{
    const sensor_slot_t *s = slot_of(channel, kind);
    if (s == NULL || s->last_trig_us < 0) {
        return UINT32_MAX;
    }
    int64_t delta = esp_timer_get_time() - s->last_trig_us;
    if (delta < 0) {
        return 0;
    }
    return (uint32_t)(delta / 1000);
}

bool sensor_wait_trigger(int channel, sensor_kind_t kind, uint32_t timeout_ms)
{
    if (!sensor_pin_present(channel, kind)) {
        return false;
    }
    /* 已经触发着就直接算成功 —— 料本来就压着微动时，"开始送料"应该马上动作，
     * 而不是死等一次新的边沿（那会一直等到超时） */
    if (sensor_triggered(channel, kind)) {
        return true;
    }
    uint32_t base = sensor_trigger_seq(channel, kind);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (sensor_trigger_seq(channel, kind) != base) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
    return false;
}

bool sensor_wait_release(int channel, sensor_kind_t kind, uint32_t timeout_ms)
{
    if (!sensor_pin_present(channel, kind)) {
        return false;
    }
    if (!sensor_triggered(channel, kind)) {
        return true;
    }
    uint32_t base = sensor_release_seq(channel, kind);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (sensor_release_seq(channel, kind) != base) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
    return false;
}

/* ==========================================================================
 * 挤出机到位信号
 * ========================================================================== */

bool extruder_inplace_triggered(void)
{
    return s_extruder_stable != 0;
}

uint32_t extruder_inplace_seq(void)
{
    return s_extruder_seq;
}

int extruder_inplace_raw_level(void)
{
    return (s_extruder_pin >= 0) ? s_extruder_raw : -1;
}

uint32_t extruder_inplace_ms_since(void)
{
    if (s_extruder_last_us < 0) {
        return UINT32_MAX;
    }
    int64_t delta = esp_timer_get_time() - s_extruder_last_us;
    if (delta < 0) {
        return 0;
    }
    return (uint32_t)(delta / 1000);
}

void extruder_inplace_clear(void)
{
    s_extruder_stable = 0;
    s_extruder_debounce = 0;
    if (s_extruder_pin >= 0) {
        /* 清状态时也把软件值拉到当前引脚状态，避免"清完立刻又变回到位" */
        s_extruder_stable =
            level_is_active(gpio_get_level((gpio_num_t)s_extruder_pin));
    }
}

void extruder_inplace_notify_from_mqtt(void)
{
    /* 走 MQTT 模式时，把上报直接记成一次边沿 —— 效果和 GPIO 触发完全一样，
     * 自吸流程不需要区分信号从哪来 */
    s_extruder_stable = 1;
    s_extruder_seq++;
    s_extruder_last_us = esp_timer_get_time();
}

bool extruder_inplace_wait(uint32_t timeout_ms)
{
    if (extruder_inplace_triggered()) {
        return true;
    }
    uint32_t base = s_extruder_seq;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_extruder_seq != base) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_MS));
    }
    return false;
}

/* ==========================================================================
 * 状态描述（给网页）
 * ========================================================================== */

int sensor_describe_json(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return 0;
    }
    int off = snprintf(buf, buflen, "{\"enabled\":%u,\"extruder\":%d,\"ch\":[",
                       (unsigned)sensor_enabled_mask(),
                       extruder_inplace_triggered() ? 1 : 0);

    for (int c = 0; c < BOARD_CHANNEL_COUNT; c++) {
        off += snprintf(buf + off, buflen - off, "%s[%d,%d,%d]",
                        c ? "," : "",
                        sensor_triggered(c, SENSOR_STOP) ? 1 : 0,
                        sensor_triggered(c, SENSOR_START) ? 1 : 0,
                        sensor_triggered(c, SENSOR_AUTOLOAD) ? 1 : 0);
    }

    off += snprintf(buf + off, buflen - off, "]}");
    return off;
}
