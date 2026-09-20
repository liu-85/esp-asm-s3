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
COLOR_DISTANCE_THRESHOLD = 100.0

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

def parse_slice_colors(gcode_content: str) -> Dict[int, Tuple[int, int, int]]:
    """
    从 G-code 头部注释中提取各通道颜色。

    支持格式：
      ;T0 ;color=#FFFFFF
      ; extruder 0: #FFFFFF
      ;AMS channel 1 color=#FF0000
    """
    colors = {}
    patterns = [
        re.compile(r';\s*T(\d+)\s*;?\s*color\s*=\s*(#?[0-9a-fA-F]{6})', re.IGNORECASE),
        re.compile(r';\s*extruder\s+(\d+)\s*:\s*(#?[0-9a-fA-F]{6})', re.IGNORECASE),
        re.compile(r';\s*AMS\s+channel\s+(\d+).*?color\s*[:=]\s*(#?[0-9a-fA-F]{6})', re.IGNORECASE),
    ]
    for pat in patterns:
        for m in pat.finditer(gcode_content[:65536]):
            num = int(m.group(1))
            parsed = hex_to_rgb(m.group(2))
            if parsed:
                colors[num] = parsed
        if colors:
            break
    return colors


def rewrite_gcode(
    gcode_content: str,
    ams_colors: Dict[int, Tuple[int, int, int]],
    slice_colors: Dict[int, Tuple[int, int, int]],
    first_filament: bool = True,
) -> Tuple[str, List[str]]:
    """
    改写 G-code：把 Tn / M73 P101 R[next_extruder] 替换为匹配到的 AMS 通道号。

    返回 (改写后的内容, 日志行列表)
    """
    log_lines: List[str] = []

    t_count = len(re.findall(r'^T(\d+)', gcode_content, re.MULTILINE))
    m73_count = len(re.findall(r'M73\s+P101\s+R\[?(\d+|next_extruder)\]?', gcode_content))
    log_lines.append(f"G-code 换色统计：T 指令 {t_count} 处，M73 P101 共 {m73_count} 处")

    if not ams_colors:
        log_lines.append("警告：AMS 颜色配置为空，M73 P101 使用原始通道号")
    else:
        log_lines.append(f"已加载 {len(ams_colors)} 个 AMS 通道颜色")

    lines = gcode_content.split('\n')
    out_lines: List[str] = []
    last_t: Optional[int] = None
    first_t_handled = False
    first_fil_flag = first_filament  # 保存首次换料标志

    for line in lines:
        stripped = line.strip()

        # 检测 T 指令
        m_t = re.match(r'^T(\d+)\s*(?:;.*)?$', stripped, re.IGNORECASE)
        if m_t:
            extruder_num = int(m_t.group(1))
            last_t = extruder_num
            is_first = not first_t_handled
            first_t_handled = True

            target = slice_colors.get(extruder_num)
            if target is None:
                target = ams_colors.get(extruder_num)

            if target is not None and ams_colors:
                matched, dist = find_best_channel(target, ams_colors)
                prefix = "【首次换料】" if (is_first and first_fil_flag) else ""
                if matched > 0 and dist <= COLOR_DISTANCE_THRESHOLD:
                    log_lines.append(
                        f"{prefix}T{extruder_num} ({rgb_to_hex(*target)}) → "
                        f"AMS 通道 {matched}（色差 {dist:.1f}）✓"
                    )
                elif matched > 0:
                    log_lines.append(
                        f"{prefix}T{extruder_num} ({rgb_to_hex(*target)}) → "
                        f"未匹配（最近通道 {matched}，色差 {dist:.1f}），保留切片通道 {extruder_num}"
                    )
                    matched = extruder_num
                else:
                    log_lines.append(f"{prefix}T{extruder_num}：无颜色信息，保留切片通道 {extruder_num}")
                    matched = extruder_num
            else:
                matched = extruder_num
                prefix = "【首次换料】" if (is_first and first_fil_flag) else ""
                log_lines.append(f"{prefix}T{extruder_num}：无颜色信息，保留切片通道 {extruder_num}")

            # 替换 T 指令为 M73 P101 + T
            out_lines.append(f"M73 P101 R{matched}")
            out_lines.append(f"T{extruder_num}")
            continue

        # 检测 M73 P101 R[next_extruder]
        m73 = re.match(
            r'^M73\s+P101\s+R\[?next_extruder\]?\s*(?:;.*)?$',
            stripped, re.IGNORECASE
        )
        if m73 and last_t is not None:
            target = slice_colors.get(last_t)
            if target is not None and ams_colors:
                matched, dist = find_best_channel(target, ams_colors)
                if matched > 0 and dist <= COLOR_DISTANCE_THRESHOLD:
                    out_lines.append(f"M73 P101 R{matched}")
                    log_lines.append(
                        f"M73 P101：T{last_t} ({rgb_to_hex(*target)}) → "
                        f"AMS 通道 {matched}（色差 {dist:.1f}）✓"
                    )
                else:
                    out_lines.append(f"M73 P101 R{last_t}")
                    log_lines.append(
                        f"M73 P101：T{last_t} 未匹配（最近 {matched}，色差 {dist:.1f}），保留通道 {last_t}"
                    )
            else:
                out_lines.append(f"M73 P101 R{last_t}")
                log_lines.append(f"M73 P101：T{last_t} 无颜色信息，保留切片通道 {last_t}")
            continue

        out_lines.append(line)

    return '\n'.join(out_lines), log_lines


def find_settings_file(gcode_path: str) -> Optional[str]:
    """在 G-code 所在目录及上级目录查找 filament_settings.json"""
    base = Path(gcode_path).parent
    candidates = [
        base / SETTINGS_FILENAME,
        base.parent / SETTINGS_FILENAME,
    ]
    for c in candidates:
        if c.is_file():
            return str(c)
    return None


# ============================== 后处理脚本入口（Bambu Studio 用） ==============================

def onBeforeWriteGCode(input_file, gcode_path, project, filename, output):
    """
    Bambu Studio 后处理钩子。
    自动查找同目录下的 filament_settings.json，
    改写 M73 P101 通道号，把匹配日志写入 <gcode>.ams.log。
    同时输出到 print output，在 Bambu Studio 控制台中可见。
    """
    settings_path = find_settings_file(input_file)
    ams_colors: Dict[int, Tuple[int, int, int]] = {}
    first_filament = True
    if settings_path:
        data = load_settings(settings_path)
        if data:
            ams_colors = get_ams_colors(data)
            first_filament = data.get("first_filament", True)

    try:
        with open(input_file, 'r', encoding='utf-8', errors='replace') as f:
            gcode_content = f.read()
    except OSError:
        output.write("ESP-AMS: 无法读取 G-code 文件\n", encoding='utf-8')
        return

    slice_colors = parse_slice_colors(gcode_content)
    rewritten, log_lines = rewrite_gcode(gcode_content, ams_colors, slice_colors, first_filament)

    # 写匹配日志文件
    log_file = gcode_path + '.ams.log'
    try:
        with open(log_file, 'w', encoding='utf-8') as lf:
            lf.write(f"[ESP-AMS] 匹配日志  {gcode_path}\n")
            lf.write(f"[ESP-AMS] 配置文件：{settings_path or '未找到'}\n")
            lf.write(f"[ESP-AMS] AMS 通道：{len(ams_colors)} 个  切片颜色：{len(slice_colors)} 种\n")
            lf.write(f"[ESP-AMS] 首次换料：{'开' if first_filament else '关'}\n")
            lf.write("-" * 50 + "\n")
            for line in log_lines:
                lf.write(f"  {line}\n")
            lf.write("-" * 50 + "\n")
            matched = [l for l in log_lines if "→ AMS 通道" in l and "未匹配" not in l]
            unmatched = [l for l in log_lines if "未匹配" in l]
            if matched:
                lf.write(f"\n成功匹配 {len(matched)} 处：\n")
                for l in matched:
                    lf.write(f"  {l}\n")
            if unmatched:
                lf.write(f"\n未匹配 {len(unmatched)} 处：\n")
                for l in unmatched:
                    lf.write(f"  {l}\n")
    except OSError:
        pass

    # 输出到控制台（Bambu Studio 切片时可见）
    try:
        print(f"[ESP-AMS] 配置文件：{settings_path or '未找到'}")
        print(f"[ESP-AMS] AMS 通道：{len(ams_colors)} 个  切片颜色：{len(slice_colors)} 种")
        for line in log_lines:
            print(f"[ESP-AMS] {line}")
    except Exception:
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
        _, log_lines = rewrite_gcode(gcode_content, ams_colors, slice_colors, first_fil)
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

def _cmd_export_config():
    """命令行导出配置 / 匹配"""
    import argparse
    parser = argparse.ArgumentParser(description="ESP-AMS 配置导出工具")
    sub = parser.add_subparsers(dest="cmd")

    exp = sub.add_parser("export", help="导出默认配置到指定路径")
    exp.add_argument("--ch", type=int, default=8, choices=[4, 8, 16], help="通道数")
    exp.add_argument("-o", "--output", required=True, help="输出文件路径")

    match = sub.add_parser("match", help="对 G-code 执行匹配预览")
    match.add_argument("gcode", help="G-code 文件路径")
    match.add_argument("--config", default=None, help="配置文件路径")
    match.add_argument("-l", "--log", action="store_true", help="打印匹配日志")

    args = parser.parse_args()

    if args.cmd == "export":
        data = default_settings(args.ch)
        save_settings(data, args.output)
        print(f"配置已导出：{args.output}")
    elif args.cmd == "match":
        config_path = args.config
        if not config_path:
            config_path = find_settings_file(args.gcode)
        ams_colors = {}
        first_fil = True
        if config_path:
            data = load_settings(config_path)
            if data:
                ams_colors = get_ams_colors(data)
                first_fil = data.get("first_filament", True)
        with open(args.gcode, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
        slice_colors = parse_slice_colors(content)
        rewritten, log_lines = rewrite_gcode(content, ams_colors, slice_colors, first_fil)
        if args.log:
            print(f"配置文件：{config_path or '未找到'}")
            print(f"AMS 通道：{len(ams_colors)} 个  切片颜色：{len(slice_colors)} 种")
            print(f"首次换料：{'开' if first_fil else '关'}")
            print("-" * 40)
            for line in log_lines:
                print(f"  {line}")
            print("-" * 40)
    else:
        parser.print_help()


def main():
    """
    入口函数：
      - 在 Bambu Studio 中导入时：onBeforeWriteGCode 由框架调用
      - 独立运行时（无参数）：启动 GUI
      - 带参数运行时：执行命令行子命令
    """
    # 检测是否在 Bambu Studio 环境中
    in_bambu = "BAMBU_STUDIO" in os.environ or "BAMBU" in os.environ
    if in_bambu:
        return

    if len(sys.argv) > 1:
        if sys.argv[1] in ("export", "match"):
            _cmd_export_config()
            return
        elif sys.argv[1] == "gui":
            _run_gui()
            return
        elif sys.argv[1] == "--no-gui":
            pass
        else:
            # 第一个参数是文件路径 → 命令行处理模式（兼容旧版）
            import argparse
            parser = argparse.ArgumentParser(description="ESP-AMS G-code 后处理")
            parser.add_argument('input', help='输入 G-code 文件')
            parser.add_argument('output', nargs='?', help='输出文件（默认覆盖输入）')
            parser.add_argument('--config', help='手动指定配置文件路径')
            parser.add_argument('--log', action='store_true', help='输出匹配日志')
            args = parser.parse_args()

            config_path = args.config or find_settings_file(args.input)
            ams_colors = {}
            first_fil = True
            if config_path:
                data = load_settings(config_path)
                if data:
                    ams_colors = get_ams_colors(data)
                    first_fil = data.get("first_filament", True)

            with open(args.input, 'r', encoding='utf-8', errors='replace') as f:
                content = f.read()

            slice_colors = parse_slice_colors(content)
            rewritten, log_lines = rewrite_gcode(content, ams_colors, slice_colors, first_fil)

            out_path = args.output or args.input
            with open(out_path, 'w', encoding='utf-8') as f:
                f.write(rewritten)

            # 写匹配日志
            log_file = out_path + '.ams.log'
            try:
                with open(log_file, 'w', encoding='utf-8') as lf:
                    lf.write(f"[ESP-AMS] 匹配日志  {out_path}\n")
                    lf.write(f"[ESP-AMS] 配置文件：{config_path or '未找到'}\n")
                    lf.write(f"[ESP-AMS] AMS 通道：{len(ams_colors)} 个  切片颜色：{len(slice_colors)} 种\n")
                    lf.write(f"[ESP-AMS] 首次换料：{'开' if first_fil else '关'}\n")
                    lf.write("-" * 50 + "\n")
                    for line in log_lines:
                        lf.write(f"  {line}\n")
                    lf.write("-" * 50 + "\n")
                    matched = [l for l in log_lines if "→ AMS 通道" in l and "未匹配" not in l]
                    unmatched = [l for l in log_lines if "未匹配" in l]
                    if matched:
                        lf.write(f"\n成功匹配 {len(matched)} 处：\n")
                        for l in matched:
                            lf.write(f"  {l}\n")
                    if unmatched:
                        lf.write(f"\n未匹配 {len(unmatched)} 处：\n")
                        for l in unmatched:
                            lf.write(f"  {l}\n")
            except OSError:
                pass

            if args.log:
                print(f"配置文件：{config_path or '未找到'}")
                print(f"AMS 通道：{len(ams_colors)} 个  切片颜色：{len(slice_colors)} 种")
                print(f"首次换料：{'开' if first_fil else '关'}")
                print("-" * 40)
                for line in log_lines:
                    print(f"  {line}")
                print(f"输出：{out_path}")
                print(f"匹配日志：{log_file}")
            return

    # 无参数 → 启动 GUI
    _run_gui()


if __name__ == '__main__':
    main()
