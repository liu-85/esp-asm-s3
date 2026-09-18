# ===========================================================================
# ESP-AMS-S3 —— 基于 ESP-IDF 的拓竹打印机自动换料系统（ESP32-S3）
# ===========================================================================
#
# 这是 ESP-IDF 工程的顶层 CMakeLists.txt。它只做两件事：
#   1. 指定工程名
#   2. 把 ESP-IDF 的构建系统拉进来（下面那一行 include）
#
# 真正的编译单元在 main/CMakeLists.txt 里。
#
# 构建步骤（先装好 ESP-IDF，并 source 过 export.sh / export.ps1）:
#     idf.py set-target esp32s3      # 只需第一次
#     idf.py build
#     idf.py -p COM4 flash monitor
#
# 注意：目标芯片必须是 esp32s3。本工程用的是 42 针 S3 模组（8MB Flash），
#       引脚表在 main/board_pins.h 里，改接线只改那一个文件。
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp-ams-s3)

# 启用 ESP 组件注册表（mdns 在 v5.x 已从内置移到注册表）
register_component_manager()
