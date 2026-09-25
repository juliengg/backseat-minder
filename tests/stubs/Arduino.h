#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>

constexpr int SERIAL_8N1 = 0;
class HardwareSerial;
void test_on_write(HardwareSerial &, char);
uint32_t millis();
void delay(uint32_t);

class HardwareSerial {
public:
    explicit HardwareSerial(int number) : id(number) {}
    int id;
    std::string rx, tx;
    void begin(uint32_t, int = 0, int = 0, int = 0) {}
    void setRxBufferSize(size_t) {}
    int available() const { return static_cast<int>(rx.size()); }
    int read() {
        if (rx.empty()) return -1;
        const unsigned char c = rx[0];
        rx.erase(0, 1);
        return c;
    }
    void write(char c) { tx += c; test_on_write(*this, c); }
    void print(const char *s) { while (*s) write(*s++); }
    void println(const char *s) { print(s); print("\n"); }
    void printf(const char *format, ...) {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        print(buffer);
    }
};
extern HardwareSerial Serial;
