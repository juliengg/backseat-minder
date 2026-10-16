#pragma once

namespace primary_link {
void begin();
void poll();
void received_sms(const char *sender, const char *text);
void receive_status(const char *status);
}
