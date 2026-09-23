/**
 * web_server.c —— 配置网页与 REST 接口的实现
 * ============================================================================
 *
 * 全部接口都跑在 esp_http_server 自己的任务里，所以这里的 handler **可以随便
 * 写阻塞代码**（扫 WiFi 要 2 秒、OTA 要几十秒都没问题）—— 这一点是相对
 * MicroPython 版最大的解放：那边每个 handler 都得拆成 `await` 片段，否则
 * uasyncio 事件循环被按住，网页就整个卡死。
 *
 * ⚠️ 唯一的例外是 /wifi_connect：它**不能**在这里等连接结果。配网时设备会
 *    关掉配置热点，而手机正是靠那个热点连着我们的 —— 热点一关，这个响应就
 *    再也发不回手机了。所以它先把响应发出去，再把连接交给
 *    wifi_mgr_provision_connect() 起的独立任务去做。
 *
 * ⚠️ 另一个例外是 /wifi_scan：热点上还有客户端时不许做全信道扫描
 *    （官方配网文档明确会因此把客户端踢下线），只回缓存。见该 handler 注释。
 *
 * ----------------------------------------------------------------------------
 * 目录
 * ----------------------------------------------------------------------------
 *   一、通用工具（返 JSON、读请求体、内嵌资源）
 *   二、GET  /   /app.js   /style.css
 *   三、GET  /status        ★ 前端 2 秒轮询一次的"总状态"
 *   四、GET  /log
 *   五、GET  /get_mqtt_info
 *   六、POST /wifi_scan  /wifi_connect
 *   七、POST /mqtt_connect
 *   八、POST /access_set
 *   九、POST /hardware_test  /jog_set  /stop
 *   十、POST /sensor_set  /autoload  /creep_set  /extruder_src_set   ← 本次新增
 *   十一、POST /ap_set   GET /boot_clear
 *   十二、POST /ota_upload  ★ 真正的双分区 OTA
 *   十三、注册路由
 */

#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ams_controller.h"
#include "bambu_mqtt.h"
#include "board_pins.h"
#include "clutch.h"
#include "config.h"
#include "filament_sensor.h"
#include "log_buffer.h"
#include "motor.h"
#include "wifi_manager.h"

#include "esp_http_server.h"

/* ==========================================================================
 * 内嵌的前端资源
 * ==========================================================================
 * 这三个符号由 main/CMakeLists.txt 里的 EMBED_FILES 生成，内容就是 web/ 下的
 * 三个文件。放在 .rodata 段里，读它们是纯内存操作 —— 没有文件系统挂载这一步，
 * 也就没有"挂载失败导致网页打不开"这一类故障。
 *
 * 符号名的构成规则：文件名里的非字母数字字符换成下划线，前面加 _binary_，
 * 后面加 _start / _end。
 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");

/* ==========================================================================
 * 内部状态
 * ========================================================================== */
static httpd_handle_t s_server;
static uint32_t       s_req_count;

/** 本次启动的"复位原因"描述，启动时算一次就够 */
static char s_reset_cause[24];
static char s_reset_desc[160];

/** 引脚自检结论（启动时算一次） */
static bool s_pins_ok = true;
static char s_pins_problem[128];

/** 累计启动次数（放 NVS，每次开机 +1） */
static uint32_t s_boot_count;

#define NVS_KEY_BOOT "boot"

/* ==========================================================================
 * 一、通用工具
 * ========================================================================== */

/**
 * 复位原因 → (代号, 人话说明)。
 *
 * 这一段照搬 MicroPython 版上电诊断面板的目的：那块面板存在的唯一理由是回答
 * 「接上负载就一直重启，到底是哪儿的问题」。所以"人话"部分要给出**下一步该
 * 查什么**，而不是复述代号。
 */
static void describe_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *code = "UNKNOWN";
    const char *desc = "未能识别的复位原因。";

    switch (r) {
    case ESP_RST_POWERON:
        code = "POWERON";
        desc = "正常上电。若反复出现说明板子在反复断电，重点查供电。";
        break;
    case ESP_RST_EXT:
        code = "EXT_PIN";
        desc = "外部复位脚被拉低。检查 RST 脚有没有被意外按下或干扰。";
        break;
    case ESP_RST_SW:
        code = "SOFTWARE";
        desc = "软件主动重启（网页升级、恢复出厂都会走到这里），属正常。";
        break;
    case ESP_RST_PANIC:
        code = "PANIC";
        desc = "程序异常（空指针 / 断言失败 / 栈溢出）。请看串口日志里最后一段回溯。";
        break;
    case ESP_RST_INT_WDT:
        code = "INT_WDT";
        desc = "中断看门狗超时。通常是有中断处理函数里做了耗时操作。";
        break;
    case ESP_RST_TASK_WDT:
        code = "TASK_WDT";
        desc = "任务看门狗超时。某个任务被卡住太久（常见于网络阻塞调用）。";
        break;
    case ESP_RST_WDT:
        code = "WDT";
        desc = "看门狗复位。若伴随欠压提示，先把电源换掉再排查其它。";
        break;
    case ESP_RST_BROWNOUT:
        code = "BROWNOUT";
        desc = "★ 欠压复位。供电不足是最常见的原因：换 USB 口、换短线、"
               "外接 5V（离合吸合瞬间电流很大，USB 口带不动）。";
        break;
    case ESP_RST_DEEPSLEEP:
        code = "DEEPSLEEP";
        desc = "从深度睡眠唤醒。";
        break;
    case ESP_RST_SDIO:
        code = "SDIO";
        desc = "SDIO 触发的复位。";
        break;
    default:
        break;
    }

    snprintf(s_reset_cause, sizeof(s_reset_cause), "%s", code);
    snprintf(s_reset_desc, sizeof(s_reset_desc), "%s", desc);
}

/** 读 NVS 里的启动计数并 +1 */
static void boot_count_bump(void)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        s_boot_count = 1;
        return;
    }
    uint32_t n = 0;
    nvs_get_u32(h, NVS_KEY_BOOT, &n);
    /* 溢出保护：正常永远到不了 100000 */
    n = (n >= 100000u || n == 0) ? 1u : n + 1u;
    nvs_set_u32(h, NVS_KEY_BOOT, n);
    nvs_commit(h);
    nvs_close(h);
    s_boot_count = n;
}

static void boot_count_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_BOOT, 0);
        nvs_commit(h);
        nvs_close(h);
    }
    s_boot_count = 0;
}

/** 启动时跑一次引脚自检，结果留着给网页用 */
static void pins_selfcheck(void)
{
    char err[128] = {0};
    s_pins_ok = board_pins_validate(err, sizeof(err));
    snprintf(s_pins_problem, sizeof(s_pins_problem), "%s", err);
}

/** 发一段 JSON 字符串（内容已经拼好） */
static esp_err_t send_json_raw(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/** 把一个 cJSON 对象发出去并释放它 */
static esp_err_t send_json_obj(httpd_req_t *req, cJSON *obj)
{
    if (obj == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "JSON build failed");
    }
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "JSON print failed");
    }
    esp_err_t err = send_json_raw(req, txt);
    free(txt);
    return err;
}

/** 统一风格的 {"ok":true/false,"info":"..."} 回包 */
static esp_err_t reply_ok(httpd_req_t *req, bool ok, const char *info)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddStringToObject(o, "info", info ? info : (ok ? "已生效" : "操作失败"));
    return send_json_obj(req, o);
}

/**
 * 读请求体并解析成 cJSON。
 *
 * 任何异常（没有内容 / 太长 / 不是合法 JSON）都返回 NULL —— 调用方把 NULL
 * 当成"参数为空"处理即可，不需要区分，因为所有 handler 都对缺参数有默认行为。
 */
static cJSON *read_body_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        return NULL;
    }
    char *buf = malloc((size_t)req->content_len + 1);
    if (buf == NULL) {
        return NULL;
    }

    int total = 0;
    while (total < req->content_len) {
        int got = httpd_req_recv(req, buf + total, req->content_len - total);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;               /* 慢速客户端：等下一次可读，不算失败 */
        }
        if (got <= 0) {
            free(buf);
            return NULL;
        }
        total += got;
    }
    buf[total] = '\0';

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/** 从 cJSON 里取整数，带默认值 */
static int json_int(const cJSON *o, const char *key, int def)
{
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    if (cJSON_IsNumber(v)) {
        return (int)v->valuedouble;
    }
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        return atoi(v->valuestring);
    }
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v) ? 1 : 0;
    }
    return def;
}

/** 从 cJSON 里取字符串；不存在返回 NULL */
static const char *json_str(const cJSON *o, const char *key)
{
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ==========================================================================
 * 二、静态资源
 * ========================================================================== */

static esp_err_t serve_blob(httpd_req_t *req, const uint8_t *start,
                            const uint8_t *end, const char *mime)
{
    size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, mime);
    /* 前端改一次固件就得重烧，所以让浏览器别缓太狠，免得改了看不到效果 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return serve_blob(req, index_html_start, index_html_end,
                      "text/html; charset=utf-8");
}

static esp_err_t h_appjs(httpd_req_t *req)
{
    return serve_blob(req, app_js_start, app_js_end,
                      "application/javascript; charset=utf-8");
}

static esp_err_t h_style(httpd_req_t *req)
{
    return serve_blob(req, style_css_start, style_css_end,
                      "text/css; charset=utf-8");
}

/* ==========================================================================
 * 三、GET /status —— 前端所有面板的数据来源
 * ==========================================================================
 * 特意做成"一个接口给全部数据"，而不是每个面板一个接口：
 *   · 前端每 2 秒只需发 1 个请求，而不是 7 个
 *   · 不会出现"某块面板的数据比另一块旧一个周期"的错位
 * 代价是这个 JSON 有点大（约 2KB）—— 在 ESP-IDF 下毫无压力。
 */
static esp_err_t h_status(httpd_req_t *req)
{
    ams_config_t *cfg = config_get();
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "oom");
    }

    /* ---- 网络 ---- */
    bool wifi_on = wifi_mgr_is_connected();
    cJSON_AddBoolToObject(o, "wifi_isconnected", wifi_on);
    cJSON_AddStringToObject(o, "wifi_ssid",
                            wifi_on ? wifi_mgr_current_ssid() : "");
    cJSON_AddStringToObject(o, "wifi_status_text", wifi_mgr_status_text());
    cJSON_AddStringToObject(o, "ip", wifi_mgr_sta_ip());
    cJSON_AddNumberToObject(o, "rssi", wifi_on ? wifi_mgr_rssi() : 0);
    cJSON_AddStringToObject(o, "mac", wifi_mgr_sta_mac());

    bool ap_on = wifi_mgr_ap_is_on();
    cJSON_AddBoolToObject(o, "ap_on", ap_on);
    cJSON_AddStringToObject(o, "ap_ssid", cfg->ap_ssid);
    cJSON_AddStringToObject(o, "ap_ip", wifi_mgr_ap_ip());
    cJSON_AddNumberToObject(o, "ap_clients",
                            ap_on ? wifi_mgr_ap_client_count() : 0);

    /* ---- 打印机 ---- */
    cJSON_AddBoolToObject(o, "is_mqtt_con", bambu_mqtt_is_connected());
    cJSON_AddBoolToObject(o, "mqtt_configured", bambu_mqtt_is_configured());
    cJSON_AddStringToObject(o, "mqtt_host", bambu_mqtt_host());
    cJSON_AddStringToObject(o, "mqtt_serial", bambu_mqtt_serial());
    cJSON_AddNumberToObject(o, "mqtt_rx", (double)bambu_mqtt_rx_count());
    uint32_t since_rx = bambu_mqtt_ms_since_last_rx();
    cJSON_AddNumberToObject(o, "mqtt_ms_since_rx",
                            since_rx == UINT32_MAX ? -1 : (double)since_rx);
    cJSON_AddStringToObject(o, "mqtt_last_error", bambu_mqtt_last_error());

    /* ---- 业务 ---- */
    cJSON_AddNumberToObject(o, "current_access", config_get_filament_current());
    cJSON_AddNumberToObject(o, "jog_ms", config_get_jog_ms());
    /* ★ 手动点动 PWM（2026-09-23 新增）：现场要一档一档试"多少占空比能带动
     *   电机和离合"，所以这个值必须能在网页上读到、改到、并被 /status 回显
     *   （网页靠回显判断"存进去了没有"）。 */
    cJSON_AddNumberToObject(o, "jog_speed_pct", config_get_jog_speed_pct());

    cJSON *al = cJSON_AddArrayToObject(o, "access_list");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        cJSON_AddItemToArray(al, cJSON_CreateNumber(cfg->access_list[i]));
    }
    cJSON *cl = cJSON_AddArrayToObject(o, "color_list");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        cJSON_AddItemToArray(cl, cJSON_CreateNumber((double)cfg->color_list[i]));
    }

    /* ---- 自吸参数（网页可改） ---- */
    cJSON *creep = cJSON_AddObjectToObject(o, "creep");
    cJSON_AddNumberToObject(creep, "times", cfg->creep_times);
    cJSON_AddNumberToObject(creep, "pulse_ms", cfg->creep_pulse_ms);
    cJSON_AddNumberToObject(creep, "speed_pct", cfg->creep_speed_pct);
    cJSON_AddNumberToObject(o, "extruder_src", cfg->extruder_src);
    cJSON_AddBoolToObject(o, "extruder_pin_used",
                          BOARD_PIN_EXTRUDER_INPLACE >= 0 &&
                          !BOARD_EXTRUDER_INPLACE_UNUSED);

    /* ---- 辅助送料参数 ---- */
    cJSON *assist = cJSON_AddObjectToObject(o, "assist");
    cJSON_AddBoolToObject(assist, "enabled", cfg->assist_enabled != 0);
    cJSON_AddNumberToObject(assist, "speed_pct", cfg->assist_speed_pct);
    cJSON_AddNumberToObject(assist, "ms", cfg->assist_ms);
    /* ★ 阶段内保持离合吸合（2026-09-23 新增）：1 = 阶段内离合一直吸着、
     *   只脉冲电机；0 = 老行为（每送一次吸合-断开一整套）。 */
    cJSON_AddBoolToObject(assist, "hold", config_get_assist_hold() != 0);

    /* ---- 退料参数（全是现场要调的，网页「硬件调试」里可改） ---- */
    cJSON *retract = cJSON_AddObjectToObject(o, "retract");
    cJSON_AddNumberToObject(retract, "wait_ms", cfg->retract_wait_ms);
    cJSON_AddNumberToObject(retract, "cont_ms", cfg->retract_cont_ms);
    cJSON_AddNumberToObject(retract, "creep_ms", cfg->retract_creep_ms);
    cJSON_AddNumberToObject(retract, "gap_ms", cfg->retract_gap_ms);
    cJSON_AddNumberToObject(retract, "creep_max", cfg->retract_creep_max);

    /* ---- 换料温度（每个料盘位一个）----
     * 走 config_get_temper() 而不是直读 cfg->temper：没配过的位在 NVS 里
     * 是 0，那表示"用默认值"，直接发 0 给前端界面上就会显示 0℃。 */
    cJSON *temper = cJSON_AddArrayToObject(o, "temper");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        cJSON_AddItemToArray(temper,
                             cJSON_CreateNumber(config_get_temper(i)));
    }

    /* ---- 运行环境 ---- */
    cJSON_AddNumberToObject(o, "mem_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "mem_min_free",
                            (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "uptime_ms",
                            (double)(esp_timer_get_time() / 1000));

    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(o, "part", running ? running->label : "?");
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        cJSON_AddStringToObject(o, "app_version", app->version);
        cJSON_AddStringToObject(o, "idf_version", app->idf_ver);
        cJSON_AddStringToObject(o, "build_date", app->date);
        cJSON_AddStringToObject(o, "build_time", app->time);
    }

    /* ---- 上电诊断 ---- */
    cJSON *rst = cJSON_AddObjectToObject(o, "reset");
    cJSON_AddStringToObject(rst, "cause", s_reset_cause);
    cJSON_AddStringToObject(rst, "cause_desc", s_reset_desc);
    cJSON_AddNumberToObject(rst, "boot_count", (double)s_boot_count);
    cJSON_AddNumberToObject(rst, "uptime_ms",
                            (double)(esp_timer_get_time() / 1000));

    cJSON *safety = cJSON_AddObjectToObject(o, "boot_safety");
    cJSON_AddBoolToObject(safety, "ok", s_pins_ok);
    cJSON *probs = cJSON_AddArrayToObject(safety, "problems");
    if (!s_pins_ok) {
        cJSON_AddItemToArray(probs, cJSON_CreateString(s_pins_problem));
    }

    /* ---- 硬件状态 ----
     * ★ 这里复用 ams_controller 生成的那段 JSON，然后并进来。
     *   好处是离合/电机的字段定义只有一处，改那边这边自动跟上，
     *   不会出现"网页显示的和日志里不一致"这种最难查的问题。 */
    /* ★ 512 → 640：ams_describe_hardware_json 里新增了一整块 drive 回执，
     *   要保证 cJSON_Parse 拿到的是完整 JSON（见那边的余量检查注释）。 */
    /* ★ 再提到 800：drive 块又多了 4 个回读字段（rb_in1/rb_in2/lv_in1/lv_in2）
     *   和 dir，约 60 字节。缓冲不够的话 snprintf 会把 JSON 尾巴截掉 →
     *   cJSON_Parse 失败 → 整个 hardware 面板消失（比不显示回读更糟）。 */
    char hwbuf[800];
    if (ams_describe_hardware_json(hwbuf, sizeof(hwbuf)) > 0) {
        cJSON *hw = cJSON_Parse(hwbuf);
        if (hw) {
            /* 补两个前端要用的别名 */
            cJSON_AddNumberToObject(hw, "active_channel",
                                    clutch_active_channel());
            cJSON_AddBoolToObject(hw, "is_busy", ams_is_busy());
            cJSON_AddItemToObject(o, "hardware", hw);
        }
    }

    /* ---- 微动状态（本次新增） ---- */
    char sdbuf[512];
    if (sensor_describe_json(sdbuf, sizeof(sdbuf)) > 0) {
        cJSON *sd = cJSON_Parse(sdbuf);
        if (sd) {
            cJSON_AddItemToObject(o, "sensors", sd);
        }
    }

    /* ---- AMS 业务状态 ---- */
    cJSON *ams = cJSON_AddObjectToObject(o, "ams");
    cJSON_AddNumberToObject(ams, "state", (int)ams_get_state());
    cJSON_AddStringToObject(ams, "state_text", ams_state_text());
    cJSON_AddBoolToObject(ams, "busy", ams_is_busy());
    cJSON_AddNumberToObject(ams, "active_material", ams_active_material());
    cJSON_AddNumberToObject(ams, "current_channel", ams_current_printer_channel());

    ams_diag_t dg;
    ams_get_diag(&dg);
    cJSON *diag = cJSON_AddObjectToObject(ams, "diag");
    cJSON_AddNumberToObject(diag, "exchange_ok", (double)dg.exchange_ok);
    cJSON_AddNumberToObject(diag, "exchange_fail", (double)dg.exchange_fail);
    cJSON_AddNumberToObject(diag, "autoload_ok", (double)dg.autoload_ok);
    cJSON_AddNumberToObject(diag, "autoload_fail", (double)dg.autoload_fail);
    cJSON_AddNumberToObject(diag, "load_ok", (double)dg.load_ok);
    cJSON_AddNumberToObject(diag, "retract_ok", (double)dg.retract_ok);
    cJSON_AddNumberToObject(diag, "jog_count", (double)dg.jog_count);
    cJSON_AddNumberToObject(diag, "last_error_ms",
                            dg.last_error_ms == UINT32_MAX ? -1
                                                           : (double)dg.last_error_ms);
    cJSON_AddStringToObject(diag, "last_error", dg.last_error);

    /* ---- WiFi 扫描结果（只读缓存，绝不在这里触发扫描） ---- */
    cJSON *ssids = cJSON_AddArrayToObject(o, "ssids");
    wifi_mgr_ap_info_t list[WIFI_MGR_SCAN_MAX];
    int n = wifi_mgr_scan_last(list, WIFI_MGR_SCAN_MAX);
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(ssids, cJSON_CreateString(list[i].ssid));
    }

    /* ---- 板型与剩余可用 IO ---- */
    cJSON *board = cJSON_AddObjectToObject(o, "board");
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    cJSON_AddStringToObject(board, "name", "ESP32-C3（2 通道降级）");
#else
    cJSON_AddStringToObject(board, "name", "ESP32-S3（42 针）");
#endif
    cJSON_AddNumberToObject(board, "channels", BOARD_CHANNEL_COUNT);
    cJSON *spare = cJSON_AddArrayToObject(board, "spare_pins");
    const board_spare_pin_t spares[] = BOARD_SPARE_PINS;
    for (size_t i = 0; i < sizeof(spares) / sizeof(spares[0]); i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "pin", spares[i].pin);
        cJSON_AddStringToObject(item, "note", spares[i].note);
        cJSON_AddItemToArray(spare, item);
    }

    cJSON_AddNumberToObject(o, "req_count", (double)s_req_count);

    return send_json_obj(req, o);
}

/* ==========================================================================
 * 四、GET /log
 * ==========================================================================
 * 返回 {"log": ["行1","行2",...], "mem_free": N, "count": n}
 * 前端是把 log 当**数组**逐行渲染的（要按行做错误高亮），所以这里切开再发。
 */
static esp_err_t h_log(httpd_req_t *req)
{
    /* 这块缓冲 ≈ 5.5KB。httpd 任务栈只有 8KB，cJSON 还要再堆出响应
     * 对象，放栈上会把 httpd 任务栈顶穿。改为 static，handler 是
     * esp_http_server 单线程串行执行的，不会有并发访问问题。 */
    static char     buf[AMS_LOG_LINES * (AMS_LOG_LINE_MAX + 1) + 64];
    static uint32_t seqs[AMS_LOG_LINES];
    int got = 0;
    ams_log_recent_lines(buf, sizeof(buf), AMS_LOG_LINES,
                         seqs, AMS_LOG_LINES, &got);

    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    /* log 是文本行，seq 是它们各自的行号。前端靠 seq 做"只追加新行" ——
     * 之前前端只拿到文本，只能整串比对，环形缓冲一滚动整串就变了，
     * 于是每两秒整屏重建一次：看着闪、还会丢行。 */
    cJSON *arr  = cJSON_AddArrayToObject(o, "log");
    cJSON *sarr = cJSON_AddArrayToObject(o, "seq");
    char *p = buf;
    int idx = 0;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (*p) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(p));
            /* buf 装不下时 ams_log_recent_lines 会提前 break，所以理论上
             * 不会出现 idx > got；兜个 -1，前端拿到无效 seq 会自动退回
             * 整串比对（老行为），宁可不优化也不能渲染错。 */
            cJSON_AddItemToArray(sarr, cJSON_CreateNumber(
                (idx < got) ? (double)seqs[idx] : -1.0));
            idx++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    cJSON_AddNumberToObject(o, "count", idx);
    cJSON_AddNumberToObject(o, "mem_free", (double)esp_get_free_heap_size());
    return send_json_obj(req, o);
}

/* ==========================================================================
 * 五、GET /get_mqtt_info
 * ==========================================================================
 * ⚠️ **不回传访问码**。它和打印机密码等价，而网页是走明文 HTTP 的，传回去
 *    等于在局域网上广播。前端那边做了对应处理：留空表示"不改"。
 */
static esp_err_t h_get_mqtt_info(httpd_req_t *req)
{
    ams_config_t *cfg = config_get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "host", cfg->mqtt_host);
    cJSON_AddStringToObject(o, "serial", cfg->mqtt_serial);
    cJSON_AddStringToObject(o, "user", cfg->mqtt_user);
    cJSON_AddNumberToObject(o, "port", cfg->mqtt_port ? cfg->mqtt_port : 8883);
    cJSON_AddStringToObject(o, "client_id", cfg->mqtt_client_id);
    cJSON_AddBoolToObject(o, "saved", config_mqtt_ready());
    cJSON_AddBoolToObject(o, "connected", bambu_mqtt_is_connected());
    /* 明确告诉前端"密码不回传"，让占位符写成"留空表示不修改" */
    cJSON_AddBoolToObject(o, "password_hidden", true);
    return send_json_obj(req, o);
}

/* ==========================================================================
 * 六、WiFi 扫描 / 连接
 * ========================================================================== */

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    /* ★ 热点上有客户端连着时，**绝对不做全信道扫描**。
     *
     *   ESP-IDF 官方配网文档（api-reference/provisioning/wifi_provisioning）里
     *   写得很明确：「一次性扫描所有信道可能会导致 Wi-Fi 驱动没有足够时间发送
     *   信标，进而导致与部分站点断连」，官方为此才改成"分组扫描、每组 4 个
     *   信道、组间至少等 120ms"。
     *
     *   而配网页恰恰就是"手机连在热点上"的时候打开的 —— 旧版一进 WiFi 页就
     *   自动扫一次，正好把手机踢下线：请求发不出去、响应回不来，用户看到的
     *   就是"配网总是失败"；手机重连后又触发下一次扫描，形成死循环。
     *
     *   这里只读缓存（不扫描），并把原因写在 info 里。缓存为空时前端会引导
     *   用户手动填 SSID，配网这条路照样走得通。 */
    int clients = wifi_mgr_ap_is_on() ? wifi_mgr_ap_client_count() : 0;

    wifi_mgr_ap_info_t list[WIFI_MGR_SCAN_MAX];
    int n;
    if (clients > 0) {
        n = wifi_mgr_scan_last(list, WIFI_MGR_SCAN_MAX);   /* 只读缓存，不扫描 */
    } else {
        /* 没人连着热点（或者热点压根没开）→ 放心扫 */
        ams_log("网页触发 WiFi 扫描…");
        n = wifi_mgr_scan_cached(list, WIFI_MGR_SCAN_MAX, true);
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", n >= 0);
    cJSON *arr = cJSON_AddArrayToObject(o, "ssids");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(list[i].ssid));
    }
    cJSON *detail = cJSON_AddArrayToObject(o, "list");
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", list[i].rssi);
        cJSON_AddNumberToObject(item, "channel", list[i].channel);
        cJSON_AddItemToArray(detail, item);
    }
    if (clients > 0) {
        char info[256];
        snprintf(info, sizeof(info),
                 "热点上正有 %d 台设备在用，全信道扫描会把它们踢下线，"
                 "所以本次不扫描，只列出已缓存的 %d 个网络；"
                 "列表里没有目标 WiFi 时请直接在上面填写名称",
                 clients, n > 0 ? n : 0);
        cJSON_AddStringToObject(o, "info", info);
        cJSON_AddBoolToObject(o, "scan_skipped", 1);
    } else if (n < 0) {
        cJSON_AddStringToObject(o, "info", "扫描失败（射频忙或超时），稍后再试");
    } else {
        char info[64];
        snprintf(info, sizeof(info), "扫描到 %d 个 WiFi", n);
        cJSON_AddStringToObject(o, "info", info);
    }
    return send_json_obj(req, o);
}

static esp_err_t h_wifi_connect(httpd_req_t *req)
{
    cJSON *body = read_body_json(req);
    const char *ssid = json_str(body, "name");
    const char *pass = json_str(body, "password");

    if (ssid == NULL || ssid[0] == '\0') {
        cJSON_Delete(body);
        return reply_ok(req, false, "请先选一个 WiFi，或直接填写 WiFi 名称");
    }

    ams_log("网页请求连接 WiFi: %s", ssid);

    /* ★ 先落盘，再连（ESP-IDF 官方配网流程的顺序）。
     *   老版本是"连上了才存"，于是密码填错一次、或者单射频下没连上，用户
     *   填的东西就全丢了 —— 重启后仍然回到配网模式，得从头再填一遍。
     *   保存失败不影响本次尝试，但要在日志里说清楚。 */
    bool saved = (config_set_wifi(ssid, pass ? pass : "") == ESP_OK);
    if (!saved) {
        ams_log_err("WiFi 凭据保存失败（本次仍会尝试连接）");
    }

    /* ★ 响应必须**先**发出去，再去动射频。
     *   接下来这一步会关掉配置热点，而手机正是靠这个热点连着我们的 ——
     *   热点一关手机就掉线。响应要是还没发完，用户永远看不到结果，
     *   现象就是"点连接没反应"或者页面报连接失败。
     *   真正的连接在 wifi_mgr_provision_connect() 起的任务里做。 */
    esp_err_t err = wifi_mgr_provision_connect(ssid, pass ? pass : "");
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return reply_ok(req, false, "启动连接流程失败，请重试");
    }

    return reply_ok(req, true,
                    saved ? "配置已保存，正在关闭配置热点并连接路由器…"
                            "手机会短暂掉线，稍后刷新本页看结果"
                          : "正在连接…（注意：配置保存失败，重启后要重新填写）");
}

/* ==========================================================================
 * 七、打印机参数
 * ========================================================================== */

static esp_err_t h_mqtt_connect(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    ams_config_t *cfg = config_get();

    /* 任何一项没传都保持原值 —— 前端"留空表示不修改"的语义 */
    const char *host   = json_str(b, "host");
    const char *serial = json_str(b, "serial");
    const char *user   = json_str(b, "user");
    const char *pass   = json_str(b, "password");
    const char *cid    = json_str(b, "client_id");
    int port = json_int(b, "port", cfg->mqtt_port ? cfg->mqtt_port : 8883);

    if (host == NULL && serial == NULL && pass == NULL) {
        cJSON_Delete(b);
        return reply_ok(req, false, "没有需要修改的内容");
    }

    esp_err_t err = config_set_mqtt(host, serial, user, pass, port, cid);
    cJSON_Delete(b);
    if (err != ESP_OK) {
        return reply_ok(req, false, "保存失败");
    }

    if (!config_mqtt_ready()) {
        return reply_ok(req, false,
                        "已保存，但还缺 IP / 序列号 / 访问码，暂时不会去连打印机");
    }

    ams_log("打印机参数已更新，正在重连 MQTT…");
    err = bambu_mqtt_reconfigure();
    if (err != ESP_OK) {
        return reply_ok(req, false, "已保存，但重建连接失败，请看设备日志");
    }
    return reply_ok(req, true,
                    "已保存，正在后台连接打印机（连不上会自动重试）");
}

/* ==========================================================================
 * 八、通道映射与颜色
 * ========================================================================== */

static esp_err_t h_access_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    if (b == NULL) {
        return reply_ok(req, false, "请求体不是合法 JSON");
    }

    const cJSON *al = cJSON_GetObjectItemCaseSensitive(b, "access_list");
    const cJSON *cl = cJSON_GetObjectItemCaseSensitive(b, "color_list");
    if (!cJSON_IsArray(al) || cJSON_GetArraySize(al) != BOARD_CHANNEL_COUNT) {
        cJSON_Delete(b);
        return reply_ok(req, false, "通道列表长度不对");
    }

    uint8_t  access[BOARD_CHANNEL_COUNT];
    uint32_t colors[BOARD_CHANNEL_COUNT];
    ams_config_t *cfg = config_get();

    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        cJSON *v = cJSON_GetArrayItem(al, i);
        if (!cJSON_IsNumber(v)) {
            cJSON_Delete(b);
            return reply_ok(req, false, "通道编号必须是数字");
        }
        access[i] = (uint8_t)v->valueint;
    }

    /* 打印机通道号必须是 1~CHANNEL_COUNT 的一个排列 —— 不允许重复，
     * 否则会出现"两个料盘位对应同一个打印机通道"，换料时必然错乱。 */
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        if (access[i] < 1 || access[i] > BOARD_CHANNEL_COUNT) {
            cJSON_Delete(b);
            return reply_ok(req, false, "通道编号超出范围");
        }
        for (int j = i + 1; j < BOARD_CHANNEL_COUNT; j++) {
            if (access[i] == access[j]) {
                cJSON_Delete(b);
                return reply_ok(req, false, "通道编号有重复，每个只能出现一次");
            }
        }
    }

    if (cJSON_IsArray(cl) &&
        cJSON_GetArraySize(cl) == BOARD_CHANNEL_COUNT) {
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            cJSON *v = cJSON_GetArrayItem(cl, i);
            colors[i] = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble
                                          : cfg->color_list[i];
        }
    } else {
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            colors[i] = cfg->color_list[i];
        }
    }

    cJSON_Delete(b);

    if (config_set_access(access, colors) != ESP_OK) {
        return reply_ok(req, false, "写入失败");
    }
    ams_log("通道与颜色已更新");
    return reply_ok(req, true, "通道与颜色已保存");
}

/* ==========================================================================
 * 九、点动 / 停止
 * ==========================================================================
 * ★ 立刻回包，动作交给 AMS 任务去做。
 *
 * 老版本这里是同步的（直接调 bus.run()，中间 sleep 好几秒），于是按一下
 * 按钮整个事件循环被按住 —— 网页转圈、状态灯停摆。现在动作在独立任务里跑，
 * 这个 handler 几十毫秒就返回了。
 *
 * 「总线忙」直接返回 ok:false 并说明，**绝不让用户排队**：排队等待正是
 * "按一下转圈半天"的观感来源。
 */
static esp_err_t h_hardware_test(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    /* 这里的 channel 是**物理料盘位**（1~4），和前端的按钮编号一致 */
    int channel   = json_int(b, "channel", 1);
    int direction = json_int(b, "direction", 1);
    int ms        = json_int(b, "times_ms", 0);   /* 0 = 用设备上的统一设置 */
    cJSON_Delete(b);

    if (direction != 1 && direction != -1) {
        return reply_ok(req, false, "方向只能是 1（进料）或 -1（退料）");
    }
    if (channel < 1 || channel > BOARD_CHANNEL_COUNT) {
        char info[64];
        snprintf(info, sizeof(info), "通道 %d 不存在", channel);
        return reply_ok(req, false, info);
    }
    if (ms <= 0) {
        ms = config_get_jog_ms();
    }

    int material = channel - 1;

    if (ams_is_busy()) {
        char info[96];
        int busy_mat = ams_active_material();
        snprintf(info, sizeof(info), "%s中，等它停下来再按",
                 ams_state_text());
        if (busy_mat >= 0) {
            /* 把"正在动的是哪一路"也说清楚 */
            snprintf(info, sizeof(info), "%s（料盘位%d）中，等它停下来再按",
                     ams_state_text(), busy_mat + 1);
        }
        return reply_ok(req, false, info);
    }

    if (!ams_post_jog(material, direction, ms)) {
        return reply_ok(req, false, "总线正忙，稍后再按");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "running", true);
    cJSON_AddNumberToObject(o, "ms", ms);
    cJSON_AddNumberToObject(o, "speed_pct", config_get_jog_speed_pct());
    char info[192];
    /* 把这次实际会用的占空比也说清楚 —— 现场就是靠这个数一档一档试
     * "多少 PWM 能带动电机和离合"（数在「手动点动」卡片里改）。 */
    snprintf(info, sizeof(info),
             "通道%d 已开始%s，%.1f 秒后自动停止（@%u%%，duty %u/255）",
             channel, direction == 1 ? "进料" : "退料", ms / 1000.0,
             (unsigned)config_get_jog_speed_pct(),
             (unsigned)motor_pct_to_duty(config_get_jog_speed_pct()));
    cJSON_AddStringToObject(o, "info", info);
    return send_json_obj(req, o);
}

/**
 * 十一、辅助送料自检
 * ==========================================================================
 * ★ 为什么单独给一个接口（2026-09-23 现场反馈）：
 *   用户报"辅助送料没作用、电磁吸合了但电机不转"。辅助送料平时是**由打印机
 *   阶段（stg=8/19/0）驱动**的，想在机器上验证一次，得等下一轮打印轮到校准
 *   阶段 —— 十几分钟起步，还未必复现。这个按钮把这件事变成"点一下"：
 *
 *     · 手动点动（全速 duty=255）能转、辅助送料自检（duty 随网页设置）不转
 *         → 就是占空比不够，把网页上的辅助送料速度往上调（默认已改为 100%）
 *     · 两个都不转
 *         → 硬件链路：H 桥使能脚 / 离合齿轮打滑 / 接线，不是软件
 *     · 回读两路 LEDC 都是 0
 *         → 信号压根没写进硬件，是软件路径问题
 *
 * 三种结论对应三种完全不同的修法，而区分它们只需要这一次点击。
 */
static esp_err_t h_assist_test(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int channel = json_int(b, "channel", 0);   /* 物理料盘位 1~4；0 = 用当前通道 */
    cJSON_Delete(b);

    if (channel < 0 || channel > BOARD_CHANNEL_COUNT) {
        char info[64];
        snprintf(info, sizeof(info), "通道 %d 不存在", channel);
        return reply_ok(req, false, info);
    }

    if (ams_is_busy()) {
        char info[96];
        int busy_mat = ams_active_material();
        if (busy_mat >= 0) {
            snprintf(info, sizeof(info), "%s（料盘位%d）中，等它停下来再按",
                     ams_state_text(), busy_mat + 1);
        } else {
            snprintf(info, sizeof(info), "%s中，等它停下来再按", ams_state_text());
        }
        return reply_ok(req, false, info);
    }

    int material = (channel > 0) ? channel - 1 : -1;
    if (!ams_post_assist_test(material)) {
        return reply_ok(req, false, "总线正忙，稍后再按");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "running", true);
    char info[160];
    snprintf(info, sizeof(info),
             "已按辅助送料配置（%u%% × %ums）驱动一次，"
             "结果看「最近一次驱动」和日志",
             (unsigned)config_get_assist_speed_pct(),
             (unsigned)config_get_assist_ms());
    cJSON_AddStringToObject(o, "info", info);
    return send_json_obj(req, o);
}

/** POST /jog_set  body: {"seconds":1.0} 或 {"seconds":1.0,"speed_pct":60}
 *
 *  ★ 2026-09-23 现场要求："在这里加一个 PWM 值的设置，我来看看目测一下多少值
 *    可以转动电机和电磁。" —— 所以这个接口现在同时管两件事：响应时间 +
 *    点动 PWM。两个都是可选的，只传其中一个也行（没传的保持原值）。
 *
 *  PWM 是"能带动电机和离合的临界占空比"唯一的量法：一档一档往上试，
 *  每按一次点动都会打一条带 duty 原值 + 硬件回读的执行回执，配合听声音
 *  就能把临界值夹出来。找到之后，辅助送料/蠕动都要在它之上留余量。
 */
static esp_err_t h_jog_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    double seconds = 0;
    const cJSON *v = b ? cJSON_GetObjectItemCaseSensitive(b, "seconds") : NULL;
    if (v == NULL) {
        v = b ? cJSON_GetObjectItemCaseSensitive(b, "ms") : NULL;
    }
    if (cJSON_IsNumber(v)) {
        seconds = v->valuedouble;
        /* 如果调用方直接给了 ms（值大于 100），按毫秒理解 */
        if (seconds > 100) {
            seconds = seconds / 1000.0;
        }
    }
    /* 可选：PWM 占空比。没传 = -1 = 保持原值 */
    int want_pct = -1;
    const cJSON *vp = b ? cJSON_GetObjectItemCaseSensitive(b, "speed_pct") : NULL;
    if (cJSON_IsNumber(vp)) {
        want_pct = (int)vp->valuedouble;
    }
    cJSON_Delete(b);

    if (seconds <= 0 && want_pct < 0) {
        return reply_ok(req, false, "请填一个大于 0 的秒数（或给 speed_pct）");
    }

    int got_ms  = config_get_jog_ms();
    int got_pct = config_get_jog_speed_pct();
    bool clamped_ms  = false;
    bool clamped_pct = false;

    if (seconds > 0) {
        int want_ms = (int)(seconds * 1000.0 + 0.5);
        got_ms = config_set_jog_ms(want_ms);
        clamped_ms = (got_ms != want_ms);
    }
    if (want_pct >= 0) {
        got_pct = config_set_jog_speed_pct(want_pct);
        clamped_pct = (got_pct != want_pct);
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "jog_ms", got_ms);
    cJSON_AddNumberToObject(o, "jog_speed_pct", got_pct);
    char info[192];
    if (clamped_ms || clamped_pct) {
        snprintf(info, sizeof(info),
                 "已保存：%.1f 秒 @%d%%（填的值超出范围，已自动夹住 —— "
                 "时间 %.1f~%.1f 秒，PWM %d~%d%%）",
                 got_ms / 1000.0, got_pct,
                 CONFIG_JOG_MIN_MS / 1000.0, CONFIG_JOG_MAX_MS / 1000.0,
                 CONFIG_JOG_SPEED_MIN, CONFIG_JOG_SPEED_MAX);
    } else {
        snprintf(info, sizeof(info), "已保存：%.1f 秒 @%d%%（duty %u/255）",
                 got_ms / 1000.0, got_pct,
                 (unsigned)motor_pct_to_duty(got_pct));
    }
    cJSON_AddStringToObject(o, "info", info);
    ams_log("手动点动参数已更新：%dms @%d%%", got_ms, got_pct);
    return send_json_obj(req, o);
}

static esp_err_t h_stop(httpd_req_t *req)
{
    ams_log("网页要求紧急停止");
    ams_post_stop();
    /* 不等命令被处理 —— 但也立刻把离合断一次，让用户按下去马上有反应 */
    clutch_release_all();
    motor_stop();
    return reply_ok(req, true, "已停止：电机停转、全部离合断开");
}

/* ==========================================================================
 * 十、★ 本次新增：微动开关 / 自吸 / 蠕动参数 / 到位信号来源
 * ========================================================================== */

static esp_err_t h_sensor_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int channel = json_int(b, "channel", 0);      /* 1 起 */
    int enabled = json_int(b, "enabled", -1);
    cJSON_Delete(b);

    if (channel < 1 || channel > BOARD_CHANNEL_COUNT) {
        return reply_ok(req, false, "通道号不对");
    }
    if (enabled < 0) {
        return reply_ok(req, false, "缺少 enabled 参数");
    }

    esp_err_t err = sensor_set_channel_enabled(channel - 1, enabled != 0);
    if (err != ESP_OK) {
        return reply_ok(req, false, "保存失败");
    }

    /*
     * 说明一下这里为什么值得给个明确的提示：
     * 关掉某一路的微动之后，这一路就退回**降级模式** —— 按时间推进送料，
     * 时长由 AMS_NO_LIMIT_LOAD_MS / AMS_NO_LIMIT_RETRACT_MS 封顶。
     * 用户如果不知道这件事，会以为"关了微动功能就坏了"。
     */
    char info[160];
    if (enabled) {
        snprintf(info, sizeof(info),
                 "料盘位%d 已启用微动：停止送料触发即停，自吸触发即可全自动上料",
                 channel);
    } else {
        snprintf(info, sizeof(info),
                 "料盘位%d 已改为「无微动」模式：送料按时间推进"
                 "（进料 %d 秒 / 退料 %d 秒），需要按实际机构调整",
                 channel, AMS_NO_LIMIT_LOAD_MS / 1000,
                 AMS_NO_LIMIT_RETRACT_MS / 1000);
    }
    ams_log("料盘位%d 微动已%s", channel, enabled ? "启用" : "关闭");
    return reply_ok(req, true, info);
}

static esp_err_t h_autoload(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int channel = json_int(b, "channel", 0);      /* 1 起 */
    cJSON_Delete(b);

    if (channel < 1 || channel > BOARD_CHANNEL_COUNT) {
        return reply_ok(req, false, "通道号不对");
    }
    if (!ams_post_autoload(channel - 1)) {
        return reply_ok(req, false, "总线正忙，等当前动作结束再试");
    }
    ams_log("网页手动触发自吸上料：料盘位%d", channel);
    return reply_ok(req, true,
                    "已开始自吸上料：送料 → 等挤出机到位 → 蠕动送料收尾");
}

static esp_err_t h_creep_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    ams_config_t *cfg = config_get();

    int times = json_int(b, "times", cfg->creep_times);
    int pulse = json_int(b, "pulse_ms", cfg->creep_pulse_ms);
    int speed = json_int(b, "speed_pct", cfg->creep_speed_pct);
    cJSON_Delete(b);

    /* 夹到安全区间 —— 这几个值直接决定会不会把料顶坏 */
    if (times < CONFIG_CREEP_TIMES_MIN) times = CONFIG_CREEP_TIMES_MIN;
    if (times > CONFIG_CREEP_TIMES_MAX) times = CONFIG_CREEP_TIMES_MAX;
    if (pulse < CONFIG_CREEP_PULSE_MIN) pulse = CONFIG_CREEP_PULSE_MIN;
    if (pulse > CONFIG_CREEP_PULSE_MAX) pulse = CONFIG_CREEP_PULSE_MAX;
    if (speed < 5)   speed = 5;
    if (speed > 100) speed = 100;

    cfg->creep_times = (uint8_t)times;
    cfg->creep_pulse_ms = (uint16_t)pulse;
    cfg->creep_speed_pct = (uint8_t)speed;
    config_save();

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "times", times);
    cJSON_AddNumberToObject(o, "pulse_ms", pulse);
    cJSON_AddNumberToObject(o, "speed_pct", speed);
    char info[128];
    snprintf(info, sizeof(info), "已保存：蠕动 %d 次 × %dms @ %d%% 速度",
             times, pulse, speed);
    cJSON_AddStringToObject(o, "info", info);
    ams_log("蠕动参数已更新：%d 次 × %dms @ %d%%", times, pulse, speed);
    return send_json_obj(req, o);
}

static esp_err_t h_extruder_src_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    ams_config_t *cfg = config_get();
    int src = json_int(b, "src", cfg->extruder_src);
    cJSON_Delete(b);

    src = (src == EXTRUDER_SRC_MQTT) ? EXTRUDER_SRC_MQTT : EXTRUDER_SRC_GPIO;
    cfg->extruder_src = (uint8_t)src;
    config_save();

    const char *info = (src == EXTRUDER_SRC_GPIO)
        ? "已改为读取 GPIO 上的挤出机到位信号（自吸流程靠那根线判断）"
        : "已改为由打印机 MQTT 上报驱动（适合没接那根线的情况）";
    ams_log("挤出机到位信号来源已切换: %s",
            src == EXTRUDER_SRC_GPIO ? "GPIO" : "MQTT");
    return reply_ok(req, true, info);
}

/** POST /assist_set  body: {"enabled":0/1, "speed_pct":60, "ms":1500, "hold":1}
 *
 *  ★ 占空比和时长都要能改：真机上 23% 这种低占空比完全带不动电机，
 *    用户看到的现象是"打印中辅助送料电机没动作"。现场需要能在网页上
 *    一边调一边听电机声，而不是每次改都重烧固件。
 *
 *  ★ hold（2026-09-23 新增）：阶段内是否保持离合吸合。
 *    1（默认）= 校准/打印中离合全程吸着，只按周期脉冲电机 —— 离合不再
 *    每送一次就"吸合-断开"折腾一遍（run7 实测校准 3 分 15 秒咬了 97 次）。
 *    0 = 老行为，每次一整套吸合-断开。留这个开关是为了能一键退回旧行为：
 *    万一连续通电让离合发热/异响，用户自己能关掉，不用等我出新固件。
 *
 *  ⚠️ hold=0 用的是 -1 判"没传"，所以 hold=0 是**合法值**，不能被当成缺省。 */
static esp_err_t h_assist_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int enabled = json_int(b, "enabled", -1);
    int pct     = json_int(b, "speed_pct", -1);
    int ms      = json_int(b, "ms", -1);
    int hold    = json_int(b, "hold", -1);
    cJSON_Delete(b);

    if (enabled < 0 && pct < 0 && ms < 0 && hold < 0) {
        return reply_ok(req, false, "缺少参数");
    }

    if (enabled >= 0) {
        config_set_assist_enabled(enabled ? 1 : 0);
    }
    if (pct >= 0) {
        config_set_assist_speed_pct((uint8_t)pct);
    }
    if (ms >= 0) {
        config_set_assist_ms(ms);
    }
    if (hold >= 0) {
        config_set_assist_hold(hold ? 1 : 0);
    }

    char info[192];
    snprintf(info, sizeof(info), "辅助送料已保存：%s @%u%% × %ums，%s",
             config_get_assist_enabled() ? "开启" : "关闭",
             (unsigned)config_get_assist_speed_pct(),
             (unsigned)config_get_assist_ms(),
             config_get_assist_hold()
                 ? "阶段内保持离合吸合（只脉冲电机）"
                 : "每次吸合-断开");
    ams_log("%s", info);
    return reply_ok(req, true, info);
}

/** POST /retract_set
 *
 *  body: {"wait_ms":5000, "cont_ms":6000, "creep_ms":2000,
 *         "gap_ms":1000, "creep_max":3}
 *
 *  只传其中几个也可以，没传的保持原值。`creep_max = 0` 是**合法值**
 *  （表示"只做连续退料、不蠕动"），所以判"没传"用的是 -1 而不是 0。
 */
static esp_err_t h_retract_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int wait_ms   = json_int(b, "wait_ms", -1);
    int cont_ms   = json_int(b, "cont_ms", -1);
    int creep_ms  = json_int(b, "creep_ms", -1);
    int gap_ms    = json_int(b, "gap_ms", -1);
    int creep_max = json_int(b, "creep_max", -1);
    cJSON_Delete(b);

    if (wait_ms < 0 && cont_ms < 0 && creep_ms < 0 && gap_ms < 0 &&
        creep_max < 0) {
        return reply_ok(req, false, "缺少参数");
    }

    uint16_t got_wait  = config_get_retract_wait_ms();
    uint16_t got_cont  = config_get_retract_cont_ms();
    uint16_t got_creep = config_get_retract_creep_ms();
    uint16_t got_gap   = config_get_retract_gap_ms();
    uint8_t  got_max   = config_get_retract_creep_max();

    if (wait_ms   >= 0) { got_wait  = config_set_retract_wait_ms(wait_ms); }
    if (cont_ms   >= 0) { got_cont  = config_set_retract_cont_ms(cont_ms); }
    if (creep_ms  >= 0) { got_creep = config_set_retract_creep_ms(creep_ms); }
    if (gap_ms    >= 0) { got_gap   = config_set_retract_gap_ms(gap_ms); }
    if (creep_max >= 0) { got_max   = config_set_retract_creep_max(creep_max); }

    char info[160];
    snprintf(info, sizeof(info),
             "退料参数已保存：先连续 %ums，拉不出来再蠕动 %u 轮 × %ums（间隔 %ums）",
             (unsigned)got_cont, (unsigned)got_max,
             (unsigned)got_creep, (unsigned)got_gap);
    ams_log("%s", info);
    return reply_ok(req, true, info);
}

/** POST /temper_set  body: {"channel":1,"temper":250}
 *
 *  channel 从 **1** 开始，和界面上的「料盘位 1~4」一致（内部转成 0 起）；
 *  channel 传 0 表示"四个位一起设"，方便用户换整盘同种料。
 *
 *  为什么需要它：退料前要把热端升到这个温度（M109），太低退不出料、
 *  太高把 PLA 烤糊。以前这个值在源码里是写死的，改一次要重新烧固件。
 */
static esp_err_t h_temper_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int channel = json_int(b, "channel", 0);
    int temper  = json_int(b, "temper", -1);
    cJSON_Delete(b);

    if (temper < 0) {
        return reply_ok(req, false, "缺少 temper");
    }

    if (channel >= 1 && channel <= BOARD_CHANNEL_COUNT) {
        config_set_temper(channel - 1, temper);
    } else if (channel == 0) {
        for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
            config_set_temper(i, temper);
        }
    } else {
        return reply_ok(req, false, "料盘位超出范围");
    }

    /* 回显全部通道的生效值（config_set_temper 会夹到安全区间，
     * 所以回显的是"实际存进去的"，不是用户填的） */
    char info[192];
    int off = snprintf(info, sizeof(info), "换料温度已保存：");
    for (int i = 0; i < BOARD_CHANNEL_COUNT; i++) {
        if (off < 0 || off >= (int)sizeof(info) - 16) {
            break;
        }
        off += snprintf(info + off, sizeof(info) - (size_t)off, "%s%d℃",
                        i ? " / " : "", config_get_temper(i));
    }
    ams_log("%s", info);
    return reply_ok(req, true, info);
}

/* ==========================================================================
 * 十一、配置热点 / 启动计数
 * ========================================================================== */

static esp_err_t h_current_channel_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int channel = json_int(b, "channel", 0);
    cJSON_Delete(b);

    if (channel < 0 || channel > BOARD_CHANNEL_COUNT) {
        char info[96];
        snprintf(info, sizeof(info), "通道号无效（范围 0~%d）", BOARD_CHANNEL_COUNT);
        return reply_ok(req, false, info);
    }

    config_set_filament_current(channel);
    ams_log("当前通道已设为 %d", channel);
    return reply_ok(req, true, "当前通道已保存");
}

static esp_err_t h_ap_set(httpd_req_t *req)
{
    cJSON *b = read_body_json(req);
    int on = json_int(b, "on", -1);
    cJSON_Delete(b);

    if (on < 0) {
        return reply_ok(req, false, "缺少 on 参数");
    }

    esp_err_t err = on ? wifi_mgr_ap_start() : wifi_mgr_ap_stop();
    if (err != ESP_OK) {
        return reply_ok(req, false, esp_err_to_name(err));
    }

    /*
     * ★ 关热点这一条要给个明确的提醒。
     *   ESP32 是单射频：热点开着的时候，STA 只能关联到**和热点同信道**的
     *   路由器。所以"关掉热点"往往是把 WiFi 连上的关键一步，而不是单纯的
     *   "省电"。这一点不讲清楚，用户会在配网页面上反复试。
     */
    if (on) {
        char info[160];
        snprintf(info, sizeof(info),
                 "配置热点已打开：%s（%s）。用手机连上它就能配网",
                 config_get()->ap_ssid,
                 wifi_mgr_ap_ip()[0] ? wifi_mgr_ap_ip() : "192.168.4.1");
        return reply_ok(req, true, info);
    }
    return reply_ok(req, true,
                    "配置热点已关闭。若刚才一直连不上路由器，"
                    "多半就是它挡着（单射频下 STA 只能连同信道的 AP）");
}

static esp_err_t h_boot_clear(httpd_req_t *req)
{
    boot_count_clear();
    ams_log("启动计数已清零");
    return reply_ok(req, true, "启动计数已清零，现在拔电重插试试");
}

/**
 * 清零业务统计与离合冲突计数。
 *
 * 为什么值得单开一个接口：排查"离合冲突"时最烦的就是历史计数混在里面 ——
 * 修好之后再来看，还是显示 3 次，分不清是又犯了还是老账。
 * 清一下就能只看新发生的。
 */
static esp_err_t h_diag_reset(httpd_req_t *req)
{
    ams_reset_diag();
    return reply_ok(req, true, "统计已清零（换料/自吸计数、离合冲突、最近错误）");
}

/* ==========================================================================
 * 十二、★ OTA：这次能升级"整机固件"了
 * ==========================================================================
 * MicroPython 版只能更新 .ams 应用包，整机固件必须插 USB —— 因为它的分区表
 * 里只有一个 factory 应用分区，没有地方能安全地写"正在运行的自己"。
 *
 * ESP-IDF 这版有 ota_0 / ota_1：
 *   ① 新固件写进**当前没在跑的那一块**（esp_ota_get_next_update_partition）
 *   ② 全程校验（esp_ota_end 会验镜像头、长度、校验和）
 *   ③ 通过后把 otadata 指向新分区 → 重启生效
 * 写一半断电也不怕：旧分区完好，设备照常启动。这是原子升级。
 *
 * 而且第一次启动新固件如果崩溃，引导程序会自动回滚到上一版 —— MicroPython
 * 版完全没有这个能力。
 */
static void restart_task(void *arg)
{
    /* 等一会儿再重启，好让"上传成功"这个响应先发出去 */
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ams_log("准备重启，切到新固件…");
    esp_restart();
}

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        return reply_ok(req, false, "没有收到数据");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return reply_ok(req, false,
                        "找不到可写入的备用分区（分区表是不是被改过？）");
    }

    ams_log("开始 OTA：目标分区 %s（%u 字节可用），固件 %d 字节",
            target->label, (unsigned)target->size, req->content_len);

    /* 用堆缓冲，不占 httpd 任务的栈 */
    const size_t CHUNK = 4096;
    char *buf = malloc(CHUNK);
    if (buf == NULL) {
        return reply_ok(req, false, "内存不足");
    }

    esp_ota_handle_t ota = 0;
    bool began = false;
    bool bad_magic = false;      /* 首字节不是 0xE9，传的压根不是固件 */
    int remaining = req->content_len;
    int total = 0;
    esp_err_t err = ESP_OK;

    while (remaining > 0) {
        int want = remaining > (int)CHUNK ? (int)CHUNK : remaining;
        int got = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;                   /* 慢网络，接着等 */
        }
        if (got <= 0) {
            ams_log_err("OTA 接收中断（已收 %d / %d 字节）", total,
                        req->content_len);
            err = ESP_FAIL;
            break;
        }

        if (!began) {
            /* ★ 先用第一块的前几字节确认这确实是一个 ESP 镜像。
             *   ESP32 系列的镜像第一个字节固定是 0xE9，后面跟着芯片型号和
             *   段数。不做这个检查的话，用户把 .ams 包或者随便一个文件传
             *   上来，会一路写到 OTA 分区末尾才失败 —— 那时备用分区已经被
             *   写脏了，虽然还能回滚，但很没必要。 */
            if (buf[0] != (char)0xE9) {
                ams_log_err("上传的内容不是 ESP 固件（首字节 0x%02X，应为 0xE9）",
                            (unsigned char)buf[0]);
                bad_magic = true;
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            /* ★ 镜像大小传 OTA_SIZE_UNKNOWN，让 esp_ota 把备用分区整块擦掉。
             *
             *   这里**不要**写 OTA_WITH_SEQUENTIAL_WRITES：那是 ESP-IDF v3.x
             *   的旧名，v4.0 起已改名为 OTA_SIZE_UNKNOWN（值相同，都是
             *   (size_t)-1），新版头文件里已把它删掉 —— 留着会报
             *   "OTA_WITH_SEQUENTIAL_WRITES undeclared"，编译直接失败。 */
            err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota);
            if (err != ESP_OK) {
                ams_log_err("esp_ota_begin 失败: %s", esp_err_to_name(err));
                break;
            }
            began = true;
        }

        err = esp_ota_write(ota, buf, got);
        if (err != ESP_OK) {
            ams_log_err("写入 OTA 分区失败（偏移 %d）: %s", total,
                        esp_err_to_name(err));
            break;
        }

        total += got;
        remaining -= got;
    }
    free(buf);

    if (err != ESP_OK || total != req->content_len) {
        if (began) {
            esp_ota_abort(ota);
        }
        char info[200];
        if (bad_magic) {
            snprintf(info, sizeof(info),
                     "这不是固件文件。网页升级只能传整机固件（.bin），"
                     "也就是 idf.py build 出来的那个；"
                     "首字节不是 0xE9，已中止且未改动任何分区");
        } else {
            snprintf(info, sizeof(info),
                     "升级失败（已收 %d / %d 字节）：%s。"
                     "备用分区已回滚，当前固件照常运行",
                     total, req->content_len, esp_err_to_name(err));
        }
        ams_log_err("OTA 中止：%s", info);
        return reply_ok(req, false, info);
    }

    /* 校验 + 切换启动分区 */
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        char info[128];
        snprintf(info, sizeof(info),
                 "固件校验失败：%s。当前固件未受影响", esp_err_to_name(err));
        ams_log_err("%s", info);
        return reply_ok(req, false, info);
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        char info[128];
        snprintf(info, sizeof(info), "切换启动分区失败：%s",
                 esp_err_to_name(err));
        ams_log_err("%s", info);
        return reply_ok(req, false, info);
    }

    ams_log("OTA 完成：%d 字节已写入 %s，即将重启", total, target->label);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    char info[200];
    snprintf(info, sizeof(info),
             "升级成功：%d 字节已写入 %s，3 秒后自动重启。"
             "如果新固件启动异常，引导程序会自动回滚到当前版本",
             total, target->label);
    cJSON_AddStringToObject(o, "info", info);
    cJSON_AddNumberToObject(o, "bytes", total);
    cJSON_AddNumberToObject(o, "delay_ms", 3000);

    esp_err_t send_err = send_json_obj(req, o);

    /* 给响应留出发出去的时间，再重启 */
    xTaskCreate(restart_task, "ota_restart", 2048, (void *)(uintptr_t)3000, 5,
                NULL);
    (void)send_err;
    return ESP_OK;
}

/* ==========================================================================
 * 十三、注册路由
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * 请求计数
 * --------------------------------------------------------------------------
 * esp_http_server 没有"每个请求进来时通知一声"的钩子，所以给每个 handler
 * 套一层只做计数的薄壳，再把它注册进 URI 表。
 *
 * 这个计数不是装饰：网页打不开时，先看 /status 里的 req_count 有没有涨 ——
 *   涨了 → 服务是活的，问题在页面/浏览器侧
 *   不涨 → 请求根本没到设备，问题在网络/端口
 * 一句话就能把排查范围砍一半。
 */
#define DEF_COUNTED(fn)                                                        \
    static esp_err_t fn##_counted(httpd_req_t *r)                              \
    {                                                                          \
        s_req_count++;                                                         \
        return fn(r);                                                          \
    }

DEF_COUNTED(h_index)
DEF_COUNTED(h_appjs)
DEF_COUNTED(h_style)
DEF_COUNTED(h_status)
DEF_COUNTED(h_log)
DEF_COUNTED(h_get_mqtt_info)
DEF_COUNTED(h_boot_clear)
DEF_COUNTED(h_diag_reset)
DEF_COUNTED(h_wifi_scan)
DEF_COUNTED(h_wifi_connect)
DEF_COUNTED(h_mqtt_connect)
DEF_COUNTED(h_access_set)
DEF_COUNTED(h_ap_set)
DEF_COUNTED(h_current_channel_set)
DEF_COUNTED(h_hardware_test)
DEF_COUNTED(h_assist_test)
DEF_COUNTED(h_jog_set)
DEF_COUNTED(h_stop)
DEF_COUNTED(h_sensor_set)
DEF_COUNTED(h_autoload)
DEF_COUNTED(h_creep_set)
DEF_COUNTED(h_extruder_src_set)
DEF_COUNTED(h_assist_set)
DEF_COUNTED(h_retract_set)
DEF_COUNTED(h_temper_set)
DEF_COUNTED(h_ota_upload)

esp_err_t web_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    describe_reset_reason();
    boot_count_bump();
    pins_selfcheck();

    ams_log("本次复位原因: %s —— %s", s_reset_cause, s_reset_desc);
    if (s_pins_ok) {
        ams_log("引脚配置自检通过：全部输出脚都在安全引脚上");
    } else {
        ams_log_err("引脚配置自检**未通过**：%s", s_pins_problem);
    }
    if (s_boot_count > 5) {
        ams_log_warn("这是第 %u 次启动 —— 在反复重启。"
                     "请看「上电诊断」面板里的复位原因",
                     (unsigned)s_boot_count);
    }

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port = WEB_SERVER_PORT;
    conf.max_uri_handlers = 26;
    conf.lru_purge_enable = true;
    /* ★ 栈要够用：/wifi_scan 最坏要阻塞 2~3 秒、OTA 那个 handler 还要在栈上
     *   做临时拼接。（/wifi_connect 现在**不再**在 httpd 任务里等 20 秒了：
     *   它把连接交给 wifi_mgr_provision_connect() 起的独立任务，
     *   处理器立刻返回 —— 见那个函数的说明。） */
    conf.stack_size = 8192;
    conf.max_open_sockets = 7;
    conf.recv_wait_timeout = 15;
    conf.send_wait_timeout = 15;
    conf.keep_alive_enable = true;

    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        ams_log_err("启动 Web 服务失败: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        /* ---- 页面资源 ---- */
        { .uri = "/",          .method = HTTP_GET,  .handler = h_index_counted },
        { .uri = "/index.html",.method = HTTP_GET,  .handler = h_index_counted },
        { .uri = "/app.js",    .method = HTTP_GET,  .handler = h_appjs_counted },
        { .uri = "/style.css", .method = HTTP_GET,  .handler = h_style_counted },

        /* ---- 状态 ---- */
        { .uri = "/status",        .method = HTTP_GET, .handler = h_status_counted },
        { .uri = "/log",           .method = HTTP_GET, .handler = h_log_counted },
        { .uri = "/get_mqtt_info", .method = HTTP_GET, .handler = h_get_mqtt_info_counted },
        { .uri = "/boot_clear",    .method = HTTP_GET, .handler = h_boot_clear_counted },
        { .uri = "/diag_reset",    .method = HTTP_POST, .handler = h_diag_reset_counted },

        /* ---- 配置 ---- */
        { .uri = "/wifi_scan",    .method = HTTP_POST, .handler = h_wifi_scan_counted },
        { .uri = "/wifi_connect", .method = HTTP_POST, .handler = h_wifi_connect_counted },
        { .uri = "/mqtt_connect", .method = HTTP_POST, .handler = h_mqtt_connect_counted },
        { .uri = "/access_set",   .method = HTTP_POST, .handler = h_access_set_counted },
        { .uri = "/ap_set",       .method = HTTP_POST, .handler = h_ap_set_counted },
        { .uri = "/current_channel_set", .method = HTTP_POST, .handler = h_current_channel_set_counted },

        /* ---- 动作 ---- */
        { .uri = "/hardware_test", .method = HTTP_POST, .handler = h_hardware_test_counted },
        { .uri = "/assist_test",   .method = HTTP_POST, .handler = h_assist_test_counted },
        { .uri = "/jog_set",       .method = HTTP_POST, .handler = h_jog_set_counted },
        { .uri = "/stop",          .method = HTTP_POST, .handler = h_stop_counted },

        /* ---- 微动 / 自吸（本次新增） ---- */
        { .uri = "/sensor_set",       .method = HTTP_POST, .handler = h_sensor_set_counted },
        { .uri = "/autoload",         .method = HTTP_POST, .handler = h_autoload_counted },
        { .uri = "/creep_set",        .method = HTTP_POST, .handler = h_creep_set_counted },
        { .uri = "/extruder_src_set", .method = HTTP_POST, .handler = h_extruder_src_set_counted },

        /* ---- 辅助送料 / 退料参数（本次新增） ---- */
        { .uri = "/assist_set",       .method = HTTP_POST, .handler = h_assist_set_counted },
        { .uri = "/retract_set",      .method = HTTP_POST, .handler = h_retract_set_counted },
        { .uri = "/temper_set",       .method = HTTP_POST, .handler = h_temper_set_counted },

        /* ---- 升级 ---- */
        { .uri = "/ota_upload", .method = HTTP_POST, .handler = h_ota_upload_counted },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ams_log_err("注册接口 %s 失败: %s", uris[i].uri,
                        esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    ams_log("Web 服务已启动，监听 0.0.0.0:%d（%u 个接口）",
            WEB_SERVER_PORT,
            (unsigned)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ams_log("Web 服务已停止");
    }
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}

uint32_t web_server_request_count(void)
{
    return s_req_count;
}
