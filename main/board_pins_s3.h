/**
 * board_pins_s3.h —— ESP32-S3（42 针）引脚表  ★ 改接线只改这一个文件 ★
 * ============================================================================
 *
 * 这个文件对应 MicroPython 版的 board_s3.py + hardware_config.py 里的引脚部分。
 * 单独抽出来是因为它最常被改：换一块模组、改一根线，都只动这里，业务代码
 * （motor / clutch / ams_controller / web_server）一行都不用碰。
 *
 * 由 board_pins.h 自动选择 include（S3 → 本文件，C3 → board_pins_c3.h）。
 */

#ifndef BOARD_PINS_S3_H
#define BOARD_PINS_S3_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 一、通道数
 * ========================================================================== */
/** 料盘位（电磁离合 + 微动组）数量。整套代码都按这个宏循环，加通道只改这里。 */
#define BOARD_CHANNEL_COUNT 4

/* ==========================================================================
 * 二、执行机构（输出）
 * ========================================================================== */
#define BOARD_PIN_MOTOR_IN1 4      /* H 桥 IN1：方向 1 = 进料（正转） */
#define BOARD_PIN_MOTOR_IN2 5      /* H 桥 IN2：方向 -1 = 退料（反转） */
#define BOARD_PIN_CLUTCH {6, 7, 8, 9}   /* 电磁离合 1~4，高电平吸合 */
#define BOARD_PIN_LED 2            /* 板载状态灯；不需要就改成 -1 */

/* ==========================================================================
 * 三、★ 微动开关（输入）：每路三个 ★
 * ==========================================================================
 * 数组下标 0~3 对应料盘位 1~4。
 * 三个数组要一一对应同一路：即 SENSOR_STOP[i] / START[i] / AUTOLOAD[i] 属于
 * 同一个料盘位、同一路电磁离合。
 */
/** 停止送料微动：触发 → 立刻停电机 */
#define BOARD_PIN_SENSOR_STOP     {11, 14, 17, 38}
/** 开始送料微动：触发 → 按常规速度送料 */
#define BOARD_PIN_SENSOR_START    {12, 15, 18, 47}
/** 自吸微动：触发 → 全自动上料（送料 → 挤出机到位 → 蠕动送料 3 次） */
#define BOARD_PIN_SENSOR_AUTOLOAD {13, 16, 21, 48}

/**
 * 挤出机到位信号（全局，不是每路一个）。
 *
 * 自吸流程用它确认"耗材真的进了挤出机"，然后才做蠕动收尾。
 * 不想接这根线也可以：把 config 里的 extruder_src 设成 MQTT，
 * 改成由打印机上报的事件驱动（很多机器没有空闲的 IO 能引出来）。
 */
#define BOARD_PIN_EXTRUDER_INPLACE 1
/** 设为 true 表示不接这根线，完全依赖 MQTT 事件 */
#define BOARD_EXTRUDER_INPLACE_UNUSED false

/* ==========================================================================
 * 四、电平约定
 * ========================================================================== */
/** 微动有效电平：0 = 低电平触发（内部上拉 + 开关对地），1 = 高电平触发 */
#define BOARD_SENSOR_ACTIVE_LEVEL 0
/** 微动是否启用内部上拉 */
#define BOARD_SENSOR_PULL_UP true
/** 电磁离合有效电平：1 = 高电平吸合（驱动板若为低电平吸合则改 0） */
#define BOARD_CLUTCH_ACTIVE_LEVEL 1
/** 状态灯有效电平：1 表示高电平点亮（多数开发板如此） */
#define BOARD_LED_ACTIVE_LEVEL 1

/* ==========================================================================
 * 五、引脚分类（上电自检用）
 * ========================================================================== */
#define BOARD_GPIO_MAX 48

/** strapping 脚：作**输入**安全，作**输出**危险（会改上电瞬间的电平） */
#define BOARD_STRAPPING_PINS {0, 3, 45, 46}

/** 完全不能用的脚（模组内部占用） */
typedef struct {
    int gpio;
    const char *why;
} board_reserved_pin_t;

#define BOARD_RESERVED_PINS                                                    \
    {                                                                          \
        {19, "USB D-（占用后没有 USB 串口与内置 JTAG）"},                      \
        {20, "USB D+（占用后没有 USB 串口与内置 JTAG）"},                      \
        {26, "模组内置 SPI Flash (SPI0/1)"}, {27, "模组内置 SPI Flash (SPI0/1)"}, \
        {28, "模组内置 SPI Flash (SPI0/1)"}, {29, "模组内置 SPI Flash (SPI0/1)"}, \
        {30, "模组内置 SPI Flash (SPI0/1)"}, {31, "模组内置 SPI Flash (SPI0/1)"}, \
        {32, "模组内置 SPI Flash (SPI0/1)"},                                   \
        {33, "八线 PSRAM 占用（无 PSRAM 的模组上可用）"},                      \
        {34, "八线 PSRAM 占用（无 PSRAM 的模组上可用）"},                      \
        {35, "八线 PSRAM 占用（无 PSRAM 的模组上可用）"},                      \
        {36, "八线 PSRAM 占用（无 PSRAM 的模组上可用）"},                      \
        {37, "八线 PSRAM 占用（无 PSRAM 的模组上可用）"},                      \
        {43, "UART0 TX（日志 / 串口命令口）"},                                 \
        {44, "UART0 RX（日志 / 串口命令口）"},                                 \
    }

/* ==========================================================================
 * 六、剩余可用 IO —— 把本方案没占用、也不是保留脚的**全部**空闲脚列出来
 * ==========================================================================
 * 算法：干净脚集合 − 本方案已占用（电机 2 + 离合 4 + 状态灯 1 + 微动 12
 *       + 挤出机到位 1 = 20 只）
 *
 *   parses 出来是：10、39、40、41、42（5 只干净脚）
 *                 0、3、45、46（4 只 strapping 脚，仅建议作输入）
 *
 * 这是"把剩余可用 IO 全部引出"的那份清单：42 针排针上引出来的、本方案没用
 * 的那些脚，就是下面这些。
 */
typedef struct {
    int pin;
    const char *note;
} board_spare_pin_t;

#define BOARD_SPARE_PINS                                                       \
    {                                                                          \
        {10, "干净脚，可作输入或输出"},                                        \
        {39, "外部 JTAG(MTDO)；不接外部调试器时可作普通 IO，本项目留空"},      \
        {40, "外部 JTAG(MTDI)；同上"},                                         \
        {41, "外部 JTAG(MTDO)；同上"},                                         \
        {42, "外部 JTAG(MTMS)；同上"},                                         \
        {0,  "strapping(BOOT 键)，仅建议作输入"},                              \
        {3,  "strapping(外部 JTAG 信号源选择)，仅建议作输入"},                 \
        {45, "strapping(VDD_SPI 电压选择)，仅建议作输入"},                     \
        {46, "strapping(ROM 打印)，仅建议作输入"},                             \
    }

/* ==========================================================================
 * 七、工具函数（header-only，避免再多一个 .c 文件）
 * ========================================================================== */

/** 本方案实际用到的全部引脚（用于重复占用检查） */
typedef struct {
    int  pin;
    char role[32];
} board_used_pin_t;

/**
 * 把"本方案用到的所有引脚 + 角色名"填进 out（最多 max 项），返回实际数量。
 *
 * 上电时拿它跑两件事：
 *   1. 查有没有同一只脚被两个角色占用（接线表改错时最常见的错误）
 *   2. 查有没有踩到保留脚 / 把 strapping 脚当输出用
 */
static inline int board_collect_used(board_used_pin_t *out, int max)
{
    static const int clutch[BOARD_CHANNEL_COUNT] = BOARD_PIN_CLUTCH;
    static const int s_stop[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_STOP;
    static const int s_start[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_START;
    static const int s_auto[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_AUTOLOAD;
    int n = 0;

#define ADD(_pin, _role_fmt, ...)                                              \
    do {                                                                       \
        if (n < max) {                                                         \
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
        ADD(s_stop[i], "通道%d停止送料微动", i + 1);
    }
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        ADD(s_start[i], "通道%d开始送料微动", i + 1);
    }
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        ADD(s_auto[i], "通道%d自吸微动", i + 1);
    }
    if (BOARD_PIN_EXTRUDER_INPLACE >= 0 && !BOARD_EXTRUDER_INPLACE_UNUSED) {
        ADD(BOARD_PIN_EXTRUDER_INPLACE, "%s", "挤出机到位信号");
    }
#undef ADD
    return n;
}

/**
 * 引脚表自检。发现错误返回 false，并把人类可读的说明写进 err（可以为 NULL）。
 *
 * 查三件事：
 *   1. 重复占用 —— 同一只脚被两个角色用了
 *   2. 越界 / 踩保留脚 —— 用了 Flash / USB / UART 这些不能碰的脚
 *   3. strapping 脚被当输出用 —— 其中最严重的是电机 H 桥（AT8236 的 IN 脚
 *      内置下拉，上电瞬间会把 strapping 拉低，芯片进不了正常启动模式）
 */
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
            return false; /* 一次只报第一个错，避免刷屏 */                     \
        }                                                                      \
    } while (0)

    /* 1) 重复占用 */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (used[i].pin == used[j].pin) {
                FAIL("引脚 GPIO%d 被「%s」和「%s」同时占用",
                     used[i].pin, used[i].role, used[j].role);
            }
        }
    }

    /* 2) 越界 / 保留脚 */
    for (int i = 0; i < n; i++) {
        int p = used[i].pin;
        if (p < 0 || p > BOARD_GPIO_MAX) {
            FAIL("GPIO%d（%s）超出 ESP32-S3 的 GPIO0~GPIO%d 范围",
                 p, used[i].role, BOARD_GPIO_MAX);
        }
        for (size_t r = 0; r < sizeof(reserved) / sizeof(reserved[0]); r++) {
            if (reserved[r].gpio == p) {
                FAIL("GPIO%d（%s）被 %s 占用，不能用",
                     p, used[i].role, reserved[r].why);
            }
        }
    }

    /* 3) strapping 脚被当输出用 */
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
        /* 输入角色允许（内部上拉 + 开关对地，空闲就是高电平）；输出角色不允许 */
        bool is_output = (strstr(used[i].role, "电机") != NULL) ||
                         (strstr(used[i].role, "电磁离合") != NULL) ||
                         (strstr(used[i].role, "LED") != NULL);
        if (is_output) {
            FAIL("GPIO%d（%s）是 strapping 启动模式脚，不能当输出用！"
                 "上电瞬间会改掉启动电平，芯片可能进不了正常模式"
                 "（现象：反复重启、网页打不开、电机一直叫）",
                 p, used[i].role);
        }
    }
#undef FAIL

    if (ok && err && errlen) {
        err[0] = '\0';
    }
    return ok;
}

/**
 * 把完整接线表写进 buf（多行文本），用于上电打印到日志。
 * 返回写入的字符数。
 */
static inline int board_pins_describe(char *buf, size_t buflen)
{
    static const int clutch[BOARD_CHANNEL_COUNT] = BOARD_PIN_CLUTCH;
    static const int s_stop[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_STOP;
    static const int s_start[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_START;
    static const int s_auto[BOARD_CHANNEL_COUNT] = BOARD_PIN_SENSOR_AUTOLOAD;
    const board_spare_pin_t spare[] = BOARD_SPARE_PINS;
    int off = 0;

#define P(...)                                                                 \
    do {                                                                       \
        if (off < (int)buflen - 1) {                                           \
            int _w = snprintf(buf + off, buflen - off, __VA_ARGS__);           \
            if (_w > 0) { off += _w; }                                         \
        }                                                                      \
    } while (0)

    P("---- 硬件接线表（ESP32-S3 / 42 针）----\n");
    P("共享电机 : IN1=GPIO%d  IN2=GPIO%d\n", BOARD_PIN_MOTOR_IN1,
      BOARD_PIN_MOTOR_IN2);
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        P("料盘位%d  : 离合=GPIO%d | 停止=GPIO%d 开始=GPIO%d 自吸=GPIO%d\n",
          i + 1, clutch[i], s_stop[i], s_start[i], s_auto[i]);
    }
    if (BOARD_EXTRUDER_INPLACE_UNUSED) {
        P("挤出机到位: 不接线（改用打印机 MQTT 事件）\n");
    } else {
        P("挤出机到位: GPIO%d\n", BOARD_PIN_EXTRUDER_INPLACE);
    }
    if (BOARD_PIN_LED >= 0) {
        P("状态 LED : GPIO%d\n", BOARD_PIN_LED);
    } else {
        P("状态 LED : 未使用\n");
    }
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

#endif /* BOARD_PINS_S3_H */
