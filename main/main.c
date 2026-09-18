/**
 * main.c —— 启动流程
 * ============================================================================
 *
 * 这个文件只干一件事：**按正确的顺序把各个模块拉起来**。它不含任何业务逻辑。
 *
 * ----------------------------------------------------------------------------
 * ★ 启动顺序为什么是这个样子（每一步都有理由，改之前先读）
 * ----------------------------------------------------------------------------
 *
 *   0. NVS 初始化
 *      所有配置都存在 NVS 里，必须第一个起来。
 *
 *   1. 日志缓冲
 *      放在最前面，这样后面每个模块的启动日志都能进环形缓冲、在网页上看得到。
 *      （MicroPython 版是先建 Web 再建日志，结果启动阶段的东西全看不到。）
 *
 *   2. 配置
 *      wifi / MQTT / 通道映射 / 引脚相关的参数都要读它。
 *
 *   3. 引脚自检 + 打印接线表
 *      在初始化硬件**之前**做。接线表有错（重复占用、踩保留脚、strapping 脚
 *      当输出）的话，宁可在动电机之前就报出来。
 *
 *   4. 执行机构：电机 → 离合 → 微动
 *      顺序有讲究：
 *        · 电机先初始化 —— 它把两个 LEDC 通道的输出清成 0，保证上电不动；
 *        · 离合第二 —— 它一上来就把 4 路全部写成"断开"，绝不能带着上次的
 *          残留状态起跑；
 *        · 微动最后 —— 它只是输入，什么时候起都不影响安全。
 *
 *   5. AMS 控制器
 *      它会把电机/离合/微动/打印机上报接起来。**故意放在 WiFi 之前**：
 *      这样即使 WiFi 连不上，网页连热点也控制不了……所以真正的理由是这个 ——
 *      它会立刻开始轮询微动，用户一插料就能触发，不用等网络。
 *
 *   6. WiFi
 *      先尝试直连已保存的路由器；连不上（或压根没配过）才开配置热点。
 *
 *   7. Web 服务
 *      必须在 WiFi 之后 —— 它要把 socket 挂到 netif 上，netif 是 WiFi 建的。
 *
 *   8. 确认固件升级有效
 *      只有走到这里（前面全部成功）才调 esp_ota_mark_app_valid_cancel_rollback()。
 *      这一句是"回滚保险"的关键：新固件启动后如果不主动确认，下一次重启
 *      引导程序就会认为它有问题、自动切回上一版。
 *      放在最后是刻意的 —— 只有"WiFi 也起来了、网页也起来了"才算体检通过。
 *
 * ----------------------------------------------------------------------------
 * ★ 对比 MicroPython 版最大的不同：这里没有"堆碎片"这一说
 * ----------------------------------------------------------------------------
 * Python 版 main.py 的阶段划分（先开射频、再预建监听 socket、最后才 import
 * 应用）全部是为了绕开"GC 不压缩、最大连续块不够"这个物理限制。那种写法
 * 极其脆弱：往前面插一个 import 就可能让射频申请不到 24KB 连续内存而报
 * Wifi Unknown Error 0x0101。
 *
 * ESP-IDF 下 WiFi 驱动用的是**静态分配**的缓冲，编译期就定好了，不存在
 * "抢不到连续内存"的问题。所以这里的顺序完全可以按逻辑依赖来排，而不用
 * 按内存来排 —— 这就是重写的最大收益之一。
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"        /* CONFIG_IDF_TARGET / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ */

#include "esp_app_desc.h"
#include "esp_chip_info.h"    /* esp_chip_info() —— ★ v5.0 起不再由 esp_system.h 间接包含 */
#include "esp_idf_version.h"  /* ESP_IDF_VERSION / ESP_IDF_VERSION_VAL */
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

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
#include "web_server.h"
#include "wifi_manager.h"

/* ==========================================================================
 * 常量
 * ========================================================================== */

/** 开机等 WiFi 拿 IP 的最长时间（每存一组 SSID 各等这么久） */
#define BOOT_WIFI_TIMEOUT_MS 15000

/* ==========================================================================
 * 一、启动横幅
 * ========================================================================== */

static void print_banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();

    ams_log("============================================================");
    ams_log("  ESP-AMS-S3  ——  拓竹打印机自动换料系统（ESP-IDF 版）");
    ams_log("  共享电机 + 4 路电磁离合 | 每路 3 个微动");
    ams_log("============================================================");
    ams_log("  版本    : %s", app ? app->version : "?");
    ams_log("  编译    : %s %s", app ? app->date : "?", app ? app->time : "?");
    ams_log("  IDF     : %s", app ? app->idf_ver : "?");
    ams_log("  运行分区: %s", part ? part->label : "?");
    /* ★ 芯片信息必须用 esp_chip_info() 取，不存在 esp_get_revision() 这个 API。
     *   chip.revision 的格式随 IDF 版本变过：v5.0 起是 100*大版本 + 小版本，
     *   v4.x 只有大版本号。这里按版本分支处理，免得打出 "rev0.1" 这种怪值。 */
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    unsigned chip_major = (unsigned)(chip.revision / 100);
    unsigned chip_minor = (unsigned)(chip.revision % 100);
#else
    unsigned chip_major = (unsigned)chip.revision;
    unsigned chip_minor = 0;
#endif
    ams_log("  芯片    : %s rev%u.%u，%d 核 @ %dMHz",
            CONFIG_IDF_TARGET, chip_major, chip_minor, chip.cores,
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ams_log("  空闲内存: %u 字节", (unsigned)esp_get_free_heap_size());
    ams_log("  复位原因: %d", (int)esp_reset_reason());
    ams_log("============================================================");
}

/**
 * 打印接线表 + 跑引脚自检。
 *
 * 这一段是从 MicroPython 版搬过来的，因为它的价值被反复验证过：
 * 用户第一次接完线通电，最需要的不是"启动成功"，而是
 * **"我接的到底对不对、哪只脚接到了哪儿"**。
 */
static bool print_wiring_and_check(void)
{
    char buf[1400];
    board_pins_describe(buf, sizeof(buf));
    ams_log("%s", buf);

    char err[160] = {0};
    if (board_pins_validate(err, sizeof(err))) {
        ams_log("引脚配置自检通过：全部输出脚都在安全引脚上");
        return true;
    }

    ams_log_err("========================================================");
    ams_log_err("引脚配置自检未通过：");
    ams_log_err("%s", err);
    ams_log_err("请修改 main/board_pins.h 后重新编译。");
    ams_log_err("（Strapping 脚当输出用会导致上电启动模式被改掉，");
    ams_log_err("  典型现象是反复重启、网页打不开、电机一直叫。）");
    ams_log_err("========================================================");
    /* ★ 只报警不中断：让用户能连上网页、看到这条错误、再决定怎么改。
     *   直接停机反而让人无从下手（MicroPython 版就是这么做的）。 */
    return false;
}

/* ==========================================================================
 * 二、网络
 * ========================================================================== */

static void start_network(void)
{
    esp_err_t err = wifi_mgr_init();
    if (err != ESP_OK) {
        ams_log_err("WiFi 初始化失败：%s —— 网页将无法访问",
                    esp_err_to_name(err));
        return;
    }

    /* ---- 先试直连（不扫描） ---- */
    bool connected = wifi_mgr_auto_connect(BOOT_WIFI_TIMEOUT_MS);

    if (connected) {
        ams_log("========================================================");
        ams_log("已联网: %s   IP = %s   信号 %d dBm",
                wifi_mgr_current_ssid(), wifi_mgr_sta_ip(), wifi_mgr_rssi());
        ams_log("同一网络下用浏览器打开  http://%s", wifi_mgr_sta_ip());
        ams_log("或者（支持 mDNS 的设备）  http://ams.local");
        ams_log("========================================================");
        /* ★ 连上路由器时**不开**热点。
         *   单射频下 AP+STA 并存要分时复用，会明显拖慢吞吐 ——
         *   而这个网页有 20KB 上下，快慢差别是能感觉到的。
         *   需要重新配网时，在网页上点「打开配置热点」即可（ESP-IDF 下
         *   随时开关都安全，不像 MicroPython 那版在碎堆上开热点会直接复位）。 */
        return;
    }

    /* ---- 直连失败 → 转配网模式 ---- */
    ams_log_warn("没有可用的 WiFi 记录或全部连不上，转入配网模式");
    if (wifi_mgr_ap_start() == ESP_OK) {
        ams_log("========================================================");
        ams_log("已打开配置热点: %s", config_get()->ap_ssid);
        ams_log("手机连上这个热点，浏览器打开 http://%s",
                wifi_mgr_ap_ip()[0] ? wifi_mgr_ap_ip() : "192.168.4.1");
        ams_log("========================================================");
    } else {
        ams_log_err("配置热点也起不来，设备将处于离线状态");
    }
}

/* ==========================================================================
 * 三、主函数
 * ========================================================================== */

void app_main(void)
{
    /* ---- 0. NVS ---- */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区写满或版本升级：擦掉重建。这是 ESP-IDF 的标准处理，
         * 代价是配置回到默认值（会在日志里说明） */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        /* NVS 起不来不直接 abort —— 继续跑，config 会用一份内存里的默认值，
         * 至少用户还能连上网页看到"配置存不下来"这件事 */
        printf("[ams] NVS 初始化失败: %s\n", esp_err_to_name(err));
    }

    /* ---- 1. 日志缓冲（越早越好） ---- */
    ams_log_init();

    /* ---- 2. 配置 ---- */
    config_init();

    /* ---- 3. 横幅 + 接线表 + 引脚自检 ---- */
    print_banner();
    print_wiring_and_check();

    {
        char desc[640];
        config_describe(desc, sizeof(desc));
        ams_log("%s", desc);
    }

    /* ---- 4. 执行机构（顺序见文件头说明） ---- */
    if (motor_init() != ESP_OK) {
        ams_log_err("电机初始化失败，手动与自动送料都将不可用");
    }
    if (clutch_init() != ESP_OK) {
        ams_log_err("电磁离合初始化失败 —— 出于安全考虑，"
                    "后续所有送料动作都会被拒绝");
    } else {
        /* 开机先确保全断开。这一句是"绝不带着残留状态起跑"的落实点。 */
        clutch_release_all_settled();
    }
    if (sensor_init() != ESP_OK) {
        ams_log_err("微动初始化失败，各通道将走降级模式（按时间推进送料）");
    }

    /* ---- 5. AMS 控制器 ----
     * ams_init() 内部会建 MQTT 客户端（打印机参数没配全时只是建而不连，
     * 这是正常状态）。esp-mqtt 每 5 秒自己重连一次，所以这时候 WiFi
     * 还没起来也没关系。 */
    if (ams_init() != ESP_OK) {
        ams_log_err("AMS 控制器初始化失败");
    } else {
        if (ams_start() != ESP_OK) {
            ams_log_err("AMS 任务启动失败");
        } else {
            ams_log("AMS 任务已启动");
        }
    }

    /* ---- 6. 网络 ---- */
    start_network();

    /* ---- 7. Web 服务 ---- */
    bool web_ok = (web_server_start() == ESP_OK);

    /* ---- 8. 升级体检 ----
     * ★ 这一句不能省。sdkconfig 里开了
     *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE，新固件启动后如果不主动
     *   "确认有效"，下一次重启引导程序就认为它有问题、自动切回上一版 ——
     *   现象是"升级成功了，但一重启又变回旧版本"，非常难查。
     *
     *   放在这里是刻意的：只有 WiFi/网页这条链路真的起来了才确认。
     *   如果新固件把网页写坏了，就不会确认 → 下次重启自动回滚。
     *   这正是双分区 OTA 最值钱的那个特性。 */
    if (web_ok) {
        esp_err_t ota_err = esp_ota_mark_app_valid_cancel_rollback();
        if (ota_err == ESP_OK) {
            ams_log("固件体检通过，已确认本次升级有效（不会再回滚）");
        }
    } else {
        ams_log_err("Web 服务启动失败 —— **不**确认本次升级，"
                    "重启后将自动回滚到上一版固件");
    }

    /* ---- 9. 收尾 ---- */
    if (web_ok) {
        const char *ip = wifi_mgr_sta_ip();
        if (ip[0]) {
            ams_log("连上同一网络后用浏览器访问 http://%s  （或 http://ams.local）", ip);
        } else if (wifi_mgr_ap_is_on()) {
            ams_log("连上热点 %s 后用浏览器访问 http://%s",
                    config_get()->ap_ssid,
                    wifi_mgr_ap_ip()[0] ? wifi_mgr_ap_ip() : "192.168.4.1");
        }
    }

    ams_log("启动完成，用时 %d ms。AMS 已在后台运行，"
            "微动触发、打印机换料请求、网页操作都会即时响应。",
            (int)(esp_timer_get_time() / 1000));

    /* 注意：app_main 返回后 FreeRTOS 会回收这个任务的栈，
     * 所有常驻工作都在各自的 FreeRTOS 任务里：
     *   ams_task      —— 换料状态机、微动轮询、状态灯
     *   httpd         —— Web 服务
     *   mqtt_task     —— 打印机通信（esp-mqtt 自带）
     *   esp_timer     —— 微动去抖（5ms 周期）
     * 诊断期间 ams_task 每 100 轮会打一条心跳日志，可以判断它是否活着。
     * 如果 ams_task 卡死（比如卡在 motor_run 或 sensor 查询），
     * 网页仍然可以访问，但看不到任何换料动作。
     */
}
