/**
 * config.c —— 持久化配置的实现
 * ============================================================================
 *
 * 整块结构体存进 NVS 的一个 blob 里，附带 magic + version 校验：
 *   · magic 不对  → 这块数据不是我们写的（或被人为破坏了），丢掉用默认值
 *   · version 不对 → 结构体字段变过了，按新结构解析会错位，丢掉用默认值
 *
 * 这两种情况都**不会**报错退出 —— 直接把默认值写回去，让设备能正常起来。
 * 配置坏了导致不能开机是最糟糕的设计；用户宁可回到出厂设置，也不要一块砖。
 */

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "log_buffer.h"

/* ==========================================================================
 * 全局唯一实例
 * ==========================================================================
 * 只在 config_init() 里写一次，之后所有模块通过 config_get() 拿到指针**只读**。
 * 要改字段必须走下面那些 config_set_xxx()，它们会加锁再改，避免两个任务
 * （比如 web 任务和 ams 任务）同时改同一个字段而互相覆盖。
 */
static ams_config_t     s_cfg;
static bool             s_inited;
static SemaphoreHandle_t s_lock;

/* ==========================================================================
 * 默认值
 * ========================================================================== */

/** 出厂默认配色（按 4 通道排，不足时按 i < 4 取，多余的留 0） */
static const uint32_t DEFAULT_COLORS[4] = {
    0xE53935u,  /* 红 */
    0x43A047u,  /* 绿 */
    0x1E88E5u,  /* 蓝 */
    0xFDD835u,  /* 黄 */
};

static void config_load_defaults(ams_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic   = CONFIG_MAGIC;
    c->version = CONFIG_VERSION;

    /* ---- WiFi：一组都没有，等用户配网 ---- */
    c->profile_count = 0;

    /* ---- 配置热点：沿用老名字，老用户不用重新记 ---- */
    strncpy(c->ap_ssid, CONFIG_DEFAULT_AP_SSID, sizeof(c->ap_ssid) - 1);
    strncpy(c->ap_pass, CONFIG_DEFAULT_AP_PASS, sizeof(c->ap_pass) - 1);
    c->ap_channel = CONFIG_DEFAULT_AP_CHANNEL;

    /* ---- 打印机 MQTT：空的，等用户填 ---- */
    strncpy(c->mqtt_user, "bblp", sizeof(c->mqtt_user) - 1);
    strncpy(c->mqtt_client_id, "mqttx_3c73cd31",
            sizeof(c->mqtt_client_id) - 1);
    c->mqtt_port = 8883;

    /* ---- 通道映射：物理料盘位 1/2/3/4 对应打印机通道 1/2/3/4 ---- */
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        c->access_list[i] = (uint8_t)(i + 1);
        c->color_list[i]  = (i < 4) ? DEFAULT_COLORS[i] : 0;
    }
    c->filament_current = 0;    /* 0 = 未知 */

    /* ---- 动作参数 ---- */
    c->jog_ms          = CONFIG_JOG_DEF_MS;
    c->jog_speed_pct   = CONFIG_JOG_SPEED_DEF;   /* 手动点动全速 */
    c->creep_times     = CONFIG_CREEP_TIMES_DEF;
    c->creep_pulse_ms  = CONFIG_CREEP_PULSE_MS_DEF;
    c->creep_speed_pct = CONFIG_CREEP_SPEED_PCT_DEF;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    /* C3 没有挤出机到位 GPIO 线，默认走 MQTT 事件（hw_switch_state） */
    c->extruder_src    = EXTRUDER_SRC_MQTT;
#else
    c->extruder_src    = EXTRUDER_SRC_GPIO;
#endif

    /* ---- 辅助送料参数 ---- */
    c->assist_enabled   = 1;   /* 默认开启 */
    c->assist_speed_pct = CONFIG_ASSIST_PCT_DEF;
    c->assist_ms        = CONFIG_ASSIST_MS_DEF;
    c->assist_hold      = 1;   /* 默认"阶段内保持离合吸合、只脉冲电机" */

    /* ---- 退料参数（C3 无微动降级模式） ---- */
    c->retract_wait_ms   = CONFIG_RETRACT_WAIT_MS_DEF;
    c->retract_cont_ms   = CONFIG_RETRACT_CONT_MS_DEF;
    c->retract_creep_ms  = CONFIG_RETRACT_CREEP_MS_DEF;
    c->retract_gap_ms    = CONFIG_RETRACT_GAP_MS_DEF;
    c->retract_creep_max = CONFIG_RETRACT_CREEP_MAX_DEF;

    /* ★ 微动默认**全部视为未安装**，程序走降级模式（按时间推进送料）。
     *   原因：没接微动却以为接了，程序会一直等一个永远不来的信号 —— 表现为
     *   "推料推不动、每步都超时"，比按时间推进更难排查。
     *   接好微动后：网页「硬件调试」里勾上，或者改这里的位掩码。
     *   bit0=料盘位1，bit1=料盘位2，bit2=料盘位3，bit3=料盘位4 */
    c->sensor_enabled_mask = 0x00;

    memset(c->reserved, 0, sizeof(c->reserved));
}

/* ==========================================================================
 * 一次性配置补丁
 * ==========================================================================
 * 为什么要有这一套：本次新增的字段是从 reserved 里抠出来的（见 config.h），
 * 老固件写下的是 0。要靠一个独立的水位线键才能区分"老配置没设置过"和
 * "用户就是选了 0"。补丁只跑一次，之后完全信任存下来的值。
 */
static void config_mark_patched(void)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, CONFIG_KEY_PATCH, CONFIG_PATCH_LEVEL) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/**
 * 补丁 1（2026-09-22）：把明显"带不动电机"的占空比提上来 + 补齐新增时长字段。
 *
 * 依据：真机同一轮换色里，45% 占空比的蠕动退料跑满 12 轮 × 2000ms
 * （合计 24 秒）一根料都没拉动，电机动静都没有；紧接着 100% 的连续退料
 * 5 秒就把料拉出了挤出机（打印机 hw_switch_state 同一秒 1 → 0）。
 * 所以低于 60% 的值属于"怎么调都没用"，直接按新默认值处理。
 * 用户自己调高过的值一律保留，不动。
 *
 * @return true = 改了配置（调用方负责 config_save() 落盘）
 */
static bool config_apply_patch_once(void)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t lvl = 0;
    esp_err_t err = nvs_get_u8(h, CONFIG_KEY_PATCH, &lvl);
    nvs_close(h);

    if (err == ESP_OK && lvl >= CONFIG_PATCH_LEVEL) {
        return false;
    }

    bool changed = false;

    if (s_cfg.creep_speed_pct < CONFIG_PWM_FUTILE_PCT) {
        ams_log_warn("配置补丁1：蠕动速度 %u%% 带不动本机构的电机"
                     "（实测低于 %u%% 只会堵转、听不到声），改按默认 %u%%",
                     (unsigned)s_cfg.creep_speed_pct,
                     (unsigned)CONFIG_PWM_FUTILE_PCT,
                     (unsigned)CONFIG_CREEP_SPEED_PCT_DEF);
        s_cfg.creep_speed_pct = CONFIG_CREEP_SPEED_PCT_DEF;
        changed = true;
    }
    if (s_cfg.assist_speed_pct < CONFIG_PWM_FUTILE_PCT) {
        ams_log_warn("配置补丁1：辅助送料速度 %u%% 带不动电机，改按默认 %u%%",
                     (unsigned)s_cfg.assist_speed_pct,
                     (unsigned)CONFIG_ASSIST_PCT_DEF);
        s_cfg.assist_speed_pct = CONFIG_ASSIST_PCT_DEF;
        changed = true;
    }
    if (s_cfg.retract_creep_ms == 0) {
        s_cfg.retract_creep_ms = CONFIG_RETRACT_CREEP_MS_DEF;
        changed = true;
    }
    if (s_cfg.retract_gap_ms == 0) {
        s_cfg.retract_gap_ms = CONFIG_RETRACT_GAP_MS_DEF;
        changed = true;
    }
    if (s_cfg.retract_creep_max == 0) {
        s_cfg.retract_creep_max = CONFIG_RETRACT_CREEP_MAX_DEF;
        changed = true;
    }
    if (s_cfg.assist_ms == 0) {
        s_cfg.assist_ms = CONFIG_ASSIST_MS_DEF;
        changed = true;
    }

    /* ---- 补丁 2（2026-09-23）：新增字段，老配置里那两字节恒为 0 ----
     *
     * 新加的两个字段是塞在结构体尾部填充里的（见 config.h 的说明），
     * 老固件从来没写过它们 → 读出来都是 0。而这两个 0 的含义并不一样：
     *
     *   · jog_speed_pct = 0 → config_get_jog_speed_pct() 把它当"没设置过"，
     *     返回默认全速 100%。**不用在这里动**，保持 0 反而能把"老配置"
     *     这个事实留在数据里（用户真去调过之后才会变成 5~100）。
     *
     *   · assist_hold   = 0 → 会被当成"关闭保持吸合"。可现场的要求是**开启**
     *     （阶段内离合全程吸着、只脉冲电机，别每送一次就吸合-断开折腾机械）。
     *     所以这里显式置 1。
     *
     * ⚠️ 只在打补丁这一次做。补丁水位线写下之后完全信任 NVS —— 用户以后
     *    在网页上把"保持吸合"关掉，重启也不会被这里改回来。
     *    （`lvl < 2` 而不是 `lvl < CONFIG_PATCH_LEVEL`：以后加补丁 3 时，
     *     这段不会被重复执行，只做它该做的那一次。） */
    if (lvl < 2) {
        s_cfg.assist_hold = 1;
        changed = true;
        ams_log("配置补丁2：启用「辅助送料阶段内保持离合吸合」（只脉冲电机）");
    }

    config_mark_patched();
    if (changed) {
        ams_log("配置补丁1 已应用：蠕动 %ums + 间隔 %ums（%u 轮）+ 连续 %ums，"
                "辅助送料 %ums @ %u%%",
                (unsigned)s_cfg.retract_creep_ms,
                (unsigned)s_cfg.retract_gap_ms,
                (unsigned)s_cfg.retract_creep_max,
                (unsigned)s_cfg.retract_cont_ms,
                (unsigned)s_cfg.assist_ms,
                (unsigned)s_cfg.assist_speed_pct);
    } else {
        ams_log("配置补丁1 无需改动（水位线已写入）");
    }
    return changed;
}

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

esp_err_t config_init(void)
{
    if (!s_inited) {
        s_lock = xSemaphoreCreateMutex();
        s_inited = true;
    }

    /* NVS 本身的初始化放在 main.c 里（那里要给"分区被改过"做 erase 重试），
     * 这里只负责打开命名空间、读写配置。 */
    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ams_log_err("打开 NVS 命名空间失败(%s)，本次运行使用默认配置（改动不会被保存）",
                    esp_err_to_name(err));
        config_load_defaults(&s_cfg);
        return err;
    }

    size_t len = sizeof(s_cfg);
    memset(&s_cfg, 0, sizeof(s_cfg));
    err = nvs_get_blob(h, CONFIG_KEY, &s_cfg, &len);
    if (err != ESP_OK || len != sizeof(s_cfg)) {
        /* 第一次开机 / 数据被换过 */
        ams_log("没有已保存的配置（%s），写入一份默认值",
                err == ESP_ERR_NVS_NOT_FOUND ? "首次开机" : esp_err_to_name(err));
        config_load_defaults(&s_cfg);
        nvs_set_blob(h, CONFIG_KEY, &s_cfg, sizeof(s_cfg));
        nvs_commit(h);
        nvs_close(h);
        return ESP_OK;
    }

    if (s_cfg.magic != CONFIG_MAGIC || s_cfg.version != CONFIG_VERSION) {
        ams_log_warn("配置格式已变（magic=0x%08X version=%u），恢复默认值",
                     (unsigned)s_cfg.magic, (unsigned)s_cfg.version);
        config_load_defaults(&s_cfg);
        nvs_set_blob(h, CONFIG_KEY, &s_cfg, sizeof(s_cfg));
        nvs_commit(h);
        nvs_close(h);
        return ESP_OK;
    }

    nvs_close(h);

    /* ---- 加载完做一次范围修正：手工改过 NVS 或从老版本升上来时兜底 ---- */
    if (s_cfg.profile_count > CONFIG_WIFI_MAX_PROFILES) {
        s_cfg.profile_count = CONFIG_WIFI_MAX_PROFILES;
    }
    if (s_cfg.jog_ms < CONFIG_JOG_MIN_MS || s_cfg.jog_ms > CONFIG_JOG_MAX_MS) {
        s_cfg.jog_ms = CONFIG_JOG_DEF_MS;
    }
    if (s_cfg.creep_times < CONFIG_CREEP_TIMES_MIN ||
        s_cfg.creep_times > CONFIG_CREEP_TIMES_MAX) {
        s_cfg.creep_times = CONFIG_CREEP_TIMES_DEF;
    }
    if (s_cfg.creep_pulse_ms < CONFIG_CREEP_PULSE_MIN ||
        s_cfg.creep_pulse_ms > CONFIG_CREEP_PULSE_MAX) {
        s_cfg.creep_pulse_ms = CONFIG_CREEP_PULSE_MS_DEF;
    }
    if (s_cfg.creep_speed_pct == 0 || s_cfg.creep_speed_pct > 100) {
        s_cfg.creep_speed_pct = CONFIG_CREEP_SPEED_PCT_DEF;
    }
    /* 辅助送料参数校验 */
    if (s_cfg.assist_speed_pct < CONFIG_ASSIST_PCT_MIN ||
        s_cfg.assist_speed_pct > CONFIG_ASSIST_PCT_MAX) {
        s_cfg.assist_speed_pct = CONFIG_ASSIST_PCT_DEF;
    }
    /* 退料参数校验 */
    if (s_cfg.retract_wait_ms < CONFIG_RETRACT_WAIT_MIN ||
        s_cfg.retract_wait_ms > CONFIG_RETRACT_WAIT_MAX) {
        s_cfg.retract_wait_ms = CONFIG_RETRACT_WAIT_MS_DEF;
    }
    if (s_cfg.retract_cont_ms < CONFIG_RETRACT_CONT_MIN ||
        s_cfg.retract_cont_ms > CONFIG_RETRACT_CONT_MAX) {
        s_cfg.retract_cont_ms = CONFIG_RETRACT_CONT_MS_DEF;
    }
    /* ---- 本次新增的时长参数 ----
     * 下限判断天然把"老配置留下的 0"兜成默认值（这几个字段有效值都 > 0）。
     * 唯一的例外是 retract_creep_max：0 是合法值（= 不蠕动），所以这里只能
     * 判上限，0 该不该变默认由 config_apply_patch_once() 那个一次性补丁决定。 */
    if (s_cfg.retract_creep_ms < CONFIG_RETRACT_CREEP_MS_MIN ||
        s_cfg.retract_creep_ms > CONFIG_RETRACT_CREEP_MS_MAX) {
        s_cfg.retract_creep_ms = CONFIG_RETRACT_CREEP_MS_DEF;
    }
    if (s_cfg.retract_gap_ms < CONFIG_RETRACT_GAP_MS_MIN ||
        s_cfg.retract_gap_ms > CONFIG_RETRACT_GAP_MS_MAX) {
        s_cfg.retract_gap_ms = CONFIG_RETRACT_GAP_MS_DEF;
    }
    if (s_cfg.retract_creep_max > CONFIG_RETRACT_CREEP_MAX_MAX) {
        s_cfg.retract_creep_max = CONFIG_RETRACT_CREEP_MAX_DEF;
    }
    if (s_cfg.assist_ms < CONFIG_ASSIST_MS_MIN ||
        s_cfg.assist_ms > CONFIG_ASSIST_MS_MAX) {
        s_cfg.assist_ms = CONFIG_ASSIST_MS_DEF;
    }
    /* ---- 2026-09-23 新增的两个字段 ----
     * jog_speed_pct：0 是合法含义（"没设置过"→ 默认全速），所以只判上限；
     *   越界（比如 NVS 被手改过）一律归 0，由 getter 落回默认值。
     * assist_hold：规范化成 0/1，避免 NVS 里的脏值被当布尔用。 */
    if (s_cfg.jog_speed_pct > CONFIG_JOG_SPEED_MAX) {
        s_cfg.jog_speed_pct = 0;
    }
    s_cfg.assist_hold = s_cfg.assist_hold ? 1 : 0;
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        if (s_cfg.access_list[i] == 0 ||
            s_cfg.access_list[i] > BOARD_CHANNEL_COUNT) {
            s_cfg.access_list[i] = (uint8_t)(i + 1);
        }
        if (s_cfg.color_list[i] == 0) {
            s_cfg.color_list[i] = DEFAULT_COLORS[i];
        }
    }

    ams_log("配置已加载：WiFi 记录 %u 组，打印机%s，微动掩码 0x%02X",
            (unsigned)s_cfg.profile_count,
            config_mqtt_ready() ? "已配置" : "未配置",
            (unsigned)s_cfg.sensor_enabled_mask);

    /* ---- 一次性配置补丁 ----
     * 放在校验之后：校验只保证"值在合法区间内"，它没法知道某个合法值
     * （比如 0 轮蠕动）到底是用户选的还是老配置留下的 0 —— 那是补丁的事。 */
    if (config_apply_patch_once()) {
        config_save();
    }

    return ESP_OK;
}

ams_config_t *config_get(void)
{
    return &s_cfg;
}

esp_err_t config_save(void)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, CONFIG_KEY, &s_cfg, sizeof(s_cfg));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ams_log_err("配置保存失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_reset_business(void)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    /* ★ 只重置业务参数，**保留 WiFi 记录与打印机参数**。
     *   反过来做的话，一旦重置就把用户困在配网模式里 —— 而配网本身又需要
     *   WiFi，容易变成"越重置越连不上"的死循环。 */
    config_wifi_profile_t keep_profiles[CONFIG_WIFI_MAX_PROFILES];
    uint8_t keep_count = s_cfg.profile_count;
    memcpy(keep_profiles, s_cfg.profiles, sizeof(keep_profiles));

    config_load_defaults(&s_cfg);
    memcpy(s_cfg.profiles, keep_profiles, sizeof(keep_profiles));
    s_cfg.profile_count = keep_count;

    xSemaphoreGive(s_lock);

    ams_log("业务参数已恢复默认（WiFi 记录保留）");
    return config_save();
}

/* ==========================================================================
 * 便捷访问器
 * ========================================================================== */

bool config_mqtt_ready(void)
{
    return (s_cfg.mqtt_host[0] != '\0') &&
           (s_cfg.mqtt_serial[0] != '\0') &&
           (s_cfg.mqtt_pass[0] != '\0');
}

int config_get_filament_current(void)
{
    return s_cfg.filament_current;
}

void config_set_filament_current(int printer_channel)
{
    if (printer_channel < 0 || printer_channel > BOARD_CHANNEL_COUNT) {
        printer_channel = 0;
    }
    s_cfg.filament_current = (int8_t)printer_channel;
    config_save();
}

uint16_t config_get_jog_ms(void)
{
    return s_cfg.jog_ms;
}

uint16_t config_set_jog_ms(int value)
{
    if (value < CONFIG_JOG_MIN_MS) {
        value = CONFIG_JOG_MIN_MS;
    }
    if (value > CONFIG_JOG_MAX_MS) {
        value = CONFIG_JOG_MAX_MS;
    }
    bool changed = (s_cfg.jog_ms != (uint16_t)value);
    s_cfg.jog_ms = (uint16_t)value;
    config_save();
    if (changed) {
        ams_log("进退响应时间已改为 %.1f 秒（%d 个通道统一生效）",
                value / 1000.0, BOARD_CHANNEL_COUNT);
    }
    return s_cfg.jog_ms;
}

/* ---- 手动点动 PWM ----
 *
 * 0 的含义是"没设置过"，只可能来自**老配置**：这个字段是老固件留下的
 * 尾部填充字节（见 config.h 的结构体说明），从没被写过，读出来就是 0。
 * 全新安装走 config_load_defaults()，那里直接写 CONFIG_JOG_SPEED_DEF(100)。
 * 所以 0 一律按默认值返回 —— 改动前点动就是全速，这样行为完全一致。 */
uint8_t config_get_jog_speed_pct(void)
{
    uint8_t v = s_cfg.jog_speed_pct;
    if (v == 0) {
        return (uint8_t)CONFIG_JOG_SPEED_DEF;
    }
    if (v < CONFIG_JOG_SPEED_MIN) {
        return (uint8_t)CONFIG_JOG_SPEED_MIN;
    }
    if (v > CONFIG_JOG_SPEED_MAX) {
        return (uint8_t)CONFIG_JOG_SPEED_MAX;
    }
    return v;
}

uint8_t config_set_jog_speed_pct(int pct)
{
    if (pct < CONFIG_JOG_SPEED_MIN) {
        pct = CONFIG_JOG_SPEED_MIN;
    }
    if (pct > CONFIG_JOG_SPEED_MAX) {
        pct = CONFIG_JOG_SPEED_MAX;
    }
    bool changed = (s_cfg.jog_speed_pct != (uint8_t)pct);
    s_cfg.jog_speed_pct = (uint8_t)pct;
    config_save();
    if (changed) {
        /* 只打百分比，不在这里换 duty —— 换算的唯一定义在 motor_pct_to_duty()，
         * 点动回执里会把真正的 duty 原值（含硬件回读）打出来。 */
        ams_log("手动点动 PWM 已改为 %d%%（只影响手动点动，不影响自动换料）",
                pct);
    }
    return s_cfg.jog_speed_pct;
}

uint8_t config_get_assist_hold(void)
{
    return s_cfg.assist_hold ? 1 : 0;
}

void config_set_assist_hold(uint8_t on)
{
    uint8_t v = on ? 1 : 0;
    bool changed = (s_cfg.assist_hold != v);
    s_cfg.assist_hold = v;
    config_save();
    if (changed) {
        ams_log("辅助送料「阶段内保持离合吸合」已%s", v ? "开启" : "关闭");
    }
}

bool config_sensor_enabled(int channel_index)
{
    if (channel_index < 0 || channel_index >= BOARD_CHANNEL_COUNT) {
        return false;
    }
    return (s_cfg.sensor_enabled_mask & (1u << channel_index)) != 0;
}

int config_printer_channel_of(int material_index)
{
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        return -1;
    }
    return s_cfg.access_list[material_index];
}

int config_material_index_of(int printer_channel)
{
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        if (s_cfg.access_list[i] == printer_channel) {
            return i;
        }
    }
    return -1;
}

/* ==========================================================================
 * 设置项
 * ========================================================================== */

esp_err_t config_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    pass = pass ? pass : "";

    /* 已经存过就搬到最前面（最近成功的排在前面，开机时优先试它） */
    int found = -1;
    for (int i = 0; i < s_cfg.profile_count; i++) {
        if (strncmp(s_cfg.profiles[i].ssid, ssid,
                    sizeof(s_cfg.profiles[i].ssid)) == 0) {
            found = i;
            break;
        }
    }
    if (found > 0) {
        config_wifi_profile_t tmp = s_cfg.profiles[found];
        for (int i = found; i > 0; i--) {
            s_cfg.profiles[i] = s_cfg.profiles[i - 1];
        }
        s_cfg.profiles[0] = tmp;
    } else if (found < 0) {
        /* 新记录：整体后移一格，插到最前面，把最老的挤出去 */
        int last = s_cfg.profile_count;
        if (last >= CONFIG_WIFI_MAX_PROFILES) {
            last = CONFIG_WIFI_MAX_PROFILES - 1;
        }
        for (int i = last; i > 0; i--) {
            s_cfg.profiles[i] = s_cfg.profiles[i - 1];
        }
        if (s_cfg.profile_count < CONFIG_WIFI_MAX_PROFILES) {
            s_cfg.profile_count++;
        }
    }

    strncpy(s_cfg.profiles[0].ssid, ssid,
            sizeof(s_cfg.profiles[0].ssid) - 1);
    s_cfg.profiles[0].ssid[sizeof(s_cfg.profiles[0].ssid) - 1] = '\0';
    strncpy(s_cfg.profiles[0].pass, pass,
            sizeof(s_cfg.profiles[0].pass) - 1);
    s_cfg.profiles[0].pass[sizeof(s_cfg.profiles[0].pass) - 1] = '\0';

    xSemaphoreGive(s_lock);
    return config_save();
}

esp_err_t config_set_ap(const char *ssid, const char *pass)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (ssid && ssid[0]) {
        strncpy(s_cfg.ap_ssid, ssid, sizeof(s_cfg.ap_ssid) - 1);
        s_cfg.ap_ssid[sizeof(s_cfg.ap_ssid) - 1] = '\0';
    }
    if (pass && pass[0]) {
        /* WPA2 要求 8~63 位；短了就退回默认密码，避免热点开不起来 */
        size_t n = strlen(pass);
        if (n >= 8 && n <= 63) {
            strncpy(s_cfg.ap_pass, pass, sizeof(s_cfg.ap_pass) - 1);
            s_cfg.ap_pass[sizeof(s_cfg.ap_pass) - 1] = '\0';
        } else {
            ams_log_warn("热点密码长度必须 8~63 位，本次改动忽略");
        }
    }
    xSemaphoreGive(s_lock);
    return config_save();
}

esp_err_t config_set_mqtt(const char *host, const char *serial,
                          const char *user, const char *pass,
                          int port, const char *client_id)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

#define SET_STR(_field, _src)                                                  \
    do {                                                                       \
        if ((_src) != NULL) {                                                  \
            strncpy(s_cfg._field, (_src), sizeof(s_cfg._field) - 1);            \
            s_cfg._field[sizeof(s_cfg._field) - 1] = '\0';                     \
        }                                                                      \
    } while (0)

    SET_STR(mqtt_host, host);
    SET_STR(mqtt_serial, serial);
    SET_STR(mqtt_user, user);
    SET_STR(mqtt_pass, pass);
    SET_STR(mqtt_client_id, client_id);
#undef SET_STR

    if (port > 0 && port < 65536) {
        s_cfg.mqtt_port = (uint16_t)port;
    }

    xSemaphoreGive(s_lock);
    return config_save();
}

esp_err_t config_set_access(const uint8_t *access_list,
                            const uint32_t *color_list)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (access_list) {
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            uint8_t v = access_list[i];
            /* 只接受 1..BOARD_CHANNEL_COUNT；其余保持原值，避免网页传脏数据
             * 把映射搞成"两个物理位对应同一打印机通道" */
            if (v >= 1 && v <= BOARD_CHANNEL_COUNT) {
                s_cfg.access_list[i] = v;
            }
        }
    }
    if (color_list) {
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            if (color_list[i] <= 0xFFFFFFu) {
                s_cfg.color_list[i] = color_list[i];
            }
        }
    }
    xSemaphoreGive(s_lock);
    return config_save();
}

/* ==========================================================================
 * 颜色匹配
 * ==========================================================================
 * 4 通道自动换料的核心：拿切片器的目标颜色（0xRRGGBB），和 4 个通道
 * 已配置的颜色做距离比较，选最接近的那个。
 *
 * 距离度量用感知加权平方和：
 *     d² = 0.30² × ΔR² + 0.59² × ΔG² + 0.11² × ΔB²
 * 权重是人眼对亮度的感知（BT.601 亮度系数），比朴素欧氏距离
 * 更贴近人对色差的主观感受 —— 绿色差异比蓝色差异"显得"更大。
 *
 * 返回 0 表示完全匹配；UINT32_MAX 表示 4 个通道全是 0（未配颜色）。
 */

uint32_t config_get_color(int material_index)
{
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        return 0;
    }
    return s_cfg.color_list[material_index];
}

uint32_t config_color_match(uint32_t target_rgb, int *out_material)
{
    if (out_material) {
        *out_material = -1;
    }
    if (target_rgb > 0xFFFFFFu) {
        return UINT32_MAX;
    }

    uint32_t tr = (target_rgb >> 16) & 0xFF;
    uint32_t tg = (target_rgb >> 8) & 0xFF;
    uint32_t tb = target_rgb & 0xFF;

    uint32_t best_dist = UINT32_MAX;
    int best_idx = -1;

    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        uint32_t c = s_cfg.color_list[i];
        if (c > 0xFFFFFFu) {
            continue;   /* 未配置，跳过 */
        }
        uint32_t cr = (c >> 16) & 0xFF;
        uint32_t cg = (c >> 8) & 0xFF;
        uint32_t cb = c & 0xFF;

        int32_t dr = (int32_t)tr - (int32_t)cr;
        int32_t dg = (int32_t)tg - (int32_t)cg;
        int32_t db = (int32_t)tb - (int32_t)cb;

        /* 感知加权：wR=30, wG=59, wB=11（避免浮点，用整数近似 0.30/0.59/0.11）
         * 平方后再除以 100² 保持量纲一致 */
        uint32_t dist = (uint32_t)(dr * dr * 900 +
                                    dg * dg * 3481 +
                                    db * db * 121);

        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }

    if (best_idx >= 0 && out_material) {
        *out_material = best_idx;
    }
    return (best_idx < 0) ? UINT32_MAX : best_dist;
}

int config_describe(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) {
        return 0;
    }
    int off = 0;
#define P(...)                                                                 \
    do {                                                                       \
        if (off < (int)buflen - 1) {                                           \
            int _w = snprintf(buf + off, buflen - off, __VA_ARGS__);           \
            if (_w > 0) { off += _w; }                                         \
        }                                                                      \
    } while (0)

    P("---- 业务配置 ----\n");
    P("WiFi 记录 : %u 组%s", (unsigned)s_cfg.profile_count,
      s_cfg.profile_count ? "\n" : "（未配网）\n");
    for (int i = 0; i < s_cfg.profile_count; i++) {
        P("   %d) %s\n", i + 1, s_cfg.profiles[i].ssid);
    }
    P("配置热点 : %s（信道 %u）\n", s_cfg.ap_ssid, (unsigned)s_cfg.ap_channel);
    P("打印机   : %s:%u  序列号=%s  %s\n",
      s_cfg.mqtt_host[0] ? s_cfg.mqtt_host : "(未填)",
      (unsigned)s_cfg.mqtt_port,
      s_cfg.mqtt_serial[0] ? s_cfg.mqtt_serial : "(未填)",
      config_mqtt_ready() ? "（已配置）" : "（未配置完整）");
    P("通道映射 : ");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        P("位%d→通道%d%s", i + 1, s_cfg.access_list[i],
          i + 1 < BOARD_CHANNEL_COUNT ? "  " : "\n");
    }
    if (s_cfg.filament_current) {
        P("当前料盘 : %d\n", (int)s_cfg.filament_current);
    } else {
        P("当前料盘 : 未知\n");
    }
    P("手动点动 : %.1f 秒 @%u%%\n", s_cfg.jog_ms / 1000.0,
      (unsigned)config_get_jog_speed_pct());
    P("自吸参数 : 蠕动 %u 次 × %ums @%u%%\n",
      (unsigned)s_cfg.creep_times, (unsigned)s_cfg.creep_pulse_ms,
      (unsigned)s_cfg.creep_speed_pct);
    P("挤出机信号来源: %s\n",
      s_cfg.extruder_src == EXTRUDER_SRC_MQTT ? "打印机 MQTT 事件" : "GPIO 输入");
    P("微动安装 : ");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        P("位%d=%s%s", i + 1,
          config_sensor_enabled(i) ? "已装" : "未装",
          i + 1 < BOARD_CHANNEL_COUNT ? "  " : "\n");
    }
    P("辅助送料 : %s @%u%% × %ums，%s\n",
      s_cfg.assist_enabled ? "开启" : "关闭",
      (unsigned)s_cfg.assist_speed_pct, (unsigned)s_cfg.assist_ms,
      config_get_assist_hold() ? "阶段内保持离合吸合（只脉冲电机）"
                               : "每次吸合-断开");
    P("配置结构长度: %u 字节（改动必须保持不变，否则老配置会被丢弃）\n",
      (unsigned)sizeof(ams_config_t));
    /* 退料现在是"连续退料优先"：先全速拉 retract_cont_ms（拉到打印机报
     * 无料就提前停），没拉出来再用蠕动拱 retract_creep_max 轮。 */
    P("退料参数 : 连续 %ums → 蠕动 %u 轮 × %ums（间隔 %ums）\n",
      (unsigned)s_cfg.retract_cont_ms,
      (unsigned)s_cfg.retract_creep_max,
      (unsigned)s_cfg.retract_creep_ms,
      (unsigned)s_cfg.retract_gap_ms);
    P("换料温度 : ");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        P("位%d=%d℃%s", i + 1, config_get_temper(i),
          i + 1 < BOARD_CHANNEL_COUNT ? "  " : "\n");
    }
#undef P
    return off;
}

/* ==========================================================================
 * 辅助送料 / 退料参数访问器
 * ========================================================================== */

uint8_t config_get_assist_speed_pct(void)
{
    return s_cfg.assist_speed_pct;
}

void config_set_assist_speed_pct(uint8_t pct)
{
    if (pct < CONFIG_ASSIST_PCT_MIN) pct = CONFIG_ASSIST_PCT_MIN;
    if (pct > CONFIG_ASSIST_PCT_MAX) pct = CONFIG_ASSIST_PCT_MAX;
    s_cfg.assist_speed_pct = pct;
    config_save();
}

uint8_t config_get_assist_enabled(void)
{
    return s_cfg.assist_enabled;
}

void config_set_assist_enabled(uint8_t on)
{
    s_cfg.assist_enabled = on ? 1 : 0;
    config_save();
}

uint16_t config_get_retract_wait_ms(void)
{
    return s_cfg.retract_wait_ms;
}

uint16_t config_set_retract_wait_ms(int value)
{
    if (value < CONFIG_RETRACT_WAIT_MIN) value = CONFIG_RETRACT_WAIT_MIN;
    if (value > CONFIG_RETRACT_WAIT_MAX) value = CONFIG_RETRACT_WAIT_MAX;
    s_cfg.retract_wait_ms = (uint16_t)value;
    config_save();
    return s_cfg.retract_wait_ms;
}

uint16_t config_get_retract_cont_ms(void)
{
    return s_cfg.retract_cont_ms;
}

uint16_t config_set_retract_cont_ms(int value)
{
    if (value < CONFIG_RETRACT_CONT_MIN) value = CONFIG_RETRACT_CONT_MIN;
    if (value > CONFIG_RETRACT_CONT_MAX) value = CONFIG_RETRACT_CONT_MAX;
    s_cfg.retract_cont_ms = (uint16_t)value;
    config_save();
    return s_cfg.retract_cont_ms;
}

uint16_t config_get_assist_ms(void)
{
    return s_cfg.assist_ms;
}

uint16_t config_set_assist_ms(int value)
{
    if (value < CONFIG_ASSIST_MS_MIN) value = CONFIG_ASSIST_MS_MIN;
    if (value > CONFIG_ASSIST_MS_MAX) value = CONFIG_ASSIST_MS_MAX;
    s_cfg.assist_ms = (uint16_t)value;
    config_save();
    return s_cfg.assist_ms;
}

uint16_t config_get_retract_creep_ms(void)
{
    return s_cfg.retract_creep_ms;
}

uint16_t config_set_retract_creep_ms(int value)
{
    if (value < CONFIG_RETRACT_CREEP_MS_MIN) {
        value = CONFIG_RETRACT_CREEP_MS_MIN;
    }
    if (value > CONFIG_RETRACT_CREEP_MS_MAX) {
        value = CONFIG_RETRACT_CREEP_MS_MAX;
    }
    s_cfg.retract_creep_ms = (uint16_t)value;
    config_save();
    return s_cfg.retract_creep_ms;
}

uint16_t config_get_retract_gap_ms(void)
{
    return s_cfg.retract_gap_ms;
}

uint16_t config_set_retract_gap_ms(int value)
{
    if (value < CONFIG_RETRACT_GAP_MS_MIN) value = CONFIG_RETRACT_GAP_MS_MIN;
    if (value > CONFIG_RETRACT_GAP_MS_MAX) value = CONFIG_RETRACT_GAP_MS_MAX;
    s_cfg.retract_gap_ms = (uint16_t)value;
    config_save();
    return s_cfg.retract_gap_ms;
}

uint8_t config_get_retract_creep_max(void)
{
    return s_cfg.retract_creep_max;
}

uint8_t config_set_retract_creep_max(int value)
{
    /* 0 是合法值（= 不蠕动，只做连续退料），所以下限是 0 而不是 1 */
    if (value < 0) { value = 0; }
    if (value > CONFIG_RETRACT_CREEP_MAX_MAX) {
        value = CONFIG_RETRACT_CREEP_MAX_MAX;
    }
    s_cfg.retract_creep_max = (uint8_t)value;
    config_save();
    return s_cfg.retract_creep_max;
}

int config_get_temper(int material_index)
{
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        return CONFIG_TEMPER_DEF;
    }
    /* 0 表示"没配过" —— 老固件升上来的 NVS 里这个字段全是 0，
     * 落回默认值就能直接跑，不用用户先去网页填一遍。 */
    uint16_t v = s_cfg.temper[material_index];
    return v ? (int)v : CONFIG_TEMPER_DEF;
}

void config_set_temper(int material_index, int value)
{
    if (material_index < 0 || material_index >= BOARD_CHANNEL_COUNT) {
        return;
    }
    if (value < CONFIG_TEMPER_MIN) value = CONFIG_TEMPER_MIN;
    if (value > CONFIG_TEMPER_MAX) value = CONFIG_TEMPER_MAX;
    s_cfg.temper[material_index] = (uint16_t)value;
    config_save();
}
