#include "WioRadio.h"
#include "BoardPins.h"
#include <helpers/radiolib/CustomSX1262.h>
SPIClass &wioRadioSpi() { static SPIClass spi(FSPI); return spi; }
CustomSX1262 &wioRadio() {
    static Module module(pins::LORA_CS, pins::LORA_DIO1, pins::LORA_RESET, pins::LORA_BUSY, wioRadioSpi());
    static CustomSX1262 radio(&module); return radio;
}
