#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/**
 * 引脚表选择器 —— 根据目标芯片自动 include 对应的引脚定义。
 *
 *   ESP32-S3 → board_pins_s3.h（4 通道完整方案）
 *   ESP32-C3 → board_pins_c3.h（2 通道降级方案）
 *
 * 业务代码（motor / clutch / filament_sensor / ams_controller / web_server）
 * 全部 #include "board_pins.h"，不用改。
 */
#if defined(ESP_IDF_TARGET_C3)
  #include "board_pins_c3.h"
#else
  #include "board_pins_s3.h"
#endif

#endif /* BOARD_PINS_H */
