/**
 * board_pins_c3.h —— ESP32-C3 引脚表（4 通道方案，微动降级）
 * ============================================================================
 *
 * ESP32-C3 总共只有 10 个通用 GPIO（0-9），其中：
 *   - GPIO4/5 = SPI Flash 占用（模组内部），不可用
 *   - GPIO8/9 = strapping（BOOT 按钮 / 启动模式），仅作输入
 *   - 实际可用 8 个脚（0,1,2,3,6,7）+ 3 个备用输入脚（4,5 是 Flash 占用，
 *     但有些模组把 GPIO10 也引出，可作为备用）
 *
 * 4 通道分配方案（电机 2 + 离合 4 + 微动 4 = 10 个角色）：
 *   - 电磁离合 ×4：GPIO0/1/2/3
 *   - 电机 H 桥 ×2：GPIO6/7（PWM 输出）
 *   - 停止微动 ×4：GPIO4/5（共享 Flash 引脚！若你的 C3 模组把 4/5 引出来
 *     才能用；若被 Flash 占用则全部退化为 -1 走超时模式）
 *   - 开始微动 ×4 / 自吸微动 ×4：无可用脚，全部 -1 走 MQTT + 超时
 *   - 挤出机到位：走 MQTT 事件（hw_switch_state）
 *
 * 与 S3 版的区别：
 *   - BOARD_CHANNEL_COUNT = 4（与 S3 一致，UI 不缩水）
 *   - 4/5 引脚可能不可用（取决于模组是否把这两个脚引出来）
 *   - BOARD_EXTRUDER_INPLACE_UNUSED = true（完全依赖 MQTT）
 *   - BOARD_GPIO_MAX = 10
 *   - 同通道"离合引脚 = 微动引脚"是允许的（board_pins_validate 白名单）
 */

#ifndef BOARD_PINS_C3_H
#define BOARD_PINS_C3_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 一、通道数（C3 与 S3 对齐，UI 不缩水）
 * ========================================================================== */
#define BOARD_CHANNEL_COUNT 4

/* ==========================================================================
 * 二、执行机构（输出）
 * ========================================================================== */
#define BOARD_PIN_MOTOR_IN1 6      /* H 桥 IN1：进料（正转） */
#define BOARD_PIN_MOTOR_IN2 7      /* H 桥 IN2：退料（反转） */
#define BOARD_PIN_CLUTCH {0, 1, 2, 3}  /* 电磁离合 1~4（输入：微动开关对 GND + 内部上拉） */
#define BOARD_PIN_LED -1          /* C3 DevKit 板载 LED 是 WS2812，用 GPIO8 但它是 strapping，这里不用 */

/* ==========================================================================
 * 三、★ 微动开关（输入）：C3 只有 GPIO4/5 可引出来 ★
 * ==========================================================================
 * 分配策略（把有限的引脚给最关键的"停止"信号）：
 *   - 通道 1 / 2 的"停止送料微动" → GPIO4 / GPIO5
 *     （若模组没把 4/5 引出来，把下面两个宏改成 -1，走纯超时模式）
 *   - 通道 3 / 4 的"停止微动"     → -1（没脚了，走超时模式）
 *   - 所有通道的"开始微动 / 自吸微动" → -1（没脚，走 MQTT 事件 + 超时）
 *
 * -1 表示该传感器不可用，filament_sensor.c 会跳过对应 GPIO，
 * 进料流程退化为纯超时模式（AMS_NO_LIMIT_LOAD_MS 计时封顶）。
 */
/** 停止送料微动：通道 1/2 接 GPIO4/5，通道 3/4 走超时 */
#define BOARD_PIN_SENSOR_STOP     {4, 5, -1, -1}
/** 开始送料微动：全部 -1，走 MQTT + 超时 */
#define BOARD_PIN_SENSOR_START    {-1, -1, -1, -1}
/** 自吸微动：全部 -1，走 MQTT + 超时 */
#define BOARD_PIN_SENSOR_AUTOLOAD {-1, -1, -1, -1}

/** 挤出机到位信号：C3 完全依赖 MQTT 事件（hw_switch_state） */
#define BOARD_PIN_EXTRUDER_INPLACE -1
#define BOARD_EXTRUDER_INPLACE_UNUSED true

/* ==========================================================================
 * 四、电平约定（与 S3 一致）
 * ========================================================================== */
#define BOARD_SENSOR_ACTIVE_LEVEL 0
#define BOARD_SENSOR_PULL_UP true
#define BOARD_CLUTCH_ACTIVE_LEVEL 1
#define BOARD_LED_ACTIVE_LEVEL 1

/* ==========================================================================
 * 五、引脚分类（C3）
 * ========================================================================== */
#define BOARD_GPIO_MAX 10

/** C3 的 strapping 脚：GPIO0/1/2（启动模式 / JTAG / BOOT），仅作输入安全 */
#define BOARD_STRAPPING_PINS {0, 1, 2, 8, 9}

/**
 * C3 保留脚（模组内部占用，**不要引出来**）。
 *
 * 注意：GPIO4/5 在标准 WROOM 模组上是 SPI Flash 的 SPID / SPICLK，
 * 但大多数 C3 模组把这两个脚也引到了板边（作为通用 IO 引出），
 * 实际能否当输入用取决于你的具体模组。
 * 本表里把 4/5 放在"保留脚"列表只表示"模组内部可能占用"，
 * 校验时如果 4/5 被用作输入（微动开关）会通过白名单放行。
 */
typedef struct {
    int gpio;
    const char *why;
} board_reserved_pin_t;

#define BOARD_RESERVED_PINS                                                    \
    {                                                                          \
        {4, "SPI Flash SPID（WROOM 模组内部，但很多板子引出来了）"},         \
        {5, "SPI Flash SPICLK（WROOM 模组内部，但很多板子引出来了）"},       \
        {12, "SPI Flash SPICS0（模组内部占用）"},                              \
        {13, "SPI Flash SPID（模组内部占用）"},                               \
        {14, "SPI Flash SPICLK（模组内部占用）"},                             \
        {15, "SPI Flash SPIQ（模组内部占用）"},                               \
        {16, "SPI Flash SPIDQ（模组内部占用）"},                              \
        {17, "SPI Flash SPIQ（模组内部占用）"},                               \
        {20, "UART0 RX（日志 / 串口命令口）"},                                \
        {21, "UART0 TX（日志 / 串口命令口）"},                                \
    }

/* ==========================================================================
 * 六、剩余可用 IO —— C3 上已全部用完（电机 2 + 离合 2 = 4 只，无剩余）
 * ========================================================================== */
typedef struct {
    int pin;
    const char *note;
} board_spare_pin_t;

#define BOARD_SPARE_PINS                                                       \
    {                                                                          \
        {3,  "干净脚，可作输入（微动 / 传感器）"},                            \
        {10, "干净脚，可作输入（微动 / 传感器）"},                            \
    }

/* ==========================================================================
 * 七、工具函数（与 S3 版相同，header-only）
 * ========================================================================== */

typedef struct {
    int  pin;
    char role[32];
} board_used_pin_t;

static inline int board_collect_used(board_used_pin_t *out, int max)
{
    static const int clutch[BOARD_CHANNEL_COUNT] = BOARD_PIN_CLUTCH;
    static const int s_stop[BOARD_CHANNEL_COUNT]  = BOARD_PIN_SENSOR_STOP;
    static const int s_start[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_START;
    static const int s_auto[BOARD_CHANNEL_COUNT]  = BOARD_PIN_SENSOR_AUTOLOAD;
    int n = 0;

#define ADD(_pin, _role_fmt, ...)                                              \
    do {                                                                       \
        if (_pin >= 0 && n < max) {                                            \
            out[n].pin = (_pin);                                               \
            snprintf(out[n].role, sizeof(out[n].role), _role_fmt, __VA_ARGS__);\
            n++;                                                               \
        }                                                                      \
    } while (0)

    ADD(BOARD_PIN_MOTOR_IN1, "%s", "电机 IN1");
    ADD(BOARD_PIN_MOTOR_IN2, "%s", "电机 IN2");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        ADD(clutch[i], "电磁离合%d", i + 1);
    }
    if (BOARD_PIN_LED >= 0) {
        ADD(BOARD_PIN_LED, "%s", "状态 LED");
    }
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        ADD(s_stop[i],  "通道%d停止送料微动", i + 1);
    }
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        ADD(s_start[i], "通道%d开始送料微动", i + 1);
    }
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        ADD(s_auto[i],  "通道%d自吸微动", i + 1);
    }
    if (BOARD_PIN_EXTRUDER_INPLACE >= 0 && !BOARD_EXTRUDER_INPLACE_UNUSED) {
        ADD(BOARD_PIN_EXTRUDER_INPLACE, "%s", "挤出机到位信号");
    }
#undef ADD
    return n;
}

static inline bool board_pins_validate(char *err, size_t errlen)
{
    board_used_pin_t used[32];
    int n = board_collect_used(used, (int)(sizeof(used) / sizeof(used[0])));
    const board_reserved_pin_t reserved[] = BOARD_RESERVED_PINS;
    const int strapping[] = BOARD_STRAPPING_PINS;

    bool ok = true;
#define FAIL(_fmt, ...)                                                        \
    do {                                                                       \
        ok = false;                                                            \
        if (err && errlen) {                                                   \
            snprintf(err, errlen, _fmt, ##__VA_ARGS__);                        \
            return false;                                                      \
        }                                                                      \
    } while (0)

    /* 1) 重复占用 —— 同通道"离合 = 微动"是 C3 设计允许的（一个引脚既当
     *    离合又当停止微动，用输入电平判断离合状态 + 开关触发），白名单放行 */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (used[i].pin != used[j].pin) {
                continue;
            }
            /* 角色里带"通道 N"前缀的才算同通道冲突 */
            const char *ch_i = strstr(used[i].role, "通道");
            const char *ch_j = strstr(used[j].role, "通道");
            if (ch_i && ch_j && ch_i[2] == ch_j[2]) {
                continue;   /* 同通道冲突，放行 */
            }
            FAIL("引脚 GPIO%d 被「%s」和「%s」同时占用",
                 used[i].pin, used[i].role, used[j].role);
        }
    }

    /* 2) 越界 / 保留脚 —— 4/5 当"微动输入"放行（见 reserved 注释） */
    for (int i = 0; i < n; i++) {
        int p = used[i].pin;
        if (p < 0 || p > BOARD_GPIO_MAX) {
            FAIL("GPIO%d（%s）超出 ESP32-C3 的 GPIO0~GPIO%d 范围",
                 p, used[i].role, BOARD_GPIO_MAX);
        }
        for (size_t r = 0; r < sizeof(reserved) / sizeof(reserved[0]); r++) {
            if (reserved[r].gpio != p) {
                continue;
            }
            /* 白名单：GPIO4/5 当"停止微动输入"允许（WROOM 引出来的） */
            bool is_stop_input = (strstr(used[i].role, "停止") != NULL);
            if ((p == 4 || p == 5) && is_stop_input) {
                break;   /* 放行，不再判 reserved 冲突 */
            }
            FAIL("GPIO%d（%s）被 %s 占用，不能用",
                 p, used[i].role, reserved[r].why);
        }
    }

    /* 3) strapping 脚被当**输出**用（输入安全，输出危险）
     *    C3 上 GPIO0/1/2 是 strapping：0/1 作输入（离合 + 停止微动）允许，
     *    作输出（电机驱动 / LED）会改上电平、芯片进不了正常模式 */
    for (int i = 0; i < n; i++) {
        int p = used[i].pin;
        bool is_strap = false;
        for (size_t s = 0; s < sizeof(strapping) / sizeof(strapping[0]); s++) {
            if (strapping[s] == p) {
                is_strap = true;
                break;
            }
        }
        if (!is_strap) {
            continue;
        }
        bool is_output = (strstr(used[i].role, "电机") != NULL) ||
                         (strstr(used[i].role, "LED") != NULL);
        if (is_output) {
            FAIL("GPIO%d（%s）是 C3 strapping 脚，不能当输出用！"
                 "上电瞬间会改掉启动电平，芯片可能进不了正常模式",
                 p, used[i].role);
        }
    }
#undef FAIL

    if (ok && err && errlen) {
        err[0] = '\0';
    }
    return ok;
}

static inline int board_pins_describe(char *buf, size_t buflen)
{
    static const int clutch[BOARD_CHANNEL_COUNT] = BOARD_PIN_CLUTCH;
    static const int s_stop[BOARD_CHANNEL_COUNT]  = BOARD_PIN_SENSOR_STOP;
    static const int s_start[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_START;
    static const int s_auto[BOARD_CHANNEL_COUNT]  = BOARD_PIN_SENSOR_AUTOLOAD;
    const board_spare_pin_t spare[] = BOARD_SPARE_PINS;
    int off = 0;

#define P(...)                                                                 \
    do {                                                                       \
        if (off < (int)buflen - 1) {                                            \
            int _w = snprintf(buf + off, buflen - off, __VA_ARGS__);           \
            if (_w > 0) { off += _w; }                                         \
        }                                                                      \
    } while (0)

    P("---- 硬件接线表（ESP32-C3 / 2 通道降级）----\n");
    P("共享电机 : IN1=GPIO%d  IN2=GPIO%d\n",
      BOARD_PIN_MOTOR_IN1, BOARD_PIN_MOTOR_IN2);
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        P("料盘位%d  : 离合=GPIO%d | 停止=GPIO%d 开始=GPIO%d 自吸=GPIO%d\n",
          i + 1, clutch[i], s_stop[i], s_start[i], s_auto[i]);
    }
    P("挤出机到位: 不接线（依赖 MQTT hw_switch_state）\n");
    P("状态 LED : 未使用（C3 板载 WS2812 在 strapping 脚上）\n");
    P("剩余可用 : ");
    for (size_t i = 0; i < sizeof(spare) / sizeof(spare[0]); i++) {
        P("GPIO%d%s", spare[i].pin,
          i + 1 < sizeof(spare) / sizeof(spare[0]) ? ", " : "\n");
    }
#undef P
    return off;
}

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PINS_C3_H */
