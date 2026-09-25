#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Shared by ESP-IDF, Arduino and the host tests. Length excludes the newline.
namespace cellular {
constexpr size_t MAX_FRAME = 256;
constexpr size_t PHONE_CAPACITY = 32;
constexpr size_t MAX_MESSAGE = 160;
constexpr uint32_t BAUD = 115200;
constexpr uint32_t ACK_TIMEOUT_MS = 3000;
constexpr uint32_t MODEM_TIMEOUT_MS = 100000;
constexpr uint32_t RESULT_TIMEOUT_MS = 110000;

inline bool valid_number(const char *number)
{
    if (!number) return false;
    const bool international = *number == '+';
    if (international) ++number;
    size_t digits = 0;
    if (international && *number == '0') return false;
    for (; *number; ++number) {
        if (*number < '0' || *number > '9' || ++digits > 15) return false;
    }
    return digits >= 7;
}

inline bool valid_message(const char *message)
{
    if (!message || !*message) return false;
    size_t septets = 0;
    for (const char *p = message; *p; ++p) {
        // Printable GSM-compatible ASCII only; no separators, AT controls or UTF-8.
        if (*p < 32 || *p > 126 || *p == '|' || *p == '`') return false;
        septets += strchr("^{}[]\\~", *p) ? 2 : 1;
        if (septets > MAX_MESSAGE) return false;
    }
    return true;
}

struct Sms {
    char number[PHONE_CAPACITY];
    char message[MAX_MESSAGE + 1];
};

// Modifies the frame; returns a fixed, non-sensitive error reason or nullptr.
inline const char *parse_sms(char *frame, Sms &sms)
{
    if (strncmp(frame, "SEND_SMS|", 9) != 0) return "UNKNOWN_COMMAND";
    char *number = frame + 9;
    char *separator = strchr(number, '|');
    if (!separator) return "BAD_FRAME";
    *separator++ = '\0';
    if (!valid_number(number)) return "BAD_NUMBER";
    if (!valid_message(separator)) return "BAD_MESSAGE";
    strcpy(sms.number, number);
    strcpy(sms.message, separator);
    return nullptr;
}

enum class FrameEvent { None, Ready, Invalid, Oversized };

class LineBuffer {
public:
    FrameEvent push(char c)
    {
        if (c == '\n') {
            FrameEvent event = error_;
            if (event == FrameEvent::None) {
                if (used_ && data_[used_ - 1] == '\r') --used_;
                data_[used_] = '\0';
                event = used_ ? FrameEvent::Ready : FrameEvent::None;
            }
            used_ = 0;
            error_ = FrameEvent::None;
            return event;
        }
        if (error_ != FrameEvent::None) return FrameEvent::None;
        if (used_ == MAX_FRAME) {
            error_ = FrameEvent::Oversized;
        } else if ((c < 32 && c != '\r') || c > 126 ||
                   (used_ && data_[used_ - 1] == '\r')) {
            error_ = FrameEvent::Invalid;
        } else {
            data_[used_++] = c;
        }
        return FrameEvent::None;
    }
    char *data() { return data_; }

private:
    char data_[MAX_FRAME + 1] = {};
    size_t used_ = 0;
    FrameEvent error_ = FrameEvent::None;
};
} // namespace cellular
