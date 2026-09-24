# -*- coding: utf-8 -*-
"""
ESP-AMS 换色配置工具
====================

功能：
  1. 选择通道数（4 / 8 / 16）
  2. 配置每个通道的颜色（左键调色，右键启禁用）
  3. 首次换料开关
  4. 配置文件管理（新建 / 打开 / 目录）
  5. 导出 filament_settings.json（供 Bambu Studio 后处理脚本使用）
  6. G-code 匹配预览弹窗（显示每个 T 指令匹配到哪个通道）
  7. 可独立运行（GUI），也可嵌入 Bambu Studio 作为后处理脚本

独立运行（GUI）：
  python esp_ams_postprocess.py
  或编译成 EXE 后直接运行。

Bambu Studio 后处理脚本：
  本文件放在 {BambuStudio}/scripts/filament/ 目录下，
  在偏好设置 → G-code 文件编辑 → 后处理脚本中启用。
  导出后的 filament_settings.json 放在 G-code 输出目录同位置，
  后处理脚本会自动查找并加载，匹配日志写入 <gcode>.ams.log。
"""

import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ============================== 常量 ==============================

# 默认 8 通道颜色（白、灰、黑、红、绿、蓝、橙、黄）
DEFAULT_COLORS_8CH: List[Tuple[int, int, int]] = [
    (255, 255, 255),  # 白
    (128, 128, 128),  # 灰
    (30, 30, 30),     # 黑
    (220, 50, 50),    # 红
    (50, 180, 80),    # 绿
    (50, 80, 220),    # 蓝
    (230, 140, 40),   # 橙
    (220, 210, 40),   # 黄
]

# 16 通道额外颜色（紫、粉、棕、浅绿、深红、浅蓝、灰蓝、金黄）
DEFAULT_COLORS_16CH_EXTRA: List[Tuple[int, int, int]] = [
    (160, 32, 240),   # 紫
    (255, 105, 180),  # 粉
    (139, 69, 19),    # 棕
    (144, 238, 144),  # 浅绿
    (139, 0, 0),      # 深红
    (135, 206, 250),  # 浅蓝
    (100, 149, 237),  # 灰蓝
    (255, 215, 0),    # 金黄
]

SETTINGS_FILENAME = "filament_settings.json"

# 项目里还可能存在这两种叫法的配置（AMS_colors.json 是网页导出的），
# 一并认，免得"明明配了却说没找到"。
LEGACY_SETTINGS_NAMES = ("filament_settings.json", "AMS_colors.json",
                         "esp_ams_map.json")

# ★★ 换料握手信令：`M140 S<n>;EXT`（n = 切片器的耗材序号 + 1）
#    这是**主握手通路** —— 固件读打印机的 bed_target_temper，落在 (0,17)
#    就当成通道号。起始 G-code 写的是 {initial_no_support_extruder+1}，
#    换料 G-code 写的是 {next_extruder + 1}，切片后都变成这一行的字面量。
#
#    ⚠️ 2026-09-24 才发现：本脚本以前**一个字都没改过 M140**，只改了
#    M73 P101（而那份换料 G-code 里 M73 根本没进最终文件）。于是
#    "颜色匹配"整条链路是断的：切片槽号被原样当成物理通道号用了。
#    现场表现就是用户报的"切片软件通道颜色要跟项目一致才能换对颜色"。
#
#    注释标记容错：TOP AMS 的 EXE 产出的是被截断的 `;ET`（少一个 X），
#    两种都认；改写时一律规范成 `;EXT`。
EXT_MARK_RE = re.compile(r'^(\s*M140\s+S)(\d+)(\s*;\s*E[XT]+\s*)$',
                         re.IGNORECASE)

COLOR_DISTANCE_THRESHOLD = 100.0

# G-code 头部映射块的标识行。**同时是幂等守卫**：文件里已经有它 =
# 已经映射过，绝不能再按"值-1=切片槽号"映射第二遍（会把白映射成橄榄）。
MAP_HEADER = ";=========== ESP-AMS 颜色映射（切片槽 → 物理通道）==========="

# 颜色名兜底表：`filament_colour` 缺失时，从 filament_settings_id 的
# 名字里（"Generic PETG 240 230红" / "PLA 白色"）猜颜色。
# ⚠️ 顺序有讲究：长名在前，否则"浅蓝"会被"蓝"先吃掉。
COLOR_NAME_TABLE: List[Tuple[str, Tuple[int, int, int]]] = [
    ("浅蓝", (135, 206, 250)), ("灰蓝", (100, 149, 237)),
    ("深红", (139, 0, 0)), ("浅绿", (144, 238, 144)),
    ("金黄", (255, 215, 0)), ("橙色", (230, 140, 40)),
    ("黄色", (220, 210, 40)), ("灰色", (128, 128, 128)),
    ("白色", (255, 255, 255)), ("黑色", (30, 30, 30)),
    ("红色", (220, 50, 50)), ("绿色", (50, 180, 80)),
    ("蓝色", (50, 80, 220)), ("紫色", (160, 32, 240)),
    ("粉色", (255, 105, 180)), ("棕色", (139, 69, 19)),
    ("白", (255, 255, 255)), ("黑", (30, 30, 30)),
    ("红", (220, 50, 50)), ("绿", (50, 180, 80)),
    ("蓝", (50, 80, 220)), ("黄", (220, 210, 40)),
    ("橙", (230, 140, 40)), ("紫", (160, 32, 240)),
    ("粉", (255, 105, 180)), ("棕", (139, 69, 19)),
    ("灰", (128, 128, 128)),
]

# 弹窗策略（配置里的 "popup" 键）
#   auto（默认）—— 只在**需要人做决定**时才弹：有颜色匹配不上、有歧义、
#                  或者切片颜色比通道还多。一切正常就静默完成，不打断切片。
#   always      —— 每次切片都弹映射确认窗（复刻 TOP AMS 的 EXE 行为）
#   never       —— 从不弹，只写日志和 G-code 注释
POPUP_AUTO, POPUP_ALWAYS, POPUP_NEVER = "auto", "always", "never"

# 各通道数默认颜色
def _default_colors_for_channel_count(n: int) -> List[Tuple[int, int, int]]:
    """生成 n 个通道的默认颜色"""
    if n <= 4:
        # 4 通道：白、灰、黑、蓝
        return [
            (255, 255, 255),
            (128, 128, 128),
            (30, 30, 30),
            (50, 80, 220),
        ]
    elif n <= 6:
        # 6 通道：白、灰、黑、蓝、红、绿
        return [
            (255, 255, 255),
            (128, 128, 128),
            (30, 30, 30),
            (50, 80, 220),
            (220, 50, 50),
            (50, 180, 80),
        ]
    elif n <= 8:
        return list(DEFAULT_COLORS_8CH)
    else:
        colors = list(DEFAULT_COLORS_8CH) + list(DEFAULT_COLORS_16CH_EXTRA)
        if len(colors) < n:
            while len(colors) < n:
                colors.extend(colors[:8])
        return colors[:n]


# ============================== AMS 起始 G-code 占位符 ==============================

def find_ams_initial_tray(
    gcode_content: str,
    ams_colors: Dict[int, Tuple[int, int, int]],
    slice_colors: Dict[int, Tuple[int, int, int]],
) -> int:
    """
    根据 G-code 内容查找首层目标 AMS 通道号。

    优先级：
      1. G-code 里第一个出现的 T 指令（切片器对首层使用的颜色），用
         slice_colors[T] 在 ams_colors 里匹配最接近的颜色
      2. 没有切片颜色信息 → 取 ams_colors 里编号最小的启用通道
      3. 全都没匹配上 → 返回 0（由调用方回退到 1）
    """
    if not ams_colors:
        return 0

    # 1. 找第一个 T 指令（通常是首层用的耗材）
    first_t = None
    m = re.search(r'^T(\d+)', gcode_content, re.MULTILINE)
    if m:
        first_t = int(m.group(1))

    if first_t is not None:
        target = slice_colors.get(first_t)
        if target is not None:
            matched, dist = find_best_channel(target, ams_colors)
            if matched > 0 and dist <= COLOR_DISTANCE_THRESHOLD:
                return matched

    # 2. 回退：启用通道中编号最小的那个
    return min(ams_colors.keys())


def substitute_ams_initial_tray(
    gcode_content: str,
    ams_colors: Dict[int, Tuple[int, int, int]],
    slice_colors: Dict[int, Tuple[int, int, int]],
) -> Tuple[str, List[str]]:
    """
    把 G-code 里的 {ams_initial_tray} 占位符替换为具体的 AMS 通道号。
    返回 (新内容, 日志行列表)。
    """
    if '{ams_initial_tray}' not in gcode_content:
        return gcode_content, []

    target = find_ams_initial_tray(gcode_content, ams_colors, slice_colors)
    if target == 0:
        target = 1
        log = f"警告：无法匹配首层颜色，{{ams_initial_tray}} 回退为通道 1"
    else:
        log = f"已替换 {{ams_initial_tray}} → 通道 {target}"

    return gcode_content.replace('{ams_initial_tray}', str(target)), [log]


# ============================== 颜色工具 ==============================

def rgb_to_hex(r: int, g: int, b: int) -> str:
    return "#{:02X}{:02X}{:02X}".format(r, g, b)


def hex_to_rgb(h: str) -> Optional[Tuple[int, int, int]]:
    h = h.strip().lstrip('#')
    if len(h) != 6:
        return None
    try:
        return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    except (ValueError, IndexError):
        return None


def _is_dark(c: Tuple[int, int, int]) -> bool:
    r, g, b = c
    return 0.299 * r + 0.587 * g + 0.114 * b < 128


def color_distance(c1: Tuple[int, int, int], c2: Tuple[int, int, int]) -> float:
    """RGB 欧氏距离"""
    return ((c1[0] - c2[0]) ** 2 +
            (c1[1] - c2[1]) ** 2 +
            (c1[2] - c2[2]) ** 2) ** 0.5


def find_best_channel(
    target: Tuple[int, int, int],
    ams_colors: Dict[int, Tuple[int, int, int]],
) -> Tuple[int, float]:
    """在 AMS 通道里找最接近 target 的通道号（1 起），返回 (ch, dist)"""
    best_ch = -1
    best_dist = float('inf')
    for ch, c in ams_colors.items():
        d = color_distance(target, c)
        if d < best_dist:
            best_dist = d
            best_ch = ch
    return best_ch, best_dist


# ============================== 配置文件读写 ==============================

def default_settings(channel_count: int = 8) -> dict:
    colors = _default_colors_for_channel_count(channel_count)
    materials = []
    for i, c in enumerate(colors, start=1):
        materials.append({
            "channel": i,
            "color": list(c),
            "enabled": True,
        })
    return {
        "channel_count": channel_count,
        "selected_channel": 1,
        "first_filament": True,  # 首次换料开关
        "materials": materials,
    }


def load_settings(path: str) -> Optional[dict]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        # 兼容 AMS_colors.json 的 AMS[] 格式
        if "AMS" in data and "materials" not in data:
            materials = []
            for item in data.get("AMS", []):
                ch = int(item.get("channel", 0))
                c = item.get("color", [128, 128, 128])
                if isinstance(c, str):
                    parsed = hex_to_rgb(c)
                    c = list(parsed) if parsed else [128, 128, 128]
                materials.append({"channel": ch, "color": c, "enabled": True})
            data["materials"] = materials
            data.setdefault("channel_count", len(materials))
            data.setdefault("selected_channel", 1)
            data.setdefault("first_filament", True)
        if "materials" not in data:
            return None
        return data
    except (json.JSONDecodeError, OSError):
        return None


def save_settings(data: dict, path: str) -> bool:
    try:
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        return True
    except OSError:
        return False


def get_ams_colors(data: dict) -> Dict[int, Tuple[int, int, int]]:
    """从配置中提取 {channel: (r,g,b)}，只包含启用的通道"""
    result = {}
    for m in data.get("materials", []):
        ch = int(m.get("channel", 0))
        c = m.get("color", [])
        if ch > 0 and len(c) >= 3 and m.get("enabled", True):
            result[ch] = (int(c[0]), int(c[1]), int(c[2]))
    return result


# ============================== G-code 改写 ==============================

def name_to_rgb(text: str) -> Optional[Tuple[int, int, int]]:
    """从一段文字（耗材预设名）里猜颜色。猜不到返回 None。"""
    if not text:
        return None
    for name, rgb in COLOR_NAME_TABLE:
        if name in text:
            return rgb
    return None


def parse_slice_colors(gcode_content: str) -> Dict[int, Tuple[int, int, int]]:
    """
    从 G-code 头部注释里提取**切片器实际用的**耗材颜色。

    返回 {耗材序号(0 起): (r,g,b)}。

    ★★ 真实格式（Bambu Studio 02.08 实测，2026-09-24）——

        ; extruder_colour = #018001
        ; filament_colour = #808000;#FFFFFF;#101010;#0000FF
        ; filament_settings_id = "Generic PETG 240 230红";"…白色";"…黑色";"…蓝色"

    ⚠️ 本函数原来只认 `;T0 ;color=#FFFFFF` / `; extruder 0: #FFFFFF` 这类
    **第三方工具自己写的**格式，Bambu Studio 一个都不产出 → 永远返回空 →
    颜色匹配整条链路静默失效（不报错、只是不匹配）。这是"配了颜色也不生效"
    的根因之一。

    三级兜底，越靠后置信度越低（调用方按 confidence 决定要不要提醒用户）：
      ① `; filament_colour = ...`（最权威，就是切片器里的色块）
      ② `; extruder_colour = ...`（单色打印时只有这一个）
      ③ `; filament_settings_id` 里的名字（"…红"/"…白色"）—— 仅在前两级
         完全拿不到时才用
    """
    colors: Dict[int, Tuple[int, int, int]] = {}

    def _split_colors(val: str) -> List[Tuple[int, int, int]]:
        out: List[Tuple[int, int, int]] = []
        for part in val.split(';'):
            p = part.strip().strip('"').strip()
            if not p:
                out.append((0, 0, 0))
                continue
            rgb = hex_to_rgb(p)
            out.append(rgb if rgb else (0, 0, 0))
        return out

    head = gcode_content[:262144]

    # ① filament_colour：形如 `; filament_colour = #808000;#FFFFFF;...`
    m = re.search(r'^;\s*filament_colour\s*=\s*(.+)$', head, re.MULTILINE)
    if m:
        vals = _split_colors(m.group(1))
        for i, c in enumerate(vals):
            # 0x000000 有两种可能：真的是黑色，或者那一格是空的。
            # 用 filament_settings_id 里同序号的名字来分辨。
            colors[i] = c

    # ② extruder_colour：单色/多挤出机机型才有
    if not colors:
        m = re.search(r'^;\s*extruder_colour\s*=\s*(.+)$', head, re.MULTILINE)
        if m:
            vals = _split_colors(m.group(1))
            for i, c in enumerate(vals):
                colors[i] = c

    # ③ 名字兜底：把拿到 (0,0,0) 或压根没有的槽位，用预设名里带的中文补齐
    m = re.search(r'^;\s*filament_settings_id\s*=\s*(.+)$', head, re.MULTILINE)
    if m:
        names = [p.strip().strip('"') for p in m.group(1).split(';')]
        for i, nm in enumerate(names):
            if colors.get(i) in (None, (0, 0, 0)):
                guess = name_to_rgb(nm)
                if guess:
                    colors[i] = guess

    return colors


def build_channel_map(
    ams_colors: Dict[int, Tuple[int, int, int]],
    slice_colors: Dict[int, Tuple[int, int, int]],
    threshold: float = COLOR_DISTANCE_THRESHOLD,
) -> Tuple[Dict[int, int], List[str], List[Dict]]:
    """
    算出「切片耗材序号(0 起) → 物理通道号(1 起)」的映射表。

    返回 (映射表, 日志行, 明细行列表)。
    明细行是给弹窗/注释用的结构化数据，每项：
        {"idx":0, "color":(r,g,b), "ch":1, "dist":0.0, "status":"ok"}

    ★★ 这就是"切片软件通道颜色随便设，开发板自动换对颜色"的实现点。
       切片器的槽位号只在这张表里活一次，写进 G-code 的永远是物理通道号 ——
       固件那边一个字都不用改（它本来就只认通道号）。
    """
    mapping: Dict[int, int] = {}
    logs: List[str] = []
    detail: List[Dict] = []

    if not ams_colors:
        for idx in sorted(slice_colors):
            mapping[idx] = idx + 1
            detail.append({"idx": idx, "color": slice_colors[idx],
                           "ch": idx + 1, "dist": -1.0, "status": "no_cfg"})
        logs.append("警告：项目通道颜色为空（GUI 里没配 / 找不到配置文件）"
                    "→ 只能按通道号换料，颜色对不对全看运气")
        return mapping, logs, detail

    if not slice_colors:
        logs.append("警告：切片文件里没有颜色信息 → 只能按通道号换料")
        for idx in range(len(ams_colors)):
            mapping[idx] = idx + 1
            detail.append({"idx": idx, "color": None, "ch": idx + 1,
                           "dist": -1.0, "status": "no_slice"})
        return mapping, logs, detail

    used: Dict[int, List[int]] = {}      # 通道 → 哪些切片槽占了它
    for idx in sorted(slice_colors):
        c = slice_colors[idx]
        best_ch, dist = find_best_channel(c, ams_colors)
        if best_ch > 0 and dist <= threshold:
            mapping[idx] = best_ch
            status = "ok"
            logs.append("切片槽%d（%s）→ 通道%d（色差 %.1f）✓"
                        % (idx + 1, rgb_to_hex(*c), best_ch, dist))
        else:
            mapping[idx] = idx + 1
            status = "far" if best_ch > 0 else "none"
            logs.append("切片槽%d（%s）→ **匹配不上**（最近是通道%d，色差 %.1f "
                        "> 阈值 %.0f），保留原通道号 %d"
                        % (idx + 1, rgb_to_hex(*c), best_ch, dist,
                           threshold, idx + 1))
        used.setdefault(mapping[idx], []).append(idx)
        detail.append({"idx": idx, "color": c, "ch": mapping[idx],
                       "dist": dist, "status": status})

    # 两个不同的切片槽落到同一个物理通道 → 值得提一句（可能是同料，也可能是
    # 两个白料盘漏配了颜色；后者打出来颜色就会串）
    for ch, idxs in sorted(used.items()):
        if len(idxs) > 1:
            logs.append("注意：切片槽 %s 都映射到通道 %d —— 若它们真是同一种料"
                        "就正常；否则请在 GUI 里把两个通道的颜色区分开"
                        % ("、".join(str(i + 1) for i in idxs), ch))

    return mapping, logs, detail


def map_line_for(idx: int, ch: int, color, dist: float, status: str) -> str:
    """给 G-code 里插的注释行：现场 `grep ESP-AMS` 就能核验映射。"""
    if color is None:
        return ";ESP-AMS 切片槽%d → 通道%d（切片没给颜色）" % (idx + 1, ch)
    if status == "ok":
        return (";ESP-AMS 切片槽%d = %s → **通道%d**（色差 %.1f）"
                % (idx + 1, rgb_to_hex(*color), ch, dist))
    return (";ESP-AMS 切片槽%d = %s → 通道%d（⚠ 色差 %.1f，没匹配上，原样保留）"
            % (idx + 1, rgb_to_hex(*color), ch, max(dist, 0.0)))


def rewrite_gcode(
    gcode_content: str,
    ams_colors: Dict[int, Tuple[int, int, int]],
    slice_colors: Dict[int, Tuple[int, int, int]],
    first_filament: bool = True,
    threshold: float = COLOR_DISTANCE_THRESHOLD,
) -> Tuple[str, List[str], Dict, List[Dict]]:
    """
    改写 G-code 里的换料信令，把**切片槽号**换成**物理通道号**。

    返回 (改写后的内容, 日志行, 汇总 dict, 映射明细)

    ★ 原则：**只改已有的信令，不新增任何信令。**
      （老版本会顺手给每个 `T<n>` 插一条 `M73 P101 R<n>`，见下面 ② 的注释 ——
        那是个能凭空多触发几次换料的 bug。）

    改两处：
      ① ★ `M140 S<n>;EXT` —— 主握手通路（值 = 切片槽号 + 1）。
         **以前完全没改过这一条**，所以颜色匹配整条链路是断的。
      ② `M73 P101 R<n>`   —— 备用通路（宏里写了才会出现；n = 切片槽号）。

    `first_filament` 保留只为兼容旧调用（首次换料的冲刷量归宏管，本脚本不碰）。
    """
    del first_filament

    log_lines: List[str] = []

    # ★ 幂等守卫：头部已经有映射块 → 这个文件已经被处理过了。
    #
    #   为什么必须有：改写后的 `M140 S<n>` 里 n 是**物理通道号**，
    #   再按"值 - 1 = 切片槽号"去映射一次就会变成别的通道（实测：白 → 橄榄）。
    #   而"重复执行"在这条链路上很容易发生 —— 手动再跑一次、切片器把已导出
    #   的文件又交给后处理一次、或者拿旧文件做实验。
    mapping, map_logs, detail = build_channel_map(ams_colors, slice_colors,
                                                  threshold)
    if MAP_HEADER in gcode_content:
        log_lines.append("这个 G-code 已经映射过了（头部有 " + MAP_HEADER
                         + "）→ 跳过，不重复改写")
        summary = {"ams_channels": len(ams_colors),
                   "slice_colors": len(slice_colors),
                   "m140_ok": 0, "m140_miss": 0, "m73_ok": 0,
                   "change_points": 0, "needs_human": False,
                   "already_done": True}
        return gcode_content, log_lines, summary, detail

    log_lines.extend(map_logs)

    summary = {
        "ams_channels": len(ams_colors),
        "slice_colors": len(slice_colors),
        "m140_ok": 0, "m140_miss": 0,
        "m73_ok": 0, "change_points": 0,
        "needs_human": False,
    }
    # 需要人来拍板的情形：颜色匹配不上、或者压根没有颜色可比
    if any(d["status"] in ("far", "none", "no_slice", "no_cfg") for d in detail):
        summary["needs_human"] = True

    t_real = sum(1 for l in gcode_content.split('\n')
                 if re.match(r'^T(\d+)\s*(?:;.*)?$', l.strip(), re.IGNORECASE)
                 and int(re.match(r'^T(\d+)', l.strip(), re.IGNORECASE)
                         .group(1)) < 100)
    m73_count = len(re.findall(r'M73\s+P101\s+R\d+', gcode_content))
    m140_count = sum(1 for l in gcode_content.split('\n')
                     if EXT_MARK_RE.match(l))
    log_lines.insert(0, "G-code 统计：M140 换料信令 %d 处，真换料点 T<n> %d 处，"
                        "M73 P101 %d 处" % (m140_count, t_real, m73_count))

    # ★ 改写 {ams_initial_tray} 占位符（起始 G-code 用自定义占位符时才有）
    if '{ams_initial_tray}' in gcode_content:
        gcode_content, ams_tray_logs = substitute_ams_initial_tray(
            gcode_content, ams_colors, slice_colors)
        log_lines.extend(ams_tray_logs)

    lines = gcode_content.split('\n')
    out_lines: List[str] = []
    ptr: Dict[int, int] = {d["idx"]: i for i, d in enumerate(detail)}

    for line in lines:

        # ---------- ① M140 S<n>;EXT：主握手通路 ----------
        m = EXT_MARK_RE.match(line)
        if m:
            slice_ch = int(m.group(2))
            idx = slice_ch - 1                      # 切片槽号 = 值 - 1
            new_ch = mapping.get(idx, slice_ch)
            d = detail[ptr[idx]] if idx in ptr else None
            color = d["color"] if d else slice_colors.get(idx)
            dist = d["dist"] if d else -1.0
            status = d["status"] if d else "no_slice"

            out_lines.append(map_line_for(idx, new_ch, color, dist, status))
            out_lines.append("%s%d%s" % (m.group(1), new_ch, ";EXT"))
            if new_ch == slice_ch:
                summary["m140_miss"] += 1
            else:
                summary["m140_ok"] += 1
            continue

        # ---------- ② 裸露的 T 指令：**只加注释、绝不新增信令** ----------
        #
        # ★★ 这里删掉了一段"把 T<n> 就地展开成 M73 P101 R<n>"的老逻辑。
        #    它是**危险的**，触发条件还特别常见：
        #      · Bambu 的起始/结束宏里本来就写着 `T1000`（"没有 AMS 工具"的
        #        内部标记）和 `T255`（卸载），它们会命中 `^T(\d+)`；
        #      · 老逻辑于是给每个 T 都插一条 `M73 P101 R1001`。
        #    而固件把 `mc_percent == 101` 当作**换料请求**（ams_controller.c
        #    的 r->mc_percent == 101 分支）→ 凭空多出 3 次换料请求，
        #    通道号还是 1001 这种垃圾值。
        #    两份真实切片里这类 T 各有 3 处，也就是说"一挂脚本就多触发 3 次"。
        #
        #    规则：**只改已有的信令，不加新的。** 时序是宏作者的，改时序要
        #    有实测支撑，不能靠后处理脚本顺手"加固"。
        m_t = re.match(r'^T(\d+)\s*(?:;.*)?$', line.strip(), re.IGNORECASE)
        if m_t:
            n = int(m_t.group(1))
            if n < 100 and n in ptr:      # <100 才是真的换料点
                d = detail[ptr[n]]
                out_lines.append(
                    ";ESP-AMS 换料点：切片槽%d = %s → 通道%d"
                    % (n + 1, rgb_to_hex(*d["color"]) if d["color"] else "（无）",
                       d["ch"]))
                summary["change_points"] = summary.get("change_points", 0) + 1
            out_lines.append(line)
            continue

        # ---------- ③ 已有的 M73 P101 R<n>（部分宏会写）----------
        #   注意：这里的 n 是**切片器的 0 起序号**（宏里写 R[next_extruder]），
        #   而 M140 S<n> 里的 n 是"序号 + 1"。两者口径不同，别抄错。
        m73 = re.match(r'^(M73\s+P101\s+R)(\d+)(\s*;.*)?$',
                       line.strip(), re.IGNORECASE)
        if m73:
            idx = int(m73.group(2))
            new_ch = mapping.get(idx, idx + 1)
            out_lines.append("%s%d" % (m73.group(1), new_ch)
                             + (m73.group(3) or ""))
            if new_ch != idx + 1:
                summary["m73_ok"] = summary.get("m73_ok", 0) + 1
            continue

        out_lines.append(line)

    result = '\n'.join(out_lines)

    # ---------- ★ 把映射摘要写进文件头注释 ----------
    # 为什么要写进文件：`.ams.log` 放在输出目录里，用户不一定会去看；
    # 而切片完的 G-code 本身就是要发给打印机的东西，"打开看一眼映射对不对"
    # 是最顺手的一次核验。现场 `grep ESP-AMS xxx.gcode` 即可。
    # ★ 幂等守卫见函数开头（MAP_HEADER 检查）。

    if mapping:
        stamp = [MAP_HEADER]
        for d in detail:
            stamp.append(map_line_for(d["idx"], d["ch"], d["color"],
                                      d["dist"], d["status"]))
        stamp.append("; 只改两处已有信令：M140 S<n>;EXT 和 M73 P101 R<n>。"
                     "不新增任何信令。")
        stamp.append(";======================================================")
        result = '\n'.join(stamp) + '\n' + result

    return result, log_lines, summary, detail


def find_settings_file(gcode_path: str) -> Optional[str]:
    """在 G-code 所在目录及上级目录查找配置文件"""
    base = Path(gcode_path).parent
    for d in (base, base.parent):
        for name in LEGACY_SETTINGS_NAMES:
            c = d / name
            if c.is_file():
                return str(c)
    return None


def _app_base() -> Path:
    """脚本/EXE 所在目录（PyInstaller 打包后就是 EXE 所在目录）"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).parent
    return Path(__file__).parent


def _bambu_config_dir() -> Optional[str]:
    """查找 GUI 导出的配置文件目录。

    ★★ 这里踩过一个"配了也不生效"的坑（2026-09-24）：GUI（esp_ams_tool.exe）
       把配置写在 **<EXE同目录>/ams_profiles/filament_settings.json**，
       而本函数的候选目录里**没有 ams_profiles** —— 于是 EXE 作为后处理脚本
       跑的时候永远找不到配置，只能退回"按通道号换料"，且只在控制台里
       （windowed 打包后连控制台都没有）打一句，用户完全看不到。
       现在把 ams_profiles 排在最前，并且加一个"上次打开过的目录"记忆文件。
    """
    base = _app_base()
    candidates: List[Path] = []
    for root in (base, base.parent, Path.home()):
        for sub in ("ams_profiles", "ESP_AMS", "filament_settings", ""):
            candidates.append(root / sub if sub else root)
    candidates.append(Path.home() / "ESP_AMS")
    candidates.append(Path(gcode_last_dir_hint() or "."))

    for c in candidates:
        for name in LEGACY_SETTINGS_NAMES:
            target = c / name
            if target.is_file():
                return str(c)
    return None


def gcode_last_dir_hint() -> Optional[str]:
    """读 GUI 记下的"上次使用的配置目录"（没有就返回 None）。"""
    try:
        p = _app_base() / ".esp_ams_last_dir"
        if p.is_file():
            v = p.read_text(encoding="utf-8").strip()
            return v or None
    except OSError:
        pass
    return None


def find_settings_extended() -> Optional[str]:
    """比 find_settings_file 更宽的搜索：脚本目录 + ams_profiles + 用户目录"""
    config_dir = _bambu_config_dir()
    if config_dir:
        for name in LEGACY_SETTINGS_NAMES:
            target = Path(config_dir) / name
            if target.is_file():
                return str(target)
    return None


# ============================== 后处理脚本入口（Bambu Studio 用） ==============================

def safe_print(*args) -> None:
    """打印到控制台。

    ★ PyInstaller 用 `--windowed` 打包后 sys.stdout 是 None，直接 print()
      会抛 AttributeError；而 Bambu Studio 又把后处理进程的 stdout 收走，
      所以任何一次裸 print 都可能把整个后处理搞崩 —— 而崩了就是"窗口一闪
      然后什么都没发生"，正是现场看到的现象。这里统一兜住。
    """
    try:
        if sys.stdout is None:
            return
        print(*args)
    except Exception:
        pass


def load_config_for(gcode_path: str, explicit: Optional[str] = None):
    """统一的配置加载：找文件 → 读出通道颜色 / 首次换料 / 弹窗策略。

    返回 (settings_path, ams_colors, first_filament, popup_mode)
    """
    settings_path = explicit or find_settings_file(gcode_path) \
        or find_settings_extended()
    ams_colors: Dict[int, Tuple[int, int, int]] = {}
    first_filament = True
    popup_mode = POPUP_AUTO

    if settings_path:
        data = load_settings(settings_path)
        if data:
            ams_colors = get_ams_colors(data)
            first_filament = bool(data.get("first_filament", True))
            popup_mode = str(data.get("popup", POPUP_AUTO)).lower()
            if popup_mode not in (POPUP_AUTO, POPUP_ALWAYS, POPUP_NEVER):
                popup_mode = POPUP_AUTO
        else:
            safe_print("[ESP-AMS] 警告：配置文件读不出来（不是有效 JSON？）：%s"
                       % settings_path)
            settings_path = None
    else:
        safe_print("[ESP-AMS] 没找到配置文件 —— 只能按通道号换料，"
                   "颜色对不对全看切片槽和物理通道是不是同一个顺序。"
                   "请双击 esp_ams_tool 配一次通道颜色。")
    return settings_path, ams_colors, first_filament, popup_mode


def format_match_log(gcode_path: str, settings_path: Optional[str],
                     ams_colors, slice_colors, first_filament: bool,
                     log_lines: List[str]) -> str:
    """`.ams.log` 的正文。"""
    out = []
    out.append("[ESP-AMS] 匹配日志  %s" % gcode_path)
    out.append("[ESP-AMS] 配置文件：%s" % (settings_path or "**没找到**"))
    out.append("[ESP-AMS] 项目通道：%d 个   切片颜色：%d 种"
               % (len(ams_colors), len(slice_colors)))
    for ch, c in sorted(ams_colors.items()):
        out.append("[ESP-AMS]   通道%d = %s" % (ch, rgb_to_hex(*c)))
    out.append("[ESP-AMS] 首次换料：%s" % ("开" if first_filament else "关"))
    out.append("-" * 56)
    for l in log_lines:
        out.append("  " + l)
    out.append("-" * 56)
    bad = [l for l in log_lines if "匹配不上" in l]
    if bad:
        out.append("")
        out.append("⚠ 有 %d 处颜色没匹配上（已保留原通道号）。"
                   "要么切片里用的颜色在项目通道里没有，要么需要把阈值放宽。"
                   % len(bad))
    return "\n".join(out) + "\n"


def show_map_window(detail: List[Dict], ams_colors,
                    gcode_path: str, settings_path: Optional[str],
                    goto: str = "") -> None:
    """弹一个"切片槽 → 物理通道"的确认窗（复刻 TOP AMS EXE 的交互）。

    只在两种情况下被调用：配置里写了 `popup=always`，或者映射需要人拍板。
    窗口是**非阻塞收尾**的：确认即关，不会改变已经写好的 G-code。
    """
    try:
        import tkinter as tk
        from tkinter import ttk
    except Exception:
        return

    try:
        root = tk.Tk()
    except Exception:
        # 无显示环境（或已经从别的 Tk 实例里起来）→ 静默跳过
        return

    root.title("ESP-AMS 切片槽 → 物理通道 映射确认")
    root.resizable(False, False)

    ttk.Label(root, text=f"文件名：{Path(gcode_path).name}",
              padding=(12, 10, 12, 2)).pack(anchor="w")
    ttk.Label(root, text=f"配置文件：{settings_path or '**没找到**'}",
              padding=(12, 0, 12, 6)).pack(anchor="w")

    box = ttk.Frame(root, padding=(12, 0, 12, 6))
    box.pack(fill=tk.X)
    ttk.Label(box, text="切片槽", width=8).grid(row=0, column=0)
    ttk.Label(box, text="切片里设的颜色", width=16).grid(row=0, column=1)
    ttk.Label(box, text="→ 实际换料通道", width=16).grid(row=0, column=2)
    ttk.Label(box, text="结论", width=18).grid(row=0, column=3)

    status_text = {
        "ok": "✓ 已按颜色对上",
        "far": "⚠ 色差太大，原样保留",
        "none": "⚠ 项目里没有这个颜色",
        "no_slice": "⚠ 切片没给颜色",
        "no_cfg": "⚠ 项目未配置颜色",
    }
    for i, d in enumerate(detail, start=1):
        c = d["color"] or (0, 0, 0)
        hexs = rgb_to_hex(*c) if d["color"] else "（无）"
        ttk.Label(box, text=f"第 {d['idx'] + 1} 个").grid(row=i, column=0)
        ttk.Label(box, text=hexs).grid(row=i, column=1)
        ch_txt = f"通道 {d['ch']}"
        if d["status"] == "ok" and d["dist"] >= 0:
            ch_txt += f"（色差 {d['dist']:.1f}）"
        ttk.Label(box, text=ch_txt).grid(row=i, column=2)
        ttk.Label(box, text=status_text.get(d["status"], d["status"])) \
            .grid(row=i, column=3)

    ttk.Label(root, text="映射已写进 G-code（搜 ESP-AMS 就能看到这几行），"
                         "确认无误直接关窗即可。",
              padding=(12, 4, 12, 2), wraplength=420).pack(anchor="w")

    btns = ttk.Frame(root, padding=(12, 6, 12, 12))
    btns.pack(fill=tk.X)

    def _open_tool():
        """另开一个进程启动配置 GUI —— 本进程不能自己开 Tk 主循环，
        它正处在"给切片器写文件"的关键路径上，卡住就会把切片顶住。"""
        try:
            import subprocess
            if getattr(sys, "frozen", False):
                exe = sys.executable
                subprocess.Popen([exe, "gui"], close_fds=True)
            else:
                subprocess.Popen([sys.executable, os.path.abspath(__file__),
                                  "gui"], close_fds=True)
        except Exception:
            pass
        root.destroy()

    ttk.Button(btns, text="确定", command=root.destroy).pack(side=tk.LEFT, padx=6)
    ttk.Button(btns, text="打开配置工具改颜色", command=_open_tool) \
        .pack(side=tk.LEFT, padx=6)

    try:
        root.attributes("-topmost", True)
    except Exception:
        pass
    root.mainloop()


def process_file(in_path: str, out_path: Optional[str] = None,
                 explicit_config: Optional[str] = None,
                 write_back: bool = True,
                 popup_mode: Optional[str] = None,
                 do_popup: bool = True,
                 verbose: bool = True) -> Dict:
    """后处理的**唯一**实现，CLI 与 Bambu Studio 钩子都走这里。

    返回汇总 dict（含 log_lines / detail / summary），方便调用方决定退出码。
    """
    settings_path, ams_colors, first_filament, cfg_popup = \
        load_config_for(in_path, explicit_config)
    mode = (popup_mode or cfg_popup or POPUP_AUTO).lower()

    try:
        with open(in_path, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except OSError as e:
        safe_print("[ESP-AMS] 读不到 G-code：%s（%s）" % (in_path, e))
        return {"ok": False, "error": "read_failed", "log_lines": []}

    slice_colors = parse_slice_colors(content)
    rewritten, log_lines, summary, detail = rewrite_gcode(
        content, ams_colors, slice_colors, first_filament)

    out = out_path or in_path
    if write_back:
        try:
            with open(out, 'w', encoding='utf-8', newline='\n') as f:
                f.write(rewritten)
        except OSError as e:
            safe_print("[ESP-AMS] 写不回 G-code：%s（%s）" % (out, e))
            return {"ok": False, "error": "write_failed", "log_lines": log_lines}

    log_file = ""
    if write_back:
        log_file = out + '.ams.log'
        try:
            with open(log_file, 'w', encoding='utf-8') as lf:
                lf.write(format_match_log(in_path, settings_path, ams_colors,
                                          slice_colors, first_filament, log_lines))
        except OSError:
            log_file = ""

    if verbose:
        safe_print("[ESP-AMS] 配置文件：%s" % (settings_path or "**没找到**"))
        safe_print("[ESP-AMS] 项目通道 %d 个 / 切片颜色 %d 种"
                   % (len(ams_colors), len(slice_colors)))
        for line in log_lines:
            safe_print("[ESP-AMS] " + line)
        safe_print("[ESP-AMS] M140 信令改写了 %d 处（保持原样 %d 处）"
                   % (summary.get("m140_ok", 0), summary.get("m140_miss", 0)))
        if log_file:
            safe_print("[ESP-AMS] 明细日志：%s" % log_file)

    # ---- 弹窗判定 ----
    need_popup = (mode == POPUP_ALWAYS) or \
                 (mode == POPUP_AUTO and summary.get("needs_human"))
    if do_popup and need_popup and detail:
        show_map_window(detail, ams_colors, in_path, settings_path)

    return {"ok": True, "settings_path": settings_path, "log_lines": log_lines,
            "summary": summary, "detail": detail, "log_file": log_file,
            "slice_colors": slice_colors, "ams_colors": ams_colors}


def onBeforeWriteGCode(input_file, gcode_path, project, filename, output):
    """Bambu Studio 后处理钩子。

    ⚠️ 这个钩子只在把本文件当 Python 模块挂进切片器时才会被调用；
    通过"偏好设置 → 后处理脚本"配 .bat/.exe 的方式走的是命令行入口。
    两条路共用同一套映射逻辑（rewrite_gcode），结果完全一致。
    """
    settings_path, ams_colors, first_filament, popup_mode = \
        load_config_for(input_file)
    try:
        with open(input_file, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except OSError:
        output.write("ESP-AMS: 无法读取 G-code 文件\n", encoding='utf-8')
        return

    slice_colors = parse_slice_colors(content)
    rewritten, log_lines, summary, detail = rewrite_gcode(
        content, ams_colors, slice_colors, first_filament)

    for line in log_lines:
        safe_print("[ESP-AMS] " + line)

    try:
        with open(gcode_path + '.ams.log', 'w', encoding='utf-8') as lf:
            lf.write(format_match_log(input_file, settings_path, ams_colors,
                                      slice_colors, first_filament, log_lines))
    except OSError:
        pass

    output.write(rewritten, encoding='utf-8')


# ============================== GUI（Tkinter） ==============================

def _run_gui():
    """启动 Tkinter GUI，匹配第三方 EXE 工具界面风格"""
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, colorchooser

    # ---- 状态 ----
    state = {
        "data": default_settings(8),
        "ch_count": 8,
        "dir": str(Path.home() / "ESP_AMS"),
        "filename": SETTINGS_FILENAME,
    }

    root = tk.Tk()
    root.title("ESP-AMS 换色配置工具")
    root.resizable(False, False)

    # ============ 顶部：通道数选择（4通道配置/6通道配置/8通道配置/16通道配置） ============
    top_frame = ttk.Frame(root, padding=(10, 8, 10, 4))
    top_frame.pack(fill=tk.X)

    ttk.Label(top_frame, text="通道数：").pack(side=tk.LEFT)
    ch_var = tk.IntVar(value=state["ch_count"])
    ch_buttons = ttk.Frame(top_frame)
    ch_buttons.pack(side=tk.LEFT, padx=5)

    def _apply_channel_count(n):
        """切换通道数，保留已有颜色"""
        old_materials = state["data"]["materials"]
        new_data = default_settings(n)
        for i in range(n):
            if i < len(old_materials):
                new_data["materials"][i]["color"] = old_materials[i].get(
                    "color", new_data["materials"][i]["color"])
                new_data["materials"][i]["enabled"] = old_materials[i].get("enabled", True)
        state["data"] = new_data
        state["ch_count"] = n
        ch_var.set(n)
        _draw_colors()

    for ch in [4, 8, 16]:
        btn = ttk.Radiobutton(
            ch_buttons, text=f"{ch}通道", value=ch,
            variable=ch_var,
            command=lambda c=ch: _apply_channel_count(c),
        )
        btn.pack(side=tk.LEFT, padx=4)

    # ============ 色块区域（顶部，大色块 + 编号） ============
    color_label = ttk.Label(root, text="智能匹配下方颜色", foreground="#888")
    color_label.pack(pady=(2, 0))

    color_frame = ttk.LabelFrame(root, text="预设耗材（左键调色，右键启禁用）", padding=8)
    color_frame.pack(fill=tk.X, padx=10, pady=4)

    color_canvas = tk.Canvas(color_frame, bg="#F0F0F0", height=60)
    color_canvas.pack(fill=tk.X)
    color_items: List[dict] = []

    def _draw_colors():
        color_canvas.delete("all")
        color_items.clear()
        n = state["ch_count"]
        cols = min(n, 8)
        rows = (n + cols - 1) // cols
        cell_w, cell_h = 52, 44

        materials = state["data"]["materials"]
        while len(materials) < n:
            materials.append({"channel": len(materials) + 1,
                              "color": [128, 128, 128], "enabled": True})

        for i in range(n):
            m = materials[i]
            ch = m.get("channel", i + 1)
            c = tuple(m.get("color", [128, 128, 128]))
            enabled = m.get("enabled", True)
            row, col = divmod(i, cols)
            x = col * (cell_w + 8) + 4
            y = row * (cell_h + 8) + 4

            hex_c = rgb_to_hex(*c) if enabled else "#CCCCCC"
            outline = "#333333" if enabled else "#999999"
            text_fill = "white" if (enabled and _is_dark(c)) else (
                "#666666" if not enabled else "black")

            rect = color_canvas.create_rectangle(
                x, y, x + cell_w, y + cell_h,
                fill=hex_c, outline=outline, width=2,
            )
            text = color_canvas.create_text(
                x + cell_w // 2, y + cell_h // 2,
                text=str(ch), font=("Arial", 12, "bold"),
                fill=text_fill,
            )
            item = {"ch": ch, "color": list(c), "enabled": enabled,
                    "rect_id": rect, "text_id": text}
            color_items.append(item)
            color_canvas.tag_bind(rect, "<Button-1>",
                                  lambda e, it=item: _on_color_click(e, it))
            color_canvas.tag_bind(text, "<Button-1>",
                                  lambda e, it=item: _on_color_click(e, it))
            color_canvas.tag_bind(rect, "<Button-3>",
                                  lambda e, it=item: _on_color_right_click(e, it))

        total_w = cols * (cell_w + 8) + 8
        total_h = rows * (cell_h + 8) + 8
        color_canvas.config(width=total_w, height=max(total_h, 60))

    def _on_color_click(event, item):
        ch = item["ch"]
        cur_hex = rgb_to_hex(*item["color"])
        result = colorchooser.askcolor(
            initialcolor=cur_hex,
            parent=root,
            title=f"选择通道 {ch} 颜色",
        )
        if result[1]:
            rgb = hex_to_rgb(result[1])
            if rgb:
                for m in state["data"]["materials"]:
                    if m["channel"] == ch:
                        m["color"] = list(rgb)
                        break
                _draw_colors()

    def _on_color_right_click(event, item):
        """右键：切换通道启用/禁用"""
        ch = item["ch"]
        for m in state["data"]["materials"]:
            if m["channel"] == ch:
                m["enabled"] = not m.get("enabled", True)
                break
        _draw_colors()

    # ============ 配置文件管理 ============
    config_frame = ttk.LabelFrame(root, text="配置文件管理", padding=8)
    config_frame.pack(fill=tk.X, padx=10, pady=4)

    # 第一行：下拉框 + 新建 + 打开目录
    row1 = ttk.Frame(config_frame)
    row1.pack(fill=tk.X)

    config_names: List[str] = []

    def _refresh_config_names(combo=None):
        """刷新配置文件列表，combo 为 None 时只更新 config_names"""
        nonlocal config_names
        config_names = []
        try:
            for f in os.listdir(state["dir"]):
                if f.endswith(".json"):
                    config_names.append(f)
        except OSError:
            pass
        if not config_names:
            config_names = [SETTINGS_FILENAME]
        if combo is not None:
            combo['values'] = config_names
            combo.set(config_names[0])

    config_var = tk.StringVar(value=SETTINGS_FILENAME)
    config_combo = ttk.Combobox(row1, textvariable=config_var,
                                 values=[SETTINGS_FILENAME], width=20, state="readonly")
    config_combo.pack(side=tk.LEFT, padx=4)

    # 初始化时填充
    _refresh_config_names(config_combo)

    def _new_config():
        name = f"default_{state['ch_count']}ch.json"
        state["data"] = default_settings(state["ch_count"])
        path = os.path.join(state["dir"], name)
        save_settings(state["data"], path)
        config_var.set(name)
        _draw_colors()

    def _open_config():
        path = filedialog.askopenfilename(
            parent=root, title="打开配置文件",
            filetypes=[("JSON", "*.json")],
        )
        if path:
            data = load_settings(path)
            if data:
                state["data"] = data
                state["ch_count"] = data.get("channel_count", 8)
                ch_var.set(state["ch_count"])
                state["dir"] = str(Path(path).parent)
                config_var.set(os.path.basename(path))
                _refresh_config_names(config_combo)
                _draw_colors()
            else:
                messagebox.showerror("无法打开", "文件不是有效的 AMS 配置文件")

    def _open_dir():
        d = filedialog.askdirectory(parent=root, title="选择配置目录")
        if d:
            state["dir"] = d
            _refresh_config_names(config_combo)

    ttk.Button(row1, text="新建", command=_new_config).pack(side=tk.LEFT, padx=4)
    ttk.Button(row1, text="打开目录", command=_open_dir).pack(side=tk.LEFT, padx=4)

    # ============ 底部按钮区域 ============
    btn_frame = ttk.Frame(root, padding=(10, 6, 10, 10))
    btn_frame.pack(fill=tk.X)

    # 首次换料开关
    first_var = tk.BooleanVar(value=True)

    def _on_first_toggle():
        state["data"]["first_filament"] = first_var.get()
        first_btn.config(
            text=f"首次换料：{'开' if first_var.get() else '关'}",
            background="#4CAF50" if first_var.get() else "#999999",
        )

    first_btn = ttk.Button(btn_frame, text="首次换料：开", command=_on_first_toggle)
    first_btn.pack(side=tk.LEFT, padx=8)
    first_btn.configure(style="Green.TButton")

    # 自定义绿色按钮样式
    style = ttk.Style()
    style.configure("Green.TButton", font=("Arial", 10, "bold"))

    # 确定按钮
    def _do_confirm():
        state["data"]["channel_count"] = state["ch_count"]
        state["data"]["first_filament"] = first_var.get()
        name = config_var.get()
        export_path = os.path.join(state["dir"], name)
        if save_settings(state["data"], export_path):
            _show_export_dialog(export_path)
        else:
            messagebox.showerror("保存失败", f"无法写入：{export_path}")

    confirm_btn = ttk.Button(btn_frame, text="确定", command=_do_confirm)
    confirm_btn.pack(side=tk.LEFT, padx=8)

    # G-code 匹配预览
    def _do_match_preview():
        gcode_path = filedialog.askopenfilename(
            parent=root, title="选择 G-code 文件",
            filetypes=[
                ("G-code", "*.gcode"),
                ("TXT", "*.txt"),
                ("所有文件", "*.*"),
            ],
        )
        if not gcode_path:
            return
        ams_colors = get_ams_colors(state["data"])
        first_fil = state["data"].get("first_filament", True)
        try:
            with open(gcode_path, 'r', encoding='utf-8', errors='replace') as f:
                gcode_content = f.read()
        except OSError as e:
            messagebox.showerror("读取失败", f"无法读取：{e}")
            return
        slice_colors = parse_slice_colors(gcode_content)
        _, log_lines, _s, _d = rewrite_gcode(gcode_content, ams_colors,
                                             slice_colors, first_fil)
        _show_match_result_dialog(log_lines, gcode_path)

    ttk.Button(btn_frame, text="G-code 匹配预览", command=_do_match_preview).pack(side=tk.LEFT, padx=8)

    def _show_export_dialog(export_path):
        """导出后弹窗确认，显示通道颜色配置（第二个截图的效果）"""
        ams_colors = get_ams_colors(state["data"])
        enabled_map = {m.get("channel"): m.get("enabled", True)
                       for m in state["data"]["materials"]}

        info_lines = [
            f"通道数：{state['ch_count']}",
            f"首次换料：{'开' if state['data'].get('first_filament') else '关'}",
            f"配置路径：{export_path}",
            "",
            "已配置颜色：",
        ]
        for ch, c in sorted(ams_colors.items()):
            mark = "✓ 启用" if enabled_map.get(ch, True) else "✗ 禁用"
            info_lines.append(f"  通道 {ch}: {rgb_to_hex(*c)}  {mark}")
        info_lines.append("")
        info_lines.append("请将配置文件与切片 G-code 放在同一目录")
        info_lines.append("Bambu Studio 后处理脚本会自动加载并匹配换色指令")
        info_lines.append(f"匹配日志写入：{Path(export_path).parent / 'xxx.ams.log'}")

        messagebox.showinfo("导出成功", "\n".join(info_lines))

    def _show_match_result_dialog(log_lines, gcode_path):
        """显示 G-code 匹配结果弹窗（第二个截图的效果）"""
        win = tk.Toplevel(root)
        win.title(f"匹配结果 - {Path(gcode_path).name}")
        win.geometry("560x400")
        win.transient(root)
        win.grab_set()

        ttk.Label(win, text="G-code 匹配预览", font=("Arial", 10, "bold")).pack(anchor=tk.W, padx=8, pady=(8, 0))
        text = "\n".join(log_lines)
        txt = tk.Text(win, wrap=tk.WORD, font=("Consolas", 9), bg="#FAFAFA")
        txt.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)
        txt.insert("1.0", text)
        txt.config(state=tk.DISABLED)

        ttk.Button(win, text="关闭", command=win.destroy).pack(anchor=tk.E, padx=8, pady=8)

    # 提示文字
    ttk.Label(root, text="右键点击上方耗材色块可切换边框禁用通道",
              font=("Arial", 8), foreground="#888").pack(pady=(0, 6))

    # ============ 初始化 ============
    _draw_colors()

    # 尝试自动加载同目录已有的配置文件
    script_dir = Path(__file__).parent
    # EXE 编译后 __file__ 是临时目录，改用 sys.executable 所在目录
    if getattr(sys, "frozen", False):
        base_dir = Path(sys.executable).parent
    else:
        base_dir = script_dir

    # 优先加载当前脚本目录下的 filament_settings.json
    auto_settings = base_dir / SETTINGS_FILENAME
    if not auto_settings.is_file():
        # 再尝试 state 目录
        auto_settings = Path(state["dir"]) / SETTINGS_FILENAME
    if auto_settings.is_file():
        loaded = load_settings(str(auto_settings))
        if loaded:
            state["data"] = loaded
            state["ch_count"] = loaded.get("channel_count", 8)
            ch_var.set(state["ch_count"])
            state["dir"] = str(auto_settings.parent)
            config_var.set(auto_settings.name)
            _refresh_config_names(config_combo)
            first_var.set(state["data"].get("first_filament", True))
            _draw_colors()

    root.mainloop()


# ============================== 命令行模式 ==============================

def _find_pythonw() -> Optional[str]:
    """找一个 pythonw.exe（GUI 子系统 → 起进程时**不会创建控制台窗口**）。

    这就是"切片后弹个黑窗一闪而过"的解法：把后处理命令从 .bat 换成
    `pythonw.exe esp_ams_postprocess.py`，或者用同样无窗口的 esp_ams_tool.exe。
    """
    import shutil

    cands: List[str] = []

    # ★ 必须自己扫 PATH 的**每一个**目录，不能只用 shutil.which()——
    #   它返回第一个命中项，而本机 PATH 里排第一的恰好是沙箱内的临时
    #   Python（.workbuddy\binaries\...）。那条路径对用户是"过几天就没了"，
    #   填进切片器后表现为"某天开始后处理从来不生效"。
    for d in os.environ.get("PATH", "").split(os.pathsep):
        d = d.strip().strip('"')
        if d:
            cands.append(os.path.join(d, "pythonw.exe"))
    la = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Python")
    if os.path.isdir(la):
        for sub in sorted(os.listdir(la)):
            cands.append(os.path.join(la, sub, "pythonw.exe"))
    cands.append(str(_app_base() / "pythonw.exe"))
    for exe in ("pythonw", "pythonw.exe"):
        p = shutil.which(exe)
        if p:
            cands.append(p)

    def rank(p: str) -> int:
        low = p.lower()
        if not os.path.isfile(p):
            return 99
        # 优先用户自己装的 Python；沙箱/商店别名这类"临时"解释器排后面
        if ".workbuddy" in low or "windowsapps" in low:
            return 20
        if os.sep + "programs" + os.sep + "python" + os.sep in low:
            return 0
        return 5

    cands.sort(key=rank)
    for p in cands:
        if os.path.isfile(p):
            return p
    return None


def rv_escape(p: str) -> str:
    """把路径转成 Bambu Studio 配置里那种转义写法（反斜杠双写 + 整体 \\"）。"""
    return '\\"' + p.replace("\\", "\\\\") + '\\"'


def _cmd_setup():
    """把「该往切片器里填什么」算好并打印出来（含 JSON 转义形式）。

    为什么要有：Bambu Studio 的后处理字段是**带转义的 JSON 字符串**
    （`; post_process = "\\"...\\""`），手工拼极容易错；而填错的表现是
    "切片时窗口一闪就没了" —— 极难自查。
    """
    pyw = _find_pythonw()
    script = os.path.abspath(__file__)
    lines = []
    lines.append("=" * 66)
    lines.append("Bambu Studio → 偏好设置 → 其他 → 「后处理脚本」里填下面其中一条")
    lines.append("=" * 66)
    lines.append("")
    if pyw:
        lines.append("【推荐 · 完全不会闪窗】")
        lines.append('  "%s" "%s"' % (pyw, script))
    else:
        lines.append("（没找到 pythonw.exe —— 装 Python 时勾上 Add to PATH 就有了）")
    lines.append("")
    exe = _app_base() / "esp_ams_tool.exe"
    if exe.is_file():
        lines.append("【也行 · 不需要 Python】")
        lines.append('  "%s"' % exe)
        lines.append("")
    lines.append("【最后选择 · 会闪一下黑窗，但输出会落盘方便排查】")
    lines.append('  "%s"' % (_app_base() / "esp_ams_postprocess.bat"))
    lines.append("")
    lines.append("【连 pythonw 都不想用（wscript 版，也不用 Python 控制台）】")
    lines.append('  "wscript.exe" "%s"' % (_app_base() / "esp_ams_hidden.vbs"))
    lines.append("")
    lines.append("-" * 66)
    lines.append("要粘到 .3mf / 打印机配置里的**转义**写法（整体加 \\\"，反斜杠双写）：")
    if pyw:
        esc = (rv_escape(pyw) + ' ' + rv_escape(script))
        lines.append('  "' + esc + '"')
    lines.append("")
    lines.append("-" * 66)
    lines.append("现在能找到的配置文件：%s" % (find_settings_extended() or "**没有**"))
    lines.append("（先双击 esp_ams_tool 配一次通道颜色，否则只能按通道号换料）")
    lines.append("")
    lines.append("填完**必须重新切片** —— 已切好的 .gcode 里存的是旧通道号。")
    safe_print("\n".join(lines))


def _cmd_export_config():
    """命令行导出配置 / 匹配"""
    import argparse
    parser = argparse.ArgumentParser(description="ESP-AMS 配置导出工具")
    sub = parser.add_subparsers(dest="cmd")

    exp = sub.add_parser("export", help="导出默认配置到指定路径")
    exp.add_argument("--ch", type=int, default=8, choices=[4, 8, 16], help="通道数")
    exp.add_argument("-o", "--output", required=True, help="输出文件路径")

    match = sub.add_parser("match", help="对 G-code 执行匹配预览（**不写文件**）")
    match.add_argument("gcode", help="G-code 文件路径")
    match.add_argument("--config", default=None, help="配置文件路径")
    match.add_argument("-l", "--log", action="store_true", help="打印匹配日志")

    sub.add_parser("setup", help="打印该往切片器「后处理脚本」里填的那一行")

    args = parser.parse_args()

    if args.cmd == "setup":
        _cmd_setup()
        return
    if args.cmd == "export":
        data = default_settings(args.ch)
        save_settings(data, args.output)
        safe_print("配置已导出：%s" % args.output)
    elif args.cmd == "match":
        # 预览模式：dry_run，只把映射表打出来，一个字节都不改
        res = process_file(args.gcode, explicit_config=args.config,
                           write_back=False, popup_mode=POPUP_NEVER,
                           do_popup=False, verbose=False)
        if not res.get("ok"):
            return
        out = []
        out.append("配置文件：%s" % (res["settings_path"] or "**没找到**"))
        out.append("切片颜色：%s"
                   % (", ".join("%d=%s" % (i + 1, rgb_to_hex(*c))
                                for i, c in sorted(res["slice_colors"].items()))
                      or "（切片里没有）"))
        out.append("项目通道：%s"
                   % (", ".join("通道%d=%s" % (ch, rgb_to_hex(*c))
                                for ch, c in sorted(res["ams_colors"].items()))
                      or "（没配）"))
        out.append("-" * 48)
        for line in res["log_lines"]:
            out.append("  " + line)
        out.append("-" * 48)
        s = res["summary"]
        out.append("M140 信令：改写了 %d 处，保持原样 %d 处"
                   % (s.get("m140_ok", 0), s.get("m140_miss", 0)))
        safe_print("\n".join(out))
    else:
        parser.print_help()


def main():
    """
    入口函数：
      - 在 Bambu Studio 中导入时：onBeforeWriteGCode 由框架调用
      - 独立运行时（无参数）：启动 GUI
      - 带参数运行时：执行命令行子命令
    """
    # ★★ 用 pythonw.exe / PyInstaller --windowed 运行时，sys.stdout 和
    #    sys.stderr 都是 **None**。这时候连 argparse 自己报个错都会
    #    AttributeError（它内部直接 file.write），于是现象就是"窗口一闪、
    #    什么都没发生"。这里先把它们换成黑洞，保证任何路径都不会因此崩。
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w", encoding="utf-8")
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w", encoding="utf-8")

    # 检测是否在 Bambu Studio 环境中（环境变量或模块被 import 时）
    in_bambu = (
        "BAMBU_STUDIO" in os.environ
        or "BAMBU" in os.environ
        or "sk1app" in sys.modules          # Bambu Studio 内置 Python 环境
        or any(k.startswith("sk") for k in sys.modules)  # sk1app/sklabs 模块
    )
    if in_bambu:
        return

    if len(sys.argv) > 1:
        if sys.argv[1] in ("export", "match", "setup"):
            _cmd_export_config()
            return
        elif sys.argv[1] == "gui":
            _run_gui()
            return
        elif sys.argv[1] == "--no-gui":
            pass
        else:
            # 第一个参数是文件路径 → Bambu Studio「后处理脚本」走的就是这条
            import argparse
            parser = argparse.ArgumentParser(description="ESP-AMS G-code 后处理")
            parser.add_argument('input', help='输入 G-code 文件')
            parser.add_argument('output', nargs='?',
                                help='输出文件（默认就地覆盖输入）')
            parser.add_argument('--config', help='手动指定配置文件路径')
            parser.add_argument('--log', action='store_true', help='输出匹配日志')
            parser.add_argument('--dry-run', action='store_true',
                                help='只算映射，不写文件（用来先看一眼对不对）')
            parser.add_argument('--popup', default=None,
                                choices=[POPUP_AUTO, POPUP_ALWAYS, POPUP_NEVER],
                                help='弹窗策略：auto（默认，只在需要拍板时弹）/ '
                                     'always / never')
            parser.add_argument('--no-popup', action='store_true',
                                help='等价于 --popup never')
            args = parser.parse_args()

            res = process_file(
                args.input, args.output,
                explicit_config=args.config,
                write_back=not args.dry_run,
                popup_mode=POPUP_NEVER if args.no_popup else args.popup,
                do_popup=not args.dry_run,
                verbose=not args.dry_run,
            )
            if args.dry_run:
                safe_print("（--dry-run：一个字节都没改）")
                for line in res.get("log_lines", []):
                    safe_print("  " + line)
                s = res.get("summary", {})
                safe_print("  M140 信令改写了 %d 处（保持原样 %d 处）"
                           % (s.get("m140_ok", 0), s.get("m140_miss", 0)))
            elif res.get("ok"):
                s = res["summary"]
                safe_print("完成：M140 改写了 %d 处（保持原样 %d 处）"
                           % (s.get("m140_ok", 0), s.get("m140_miss", 0)))
            sys.exit(0 if res.get("ok") else 2)

    # 无参数 → 启动 GUI
    _run_gui()


if __name__ == '__main__':
    main()
