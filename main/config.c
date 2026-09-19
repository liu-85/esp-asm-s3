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
    c->creep_times     = CONFIG_CREEP_TIMES_DEF;
    c->creep_pulse_ms  = CONFIG_CREEP_PULSE_MS_DEF;
    c->creep_speed_pct = CONFIG_CREEP_SPEED_PCT_DEF;
#if defined(ESP_IDF_TARGET_C3)
    /* C3 没有挤出机到位 GPIO 线，默认走 MQTT 事件（hw_switch_state） */
    c->extruder_src    = EXTRUDER_SRC_MQTT;
#else
    c->extruder_src    = EXTRUDER_SRC_GPIO;
#endif

    /* ★ 微动默认**全部视为未安装**，程序走降级模式（按时间推进送料）。
     *   原因：没接微动却以为接了，程序会一直等一个永远不来的信号 —— 表现为
     *   "推料推不动、每步都超时"，比按时间推进更难排查。
     *   接好微动后：网页「硬件调试」里勾上，或者改这里的位掩码。
     *   bit0=料盘位1，bit1=料盘位2，bit2=料盘位3，bit3=料盘位4 */
    c->sensor_enabled_mask = 0x00;

    memset(c->reserved, 0, sizeof(c->reserved));
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
    P("手动点动 : %.1f 秒\n", s_cfg.jog_ms / 1000.0);
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
#undef P
    return off;
}
