#pragma once
class SPIClass {
public:
    int sck = -1, miso = -1, mosi = -1, cs = -1;
    void begin(int clock, int in, int out, int select) {
        sck = clock; miso = in; mosi = out; cs = select;
    }
};
inline SPIClass SPI;
