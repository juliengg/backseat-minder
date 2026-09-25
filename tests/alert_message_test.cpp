#include <cassert>
#include <cstdio>
#include <string>
#include "alert_message.h"

int main()
{
    char message[cellular::MAX_MESSAGE + 1];
    assert(build_alert_message("Julie", message, sizeof(message)));
    assert(std::string(message) == "ALERT: Julie's Backseat Minder device has detected an unattended passenger in their vehicle.");
    assert(build_alert_message("  Anne-Marie O'Neil  ", message, sizeof(message)));
    assert(std::string(message).find("ALERT: Anne-Marie O'Neil's") == 0);
    assert(build_alert_message(std::string(63, 'A').c_str(), message, sizeof(message)));
    for (const auto &name : {std::string(""), std::string(" \t\r\n"), std::string(64, 'A'),
                            std::string("Jos\xc3\xa9"), std::string("A|B"),
                            std::string("A\nB"), std::string(63, '^')}) {
        assert(!build_alert_message(name.c_str(), message, sizeof(message)));
        assert(message[0] == '\0');
    }
    char tiny[10];
    assert(!build_alert_message("Julie", tiny, sizeof(tiny)));
    assert(tiny[0] == '\0');
    assert(!build_alert_message(nullptr, message, sizeof(message)));
    puts("PASS: exact named alert, required name, whitespace, encoding and message length limits");
}
