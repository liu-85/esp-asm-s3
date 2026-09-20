# -*- coding: utf-8 -*-
"""
ESP-AMS 全自动换色后处理脚本
================================

安装位置：Bambu Studio → 偏好设置 → G-code 文件编辑 → 后处理脚本
          （或手动放到 {BambuStudio}/scripts/filament/ 目录下）

功能：
  1. 读取同目录下的 AMS_colors.json（8 通道颜色定义）
  2. 解析 G-code 中的 Tn 换色指令
  3. 通过切片文件元数据或 AMS_colors.json 获取每个通道的颜色
  4. 将 M73 P101 R[next_extruder] 替换为匹配到的 AMS 通道号
  5. 固件端 ESP32-C3 收到 MQTT 后自动匹配颜色并执行换料

AMS_colors.json 示例：
  {
    "selected_channel": 4,
    "AMS": [
      {"channel": "1", "color": [255, 255, 255]},
      {"channel": "2", "color": [128, 128, 0]},
      ...
    ]
  }
"""

import json
import re
import os
import sys
from pathlib import Path


# ============================== 颜色工具 ==============================

def parse_hex_color(s):
    """解析 #RRGGBB 或 RRGGBB 为 (r, g, b)"""
    s = s.strip().lstrip('#')
    if len(s) == 6:
        return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))
    return None


def color_dist(c1, c2):
    """欧氏距离"""
    return ((c1[0] - c2[0]) ** 2 +
            (c1[1] - c2[1]) ** 2 +
            (c1[2] - c2[2]) ** 2) ** 0.5


# ============================== 文件定位 ==============================

def find_config_file(input_file):
    """
    在 input_file 所在目录及上级目录中查找 AMS_colors.json
    返回路径字符串，找不到返回 None
    """
    if not input_file:
        return None

    # 优先：同目录
    base = Path(input_file).parent
    candidates = [
        base / "AMS_colors.json",
        base / "ams_colors.json",
        # 上级目录
        base.parent / "AMS_colors.json" if base.parent != base else None,
    ]
    for c in candidates:
        if c and c.is_file():
            return str(c)
    return None


def load_ams_colors(config_path):
    """读取 AMS 通道颜色表，返回 {channel_num: (r,g,b)}"""
    result = {}
    if not config_path:
        return result
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        for entry in data.get("AMS", []):
            ch = int(str(entry.get("channel", "0")))
            color = entry.get("color", [])
            if len(color) >= 3 and ch > 0:
                result[ch] = (int(color[0]), int(color[1]), int(color[2]))
    except (json.JSONDecodeError, OSError, ValueError) as e:
        sys.stderr.write(f"[ESP-AMS] 读取 AMS_colors.json 失败: {e}\n")
    return result


# ============================== 切片颜色解析 ==============================

def extract_filament_colors(input_file):
    """
    从 G-code 头部注释或 BAMSLABEL 元数据中提取各通道颜色。

    优先级：
      1. G-code 中的 ;Tn ;color=#RRGGBB 注释（Bambu Studio 会写入）
      2. 切片文件同名 .json / .gcode.json 元数据

    返回 {extruder_num: (r,g,b)}，找不到返回空 dict
    """
    colors = {}

    # --- 方法 1：G-code 内的颜色注释 ---
    if input_file and os.path.isfile(input_file):
        try:
            with open(input_file, 'r', encoding='utf-8', errors='replace') as f:
                content = f.read(65536)  # 只读前 64KB，够用了
            # 匹配形如：;T0 ;color=#FFFFFF
            # 或：; extruder 0: #FFFFFF
            patterns = [
                # Bambu Studio 标准格式
                re.compile(r';\s*T(\d+)\s*;?\s*color\s*=\s*(#?[0-9a-fA-F]{6})', re.IGNORECASE),
                # 简化格式
                re.compile(r';\s*extruder\s+(\d+)\s*:\s*(#?[0-9a-fA-F]{6})', re.IGNORECASE),
                # AMS 颜色行
                re.compile(r';\s*AMS\s+channel\s+(\d+).*?color\s*[:=]\s*(#?[0-9a-fA-F]{6})', re.IGNORECASE),
            ]
            for pat in patterns:
                for m in pat.finditer(content):
                    num = int(m.group(1))
                    hex_color = m.group(2)
                    parsed = parse_hex_color(hex_color)
                    if parsed:
                        colors[num] = parsed
                if colors:
                    break
        except OSError:
            pass

    # --- 方法 2：同名 .json 元数据文件 ---
    if not colors and input_file:
        meta = input_file + '.json'
        if os.path.isfile(meta):
            try:
                with open(meta, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                # 可能格式：{"filaments": [{"color": "#FFF", "ams_channel": 1}, ...]}
                filaments = data.get("filaments", [])
                for idx, fil in enumerate(filaments):
                    hex_c = fil.get("color", "")
                    parsed = parse_hex_color(hex_c)
                    if parsed:
                        ch = fil.get("ams_channel", idx + 1)
                        colors[int(ch)] = parsed
            except (json.JSONDecodeError, OSError):
                pass

    return colors


# ============================== 匹配逻辑 ==============================

def match_ams_channel(target_color, ams_colors):
    """
    在 AMS 颜色表中找最接近的通道号。
    返回 (matched_channel, distance)；未找到返回 (-1, 0)
    """
    if not target_color or not ams_colors:
        return -1, 0

    best_ch = -1
    best_dist = float('inf')
    for ch, c in ams_colors.items():
        d = color_dist(target_color, c)
        if d < best_dist:
            best_dist = d
            best_ch = ch

    # 阈值：距离 > 100 认为匹配不可靠（色差太大）
    if best_dist > 100:
        return -1, best_dist
    return best_ch, best_dist


# ============================== G-code 改写 ==============================

def rewrite_gcode(input_file, ams_colors, slice_colors, script_path=None):
    """
    主改写函数。

    参数：
      input_file   - G-code 文件路径
      ams_colors   - {ch: (r,g,b)} AMS 实际颜色
      slice_colors - {extruder: (r,g,b)} 切片颜色（可能为空）
      script_path  - 脚本自身路径（用于生成日志文件名）

    返回修改后的完整 G-code 字符串（含换色注释）
    """
    if not input_file or not os.path.isfile(input_file):
        return ""

    with open(input_file, 'r', encoding='utf-8', errors='replace') as f:
        gcode = f.read()

    # 统计换色次数
    t_matches = len(re.findall(r'^T(\d+)', gcode, re.MULTILINE))
    m73_matches = len(re.findall(r'M73\s+P101\s+R\[?(\d+|next_extruder)\]?', gcode))

    # 生成匹配日志
    log_lines = []
    log_lines.append(f"[ESP-AMS] G-code 换色统计：T指令={t_matches} 处, M73 P101={m73_matches} 处")

    if not ams_colors:
        log_lines.append("[ESP-AMS] 警告：未找到 AMS 颜色配置，M73 P101 使用原始通道号")
        log_lines.append("[ESP-AMS] 提示：请在 G-code 同目录放置 AMS_colors.json")
    else:
        log_lines.append(f"[ESP-AMS] 已加载 {len(ams_colors)} 个 AMS 通道颜色")

    # --- 逐行改写 ---
    lines = gcode.split('\n')
    out_lines = []

    # 记录上一条 T 指令（用于匹配）
    last_t = None

    for line in lines:
        stripped = line.strip()

        # 检测 T 指令
        m = re.match(r'^T(\d+)\s*(?:;.*)?$', stripped, re.IGNORECASE)
        if m:
            extruder_num = int(m.group(1))
            last_t = extruder_num

            # 查颜色
            target_color = slice_colors.get(extruder_num)
            if target_color is None:
                # 切片没给颜色，用通道号兜底
                target_color = ams_colors.get(extruder_num)

            # 匹配 AMS 通道
            if target_color is not None:
                matched, dist = match_ams_channel(target_color, ams_colors)
                if matched > 0:
                    log_lines.append(
                        f"[ESP-AMS] T{extruder_num} → AMS 通道 {matched} "
                        f"(色差 {dist:.1f})"
                    )
                else:
                    log_lines.append(
                        f"[ESP-AMS] T{extruder_num} → 未匹配到 AMS 通道 "
                        f"(最近色差 {dist:.1f})，使用通道 {extruder_num} 兜底"
                    )
                    matched = extruder_num
            else:
                # 没有颜色信息，直接用切片通道号
                matched = extruder_num
                log_lines.append(
                    f"[ESP-AMS] T{extruder_num} → 无颜色信息，使用切片通道 {matched}"
                )

            # 改写 T 指令为 M73 P101 R[matched]（保留 T 用于状态切换）
            out_lines.append(f"M73 P101 R{matched}")
            out_lines.append(f"T{extruder_num}")  # T 保留，用于切片器状态
            continue

        # 检测 M73 P101 R[next_extruder]（模板占位符）
        m73 = re.match(
            r'^M73\s+P101\s+R\[?next_extruder\]?\s*(?:;.*)?$',
            stripped, re.IGNORECASE
        )
        if m73 and last_t is not None:
            # 把 next_extruder 替换为 last_t
            replaced = re.sub(
                r'next_extruder',
                str(last_t),
                stripped,
                flags=re.IGNORECASE
            )
            out_lines.append(replaced)
            continue

        # 其他行原样保留
        out_lines.append(line)

    # 把日志写到 G-code 头部注释
    header = '\n'.join(log_lines) + '\n'
    result = header + '\n'.join(out_lines)

    return result


# ============================== 脚本入口 ==============================

def _write_ams_log(gcode_path, log_lines, config_file, ams_colors, slice_colors):
    """把匹配日志写到 <gcode>.ams.log，同时输出到 stderr"""
    summary = '\n'.join(log_lines)
    sys.stderr.write(summary + '\n')
    if gcode_path:
        log_file = gcode_path + '.ams.log'
        try:
            with open(log_file, 'w', encoding='utf-8') as lf:
                lf.write(f"[ESP-AMS] 匹配日志  {gcode_path}\n")
                lf.write(f"[ESP-AMS] AMS 颜色：{len(ams_colors)} 通道  "
                         f"切片颜色：{len(slice_colors)} 种\n")
                lf.write(f"[ESP-AMS] 配置文件：{config_file or '未找到'}\n")
                lf.write("-" * 40 + "\n")
                lf.write(summary + "\n")
                lf.write("-" * 40 + "\n")
                matched = [l for l in log_lines if "→ AMS 通道" in l]
                unmatched = [l for l in log_lines if "未匹配" in l]
                if matched:
                    lf.write(f"\n成功匹配 {len(matched)} 处：\n")
                    for l in matched:
                        lf.write("  " + l + "\n")
                if unmatched:
                    lf.write(f"\n未匹配 {len(unmatched)} 处：\n")
                    for l in unmatched:
                        lf.write("  " + l + "\n")
        except OSError:
            pass


def onBeforeWriteGCode(input_file, gcode_path, project, filename, output):
    """
    Bambu Studio 后处理钩子（Python API 版本）
    """
    sys.stderr.write(f"[ESP-AMS] 后处理开始：{input_file}\n")

    ams_colors = {}
    slice_colors = {}

    # 1. 加载 AMS 颜色配置
    config_file = find_config_file(input_file)
    if config_file:
        ams_colors = load_ams_colors(config_file)
        sys.stderr.write(f"[ESP-AMS] 已加载 AMS 颜色：{config_file}\n")

    # 2. 解析切片颜色
    slice_colors = extract_filament_colors(input_file)
    if slice_colors:
        sys.stderr.write(f"[ESP-AMS] 切片颜色 {len(slice_colors)} 种\n")

    # 3. 改写 G-code
    rewritten = rewrite_gcode(input_file, ams_colors, slice_colors, gcode_path)

    # 4. 提取匹配日志（rewrite_gcode 返回的是完整 G-code，取头部注释块）
    if rewritten:
        # 从头部注释中提取 [ESP-AMS] 行
        log_lines = [l for l in rewritten.split('\n') if l.startswith('[ESP-AMS]')]
        _write_ams_log(gcode_path, log_lines, config_file, ams_colors, slice_colors)
        output.write(rewritten, encoding='utf-8')
        sys.stderr.write(f"[ESP-AMS] 后处理完成，已写入 {gcode_path}\n")
    else:
        with open(input_file, 'r', encoding='utf-8') as f:
            output.write(f.read(), encoding='utf-8')
        sys.stderr.write("[ESP-AMS] 未修改，透传原始 G-code\n")


def main():
    """
    独立运行模式（不依赖 Bambu Studio API）
    用法：python esp_ams_postprocess.py <gcode_file> [output_file]
    """
    import argparse
    parser = argparse.ArgumentParser(description='ESP-AMS G-code 后处理')
    parser.add_argument('input', help='输入 G-code 文件')
    parser.add_argument('output', nargs='?', help='输出文件（默认覆盖输入）')
    parser.add_argument('--config', help='手动指定 AMS_colors.json 路径')
    parser.add_argument('--log', action='store_true', help='输出匹配日志')
    args = parser.parse_args()

    ams_colors = {}
    config_file = args.config or find_config_file(args.input)
    if config_file:
        ams_colors = load_ams_colors(config_file)

    slice_colors = extract_filament_colors(args.input)

    result = rewrite_gcode(args.input, ams_colors, slice_colors, args.output)

    out_path = args.output or args.input
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(result)

    if args.log:
        print(result.split('\n')[0] if result else '(空)')
        print(f"AMS 颜色：{len(ams_colors)} 通道，切片颜色：{len(slice_colors)} 种")
        print(f"输出：{out_path}")


if __name__ == '__main__':
    main()
