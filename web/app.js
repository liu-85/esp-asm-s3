/* ===========================================================================
   ESP-AMS-S3 配置界面前端
   ===========================================================================
   设计原则（对着上一代踩过的坑定的）：

   1. **先画界面，再等数据。** 进页面立刻把 4 个通道、按钮全画出来，
      不等 /status 回来。上一版是"接口回来才渲染"，接口慢一点整页就停在
      "加载中…"，观感像是坏了。

   2. **只轮询一个接口。** 每 2 秒只请求 /status 一次，所有面板都从它取数据。
      分 7 个接口轮询的话，既费流量（手机上很关键），还会出现各面板数据
      新旧不一。

   3. **失败要说话。** 请求失败时顶栏明确提示"状态接口无响应，正在重试…"，
      而不是静默失败让用户以为设备坏了。

   4. **动作用 POST，读用 GET。** 而且动作类请求都会拿到「立刻回包」——
      设备端把动作丢进队列就返回，不会转圈等好几秒。
   =========================================================================== */

(function () {
    'use strict';

    /* =======================================================================
       基础工具
       ======================================================================= */
    function $(id) { return document.getElementById(id); }

    function esc(s) {
        return String(s === null || s === undefined ? '' : s)
            .replace(/&/g, '&amp;').replace(/</g, '&lt;')
            .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    }

    var toastTimer = null;
    function toast(msg, kind) {
        var el = $('toast');
        el.textContent = msg;
        el.className = 'toast show' + (kind ? ' ' + kind : '');
        if (toastTimer) clearTimeout(toastTimer);
        toastTimer = setTimeout(function () { el.className = 'toast'; }, 3200);
    }

    var busyCount = 0;
    function busy(on) {
        busyCount += on ? 1 : -1;
        if (busyCount < 0) busyCount = 0;
        $('overlay').className = busyCount > 0 ? 'overlay show' : 'overlay';
    }

    function store(key, val) {
        try {
            if (val === undefined) return localStorage.getItem(key);
            localStorage.setItem(key, val);
        } catch (e) { /* 隐私模式下标不上，忽略 */ }
        return null;
    }

    /** POST JSON，带回包解析 */
    function post(path, data, cb) {
        fetch(path, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data || {})
        })
        .then(function (r) { return r.text(); })
        .then(function (t) {
            var d = null;
            try { d = JSON.parse(t); } catch (e) { d = null; }
            if (cb) cb(d);
        })
        .catch(function () {
            toast('请求失败，设备可能正忙', 'bad');
            if (cb) cb(null);
        });
    }

    /** GET JSON */
    function get(path, cb, silent) {
        fetch(path, { method: 'GET', cache: 'no-store' })
        .then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.json();
        })
        .then(cb)
        .catch(function () {
            if (!silent) toast('读取失败，请刷新页面', 'bad');
        });
    }

    function fmtUptime(ms) {
        if (!ms && ms !== 0) return '-';
        var s = Math.floor(ms / 1000);
        var d = Math.floor(s / 86400);
        var h = Math.floor((s % 86400) / 3600);
        var m = Math.floor((s % 3600) / 60);
        var ss = s % 60;
        if (d > 0) return d + ' 天 ' + h + ' 小时 ' + m + ' 分';
        if (h > 0) return h + ' 小时 ' + m + ' 分 ' + ss + ' 秒';
        if (m > 0) return m + ' 分 ' + ss + ' 秒';
        return ss + ' 秒';
    }

    function fmtKB(bytes) {
        if (typeof bytes !== 'number' || bytes < 0) return '-';
        return (bytes / 1024).toFixed(1) + ' KB';
    }

    /* =======================================================================
       菜单与分页
       ======================================================================= */
    var PAGES = [
        ['status',    '运行状态', '1'],
        ['diag',      '上电诊断', '2'],
        ['wifi',      'WiFi 配置', '3'],
        ['mqtt',      '打印机配置', '4'],
        ['access',    '通道设置', '5'],
        ['hardware',  '硬件调试', '6'],
        ['sensors',   '微动与自吸', '7'],
        ['ota',       '系统升级', '8']
    ];
    var PAGE_KEY = 'ams.page';

    function buildMenu() {
        var nav = $('menu');
        nav.innerHTML = '';
        PAGES.forEach(function (p) {
            var a = document.createElement('a');
            a.href = 'javascript:void(0)';
            a.setAttribute('data-page', p[0]);
            a.innerHTML = '<span>' + esc(p[1]) + '</span>' +
                          '<span class="n">' + p[2] + '</span>';
            a.onclick = function () { showPage(p[0]); toggleMenu(false); };
            nav.appendChild(a);
        });
    }

    function showPage(name) {
        PAGES.forEach(function (p) {
            var sec = $('page_' + p[0]);
            if (sec) sec.className = 'page' + (p[0] === name ? ' on' : '');
        });
        var links = $('menu').getElementsByTagName('a');
        for (var i = 0; i < links.length; i++) {
            var on = links[i].getAttribute('data-page') === name;
            links[i].className = on ? 'on' : '';
        }
        store(PAGE_KEY, name);
        if (name !== 'status') refresh();
    }

    function toggleMenu(force) {
        var side = $('side');
        var open = (force === undefined) ? !side.classList.contains('open') : !!force;
        side.className = 'side' + (open ? ' open' : '');
        $('scrim').className = 'scrim' + (open ? ' show' : '');
    }

    /* =======================================================================
       状态渲染
       ======================================================================= */
    var apOn = false;
    var curSsid = '';
    var accessFromDevice = false;
    var lastAccessSig = '';
    var jogMs = 1000;
    var logText = null;

    function dot(el, on) { el.className = 'dot' + (on ? ' on' : ''); }

    function setAlert(msg) {
        var el = $('alertbar');
        el.textContent = msg || '';
        el.className = 'alertbar' + (msg ? ' show' : '');
    }

    function applyStatus(d) {
        d = d || {};
        var wifiOn = !!d.wifi_isconnected;
        var mqttOn = !!d.is_mqtt_con;
        apOn = !!d.ap_on;
        curSsid = wifiOn ? (d.wifi_ssid || '') : '';

        $('brand_net').textContent = wifiOn ? '已联网' : (apOn ? '热点模式' : '离线');
        dot($('dot_wifi'), wifiOn);
        dot($('dot_mqtt'), mqttOn);
        $('top_wifi').textContent = wifiOn
            ? (d.wifi_ssid || 'WiFi 已连接')
            : (apOn ? '配网热点' : '未连接');
        $('top_mqtt').textContent = mqttOn ? 'MQTT 已连接' : 'MQTT 未连接';

        /* ---- 运行状态 ---- */
        $('s_ip').textContent = d.ip ? d.ip
            : (apOn ? '—（请用 ' + (d.ap_ip || '192.168.4.1') + ' 访问）' : '未获取');

        $('s_wifi').innerHTML = wifiOn
            ? '<span class="ok">已连接 ' + esc(d.wifi_ssid || '') + '</span>'
            : '<span class="warn">未连接' +
              (d.wifi_status_text ? '（' + esc(d.wifi_status_text) + '）' : '') +
              '</span>';

        $('s_rssi').innerHTML = wifiOn
            ? (d.rssi + ' dBm' + (d.rssi > -60 ? ' <span class="ok">（很好）</span>'
                               : d.rssi > -75 ? ' <span class="dim">（一般）</span>'
                               : ' <span class="warn">（偏弱）</span>'))
            : '-';
        $('s_mac').textContent = d.mac || '-';

        /* MQTT 未连上时要区分"配置在、后台一直在重试"和"压根没配" */
        if (mqttOn) {
            $('s_mqtt').innerHTML = '<span class="ok">已连接</span>' +
                (d.mqtt_rx ? '<span class="dim">（已收 ' + d.mqtt_rx + ' 条）</span>' : '');
        } else if (d.mqtt_configured) {
            $('s_mqtt').innerHTML = '<span class="warn">未连接（配置已保存，后台重试中）</span>';
        } else {
            $('s_mqtt').innerHTML = '<span class="warn">未连接（还没配打印机参数）</span>';
        }
        if (!mqttOn && d.mqtt_last_error) {
            $('s_mqtt').innerHTML += '<span class="err"> · ' + esc(d.mqtt_last_error) + '</span>';
        }

        $('s_ap').textContent = apOn
            ? ('已打开 ' + esc(d.ap_ssid || 'AMS_WIFI') +
               (d.ap_clients ? '（' + d.ap_clients + ' 台设备）' : ''))
            : '已关闭';
        var cur = d.current_access || 0;
        $('s_cur').textContent = cur ? ('通道 ' + cur) : '未知';
        $('s_ver').textContent = (d.app_version || '?') +
            ' @ ' + (d.part || '?');
        $('st_head').textContent = d.ip ? d.ip : (apOn ? (d.ap_ip || '') : '');

        var apBtn = $('btn_ap');
        apBtn.textContent = apOn ? '关闭配置热点' : '打开配置热点';

        /* 顶栏提示：把"为什么连不上/为什么控制不了"直接说出来 */
        if (!wifiOn && apOn) {
            setAlert('设备在热点模式，注意热点开着时 STA 连不上不同信道的路由器');
        } else if (wifiOn && apOn) {
            setAlert('热点开着 —— 若 WiFi 连不上，先关掉它');
        } else {
            setAlert('');
        }

        /* ---- AMS 业务 ---- */
        var ams = d.ams || {};
        $('a_state').innerHTML = ams.busy
            ? '<span class="ok">' + esc(ams.state_text || '') + '</span>'
            : esc(ams.state_text || '-');
        var am = ams.active_material;
        $('a_material').textContent = (typeof am === 'number' && am >= 0)
            ? ('料盘位 ' + (am + 1)) : '无';

        var dg = ams.diag || {};
        $('a_exchange').innerHTML = '成功 ' + (dg.exchange_ok || 0) +
            ' 次' + ((dg.exchange_fail || 0) > 0
                ? ' / <span class="err">失败 ' + dg.exchange_fail + ' 次</span>' : '');
        $('a_autoload').innerHTML = '成功 ' + (dg.autoload_ok || 0) +
            ' 次' + ((dg.autoload_fail || 0) > 0
                ? ' / <span class="err">失败 ' + dg.autoload_fail + ' 次</span>' : '');

        if (dg.last_error && dg.last_error_ms >= 0) {
            $('a_lasterr').innerHTML = '<span class="err">' + esc(dg.last_error) +
                '</span><span class="dim">（' + fmtUptime(dg.last_error_ms) + ' 前）</span>';
        } else {
            $('a_lasterr').innerHTML = '<span class="ok">无</span>';
        }

        /* ---- WiFi 页 ---- */
        $('wifi_head').textContent = apOn
            ? ('热点 ' + (d.ap_ssid || 'AMS_WIFI') + ' · ' + (d.ap_ip || '192.168.4.1'))
            : '（可连路由器，也可开热点配网）';
        $('w_apssid').textContent = d.ap_ssid || 'AMS_WIFI';
        $('w_apip').textContent = d.ap_ip || '192.168.4.1';
        $('w_apcli').textContent = apOn ? ((d.ap_clients || 0) + ' 台') : '（热点未开）';

        renderWifiList(d.ssids || [], curSsid);

        /* ---- MQTT 页 ---- */
        $('mqtt_head').textContent = mqttOn ? '已连接' : '未连接';
        var saved = $('mqtt_saved');
        saved.innerHTML = d.mqtt_configured
            ? '<span class="ok">已保存</span>'
            : '<span class="warn">尚未保存</span>';

        /* ---- 诊断页 ---- */
        var r = d.reset || {};
        var bad = (r.cause === 'BROWNOUT' || r.cause === 'TASK_WDT' ||
                   r.cause === 'INT_WDT' || r.cause === 'PANIC' ||
                   r.cause === 'WDT');
        $('d_cause').innerHTML = bad
            ? '<span class="err">' + esc(r.cause || '未知') + '</span>'
            : esc(r.cause || '未知');
        $('d_cause_desc').textContent = r.cause_desc || '-';
        var boots = r.boot_count || 0;
        $('d_boots').innerHTML = boots > 5
            ? '<span class="err">' + boots + ' 次 · 在反复重启</span>'
            : (boots + ' 次');
        $('d_uptime').textContent = fmtUptime(r.uptime_ms);
        var mem = (typeof d.mem_free === 'number') ? d.mem_free : -1;
        $('d_mem').innerHTML = mem < 0 ? '未知'
            : (mem < 30720 ? '<span class="warn">' + fmtKB(mem) + ' · 偏低</span>'
                           : fmtKB(mem));
        $('d_mem_min').textContent = fmtKB(d.mem_min_free);
        $('d_part').textContent = d.part || '-';
        $('d_build').textContent = (d.build_date || '?') + ' ' + (d.build_time || '');

        var safety = d.boot_safety || {};
        var box = $('d_pins');
        if (safety.ok === false) {
            var items = (safety.problems || []).map(function (p) {
                return '<li>' + esc(p) + '</li>';
            }).join('');
            box.innerHTML = '<div class="kv"><span>引脚自检</span>' +
                '<b class="err">未通过</b></div>' +
                '<ul style="margin:6px 0 0 18px;padding:0;font-size:13px">' + items + '</ul>';
        } else {
            box.innerHTML = '<div class="kv"><span>引脚自检</span>' +
                '<b class="ok">通过</b></div>';
        }
        var board = d.board || {};
        if (board.name) {
            $('brand_name').textContent = board.name;
            /* OTA 提示文案按板型动态显示正确的文件名 */
            var tip = $('ota_tip');
            if (tip) {
                var chip = board.name.indexOf('C3') >= 0 ? 'c3' : 's3';
                tip.innerHTML = '要传的是 <code>idf.py build</code> 出来的 ' +
                    '<code>esp-ams-' + chip + '.bin</code>' +
                    '（或 <code>build/esp-ams-' + chip + '.bin</code>）。' +
                    '传错文件不会造成损坏 —— 设备会检查首字节是不是 <code>0xE9</code>，' +
                    '不是就直接中止，分区一个字节都不动。';
            }
        }
        if (board.spare_pins && board.spare_pins.length) {
            var sp = board.spare_pins.map(function (p) {
                return '<span class="badge" title="' + esc(p.note) + '">GPIO' + p.pin + '</span>';
            }).join('');
            box.innerHTML += '<div class="kv" style="display:block"><span>剩余可用 IO</span>' +
                '<div class="badges">' + sp + '</div></div>';
        }

        /* ---- 通道设置 ---- */
        var al = d.access_list || [], cl = d.color_list || [];
        if (al.length) {
            var sig = al.join(',') + '|' + cl.join(',') + '|' + cur;
            if (!accessFromDevice || sig !== lastAccessSig) {
                if (!accessFromDevice) { renderAccess(al, cl, cur); accessFromDevice = true; }
                else { updateCurrentOnly(al, cur); }
                lastAccessSig = sig;
            }
        } else if (!accessFromDevice) {
            renderAccess([1, 2, 3, 4], DEFAULT_COLORS, cur);
        }

        /* ---- 点动时长 ---- */
        if (typeof d.jog_ms === 'number' && d.jog_ms > 0) syncJogInput(d.jog_ms);

        /* ---- 硬件 ---- */
        updateHardware(d.hardware);

        /* ---- 微动与自吸 ---- */
        updateSensors(d);

        /* ---- 升级页 ---- */
        $('o_part').textContent = d.part || '-';
        $('o_ver').textContent = (d.app_version || '?') +
            '（编译于 ' + (d.build_date || '?') + '）';
    }

    /* =======================================================================
       通道设置
       ======================================================================= */
    var DEFAULT_COLORS = [0xd94f4f, 0x3f9d4f, 0x3f6fd9, 0xd9a93f];

    function rgbToHex(v) {
        if (typeof v !== 'number') return '#3f6fd9';
        return '#' + ('000000' + (v >>> 0).toString(16)).slice(-6);
    }
    function hexToNum(hex) {
        return parseInt(hex.replace('#', ''), 16) >>> 0;
    }

    function renderAccess(access, colors, cur) {
        var box = $('access_info');
        var html = '<div class="acc-head"><span>料盘位</span>' +
                   '<span>打印机通道</span><span>颜色</span></div>';

        for (var i = 0; i < access.length; i++) {
            var isCur = (access[i] === cur);
            html += '<div class="acc-row" data-idx="' + i + '">' +
                '<span>料盘位 ' + (i + 1) +
                (isCur ? ' <span class="cur">● 使用中</span>' : '') + '</span>' +
                '<select class="acc-sel" data-idx="' + i + '">';
            for (var c = 1; c <= access.length; c++) {
                html += '<option value="' + c + '"' +
                        (access[i] === c ? ' selected' : '') + '>通道 ' + c + '</option>';
            }
            html += '</select>' +
                '<input type="color" class="swatch acc-col" data-idx="' + i + '" value="' +
                rgbToHex(colors[i] !== undefined ? colors[i] : DEFAULT_COLORS[i]) + '">' +
                '</div>';
        }
        box.innerHTML = html;
    }

    /** 只更新"使用中"标记，不重建整块 —— 否则用户正在改的下拉框会被冲掉 */
    function updateCurrentOnly(access, cur) {
        var rows = $('access_info').getElementsByClassName('acc-row');
        for (var i = 0; i < rows.length; i++) {
            var span = rows[i].getElementsByTagName('span')[0];
            if (!span) continue;
            var idx = parseInt(rows[i].getAttribute('data-idx'), 10);
            var isCur = (access[idx] === cur);
            var txt = '料盘位 ' + (idx + 1) +
                      (isCur ? ' <span class="cur">● 使用中</span>' : '');
            if (span.innerHTML !== txt) span.innerHTML = txt;
        }
    }

    function saveAccess() {
        var sels = $('access_info').getElementsByClassName('acc-sel');
        var cols = $('access_info').getElementsByClassName('acc-col');
        var list = [], colors = [];
        for (var i = 0; i < sels.length; i++) list.push(parseInt(sels[i].value, 10));
        for (var j = 0; j < cols.length; j++) colors.push(hexToNum(cols[j].value));

        post('/access_set', { access_list: list, color_list: colors }, function (d) {
            accessFromDevice = false;
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* =======================================================================
       硬件调试
       ======================================================================= */
    var hwSig = '';

    function updateHardware(hw) {
        hw = hw || {};
        var channels = (hw.channels && hw.channels.length) ? hw.channels : [1, 2, 3, 4];
        var eng = hw.engaged || [];
        var limits = hw.limits || [];
        var busyNow = !!hw.busy;

        var html = '';
        if (busyNow) {
            html += '<div class="kv"><span>总线状态</span><b class="ok">正在动作' +
                    (hw.active_channel ? '（通道' + hw.active_channel + '）' : '') +
                    ' · 结束后自动断开</b></div>';
        }
        html += '<div class="kv"><span>进退响应时间</span><b>' +
                (jogMs / 1000).toFixed(1) + ' 秒（4 个通道统一）</b></div>';
        var md = (hw.motor_direction === undefined) ? 0 : hw.motor_direction;
        html += '<div class="kv"><span>电机方向</span><b>' + md +
                '（1=进料　-1=退料　0=停止）</b></div>';
        var cf = hw.conflicts || 0;
        html += '<div class="kv"><span>离合冲突次数</span><b' +
                (cf ? ' class="err"' : ' class="ok"') + '>' + cf + '</b></div>';

        html += '<div class="dim" style="margin-top:8px;font-size:13px">离合状态</div>' +
                '<div class="badges">';
        for (var i = 0; i < channels.length; i++) {
            var ch = channels[i];
            var on = eng.indexOf(ch) >= 0;
            html += '<span class="badge' + (on ? ' on' : '') + '">离合' + ch +
                    (on ? ' 吸合' : ' 断开') +
                    (limits[i] ? ' · 有微动' : ' · 无微动') + '</span>';
        }
        html += '</div>';
        if (eng.length > 1) {
            html += '<div class="err" style="margin-top:6px">' +
                    '检测到多路离合同时吸合，请立即断电检查驱动电路！</div>';
        }
        $('hardware_info').innerHTML = html;

        /* 点动按钮：只在通道结构变化时重建，避免每 2 秒重建一遍 DOM */
        var sig = channels.join(',') + '|' + (busyNow ? '1' : '0');
        if (sig !== hwSig) {
            hwSig = sig;
            var box = $('hardware_test');
            var h = '<div class="dim" style="font-size:13px">单通道点动（按住概念：按一次动一次）</div>';
            for (var k = 0; k < channels.length; k++) {
                h += '<div class="row" style="margin-top:8px">' +
                     '<span style="flex:0 0 66px;align-self:center">通道' + channels[k] + '</span>' +
                     '<button data-ch="' + channels[k] + '" data-dir="1" class="hw-feed"' +
                     (busyNow ? ' disabled' : '') + ' style="flex:1">进料</button>' +
                     '<button data-ch="' + channels[k] + '" data-dir="-1" class="hw-back"' +
                     (busyNow ? ' disabled' : '') + ' style="flex:1">退料</button>' +
                     '</div>';
            }
            box.innerHTML = h;
            var feeds = box.getElementsByClassName('hw-feed');
            var backs = box.getElementsByClassName('hw-back');
            function bind(el, dir) {
                for (var m = 0; m < el.length; m++) {
                    el[m].onclick = function () {
                        hardwareTest(parseInt(this.getAttribute('data-ch'), 10), dir);
                    };
                }
            }
            bind(feeds, 1);
            bind(backs, -1);
        }
    }

    function hardwareTest(channel, direction) {
        /* times_ms 故意不传：时长以设备上的统一设置为准，
           这样"改一处、4 个通道全生效"，前端不会和设备不一致 */
        post('/hardware_test', { channel: channel, direction: direction }, function (d) {
            if (d && d.ok === false) toast(d.info || '总线正忙', 'bad');
            else toast((d && d.info) || '已开始动作', 'ok');
            setTimeout(refresh, 300);
        });
    }

    /* =======================================================================
       微动与自吸（本次新增）
       ======================================================================= */
    var sensorSig = '';

    function updateSensors(d) {
        var sg = d.sensors || {};
        var enabled = sg.enabled || 0;
        var chans = sg.ch || [];

        $('sg_extruder').innerHTML = sg.extruder
            ? '<span class="ok">已到位</span>'
            : '<span class="dim">未到位</span>';
        $('sg_src').textContent = d.extruder_pin_used
            ? 'GPIO1（硬件到位信号）' : '打印机 MQTT 事件';

        var sig = enabled + '|' + JSON.stringify(chans);
        if (sig !== sensorSig) {
            sensorSig = sig;

            var names = ['停止送料', '开始送料', '自吸'];
            var html = '<div class="dim" style="margin-top:8px;font-size:13px">各路微动状态</div>';
            for (var i = 0; i < 4; i++) {
                var on = !!(enabled & (1 << i));
                var st = chans[i] || [0, 0, 0];
                html += '<div class="row" style="margin-top:8px;align-items:center">' +
                    '<span style="flex:0 0 72px">料盘位' + (i + 1) + '</span>' +
                    '<span class="badges" style="flex:1 1 auto;margin:0">';
                for (var k = 0; k < 3; k++) {
                    html += '<span class="badge' + (st[k] ? ' trig' : '') +
                            (on ? '' : ' off') + '">' + names[k] +
                            (st[k] ? ' 触发' : '') + '</span>';
                }
                html += '</span>' +
                    '<button data-ch="' + (i + 1) + '" class="sensor-toggle" ' +
                    'data-en="' + (on ? 1 : 0) + '" style="flex:0 0 auto">' +
                    (on ? '停用' : '启用') + '</button></div>';
            }
            $('sensor_list').innerHTML = html;

            var btns = $('sensor_list').getElementsByClassName('sensor-toggle');
            for (var b = 0; b < btns.length; b++) {
                btns[b].onclick = function () {
                    var ch = parseInt(this.getAttribute('data-ch'), 10);
                    var en = this.getAttribute('data-en') === '1';
                    post('/sensor_set', { channel: ch, enabled: en ? 0 : 1 },
                        function (r) {
                            toast((r && r.info) || '已更新', (r && r.ok) ? 'ok' : 'bad');
                            sensorSig = '';
                            refresh();
                        });
                };
            }

            var al = '<div class="dim" style="font-size:13px">' +
                     '按一下就对这一路执行完整自吸流程</div>';
            for (var n = 0; n < 4; n++) {
                al += '<div class="row" style="margin-top:8px">' +
                      '<span style="flex:0 0 66px;align-self:center">料盘位' + (n + 1) + '</span>' +
                      '<button class="primary al-btn" data-ch="' + (n + 1) +
                      '" style="flex:1">自吸上料</button></div>';
            }
            $('autoload_list').innerHTML = al;

            var abs = $('autoload_list').getElementsByClassName('al-btn');
            for (var q = 0; q < abs.length; q++) {
                abs[q].onclick = function () {
                    var ch = parseInt(this.getAttribute('data-ch'), 10);
                    post('/autoload', { channel: ch }, function (r) {
                        toast((r && r.info) || '已开始', (r && r.ok) ? 'ok' : 'bad');
                        refresh();
                    });
                };
            }
        }

        var cr = d.creep || {};
        ['creep_times', 'creep_pulse', 'creep_speed'].forEach(function (id, idx) {
            var el = $(id);
            if (!el || document.activeElement === el) return;
            var v = [cr.times, cr.pulse_ms, cr.speed_pct][idx];
            if (typeof v === 'number' && String(v) !== el.value) el.value = v;
        });
        if (typeof d.extruder_src === 'number') {
            $('btn_src_toggle').textContent = d.extruder_src
                ? '切换到位信号来源（当前：打印机 MQTT）'
                : '切换到位信号来源（当前：GPIO1）';
        }
    }

    /* =======================================================================
       WiFi
       ======================================================================= */
    var wifiScanned = false;
    var wifiSig = '';

    function renderWifiList(ssids, current, force) {
        if (force) wifiScanned = true;
        var sig = ssids.join('|');
        if (sig === wifiSig && !force) return;
        wifiSig = sig;

        var box = $('wifi_list');
        if (!ssids.length) {
            box.innerHTML = '<div class="wifi-item dim">' +
                (wifiScanned ? '没有扫描到 WiFi，点「重新扫描」再试'
                             : '点「重新扫描」搜索附近 WiFi') + '</div>';
            return;
        }
        box.innerHTML = '';
        var selected = false;
        ssids.forEach(function (nm, i) {
            var item = document.createElement('div');
            item.className = 'wifi-item';
            var isCur = (nm === current);
            if (isCur || (!selected && i === 0)) { item.classList.add('sel'); selected = true; }
            item.innerHTML = '<span class="mark"></span>' +
                             '<span class="nm">' + esc(nm) + '</span>' +
                             (isCur ? '<span class="now">当前</span>' : '');
            item.onclick = function () {
                var all = box.getElementsByClassName('wifi-item');
                for (var k = 0; k < all.length; k++) all[k].classList.remove('sel');
                this.classList.add('sel');
            };
            box.appendChild(item);
        });
    }

    function rescanWifi() {
        busy(true);
        var btn = $('btn_wifi_scan');
        btn.disabled = true;
        btn.textContent = '扫描中…';
        post('/wifi_scan', {}, function (d) {
            busy(false);
            btn.disabled = false;
            btn.textContent = '重新扫描';
            if (!d || !d.ok) {
                toast((d && d.info) || '扫描失败', 'bad');
                return;
            }
            renderWifiList(d.ssids || [], curSsid, true);
            toast(d.info || '扫描完成', 'ok');
        });
    }

    function connectWifi() {
        /* 优先用手动填的名称：热点上有手机连着时设备不会重新扫描，
         * 列表可能是开机时的旧数据、甚至是空的，手动填是最后一条兜底路径。 */
        var nm = ($('wifi_ssid_manual').value || '').trim();
        var sel = $('wifi_list').getElementsByClassName('sel')[0];
        if (!nm && sel && sel.getElementsByClassName('nm').length) {
            nm = sel.getElementsByClassName('nm')[0].textContent;
        }
        if (!nm) { toast('请先选一个 WiFi，或直接填写 WiFi 名称', 'bad'); return; }

        busy(true);
        post('/wifi_connect', { name: nm, password: $('wifi_pwd').value }, function (d) {
            busy(false);
            toast((d && d.info) || (d && d.ok ? '配置已保存' : '连接失败'),
                  (d && d.ok) ? 'ok' : 'bad');
            $('wifi_pwd').value = '';
            /* 设备马上要关热点去连路由器，这台手机很快会掉线；
             * 连上之后要重新打开本页看结果，所以这里不急着刷新。 */
            setTimeout(refresh, 800);
        });
    }

    /* =======================================================================
       打印机配置
       ======================================================================= */
    function loadMqttForm() {
        get('/get_mqtt_info', function (d) {
            d = d || {};
            var html = '' +
                '<div class="row"><input id="mq_host" placeholder="打印机 IP，如 192.168.2.50" value="' +
                    esc(d.host || '') + '"></div>' +
                '<div class="row"><input id="mq_serial" placeholder="序列号（打印机设置里可见）" value="' +
                    esc(d.serial || '') + '"></div>' +
                '<div class="row"><input id="mq_user" placeholder="用户名（默认 bblp）" value="' +
                    esc(d.user || 'bblp') + '"></div>' +
                '<div class="row"><input id="mq_pass" type="password" ' +
                    'placeholder="访问码（留空表示不修改）" autocomplete="off"></div>' +
                '<div class="row"><input id="mq_port" type="number" placeholder="端口" value="' +
                    esc(d.port || 8883) + '"></div>' +
                '<div class="row"><input id="mq_cid" placeholder="客户端 ID（留空自动生成）" value="' +
                    esc(d.client_id || '') + '"></div>';
            $('mqtt_form').innerHTML = html;
        }, true);
    }

    function saveMqtt() {
        var data = {
            host: val('mq_host'),
            serial: val('mq_serial'),
            user: val('mq_user'),
            port: parseInt(val('mq_port') || '8883', 10),
            client_id: val('mq_cid')
        };
        /* 访问码留空 = 不修改 —— 设备端就是这么解释的 */
        var p = $('mq_pass').value;
        if (p) data.password = p;

        busy(true);
        post('/mqtt_connect', data, function (d) {
            busy(false);
            toast((d && d.info) || '已提交', (d && d.ok) ? 'ok' : 'bad');
            setTimeout(refresh, 400);
        });
    }

    function val(id) {
        var el = $(id);
        return el ? el.value.trim() : '';
    }

    /* =======================================================================
       点动时长
       ======================================================================= */
    function syncJogInput(ms) {
        jogMs = ms;
        var el = $('jog_seconds');
        /* 正在输入时不覆盖：/status 每 2 秒来一次，
           无脑写 value 会把用户刚敲的数字冲掉 */
        if (el && document.activeElement !== el) {
            var s = (ms / 1000).toFixed(1);
            if (el.value !== s) el.value = s;
        }
    }

    function saveJog() {
        var v = parseFloat(val('jog_seconds'));
        if (isNaN(v) || v <= 0) { toast('请填一个大于 0 的秒数', 'bad'); return; }
        post('/jog_set', { seconds: v }, function (d) {
            if (d && typeof d.jog_ms === 'number') syncJogInput(d.jog_ms);
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
        });
    }

    /* =======================================================================
       蠕动参数
       ======================================================================= */
    function saveCreep() {
        post('/creep_set', {
            times:     parseInt(val('creep_times') || '3', 10),
            pulse_ms:  parseInt(val('creep_pulse') || '400', 10),
            speed_pct: parseInt(val('creep_speed') || '45', 10)
        }, function (d) {
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* =======================================================================
       热点 / 停止 / 计数
       ======================================================================= */
    function toggleAp() {
        post('/ap_set', { on: apOn ? 0 : 1 }, function (d) {
            toast((d && d.info) || '已切换', (d && d.ok) ? 'ok' : 'bad');
            setTimeout(refresh, 400);
        });
    }

    function emergencyStop() {
        post('/stop', {}, function (d) {
            toast((d && d.info) || '已停止', (d && d.ok) ? 'ok' : 'bad');
            setTimeout(refresh, 300);
        });
    }

    function resetBootCount() {
        get('/boot_clear', function (d) {
            toast((d && d.info) || '已清零', 'ok');
            refresh();
        }, true);
    }

    function resetDiag() {
        post('/diag_reset', {}, function (d) {
            /* 设备端如果没有这个接口，就退化为只提示 —— 不给用户报错 */
            toast((d && d.info) || '统计已重置', (d && d.ok === false) ? 'bad' : 'ok');
            refresh();
        });
    }

    function toggleExtruderSrc() {
        var cur = $('btn_src_toggle').textContent.indexOf('MQTT') >= 0 ? 1 : 0;
        post('/extruder_src_set', { src: cur ? 0 : 1 }, function (d) {
            toast((d && d.info) || '已切换', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* =======================================================================
       日志
       ======================================================================= */
    var logPaused = false;
    var LOG_KEY = 'ams.logclosed';

    function renderLog(lines, memFree) {
        var box = $('log_lines');
        var text = '';
        for (var i = 0; i < lines.length; i++) text += lines[i] + '\n';

        $('log_mem').textContent = (typeof memFree === 'number' && memFree >= 0)
            ? ('空闲内存 ' + (memFree / 1024).toFixed(1) + ' KB') : '';

        if (text === logText) return;
        logText = text;

        /* 贴底时跟着滚，用户翻历史时不要抢 */
        var atBottom = (box.scrollHeight - box.scrollTop - box.clientHeight) < 40;
        box.innerHTML = '';
        for (var j = 0; j < lines.length; j++) {
            var span = document.createElement('span');
            var isErr = lines[j].indexOf('★') >= 0 || lines[j].indexOf('失败') >= 0 ||
                        lines[j].indexOf('出错') >= 0 || lines[j].indexOf('未通过') >= 0;
            if (isErr) span.className = 'e';
            span.textContent = lines[j] + '\n';
            box.appendChild(span);
        }
        if (atBottom) box.scrollTop = box.scrollHeight;
    }

    function pollLog() {
        if (logPaused) return;
        if ($('logbox').classList.contains('closed')) return;
        get('/log', function (d) {
            renderLog(d.log || [], d.mem_free);
        }, true);
    }

    function toggleLog() {
        var box = $('logbox');
        var closed = !box.classList.contains('closed');
        box.className = 'logbox' + (closed ? ' closed' : '');
        $('btn_log_toggle').textContent = closed ? '展开' : '折叠';
        store(LOG_KEY, closed ? '1' : '0');
        if (!closed) pollLog();
    }

    function clearLogView() {
        $('log_lines').innerHTML = '';
        logText = '__cleared__';
    }

    /* =======================================================================
       OTA 上传
       ======================================================================= */
    function otaUpload() {
        var f = $('ota_file').files[0];
        if (!f) { toast('请先选择固件文件（.bin）', 'bad'); return; }

        var prog = $('ota_prog');
        var bar = prog.getElementsByTagName('i')[0];
        prog.style.display = 'block';
        bar.style.width = '0%';
        $('ota_box').innerHTML = '<div class="tip">正在上传 ' + esc(f.name) +
            '（' + (f.size / 1024).toFixed(0) + ' KB）… 期间不要断电。</div>';

        /* ★ 用 XHR 而不是 fetch —— 只有 XHR 能拿到上传进度，
           而 OTA 要几十秒，没进度条用户一定会以为死了 */
        var xhr = new XMLHttpRequest();
        xhr.open('POST', '/ota_upload', true);
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');

        xhr.upload.onprogress = function (e) {
            if (e.lengthComputable) {
                bar.style.width = ((e.loaded / e.total) * 100).toFixed(0) + '%';
            }
        };
        xhr.onload = function () {
            var d = null;
            try { d = JSON.parse(xhr.responseText); } catch (err) { d = null; }
            if (d && d.ok) {
                bar.style.width = '100%';
                $('ota_box').innerHTML = '<div class="tip ok">' + esc(d.info) + '</div>';
                toast('升级成功，设备即将重启', 'ok');
                /* 设备 3 秒后重启，这里给个明确的倒计时观感 */
                var left = 3;
                var t = setInterval(function () {
                    left--;
                    if (left <= 0) {
                        clearInterval(t);
                        $('ota_box').innerHTML += '<div class="tip">正在等待设备重启…' +
                            '若 20 秒后仍打不开，请刷新页面（新固件启动失败会自动回滚）</div>';
                        logPaused = true;
                    } else {
                        $('ota_box').innerHTML = '<div class="tip ok">升级成功，' +
                            left + ' 秒后重启…</div>';
                    }
                }, 1000);
            } else {
                $('ota_box').innerHTML = '<div class="tip err">' +
                    esc((d && d.info) || ('上传失败（HTTP ' + xhr.status + '）')) + '</div>';
                toast('升级失败', 'bad');
            }
        };
        xhr.onerror = function () {
            $('ota_box').innerHTML = '<div class="tip err">上传中断，' +
                '请检查网络后重试（备用分区不会被写坏）</div>';
            toast('上传中断', 'bad');
        };
        xhr.send(f);
    }

    /* =======================================================================
       轮询与启动
       ======================================================================= */
    function refresh() {
        get('/status', applyStatus, true);
    }

    function boot() {
        buildMenu();

        /* ★ 先用默认值把界面画出来，不等接口。
           接口慢的时候整页也不会停在"加载中…"。 */
        renderAccess([1, 2, 3, 4], DEFAULT_COLORS, 0);
        updateHardware({ channels: [1, 2, 3, 4], engaged: [], limits: [], motor_direction: 0, conflicts: 0 });
        updateSensors({ sensors: { enabled: 15, extruder: 0, ch: [] } });

        if (store(LOG_KEY) === '1') {
            $('logbox').className = 'logbox closed';
            $('btn_log_toggle').textContent = '展开';
        }

        $('btn_ap').onclick = toggleAp;
        $('btn_stop').onclick = emergencyStop;
        $('btn_diag_reset').onclick = resetDiag;
        $('btn_wifi_conn').onclick = connectWifi;
        $('btn_wifi_scan').onclick = rescanWifi;
        $('btn_mqtt_save').onclick = saveMqtt;
        $('btn_access_save').onclick = saveAccess;
        $('btn_jog_save').onclick = saveJog;
        $('btn_creep_save').onclick = saveCreep;
        $('btn_src_toggle').onclick = toggleExtruderSrc;
        $('btn_ota_upload').onclick = otaUpload;
        $('btn_bootclear').onclick = resetBootCount;
        $('btn_log_toggle').onclick = toggleLog;
        $('btn_log_clear').onclick = clearLogView;
        $('menu_btn').onclick = function () { toggleMenu(); };
        $('scrim').onclick = function () { toggleMenu(false); };

        showPage(store(PAGE_KEY) || 'status');
        loadMqttForm();

        get('/status', applyStatus, true);

        refresh();
        pollLog();
        setInterval(refresh, 2000);
        setInterval(pollLog, 2000);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', boot);
    } else {
        boot();
    }
})();
