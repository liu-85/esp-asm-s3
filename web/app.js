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
    var jogPct = 100;      /* 手动点动 PWM 占空比（%），2026-09-23 新增 */

    /* 百分比 → LEDC duty 原值（0~255）。
     * ★ 必须和后端 motor_pct_to_duty() 用同一种取整：那边是
     *   (pct * 255 + 50) / 100 的整数除法，等价于四舍五入。
     *   两边不一致的话，界面显示的 duty 会和日志里的对不上，
     *   而现场就是拿这两个数在互相印证。 */
    function pctToDuty(pct) {
        pct = parseInt(pct, 10);
        if (isNaN(pct) || pct <= 0) return 0;
        if (pct > 100) pct = 100;
        return Math.round(pct * 255 / 100);
    }

    function dot(el, on) { el.className = 'dot' + (on ? ' on' : ''); }

    function setAlert(msg) {
        var el = $('alertbar');
        el.textContent = msg || '';
        el.className = 'alertbar' + (msg ? ' show' : '');
    }

    /* =======================================================================
       说明块折叠（默认折叠）
       =======================================================================
     * ★ 现场要求（2026-09-24）："将说明类的多行的增加可折叠选项，默认折叠，
     *   需要查看时手动打开。" —— 改成三列之后设置区只有原来一半多宽，
     *   满屏的说明文字会把真正的设置项挤出视野。
     *
     * 做法是**运行时改写**，不是手改 HTML：把每个
     *     <div class="tip">…</div>
     * 换成
     *     <details class="tip"><summary>说明</summary><div class="tipin">…</div></details>
     * 好处：19 处静态说明一次搞定，JS 动态生成的也同样吃得到，
     * 以后再加说明块也不用记得"手动包一层 details"。
     *
     * ⚠️ 两个必须守住的点：
     *   ① 空说明不显示 —— 有些 tip 是留给 JS 填的占位（status_tip），
     *      没内容时留个空灰条很丑；
     *   ② 已折叠的不重复包 —— details 里再套 details 会把摘要吃掉。
     *      可 JS 覆写 innerHTML 会破坏结构，所以写入用 tipSet()，
     *      它写进 .tipin，不动 summary。 */
    var TIP_SUMMARY      = '说明（点开看）';
    var TIP_SUMMARY_WARN = '注意事项（点开看）';

    function collapseTips(root) {
        var scopes = root ? [root] : [document];
        var i, j;
        for (i = 0; i < scopes.length; i++) {
            var tips = scopes[i].querySelectorAll('div.tip');
            for (j = 0; j < tips.length; j++) {
                var d = tips[j];
                if (!d.parentNode) { continue; }
                if (d.textContent.replace(/\s+/g, '') === '') {
                    d.style.display = 'none';   /* 占位用的空说明，先藏起来 */
                    continue;
                }
                var det = document.createElement('details');
                det.className = d.className;
                var sum = document.createElement('summary');
                sum.textContent = (d.className.indexOf('warn') >= 0)
                    ? TIP_SUMMARY_WARN : TIP_SUMMARY;
                var inner = document.createElement('div');
                inner.className = 'tipin';
                while (d.firstChild) { inner.appendChild(d.firstChild); }
                det.appendChild(sum);
                det.appendChild(inner);
                if (d.id) { det.id = d.id; }   /* 保留 id：JS 还要按它找 */
                d.parentNode.replaceChild(det, d);
            }
        }
    }

    /** 往说明块里写内容 —— 折叠后必须写进 .tipin，直接写 innerHTML 会把
     *  summary 一起冲掉（那个块就再也展不开了）。 */
    function tipSet(el, html) {
        if (!el) { return; }
        var box = el.querySelector ? el.querySelector('.tipin') : null;
        var tgt = box || el;
        /* /status 每 2 秒来一次，内容没变就别重写 —— 重写会清掉用户在
         * 说明块里的选区，还会让浏览器白做一次排版 */
        if (tgt.innerHTML === html) { return; }
        tgt.innerHTML = html;
        el.style.display = '';
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
            /* OTA 提示文案按板型动态显示正确的文件名。
             * ★ 必须走 tipSet()：这段说明已经被 collapseTips() 包成
             *   <details><summary>…</summary><div class="tipin"> 了，
             *   直接 el.innerHTML= 会把 summary 一起冲掉 → 以后再也展不开。 */
            var chip = board.name.indexOf('C3') >= 0 ? 'c3' : 's3';
            tipSet($('ota_tip'),
                '要传的是 <code>idf.py build</code> 出来的 ' +
                '<code>esp-ams-' + chip + '.bin</code>' +
                '（或 <code>build/esp-ams-' + chip + '.bin</code>）。' +
                '传错文件不会造成损坏 —— 设备会检查首字节是不是 <code>0xE9</code>，' +
                '不是就直接中止，分区一个字节都不动。');
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
        /* 同步「当前通道」下拉框（只在用户没正在操作时更新） */
        var curSel = $('cur_channel_sel');
        if (curSel && document.activeElement !== curSel) {
            var curVal = String(cur || 0);
            if (curSel.value !== curVal) curSel.value = curVal;
        }

        /* ---- 点动时长 / 点动 PWM ---- */
        if (typeof d.jog_ms === 'number' && d.jog_ms > 0) syncJogInput(d.jog_ms);
        if (typeof d.jog_speed_pct === 'number') {
            syncJogPctInput(d.jog_speed_pct);
        }

        /* ---- 硬件 ---- */
        updateHardware(d.hardware);

        /* ---- 辅助送料 ---- */
        updateAssist(d.assist);

        /* ---- 退料参数 ---- */
        updateRetract(d.retract);

        /* ---- 换料温度 ---- */
        updateTemper(d.temper);

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

    function saveCurrentChannel() {
        var ch = parseInt($('cur_channel_sel').value, 10) || 0;
        post('/current_channel_set', { channel: ch }, function (d) {
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
                (jogMs / 1000).toFixed(1) + ' 秒 @' + jogPct +
                '%（duty ' + pctToDuty(jogPct) + '/255）· 4 个通道统一</b></div>';
        /* ★ 辅助送料「保持吸合」当前保持在哪一路 —— 保持期间状态机显示的是
         *   "空闲"（保持的是离合的机械状态，不是总线占用），所以必须单独显示，
         *   否则用户没法从界面判断"离合现在到底还吸着没有"。 */
        if (hw.assist_hold_ch) {
            html += '<div class="kv"><span>辅助送料保持</span><b class="ok">' +
                    '通道' + hw.assist_hold_ch + ' 的离合保持吸合中' +
                    '（其他阶段会自动断开）</b></div>';
        }
        var md = (hw.motor_direction === undefined) ? 0 : hw.motor_direction;
        html += '<div class="kv"><span>电机方向</span><b>' + md +
                '（1=进料　-1=退料　0=停止）</b></div>';
        var cf = hw.conflicts || 0;
        html += '<div class="kv"><span>离合冲突次数</span><b' +
                (cf ? ' class="err"' : ' class="ok"') + '>' + cf + '</b></div>';

        /* ★ 最近一次驱动的执行回执（2026-09-23 新增，同日补硬件回读）。
         *   现场反复报"日志显示在辅助送料、电机却没动"，而日志是**先**打
         *   "辅助送料"**再**调驱动的 —— 光看日志分不清"根本没驱动"和
         *   "驱动了但带不动"。这里把**硬件真正收到的数**摆出来：
         *     duty   = 我们请求写下去的原值（手动点动=255，辅助送料=按百分比）
         *     rb_*   = **从 LEDC 寄存器读回来的原值**、引脚的**实际电平**
         *   rb_in1 是 0 而 duty 非 0 → 信号压根没写进硬件（软件问题）；
         *   rb_in1 非 0 而电机不转 → 占空比或机械（H 桥使能/离合打滑）。 */
        var dv = hw.drive;
        if (dv) {
            var dvTxt;
            if (!dv.clutch_ok) {
                dvTxt = '<b class="err">离合没吸合 —— 本次没有驱动电机</b>';
            } else if (dv.skipped) {
                dvTxt = '<b>被跳过（电机已在同方向同速度运行）</b>';
            } else {
                dvTxt = '<b' + (dv.duty_raw >= 255 ? ' class="ok"' : '') + '>' +
                        'duty ' + dv.duty_raw + '/255（' + dv.speed_pct + '%）' +
                        ' · 通电 ' + dv.ran_ms + 'ms</b>';
            }
            html += '<div class="kv"><span>最近一次驱动</span>' + dvTxt + '</div>';

            if (dv.clutch_ok && !dv.skipped && dv.rb_in1 !== undefined) {
                var rbOk = (dv.rb_in1 > 0 || dv.rb_in2 > 0);
                html += '<div class="kv"><span>LEDC 回读</span><b' +
                        (rbOk ? ' class="ok"' : ' class="err"') + '>' +
                        'IN1=' + dv.rb_in1 + '/255（引脚' + dv.lv_in1 + '）　' +
                        'IN2=' + dv.rb_in2 + '/255（引脚' + dv.lv_in2 + '）</b></div>';
                if (!rbOk) {
                    html += '<div class="err" style="font-size:12px">' +
                            '⚠ 回读两路都是 0：信号没写进硬件，属于软件问题</div>';
                } else if (dv.duty_raw && dv.duty_raw < 255) {
                    html += '<div class="dim" style="font-size:12px">' +
                            '信号确实出去了。若电机仍不转，就是占空比不够 —— ' +
                            '把上面的速度调到 100%，或先用「手动点动」对照一次</div>';
                }
            }
        }

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
       点动时长 + 点动 PWM
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

    /* ★ 点动 PWM（2026-09-23 新增）：放这个框的**唯一目的**就是让用户
     *   一档一档试出"能带动电机和离合的临界占空比"。所以旁边那个提示
     *   （jog_pwm_hint）直接把当前档位换算成 duty 原值显示出来 ——
     *   现场看到的是"50% → duty 128/255"，跟日志里的回执能直接对上。 */
    function syncJogPctInput(pct) {
        jogPct = pct;
        var el = $('jog_speed');
        if (el && document.activeElement !== el) {
            if (String(pct) !== el.value) el.value = pct;
        }
        var hint = $('jog_pwm_hint');
        if (hint) hint.textContent = '= duty ' + pctToDuty(pct) + '/255';
    }

    function saveJog() {
        var v = parseFloat(val('jog_seconds'));
        var p = parseInt(val('jog_speed'), 10);
        /* 两个字段各自可选：只改 PWM 也行、只改时间也行（后端两个都是可选参数，
         * 没传的保持原值）。所以这里不要因为其中一个空着就整体拒绝。 */
        var body = {};
        if (!isNaN(v) && v > 0) body.seconds = v;
        if (!isNaN(p)) {
            if (p < 5 || p > 100) {
                toast('点动 PWM 请在 5~100 之间', 'bad');
                return;
            }
            body.speed_pct = p;
        }
        if (body.seconds === undefined && body.speed_pct === undefined) {
            toast('请填一个大于 0 的秒数，或一个 5~100 的 PWM', 'bad');
            return;
        }
        post('/jog_set', body, function (d) {
            if (d && typeof d.jog_ms === 'number') syncJogInput(d.jog_ms);
            if (d && typeof d.jog_speed_pct === 'number') {
                syncJogPctInput(d.jog_speed_pct);
            }
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
        });
    }

    /* =======================================================================
       蠕动参数
       ======================================================================= */
    function saveCreep() {
        var spd = parseInt(val('creep_speed') || '70', 10);
        /* 低于 60% 只会堵转（听不到声、料不动）—— 与其让用户对着"没反应"猜，
         * 不如保存前直接拦一下，把原因说清楚。 */
        if (!isNaN(spd) && spd < 60) {
            toast('蠕动速度低于 60% 会带不动电机（只会堵转、听不到声）。'
                  + '要更慢请加长「单次时长」，别压速度。', 'bad');
            return;
        }
        post('/creep_set', {
            times:     parseInt(val('creep_times') || '3', 10),
            pulse_ms:  parseInt(val('creep_pulse') || '400', 10),
            speed_pct: isNaN(spd) ? 70 : spd
        }, function (d) {
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* =======================================================================
       辅助送料
       ======================================================================= */
    function updateAssist(a) {
        a = a || {};
        var elCk = $('assist_enabled');
        var elSpd = $('assist_speed');
        var elMs = $('assist_ms');
        var elHd = $('assist_hold');
        if (!elCk || !elSpd) return;
        elCk.checked = !!a.enabled;
        if (elHd && typeof a.hold === 'boolean') elHd.checked = a.hold;
        if (typeof a.speed_pct === 'number' && document.activeElement !== elSpd) {
            if (String(a.speed_pct) !== elSpd.value) elSpd.value = a.speed_pct;
        }
        if (elMs && typeof a.ms === 'number' && document.activeElement !== elMs) {
            if (String(a.ms) !== elMs.value) elMs.value = a.ms;
        }
    }

    function toggleAssist() {
        var el = $('assist_enabled');
        var on = el.checked;
        post('/assist_set', { enabled: on ? 0 : 1 }, function (d) {
            el.checked = !on;
            toast((d && d.info) || '已切换', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* 「阶段内保持离合吸合」切换（2026-09-23 新增）。
     * 单独给一个按钮而不是只靠「保存」：这是个**安全回退开关** ——
     * 万一长时间保持让离合发烫或有异响，用户要能立刻关掉退回旧行为。 */
    function toggleAssistHold() {
        var el = $('assist_hold');
        var on = el.checked;
        post('/assist_set', { hold: on ? 0 : 1 }, function (d) {
            el.checked = !on;
            toast((d && d.info) || '已切换', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* 「立即自检」—— 按当前配置驱动一次辅助送料。
     *
     * ★ 为什么不靠"等下一轮打印"来验证（2026-09-23 现场反馈）：
     *   辅助送料平时是由打印机阶段（校准/打印中）触发的，想验证一次得等
     *   十几分钟。这个按钮让它立刻跑一次，并且用**硬件回读**把结果落在
     *   「最近一次驱动」里：
     *     点动（duty 255）转、自检不转 → 占空比不够
     *     两个都不转                   → 硬件链路（H 桥使能 / 离合打滑 / 接线）
     *     回读两路都是 0               → 信号没写进硬件，软件问题
     */
    function testAssist() {
        post('/assist_test', {}, function (d) {
            toast((d && d.info) || '已下发自检', (d && d.ok) ? 'ok' : 'bad');
        });
    }

    /* =======================================================================
       换色冲刷 G-code 生成器
       =======================================================================
       ★ 为什么只能"生成文本、让用户粘回切片器"，而不能"网页改一下打印机就变"

         冲刷 / 擦嘴根本不是打印机固件的流程，它是切片机设置里的
         change_filament_gcode（换料 G-code）—— **切片的那一刻就展开、写死进
         G-code 文件了**。证据就在切片文件里：`切片G.txt` 第 1378 行起
         ";===== 保留强化切刀（X260→X285）+ 固定冲刷量…"，下面 30/20/15/10mm
         的四轮冲刷是写死的常量，连 `flush_length` 变量都没用。

         切片器里的 `flush_volumes_matrix`（例子里是 0,36.1,62.7,45,...）现在
         **是失效的** —— 因为宏不引用 `flush_length` 了，改它没用。

         板子只能通过 MQTT 发**单条** gcode_line，既改不了已经切好的文件，
         也没法在打印机执行宏的中途插进去。所以"Web 自定义"唯一可行的形态
         就是：网页算好参数化的片段 → 一键复制 → 粘回切片器的换料 G-code。

       ★ 生成的范围**故意限制在「冲刷」到「最终收尾」这一段**，不生成整个宏：
         切刀段和暂停段（M73 P101 / M400 U1）是这套固件握手时序的关键，
         让用户不小心改坏得不偿失。少生成一点，风险小很多。
    */
    function genFlushGcode() {
        var rounds = parseInt(val('fl_rounds'), 10);
        if (isNaN(rounds) || rounds < 1) rounds = 1;
        if (rounds > 4) rounds = 4;

        var mms = (val('fl_mms') || '').split(/[,，\s]+/)
                    .map(function (s) { return parseFloat(s); })
                    .filter(function (n) { return !isNaN(n) && n > 0; });
        if (!mms.length) mms = [30];
        while (mms.length < rounds) mms.push(mms[mms.length - 1]);

        var first = parseInt(val('fl_first'), 10);
        if (isNaN(first) || first < 0) first = 0;
        var wipe = $('fl_wipe').checked;

        var L = [];
        function wipeBlock(n) {
            if (!wipe) return;
            L.push('; ===== 擦嘴 ' + n + ' =====');
            L.push('M400');
            L.push('M106 P1 S178');
            L.push('M400 S3');
            L.push('G1 X-38.2 F18000');
            L.push('G1 X-48.2 F3000');
            L.push('G1 X-38.2 F18000');
            L.push('G1 X-48.2 F3000');
            L.push('G1 X-38.2 F18000');
            L.push('G1 X-48.2 F3000');
            L.push('M400');
            L.push('M106 P1 S0');
            L.push('');
        }

        L.push('; ===== 换色冲刷（AMS 网页生成）=====');
        L.push('; ===== 轮数 ' + rounds + '，每轮 ' + mms.slice(0, rounds).join('/') +
               ' mm，首次额外 ' + first + ' mm，擦嘴 ' + (wipe ? '开' : '关') + ' =====');
        L.push('');

        for (var i = 0; i < rounds; i++) {
            var mm = mms[i];
            L.push('; ===== 冲刷 ' + (i + 1) + '（固定量 ' + mm + 'mm'
                   + (i === 0 ? '' : '，脉冲式') + '）=====');
            if (i === 0) {
                /* 第一轮：匀速长冲，负责把旧颜色大块带走。
                 * 前后包一层 set_filament_type，打印机据此算体积流量补偿。 */
                L.push('M400');
                L.push('M1002 set_filament_type:UNKNOWN');
                L.push('M109 S[nozzle_temperature_range_high] ; 加热到最高温度');
                L.push('M104 S[new_filament_temp] ; 余热冲刷到目标温度');
                L.push('M106 P1 S60');
                L.push('G1 E' + mm + ' F{old_filament_e_feedrate}');
                L.push('M400');
                L.push('M1002 set_filament_type:{filament_type[next_extruder]}');
            } else {
                /* 后续轮：脉冲式（小段挤出 + 微退），比匀速更容易把残色甩掉 */
                var per = Math.round((mm / 5) * 10) / 10;
                L.push('M106 P1 S60');
                for (var k = 0; k < 5; k++) {
                    L.push('G1 E' + per + ' F{new_filament_e_feedrate}');
                    L.push('G1 E0.4 F50');
                }
                L.push('G1 E-[new_retract_length_toolchange] F1800');
                L.push('G1 E[new_retract_length_toolchange] F300');
            }
            L.push('');
            wipeBlock(i + 1);
        }

        if (first > 0) {
            L.push('; ===== 首次换料额外冲刷 =====');
            L.push('{if toolchange_count == 1}');
            L.push('M400');
            L.push('M104 S[new_filament_temp]');
            L.push('M106 P1 S60');
            L.push('G1 E' + first + ' F{old_filament_e_feedrate}');
            L.push('G1 E-[new_retract_length_toolchange] F1800');
            L.push('G1 E[new_retract_length_toolchange] F300');
            L.push('{endif}');
            L.push('');
        }

        L.push('; ===== 最终收尾 =====');
        L.push('M400');
        L.push('M106 P1 S60');
        L.push('M109 S[new_filament_temp]');
        L.push('G1 E5 F{new_filament_e_feedrate} ; 补偿温等期间漏料');
        L.push('M400');
        L.push('G92 E0');
        L.push('G1 E-[new_retract_length_toolchange] F1800');
        wipeBlock('（收尾）');
        L.push('G1 Z{max_layer_z + 3.0} F3000');
        L.push('M106 P1 S0');
        L.push('{if layer_z <= (initial_layer_print_height + 0.001)}');
        L.push('M204 S[initial_layer_acceleration]');
        L.push('{else}');
        L.push('M204 S[default_acceleration]');
        L.push('{endif}');

        $('fl_out').value = L.join('\n');

        /* 粗算耗时：只用来比较参数之间的相对大小，不当精确值用。
         * 速率取切片文件里实测的 old=199 / new=299 mm/min；擦嘴一次约 5 秒。 */
        var total = 0;
        for (var j = 0; j < rounds; j++) total += mms[j];
        var secs = mms[0] / 3.32;
        for (var m = 1; m < rounds; m++) secs += mms[m] / 4.98;
        secs += first / 3.32;
        secs += (wipe ? rounds + 1 : 0) * 5;
        secs += 10;
        $('fl_est').innerHTML = '冲 ' + total + ' mm'
            + (first > 0 ? '（+首次 ' + first + '）' : '')
            + ' · 约 ' + Math.round(secs) + ' 秒';
        toast('已生成，点「复制」再粘回切片器的换料 G-code', 'ok');
    }

    function copyFlushGcode() {
        var el = $('fl_out');
        if (!el || !el.value) {
            toast('先生成片段', 'bad');
            return;
        }
        el.removeAttribute('readonly');
        el.select();
        var ok = false;
        try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
        el.setAttribute('readonly', 'readonly');
        if (ok) {
            toast('已复制到剪贴板', 'ok');
        } else {
            toast('浏览器不让自动复制，请手动选中后 Ctrl+C', 'bad');
        }
    }

    function saveAssistPct() {
        var v = parseInt(val('assist_speed'), 10);
        var ms = parseInt(val('assist_ms'), 10);
        if (isNaN(v) || v < 5 || v > 100) {
            toast('PWM 速度请在 5~100 之间（低于 60% 会带不动电机）', 'bad');
            return;
        }
        if (isNaN(ms) || ms < 200 || ms > 10000) {
            toast('每次时长请在 200~10000ms 之间', 'bad');
            return;
        }
        var hd = $('assist_hold');
        post('/assist_set', {
            speed_pct: v, ms: ms, hold: (hd && hd.checked) ? 1 : 0
        }, function (d) {
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* =======================================================================
       退料参数
       ======================================================================= */
    function updateRetract(r) {
        r = r || {};
        var map = {
            retract_wait_ms:    r.wait_ms,
            retract_cont_ms:    r.cont_ms,
            retract_creep_ms:   r.creep_ms,
            retract_gap_ms:     r.gap_ms,
            retract_creep_max:  r.creep_max
        };
        Object.keys(map).forEach(function (id) {
            var el = $(id);
            var v = map[id];
            if (!el || typeof v !== 'number') return;
            /* 正在输入的框不要被刷新顶掉 —— 否则每 5 秒一次的轮询会把手打的
             * 数字擦掉，用户会以为"改了但保存不上去" */
            if (document.activeElement === el) return;
            if (String(v) !== el.value) el.value = v;
        });
    }

    function saveRetract() {
        var c   = parseInt(val('retract_cont_ms') || '6000', 10);
        var cm  = parseInt(val('retract_creep_ms') || '2000', 10);
        var g   = parseInt(val('retract_gap_ms') || '1000', 10);
        var mx  = parseInt(val('retract_creep_max') || '0', 10);
        var w   = parseInt(val('retract_wait_ms') || '5000', 10);

        if (isNaN(c) || c < 1000 || c > 30000) {
            toast('连续退料时长请在 1000~30000ms 之间', 'bad');
            return;
        }
        if (isNaN(cm) || cm < 100 || cm > 10000) {
            toast('蠕动退料时长请在 100~10000ms 之间', 'bad');
            return;
        }
        if (isNaN(g) || g < 100 || g > 5000) {
            toast('蠕动间隔请在 100~5000ms 之间', 'bad');
            return;
        }
        if (isNaN(mx) || mx < 0 || mx > 20) {
            toast('蠕动轮数上限请在 0~20 之间（0 = 不蠕动）', 'bad');
            return;
        }
        if (isNaN(w) || w < 1000 || w > 15000) {
            toast('GPIO 模式等信号请在 1000~15000ms 之间', 'bad');
            return;
        }
        post('/retract_set', {
            wait_ms: w, cont_ms: c, creep_ms: cm,
            gap_ms: g, creep_max: mx
        }, function (d) {
            toast((d && d.info) || '已保存', (d && d.ok) ? 'ok' : 'bad');
            refresh();
        });
    }

    /* =======================================================================
       换料温度
       ======================================================================= */
    var TEMPER_MIN = 150, TEMPER_MAX = 300;
    /* 界面上只有 4 个输入框，但板子可能有 2 通道（C3）—— 多余的框要藏起来，
     * 不然用户填了却根本不生效，比没有更糟。 */
    var TEMPER_UI_MAX = 4;

    function updateTemper(arr) {
        if (!arr || !arr.length) return;
        for (var i = 0; i < TEMPER_UI_MAX; i++) {
            var el = $('temper_' + (i + 1));
            if (!el) continue;
            var lab = el.parentNode;
            if (i >= arr.length) {
                el.disabled = true;
                if (lab) lab.style.display = 'none';
                continue;
            }
            if (document.activeElement !== el &&
                typeof arr[i] === 'number' && String(arr[i]) !== el.value) {
                el.value = arr[i];
            }
        }
    }

    function saveTemper() {
        /* 逐个提交：后端每次只收一个通道，4 次请求互不覆盖 ——
         * 如果一次性把 4 个值打包送过去，任何一处格式错都会整批失败。 */
        var todo = [];
        for (var i = 0; i < TEMPER_UI_MAX; i++) {
            var el = $('temper_' + (i + 1));
            if (!el || el.disabled) continue;
            var lab = el.parentNode;
            if (lab && lab.style.display === 'none') continue;
            var v = parseInt(el.value, 10);
            if (isNaN(v) || v < TEMPER_MIN || v > TEMPER_MAX) {
                toast('料盘位' + (i + 1) + ' 的温度请在 ' +
                      TEMPER_MIN + '~' + TEMPER_MAX + '℃ 之间', 'bad');
                return;
            }
            todo.push({ channel: i + 1, temper: v });
        }
        if (!todo.length) {
            toast('没有可提交的料盘位', 'bad');
            return;
        }

        var k = 0, info = '';
        (function next() {
            if (k >= todo.length) {
                toast(info || '换料温度已保存', 'ok');
                refresh();
                return;
            }
            var it = todo[k++];
            post('/temper_set', it, function (d) {
                if (d && d.ok) { info = d.info || info; }
                else { toast((d && d.info) || '保存失败', 'bad'); return; }
                next();
            });
        })();
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

    /* ★ 以前这里有一份 LOG_KEYWORDS 白名单，只有命中的行才显示。
     *   它把最该看的「阶段：载入打印材料」那行挡掉了 —— 白名单里没有
     *   "阶段"，于是日志只剩下零散的动作行，看着就是一锅乱粥，
     *   而且卡住的那一步恰好被藏起来，根本没法排查。
     *   设备端本来就只留 48 行，全显示完全放得下，不再过滤。 */

    /* 已经渲染到设备端的第几行。-1 = 还没渲染过。
     * 靠它做"只追加新行"，不再用整串文本比对 —— 整串比对在环形缓冲
     * 滚动时必然判定成"全变了"，只能整屏重建，那就是闪烁和丢行的来源。 */
    var logLastSeq = -1;

    /* 只在「设备端没给 seq」的退化路径上用（整串比对，老行为）。
     * 本来定义在文件上半部分，和它的用处隔着几百行，挪过来。 */
    var logText = null;

    function renderLog(lines, seqs, memFree) {
        var box = $('log_lines');

        $('log_mem').textContent = (typeof memFree === 'number' && memFree >= 0)
            ? ('空闲内存 ' + (memFree / 1024).toFixed(1) + ' KB') : '';

        var atBottom = (box.scrollHeight - box.scrollTop - box.clientHeight) < 40;

        /* seq 缺失（旧固件）或长度对不上 → 退回整屏重建的老行为。
         * 会闪，但至少不会重复堆积。 */
        var hasSeq = !!(seqs && seqs.length === lines.length);
        if (!hasSeq) {
            var text = lines.join('\n');
            if (text === logText) return;
            logText = text;
            box.innerHTML = '';
            for (var j = 0; j < lines.length; j++) {
                box.appendChild(makeLogLine(lines[j]));
            }
            if (atBottom) box.scrollTop = box.scrollHeight;
            return;
        }

        /* 设备端缓冲被清空、或设备重启导致行号倒退 → 从头来 */
        if (logLastSeq >= 0 && seqs.length &&
            seqs[seqs.length - 1] < logLastSeq) {
            box.innerHTML = '';
            logLastSeq = -1;
        }

        var appended = 0;
        for (var i = 0; i < lines.length; i++) {
            var s = seqs[i];
            if (s >= 0 && s <= logLastSeq) continue;   /* 这一行已经显示过了 */
            box.appendChild(makeLogLine(lines[i]));
            appended++;
        }
        if (seqs.length) logLastSeq = seqs[seqs.length - 1];

        /* DOM 别无限长：设备端只留 48 行，这里留 400 行足够往回翻，
         * 再多纯粹是占手机内存。 */
        while (box.childNodes.length > 400) {
            box.removeChild(box.firstChild);
        }

        if (appended && atBottom) box.scrollTop = box.scrollHeight;
    }

    /** 造一行日志（错误行高亮）。抽出来是为了让两条渲染路径共用同一套判定。 */
    function makeLogLine(line) {
        var span = document.createElement('span');
        var isErr = line.indexOf('★') >= 0 ||
                    line.indexOf('失败') >= 0 ||
                    line.indexOf('出错') >= 0 ||
                    line.indexOf('未通过') >= 0;
        if (isErr) span.className = 'e';
        span.textContent = line + '\n';
        return span;
    }

    function pollLog() {
        if (logPaused) return;
        if ($('logbox').classList.contains('closed')) return;
        get('/log', function (d) {
            renderLog(d.log || [], d.seq, d.mem_free);
        }, true);
    }

    /* 日志列的显示 / 收起。
     * 桌面下"收起"= 整列让位给设置区（.logbox.closed → display:none）。
     * ★ 收起之后顶栏会出现「显示日志」按钮（body.logoff 控制），
     *   否则收起就没有任何入口能再打开它 —— 按钮自己被一起藏起来了。 */
    function setLogVisible(on) {
        var box = $('logbox');
        if (!box) { return; }
        box.className = 'logbox' + (on ? '' : ' closed');
        if (on) {
            document.body.classList.remove('logoff');
        } else {
            document.body.classList.add('logoff');
        }
        var t = $('btn_log_toggle');
        if (t) { t.textContent = on ? '收起' : '展开'; }
        store(LOG_KEY, on ? '0' : '1');
        if (on) { pollLog(); }
    }

    function toggleLog() {
        setLogVisible($('logbox').classList.contains('closed'));
    }

    function clearLogView() {
        /* ★ 清屏只清 DOM，**绝对不要动 logLastSeq**。
         *   旧代码是先把 innerHTML 置空、再用 getRenderedText() 去读 DOM
         *   当作"已看过的内容"—— 读到的当然是空串，于是下一轮轮询时
         *   设备端那 48 行原样又冒回来，用户看到的就是"清屏按钮没用"。
         *   现在游标留在原处：旧行（seq <= 游标）本来就会被跳过，
         *   新行照常追加，语义正确。 */
        $('log_lines').innerHTML = '';
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

        /* 说明块全部折叠成一行（现场要求：多行说明默认折叠，要看再点开）。
         * 放在最前面：后面的 applyStatus 会往 ota_tip 里写文案，
         * 那时它已经是 details 了，必须走 tipSet() 写进 .tipin。 */
        collapseTips();

        /* ★ 先用默认值把界面画出来，不等接口。
           接口慢的时候整页也不会停在"加载中…"。 */
        renderAccess([1, 2, 3, 4], DEFAULT_COLORS, 0);
        updateHardware({ channels: [1, 2, 3, 4], engaged: [], limits: [], motor_direction: 0, conflicts: 0 });
        updateAssist({ enabled: 1, speed_pct: 60, ms: 1500 });
        updateRetract({
            wait_ms: 5000, cont_ms: 6000,
            creep_ms: 2000, gap_ms: 1000, creep_max: 3
        });
        updateSensors({ sensors: { enabled: 15, extruder: 0, ch: [] } });

        /* 日志列默认显示；上次点过"收起"的话恢复成收起状态 */
        setLogVisible(store(LOG_KEY) !== '1');

        $('btn_ap').onclick = toggleAp;
        $('btn_stop').onclick = emergencyStop;
        $('btn_diag_reset').onclick = resetDiag;
        $('btn_wifi_conn').onclick = connectWifi;
        $('btn_wifi_scan').onclick = rescanWifi;
        $('btn_mqtt_save').onclick = saveMqtt;
        $('btn_access_save').onclick = saveAccess;
        $('btn_cur_channel_save').onclick = saveCurrentChannel;
        $('btn_jog_save').onclick = saveJog;
        $('btn_creep_save').onclick = saveCreep;
        $('btn_assist_toggle').onclick = toggleAssist;
        $('btn_assist_hold').onclick = toggleAssistHold;
        $('btn_assist_save').onclick = saveAssistPct;
        $('btn_retract_save').onclick = saveRetract;
        $('btn_temper_save').onclick = saveTemper;
        $('btn_src_toggle').onclick = toggleExtruderSrc;
        $('btn_ota_upload').onclick = otaUpload;
        $('btn_bootclear').onclick = resetBootCount;
        $('btn_assist_test').onclick = testAssist;
        $('btn_fl_gen').onclick = genFlushGcode;
        $('btn_fl_copy').onclick = copyFlushGcode;
        $('btn_log_toggle').onclick = toggleLog;
        $('btn_log_clear').onclick = clearLogView;
        /* 顶栏的「显示日志」：日志列收起后唯一的回程入口 */
        $('btn_log_show').onclick = toggleLog;
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
