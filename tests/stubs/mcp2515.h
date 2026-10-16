#pragma once
#include "Arduino.h"
#include "SPI.h"
#include <vector>

enum CAN_SPEED { CAN_500KBPS };
enum CAN_CLOCK { MCP_8MHZ };
struct can_frame { uint32_t can_id; uint8_t can_dlc; uint8_t data[8]; };
class MCP2515 {
public:
    enum ERROR { ERROR_OK, ERROR_FAIL, ERROR_ALLTXBUSY, ERROR_FAILINIT, ERROR_FAILTX };
    inline static ERROR resetResult = ERROR_OK, bitrateResult = ERROR_OK, modeResult = ERROR_OK;
    inline static unsigned resets = 0, bitrates = 0, modes = 0;
    inline static int cs = -1;
    inline static uint32_t clock = 0;
    inline static SPIClass *bus = nullptr;
    inline static std::vector<can_frame> frames;
    inline static std::vector<uint32_t> times;
    inline static std::vector<ERROR> results;
    MCP2515(int select, uint32_t speed, SPIClass *spi) { cs = select; clock = speed; bus = spi; }
    ERROR reset() { ++resets; return resetResult; }
    ERROR setBitrate(CAN_SPEED, CAN_CLOCK) { ++bitrates; return bitrateResult; }
    ERROR setNormalMode() { ++modes; return modeResult; }
    ERROR sendMessage(const can_frame *frame) {
        const auto index = frames.size();
        frames.push_back(*frame);
        times.push_back(millis());
        return index < results.size() ? results[index] : ERROR_OK;
    }
    uint8_t errorCountTX() { return 1; }
    uint8_t errorCountRX() { return 0; }
    uint8_t getErrorFlags() { return 0; }
};
