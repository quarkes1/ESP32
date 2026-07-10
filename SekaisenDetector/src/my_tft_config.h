#ifndef MY_TFT_CONFIG_H
#define MY_TFT_CONFIG_H

// ST7789 240x240 IPS 彩屏配置
#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// SPI 引脚
#define TFT_MISO -1     // 不接
#define TFT_MOSI  5     // SPI 数据
#define TFT_SCLK 18     // SPI 时钟
#define TFT_CS   -1     // 接地
#define TFT_DC   19     // 命令/数据切换
#define TFT_RST  -1     // 复用 EN
#define TFT_BL   -1     // 背光接 3.3V

#define SPI_FREQUENCY 40000000

#endif
