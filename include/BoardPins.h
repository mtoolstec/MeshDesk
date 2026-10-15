#pragma once

#include <Arduino.h>

namespace pins {
constexpr int I2C_SDA = 47;
constexpr int I2C_SCL = 48;
constexpr int EXPANDER_IRQ = 45;
constexpr uint8_t EXPANDER_ADDR = 0x21;

constexpr int LORA_SCK = 4;
constexpr int LORA_MISO = 5;
constexpr int LORA_MOSI = 6;
constexpr int LORA_CS = 21;
constexpr int LORA_RESET = 7;
constexpr int LORA_BUSY = 8;
constexpr int LORA_DIO1 = 9;

constexpr int GPS_TX = 17;
constexpr int GPS_RX = 18;

constexpr int AUDIO_MCLK = 10;
constexpr int AUDIO_BCLK = 11;
constexpr int AUDIO_LRCLK = 12;
constexpr int AUDIO_DOUT = 16;

constexpr int LCD_SCLK = 42;
constexpr int LCD_IO0 = 41;
constexpr int LCD_IO1 = 40;
constexpr int LCD_IO2 = 39;
constexpr int LCD_IO3 = 38;
constexpr int LCD_CS = 46;

enum ExpanderPin : uint8_t {
    WAKE_BUTTON = 0, I2C_IRQ = 1, SD_DETECT = 2, TOUCH_INT = 3,
    CODEC_EN = 4, LCD_POWER = 5, LCD_RESET = 6, GROVE_EN = 7,
    TOUCH_RESET = 8, GNSS_RESET = 9, USER_LED = 10, OTG_EN = 11,
    AUDIO_PA_EN = 12, GNSS_EN = 13, SD_POWER = 14, BAT_ADC_EN = 15
};
}
