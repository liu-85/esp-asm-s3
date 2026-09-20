/**
 * bambu_proto.h —— 拓竹打印机 MQTT 报文协议
 * ============================================================================
 *
 * 对应 MicroPython 版的 bambu/get_event_info.py + bambu/bambu_const.py
 * + bambu/bambu_commands.py，三块合成一个模块。
 *
 * 单独抽出来放的原因：mqtt_client.c 只管"连接 / 收发 / 重连"，报文怎么解析、
 * 命令长什么样，全都归这里。换打印机固件版本时只需要动这一个文件。
 *
 * ----------------------------------------------------------------------------
 * 一、换料是怎么被触发的（这是本项目最核心的一个判定）
 * ----------------------------------------------------------------------------
 *   打印机要换料时，会把自己的状态置成：
 *
 *       gcode_state   = "PAUSE"
 *       mc_percent    = 101          ← 101 是拓竹约定的"等 AMS 换料"标记
 *
 *   所以判定条件就是 `gcode_state == "PAUSE" && mc_percent == 101`。
 *
 *   目标通道号（**0 起**，用的时候要注意 +1）：G-code 换料宏里的
 *   M73 P101 R[next_extruder] 的 R 参数。不同固件版本把 R 参数回传到
 *   report 里用的字段名不一样（filament_next / mc_next_tray 等），
 *   解析时按候选字段探测，都没找到才退回 mc_remaining_time 兜底。
 *   ⚠️ mc_remaining_time 名义上是"剩余打印时间（分钟）"，不是通道号，
 *   旧版曾拿它当通道号用，4 通道自动换料几乎永远不触发。
 *
 *   这个约定来自上游 YBA-AMS，本项目沿用了它 —— 改之前先确认打印机会不会
 *   在某些固件版本里用别的值表达同一件事。
 *
 * ----------------------------------------------------------------------------
 * 二、命令（发出去的那些 JSON）
 * ----------------------------------------------------------------------------
 *   全部在文件末尾的 bambu_cmd_* 函数里，返回值是要发送的字节数。
 *   和 Python 版的 bambu_commands.py 一一对应，连 sequence_id 都保持一致。
 */

#ifndef BAMBU_PROTO_H
#define BAMBU_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 报文解析结果
 * ========================================================================== */

/** HMS 错误条目（最多同时留这么多条，够显示又不会撑爆栈） */
#define BAMBU_HMS_MAX      4
#define BAMBU_HMS_CODE_LEN 24
#define BAMBU_HMS_TEXT_LEN 72

typedef struct {
    char code[BAMBU_HMS_CODE_LEN];   /**< 形如 "0300_4000_0002_0001" */
    char text[BAMBU_HMS_TEXT_LEN];   /**< 中文说明；查不到表时是 "未知错误" */
} bambu_hms_t;

/** 一次推送解析出来的全部信息 */
typedef struct {
    /* ---- 换料判定 ---- */
    bool    change_needed;      /**< 需要 AMS 执行换料 */
    int     filament_next;      /**< 目标通道号，**0 起**；不需要换料时 -1 */
    int     target_color;       /**< 目标颜色 0xRRGGBB；-1 = 报文里没有 */

    /* ---- 打印机状态 ---- */
    int     gcode_state;        /**< 0=空闲 1=准备 2=打印中 3=暂停 ... */
    int     stg_cur;            /**< 当前阶段号，查表得文字 */
    const char *stage_text;     /**< 阶段中文说明（指向静态表，不要 free） */
    int     mc_percent;         /**< 打印进度百分比 */
    int     mc_remaining_time;  /**< 剩余时间（分钟） */
    int     print_error;        /**< 打印错误码，0 = 无 */
    int     ams_stage;          /**< ams.stage，1 = 等待 AMS 完成换料；-1 = 无此字段 */
    bool    is_paused;
    bool    is_printing;

    /* ---- 温度 ---- */
    float   nozzle_temper;
    float   nozzle_target;
    float   bed_temper;
    float   bed_target;

    /* ---- 错误 ---- */
    int     hms_count;
    bambu_hms_t hms[BAMBU_HMS_MAX];

    /* ---- 挤出机到位提示 ----
     * 字段名：print.hw_switch_state（ha-bambulab 确认）
     * 值域 0/1/2/3：0 = 无耗材，非零 = 有耗材
     * 1 = 打印机说耗材已到挤出机
     * 0 = 打印机说没到
     * -1 = 报文里没有这个信息（字段名对不上或该固件不上报）
     * 详见 bambu_proto.c probe_extruder_inplace() 的说明。 */
    int     extruder_inplace_hint;
} bambu_report_t;

/* ==========================================================================
 * 解析
 * ========================================================================== */

/**
 * 解析一条 report 报文（完整 JSON 文本，可以带结尾 '\0' 也可以不带）。
 *
 * @return ESP_OK 解析成功；ESP_ERR_INVALID_ARG 不是合法 JSON 或没有 "print" 段
 *
 * 注意：解析失败**不是异常情况** —— 打印机偶尔会发心跳类报文，里面没有
 * "print"，这时返回错误是正常的，调用方忽略即可。
 */
esp_err_t bambu_proto_parse(const char *json, size_t len, bambu_report_t *out);

/** 把 stg_cur 翻成中文（查内置表，返回静态字符串，永远不返回 NULL） */
const char *bambu_stage_text(int stg_cur);

/** 把 HMS 码翻成中文（查内置表，查不到返回 "未知错误"） */
const char *bambu_hms_text(const char *code);

/** 把 attr/code 两个整数拼成 HMS 码字符串，形如 "0300_4000_0002_0001" */
int bambu_hms_format(int attr, int code, char *buf, size_t buflen);

/* ==========================================================================
 * 命令构造（对应 bambu_commands.py）
 * ========================================================================== */
/* 全部返回"写进 buf 的字节数"（不含结尾 '\0'），buf 不够大会返回 -1。 */

/** 让打印机继续打印（换料完成后发） */
int bambu_cmd_resume(char *buf, size_t buflen);

/** 查询全部状态 —— 老代码里的 banbu_start（pushall） */
int bambu_cmd_pushall(char *buf, size_t buflen);

/** 开始定时推送 —— 老代码里的 START_PUSH */
int bambu_cmd_push_start(char *buf, size_t buflen);

/** 发一条 G-code（外层套 gcode_line 命令） */
int bambu_cmd_gcode(char *buf, size_t buflen, const char *gcode);

#ifdef __cplusplus
}
#endif

#endif /* BAMBU_PROTO_H */
