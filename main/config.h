/**
 * config.h —— 持久化配置（NVS）
 * ============================================================================
 *
 * 对应 MicroPython 版的 config.json + wifi.dat，换成 ESP-IDF 的 NVS 存储。
 *
 * 为什么换：
 *   · NVS 有掉电保护 —— 写一半掉电不会像 config.json 那样留下半个坏 JSON
 *     （Python 版为此专门加了 ValueError 兜底，这里从根上不需要）
 *   · 没有文件系统，就没有"挂载失败"这一类启动失败
 *   · 自带磨损均衡，频繁改 jog_ms / 当前料盘的场景更安全
 *
 * 存的东西（一个结构体整块存，带 magic + version 校验）：
 *     WiFi 账号密码（最多 3 组，最近用的排前面）
 *     配置热点自身的 SSID / 密码
 *     打印机 MQTT 参数（IP / 序列号 / 访问码 / 用户名 / 端口 / 客户端名）
 *     通道映射 access_list（打印机通道号 → 物理料盘位）
 *     通道颜色 color_list
 *     当前料盘 filament_current
 *     手动点动时长 jog_ms
 *     自吸流程的两个参数（蠕动次数 / 单次脉冲时长）
 *     微动安装掩码 sensor_enabled_mask（没装微动的通道自动走降级模式）
 *
 * ⚠️ 结构体改字段时**必须**把 CONFIG_VERSION 加一，否则旧 NVS 里的老结构
 *    会被按新结构解析，字段错位。加了版本号之后旧数据会被自动丢弃并回到默认值。
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 常量
 * ========================================================================== */
#define CONFIG_NAMESPACE       "ams"
#define CONFIG_KEY             "cfg"
#define CONFIG_VERSION         2u
#define CONFIG_MAGIC           0x414D5331u  /* "AMS1" */

#define CONFIG_WIFI_MAX_PROFILES 3
#define CONFIG_SSID_MAX          33   /* 含结尾 '\0' */
#define CONFIG_PASS_MAX          65   /* 含结尾 '\0' */
#define CONFIG_HOST_MAX          64
#define CONFIG_SERIAL_MAX        32
#define CONFIG_CLIENTID_MAX      32

/* 默认配置热点 —— 和 MicroPython 版保持一致，老用户不用重新记 */
#define CONFIG_DEFAULT_AP_SSID     "AMS_WIFI"
#define CONFIG_DEFAULT_AP_PASS     "A12345678"
#define CONFIG_DEFAULT_AP_CHANNEL  6

/* 手动点动时长的安全区间（毫秒）。网页填超范围会被夹住。
 *   下限：再短离合还没咬合就停了，没意义
 *   上限：防止手滑把关在机构里的料顶坏
 */
#define CONFIG_JOG_MIN_MS 200
#define CONFIG_JOG_MAX_MS 60000
#define CONFIG_JOG_DEF_MS 1000

/* 自吸流程收尾的"蠕动送料"参数
 *   挤出机到位信号来了之后，慢速短脉冲推 CREEP_TIMES 次，确保耗材被挤出机
 *   齿轮真正咬住，而不是虚顶在入口。
 */
#define CONFIG_CREEP_TIMES_DEF     3     /* 你要求的是三次 */
#define CONFIG_CREEP_TIMES_MIN     1
#define CONFIG_CREEP_TIMES_MAX     10
#define CONFIG_CREEP_PULSE_MS_DEF  400   /* 单次脉冲时长 */
#define CONFIG_CREEP_PULSE_MIN     100
#define CONFIG_CREEP_PULSE_MAX     3000
#define CONFIG_CREEP_SPEED_PCT_DEF 45    /* 蠕动速度（PWM 占空比百分比） */

/* 辅助送料参数（换色时同步推料帮打印机咬住新料） */
#define CONFIG_ASSIST_PCT_DEF      50   /* 辅助送料 PWM 占空比百分比 */
#define CONFIG_ASSIST_PCT_MIN      5
#define CONFIG_ASSIST_PCT_MAX      100

/* 退料参数（C3 无微动降级模式） */
#define CONFIG_RETRACT_WAIT_MS_DEF  5000  /* 蠕动退料后等待挤出机 MQTT 信号的最长时间 */
#define CONFIG_RETRACT_WAIT_MIN     1000
#define CONFIG_RETRACT_WAIT_MAX     15000
#define CONFIG_RETRACT_CONT_MS_DEF  5000  /* 收到"没料"信号后连续退料时长 */
#define CONFIG_RETRACT_CONT_MIN     1000
#define CONFIG_RETRACT_CONT_MAX     15000

/* 换料温度（每个料盘位一个，单位 ℃）
 *   退料前把热端升到这个温度：太低，热端里的料是硬的，退不出来；
 *   太高，PLA 之类会烤糊、拉丝。
 *   按材料给：PLA 220 / PETG 250 / ABS 260 / TPU 230 左右。
 *   没配过（NVS 里是 0）时用默认值 —— 这样旧固件升上来直接可用。
 */
#define CONFIG_TEMPER_DEF           250
#define CONFIG_TEMPER_MIN           150   /* 低于这个温度没有材料能退出来 */
#define CONFIG_TEMPER_MAX           300   /* 再高就超过 A1 热端上限了 */

/* ==========================================================================
 * 数据类型
 * ========================================================================== */

/** 一组保存过的 WiFi */
typedef struct {
    char ssid[CONFIG_SSID_MAX];
    char pass[CONFIG_PASS_MAX];
} config_wifi_profile_t;

/** 「挤出机到位信号」从哪里来 */
typedef enum {
    EXTRUDER_SRC_GPIO = 0,  /**< 走 GPIO1 那根线（默认） */
    EXTRUDER_SRC_MQTT = 1,  /**< 走打印机上报的 MQTT 事件 */
} config_extruder_src_t;

/** 完整配置。整块存进 NVS，所以字段顺序不能随便调。 */
typedef struct {
    uint32_t magic;
    uint32_t version;

    /* ---- WiFi ---- */
    config_wifi_profile_t profiles[CONFIG_WIFI_MAX_PROFILES];
    uint8_t  profile_count;                 /* 有效项个数，0 表示没配过 */
    char     ap_ssid[CONFIG_SSID_MAX];
    char     ap_pass[CONFIG_PASS_MAX];
    uint8_t  ap_channel;

    /* ---- 打印机 MQTT ---- */
    char     mqtt_host[CONFIG_HOST_MAX];
    char     mqtt_serial[CONFIG_SERIAL_MAX];
    char     mqtt_user[CONFIG_SERIAL_MAX];
    char     mqtt_pass[CONFIG_PASS_MAX];
    char     mqtt_client_id[CONFIG_CLIENTID_MAX];
    uint16_t mqtt_port;

    /* ---- 业务 ---- */
    uint8_t  access_list[BOARD_CHANNEL_COUNT];   /* 物理料盘位 -> 打印机通道号 */
    uint32_t color_list[BOARD_CHANNEL_COUNT];    /* 0xRRGGBB */
    int8_t   filament_current;                   /* 打印机通道号，0 = 未知 */

    /* ---- 动作参数（网页可改） ---- */
    uint16_t jog_ms;
    uint8_t  creep_times;
    uint16_t creep_pulse_ms;
    uint8_t  creep_speed_pct;
    uint8_t  extruder_src;                       /* config_extruder_src_t */

    /* ---- 辅助送料（换色时同步推料帮打印机咬住新料） ---- */
    uint8_t  assist_enabled;                     /* 0=关 1=开（默认开） */
    uint8_t  assist_speed_pct;                   /* PWM 占空比百分比 */

    /* ---- 退料参数（C3 无微动降级模式） ---- */
    uint16_t retract_wait_ms;                    /* 蠕动退料后等 MQTT 信号的最长时间 */
    uint16_t retract_cont_ms;                    /* 收到"没料"信号后连续退料时长 */

    /* ---- 微动安装情况：bit i = 1 表示料盘位 i+1 装了微动组 ---- */
    uint8_t  sensor_enabled_mask;

    /* ---- 换料温度：每个料盘位一个（℃），0 = 用 CONFIG_TEMPER_DEF ----
     * 放在原来 reserved 的位置，**结构体总长不变**，所以 NVS 里存的老
     * 数据读进来不会错位 —— 这是特意不升 CONFIG_VERSION 的原因：
     * 升版本号会把用户的 WiFi / MQTT / 通道映射全部清掉，而这里完全
     * 没必要。老固件里 reserved 恒为 0（只有 memset 0，没有别的写入），
     * 因此旧配置读出来 temper 全是 0，自动落回默认值，行为不变。
     * ⚠️ 以后动这个结构体，如果**改变了总长度**，那就必须升版本号。 */
    uint16_t temper[BOARD_CHANNEL_COUNT];
    uint8_t  reserved[16 - BOARD_CHANNEL_COUNT * 2];
} ams_config_t;

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

/**
 * 初始化配置模块：挂载 NVS，读出配置；读不到或版本不符就写入一份默认值。
 *
 * 必须在所有其它模块之前调用（wifi_manager / mqtt_client / web_server 都要读它）。
 * 返回 ESP_OK 表示至少拿到了一份可用的默认配置；NVS 彻底坏掉时返回错误，
 * 但**配置对象依然可用**（内容是默认值），调用方可以选择继续跑。
 */
esp_err_t config_init(void);

/** 取配置对象（只读指针，全局唯一，永不为 NULL） */
ams_config_t *config_get(void);

/** 把当前配置写回 NVS。改完任何字段都要调一次。 */
esp_err_t config_save(void);

/** 恢复出厂：字段全部回到默认值并写回 NVS（不动 WiFi 记录，避免被困在配网模式） */
esp_err_t config_reset_business(void);

/* ==========================================================================
 * 便捷访问器 —— 业务代码用这些，不要直接摸结构体字段
 * ========================================================================== */

/** 打印机参数是否齐全（IP / 序列号 / 访问码三项都有） */
bool config_mqtt_ready(void);

/** 当前料盘（打印机通道号，0 = 未知） */
int config_get_filament_current(void);
void config_set_filament_current(int printer_channel);

/** 手动点动时长（毫秒，已夹到安全区间） */
uint16_t config_get_jog_ms(void);

/**
 * 设置手动点动时长。value 单位毫秒，会被夹到 [CONFIG_JOG_MIN_MS, CONFIG_JOG_MAX_MS]。
 * 返回**实际生效**的值（可能和传入值不同），网页据此显示准确提示。
 */
uint16_t config_set_jog_ms(int value);

/** 某一路是否装了微动组 */
bool config_sensor_enabled(int channel_index);

/** 物理料盘位（0 起）对应哪个打印机通道号（1 起）；未映射时返回 -1 */
int config_printer_channel_of(int material_index);

/** 打印机通道号（1 起）对应哪个物理料盘位（0 起）；未映射时返回 -1 */
int config_material_index_of(int printer_channel);

/** 写一组 WiFi 并把它排到最前面 */
esp_err_t config_set_wifi(const char *ssid, const char *pass);

/** 改配置热点的 SSID / 密码（password 传 NULL 或 "" 表示保持原值） */
esp_err_t config_set_ap(const char *ssid, const char *pass);

/** 改打印机 MQTT 参数（任何一项传 NULL 表示保持原值） */
esp_err_t config_set_mqtt(const char *host, const char *serial,
                          const char *user, const char *pass,
                          int port, const char *client_id);

/** 改通道映射与颜色（两个数组都是 BOARD_CHANNEL_COUNT 长） */
esp_err_t config_set_access(const uint8_t *access_list,
                            const uint32_t *color_list);

/**
 * 取某个物理料盘位（0 起）的颜色（0xRRGGBB）。
 * 用于 4 通道自动颜色匹配：拿切片器的目标颜色和所有通道做距离比较。
 */
uint32_t config_get_color(int material_index);

/**
 * 自动颜色匹配：从 4 个通道里挑出和 target_rgb 最接近的料盘位。
 *
 * 距离度量用感知加权欧氏距离（R×0.30 + G×0.59 + B×0.11 的加权平方和），
 * 比朴素 RGB 距离更贴近人眼对色差的主观感受。
 *
 * @param target_rgb  目标颜色 0xRRGGBB
 * @param out_material 输出：最接近通道的物理料盘位下标（0 起）
 * @return 距离值（越小越接近），UINT32_MAX 表示所有通道都是 0（未配颜色）
 */
uint32_t config_color_match(uint32_t target_rgb, int *out_material);

/** 把配置整理成一段人类可读的文本，打日志用 */
int config_describe(char *buf, size_t buflen);

/** 辅助送料 PWM 占空比百分比（已夹到安全区间） */
uint8_t config_get_assist_speed_pct(void);
void config_set_assist_speed_pct(uint8_t pct);

/** 辅助送料开关（0=关 1=开） */
uint8_t config_get_assist_enabled(void);
void config_set_assist_enabled(uint8_t on);

/** 退料参数获取/设置（已夹到安全区间） */
uint16_t config_get_retract_wait_ms(void);
uint16_t config_set_retract_wait_ms(int value);
uint16_t config_get_retract_cont_ms(void);
uint16_t config_set_retract_cont_ms(int value);

/** 某料盘位的换料温度（℃）。参数越界或没配过 → CONFIG_TEMPER_DEF */
int config_get_temper(int material_index);
/** 设置某料盘位的换料温度（℃），自动夹到 [MIN, MAX] 并写回 NVS */
void config_set_temper(int material_index, int value);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
