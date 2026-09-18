# ESP-AMS-S3

用 **ESP-IDF（C 语言）** 重写的拓竹打印机自动换料系统，目标芯片 **ESP32-S3（42 针模组，8MB Flash）**。

和仓库里那份 MicroPython 版是**两套独立实现**，可以并存：MicroPython 版还在 `python_code/`，这份在 `esp-ams-s3/`，互不影响各自的构建与发布流水线。

```
esp-ams-s3/
├── CMakeLists.txt          顶层工程文件（只指定工程名 + 引入 IDF 构建系统）
├── partitions.csv          分区表：双 OTA 分区 + NVS + storage
├── sdkconfig.defaults      默认配置（每一项都写了"为什么"，改前先读）
├── main/
│   ├── main.c              启动流程（只负责按顺序拉起各模块）
│   ├── board_pins.h        ★ 引脚表 —— 改接线只改这一个文件
│   ├── config.c/.h         NVS 持久化配置（对应 config.json + wifi.dat）
│   ├── wifi_manager.c/.h   WiFi 连接与配网热点（对应 network_model.py）
│   ├── web_server.c/.h     配置网页与 REST 接口（对应 AMS_WEB.py）
│   ├── bambu_mqtt.c/.h     打印机 MQTT over TLS（对应 bambu/bambu_mqtt.py）
│   ├── bambu_proto.c/.h    拓竹报文解析（对应 get_event_info.py + bambu_const.py）
│   ├── motor.c/.h          共享直流电机（LEDC PWM 双路输出）
│   ├── clutch.c/.h         4 路电磁离合 + 互斥仲裁（对应 motor_clutch.py）
│   ├── filament_sensor.c/.h 12 个微动 + 挤出机到位信号
│   ├── ams_controller.c/.h ★ 业务状态机（对应 AMS_MODEL.py）
│   ├── log_buffer.c/.h     日志环形缓冲（对应 logout.py）
│   └── CMakeLists.txt      编译单元 + 把 web/ 三个前端文件编进固件
├── web/
│   ├── index.html          配置界面（8 个面板）
│   ├── app.js              前端逻辑（只轮询 /status 一个接口）
│   └── style.css           样式（自适应明暗主题、手机优先）
└── tools/
    └── lint_c.py           没有工具链时用的 C 源码静态检查（见文末）
```

---

## 一、为什么要用 ESP-IDF 重写

不是因为"MicroPython 不好"，而是有几件事在 MicroPython 那套架构里**做不到**或在真机上**反复出问题**：

| 问题 | MicroPython 版 | ESP-IDF 版 |
| --- | --- | --- |
| **堆碎片** | GC 不压缩。应用加载完后空闲还有 64KB，但**最大连续块只剩 3,584 字节**。于是"要一大块连续内存且只在启动时做一次"的操作（开射频约 24KB 连续堆）必须抢在应用 import 之前做完，`main.py` 的阶段划分全是为绕开这个限制，往前面插一个 import 就可能报 `Wifi Unknown Error 0x0101` | WiFi 驱动用**静态分配**缓冲，编译期定死，不存在这个问题。启动顺序可以按逻辑依赖来排，不用按内存来排 |
| **网页卡死** | 每个 handler 都得拆成 `await` 片段，否则 uasyncio 事件循环被按住 → 网页打不开。为此写了非阻塞收包、缓存探测、带超时的 ping 一堆防御代码 | `esp_http_server` 跑在自己的任务里，handler **可以随便写阻塞代码**（扫 WiFi 2 秒、OTA 几十秒都没事） |
| **MQTT 卡网页** | 同上，要写 `poll_msg()`、`mqtt_alive_cached()`、`tcp_reachable()` | `esp-mqtt` 自带任务，收包是回调，`bambu_mqtt_is_connected()` 只读一个标志、零网络 I/O |
| **整机固件 OTA** | 分区表只有一个 factory 应用分区，没有备用分区 → 只能更新"应用与界面"（`.ams` 包），整机固件必须插 USB | `ota_0` / `ota_1` 双分区 + `otadata` → 网页上直接升级**整机固件**，写入中途断电旧固件完好，新固件起不来还能自动回滚 |

另外 `sdkconfig.defaults` 里**在编译期**关掉了 WiFi 省电（`esp_wifi_set_ps(WIFI_PS_NONE)` 在 `wifi_mgr_init()` 里，另有一份 sdkconfig 说明），这一类"传大文件时掉关联"的问题从配置层消失。

---

## 二、硬件接线

### 执行机构

```
                 ┌── 离合1 ──> 料盘位1 送料轮
共享电机 ────────┼── 离合2 ──> 料盘位2 送料轮
(H桥 IN1/IN2)    ├── 离合3 ──> 料盘位3 送料轮
                 └── 离合4 ──> 料盘位4 送料轮
```

> ★ **硬性约束：任何时刻最多 1 路电磁离合吸合。**
> 两路同时咬合会让两卷料被同一个电机反向拉扯 → 料线绷断 / 打滑；两路线圈浪涌叠加还会把 5V 拉塌导致复位。
> 这条由 `clutch.c` **四重强制**（与 MicroPython 版 `FilamentMotorBus` 同样做法）：
> ① 吸合前无条件先全部断开 ② 断开后回读复核，发现残留就取消本次动作
> ③ 写引脚只有一个内部函数 `clutch_apply()`，不对外暴露，没有绕过仲裁的路径
> ④ `clutch_assert_single()` 由主循环每周期调一次，发现多路吸合立即全断

> ★ 电机那两路还有一个类似的硬件红线：**IN1 / IN2 绝不同时为高**（H 桥上下管直通会当场烧驱动芯片）。
> `motor_apply()` 的写引脚顺序因此固定为"先双路清零、再给目标通道赋值"，并额外拦截"两路都非 0"的调用。
> **不要把这个顺序"优化"成先写目标通道。**

### 引脚表（`main/board_pins.h`）

| 功能 | GPIO | 说明 |
| --- | --- | --- |
| 电机 H 桥 IN1 | 4 | 进料（正转） |
| 电机 H 桥 IN2 | 5 | 退料（反转） |
| 电磁离合 1 / 2 / 3 / 4 | 6 / 7 / 8 / 9 | 高电平吸合，四只都是干净脚 |
| 状态 LED | 2 | 不需要就改成 `-1` |
| 通道1 停止 / 开始 / 自吸 微动 | 11 / 12 / 13 | 内部上拉，开关对地，低电平触发 |
| 通道2 停止 / 开始 / 自吸 微动 | 14 / 15 / 16 | |
| 通道3 停止 / 开始 / 自吸 微动 | 17 / 18 / 21 | |
| 通道4 停止 / 开始 / 自吸 微动 | 38 / 47 / 48 | |
| 挤出机到位信号 | 1 | 全局一路，也可改由 MQTT 事件驱动 |
| **剩余可用** | 10、39、40、41、42 | 干净脚，可作输入或输出 |
| （仅建议作输入） | 0、3、45、46 | strapping 脚，作输出会改上电启动电平 |

微动电气接法（12 只都一样）：

```
GPIO ──┬── 微动开关 ── GND
       └── （内部上拉，不用外接电阻）
```

空闲 = 高电平，触发 = 低电平。这种接法断线时读到的是"未触发"，机器只会不动，不会乱动。

> ⚠️ **GPIO48** 在官方 ESP32-S3-DevKitC-1 上接了板载 RGB 灯。当**输入**读（内部上拉 + 开关对地）在多数板子上没问题；如果发现通道4的自吸微动读数不对，把 `BOARD_PIN_SENSOR_AUTOLOAD` 的第 4 项从 `48` 改成 `10` 即可（GPIO10 是空着的干净脚）。

上电时 `main.c` 会打印完整接线表，并跑一次自检，查三件事：同一只脚被两个角色占用 / 踩到保留脚（Flash、USB、UART）/ strapping 脚被当输出用。**只报警不中断** —— 让用户能连上网页看到这条错误再决定怎么改。

---

## 三、每路三个微动 + 自吸流程

### 三只微动的语义（`ams_controller.c`）

| 微动 | 触发后 |
| --- | --- |
| **停止送料** | 立刻停电机（料到位 / 料尽 / 机构顶到头都很常见） |
| **开始送料** | 按常规速度送料，直到停止送料微动触发或超时 |
| **自吸** | **全自动上料**：送料 → 等挤出机到位信号 → 蠕动送料 3 次收尾 |

### 自吸上料的完整时序

```
① 自吸微动被触发（人把料插进去、碰到这只开关）
        │
        ▼
② 吸合该路离合 + 电机进料，一直送到「停止送料微动」触发
        │  （没装微动的通道走降级模式：按 AMS_NO_LIMIT_LOAD_MS 计时封顶）
        ▼
③ 等「挤出机到位信号」——最长等 AMS_EXTRUDER_WAIT_MS（15 秒）
        │  信号来源可选：GPIO1 那根线（默认）／打印机 MQTT 上报事件
        ▼
④ 蠕动送料 × 3 次收尾
        │  慢速（creep_speed_pct，默认 45%）短脉冲（creep_pulse_ms，默认 400ms），
        │  每次之间静止 150ms，共 creep_times（默认 3）次
        ▼
⑤ 停电机 → 断开离合（并等机械彻底脱开）
```

第 ④ 步是这套机构的关键：自吸把料送到位后，耗材往往只是**虚顶在挤出机入口**，还没有被挤出机齿轮咬住。三次慢速短脉冲把料一点点推进齿轮咬合区，才能保证后续打印时挤出机真的能拉动料。

蠕动参数（次数 / 单次脉冲时长 / 速度）在网页「微动与自吸」面板上可改，存在 NVS 里。

> 「挤出机到位信号」如果机器上没有空闲 IO 能引出来，可以在网页上把来源切成 **MQTT 事件**，改由打印机上报驱动，就不用接那根线了。

---

## 四、编译与烧录

### 前置：装 ESP-IDF

**必须是 v5.x。** 本项目用了 v5 的 API（`esp_chip_info()`、`OTA_SIZE_UNKNOWN`、`esp_mqtt_client_config_t` 的嵌套字段写法），**降到 v4.x 会直接编译失败** —— v4 里 MQTT 配置还是扁平字段（`uri` / `username` / `password`）。

```bash
# Linux / macOS
git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
~/esp-idf/install.sh esp32s3
. ~/esp-idf/export.sh
```

Windows 用 `install.bat` / `export.bat`，或直接用 VS Code 的 ESP-IDF 插件。

### 编译烧录

```bash
cd esp-ams-s3

idf.py set-target esp32s3      # 只需第一次
idf.py build
idf.py -p COM4 flash monitor    # 端口按实际改：Windows 形如 COM4，Linux 形如 /dev/ttyACM0
```

### 用 esptool 手工烧（不想装 idf.py 时）

```bash
esptool.py --chip esp32s3 --port COM4 -b 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0      build/bootloader/bootloader.bin \
  0x8000   build/partition_table/partition-table.bin \
  0xf000   build/ota_data_initial.bin \
  0x20000  build/esp-ams-s3.bin
```

### 第一次上电

1. 串口会打印版本、芯片、接线表、引脚自检结论。
2. 如果 NVS 里没有能连上的 WiFi 记录 → 自动开热点 **`AMS_WIFI`**（密码 `A12345678`）。
3. 手机连上热点，浏览器打开 **http://192.168.4.1** ，在「网络」页填 WiFi。
4. 配网成功后同一网络下访问板子 IP（也支持 **http://ams.local**）。

> ⚠️ **ESP32 默认不回 ICMP，`ping` 不通是正常的** —— 别用 ping 判断板子在不在线，看网页或串口日志。

---

## 五、Web 配置界面

`web/` 下的三个文件通过 `EMBED_FILES` **编译进固件**（`.rodata` 段），不挂文件系统 —— 没有"SPIFFS 挂载失败导致网页打不开"这一类故障，也没有额外工具依赖。

| 面板 | 作用 |
| --- | --- |
| 状态 | 网络 / 打印机 / 业务 / 内存 / 固件版本 / 复位原因 / 启动次数 |
| 诊断 | 硬件自检结论、离合违规计数、换料与自吸成功失败统计、设备日志 |
| 网络 | 扫描附近 WiFi、连接、开关配置热点 |
| 打印机 | MQTT 参数（IP / 序列号 / 访问码），**读回时不回传访问码** |
| 通道 | 打印机通道号 ↔ 物理料盘位映射、通道颜色 |
| 硬件调试 | 单通道点动（进退响应时间可调）、紧急停止 |
| 微动与自吸 | 逐路开关微动（决定走正常还是降级模式）、手动触发自吸、蠕动参数 |
| 固件升级 | 上传整机 `.bin` 升级（真双分区 OTA） |

前端每 2 秒只请求 **`/status` 一个接口**（约 2KB）——不是每块面板一个请求。这样既省连接又不会出现"某块面板的数据比另一块旧一个周期"的错位。

### 复位原因诊断

`/status` 会回一个把 `esp_reset_reason()` 翻成"下一步该查什么"的说明，而不是复述代号。例如 `BROWNOUT` → "★ 欠压复位。供电不足是最常见的原因：换 USB 口、换短线、外接 5V（离合吸合瞬间电流很大，USB 口带不动）"。

---

## 六、分区表与 4MB 模组

`partitions.csv` 按 **8MB Flash** 排（共占 5MB，留 3MB 余量）：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| `nvs` | 0x9000 | 24KB | WiFi / 打印机参数 / 通道映射 / 当前料盘 |
| `otadata` | 0xF000 | 8KB | "下次从哪块 app 启动"，`esp_ota` 自动维护 |
| `phy_init` | 0x11000 | 4KB | 射频校准数据 |
| `ota_0` | 0x20000 | 1.94MB | 应用（当前版本） |
| `ota_1` | 0x210000 | 1.94MB | 应用（备用版本） |
| `storage` | 0x400000 | 1MB | 预留（放可热更资源 / 导出日志；网页资源现在编在固件里） |

### 4MB Flash 的 S3 模组

装不下上面这张表，需要：`sdkconfig.defaults` 里改成 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`，并把 `partitions.csv` 换成：

```csv
# Name,     Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x6000,
otadata,    data, ota,     0xf000,   0x2000,
phy_init,   data, phy,     0x11000,  0x1000,
ota_0,      app,  ota_0,   0x20000,  0x1c0000,
ota_1,      app,  ota_1,   0x1e0000, 0x1c0000,
```

每块应用 1.75MB（原 1.94MB），去掉 `storage`，总占 3.63MB。CI 里有一道体积检查：应用超过 `ota_0` 分区上限就让构建失败，提前拦住"编译过了但烧不进去"。

### 双分区 OTA 的回滚保险

`main.c` 的最后一步是：

```c
if (web_ok) {
    esp_ota_mark_app_valid_cancel_rollback();
}
```

**这一句不能省。** 开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 之后，新固件启动后如果不主动"确认有效"，下一次重启引导程序就认为它有问题、自动切回上一版 —— 现象是"升级成功了，但一重启又变回旧版本"，非常难查。放在最后（WiFi 和网页都起来了）是刻意的：**只有体检通过才确认**。新固件要是把网页写坏了，就不会确认，下次重启自动回滚。

---

## 七、验证状态（重要）

截至最后一次改动，这份代码的验证情况：

| 项目 | 状态 |
| --- | --- |
| `tools/lint_c.py` 静态检查（22 个 .c/.h） | ✅ 0 处可疑点 |
| `tools/lint_c.py --selftest`（判据自身可靠性） | ✅ 8/8 通过 |
| 跨模块函数声明 ↔ 定义 ↔ 调用一致性 | ✅ 逐项比对无缺失 |
| ESP-IDF v5 API 用法（`esp_chip_info` / `esp_ota_*` / `httpd_*` / `ledc_*` / MQTT 嵌套配置） | ✅ 逐项核对 |
| **`idf.py build` 真编译** | ⏳ **尚未完成** —— 开发机上没装 ESP-IDF 工具链 |

> 也就是说：**这份代码还没有被编译器跑过一遍。** 静态检查能拦住字符串截断、括号不配对、代码区混入中文这类问题，也能人工核对 API 签名，但拦不住预处理器细节、链接期符号缺失、以及各种"IDF 版本差异"。第一次 `idf.py build` 大概率还有要修的地方。
>
> 已经挂了一条 CI 专门干这件事：`.github/workflows/esp-ams-s3-build.yml`。它**只在这个目录有改动时触发**，用官方 `espressif/idf` Docker 镜像跑 `idf.py build`，不发布任何产物。推一个分支上去就能看到结果 —— 这是目前唯一能真正回答"编不编得过"的办法。

### `tools/lint_c.py`

没有交叉编译器时用的替代品，抓四类问题：

1. **代码位置出现中文字符**（最可靠的一条）。C 的标识符只能是 ASCII，中文合法出现的位置只有字符串和注释 —— 所以剔掉字符串和注释之后还能扫到中文，就一定是某个字符串被提前截断了，后面的中文掉出来变成了"代码"。
   这条比"数引号奇偶"可靠得多：`"可能检测到"意面"缺陷（打印乱丝）。"` 这行引号是**成对**的（6 个，偶数），奇偶判据完全看不见，但掉出来的 `意面` 一定暴露。**这条判据正是被一个真实 bug 逼出来的。**
2. 字符串引号数量为奇数（引号落单，作为补充判据）
3. 括号 / 花括号 / 方括号不配对（按整个文件累计，不能按行判 —— `}` 本来就经常单独占一行）
4. 代码位置出现全角标点

```bash
python tools/lint_c.py .          # 扫当前工程
python tools/lint_c.py --selftest # 先验证判据本身没坏（8 条用例）
```

两个命令都返回非 0 表示有问题，可以直接当 CI 的门禁用。
