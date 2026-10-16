#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "positioning_preview.h"
#include "esp_timer.h"

static int64_t now = 0;
int64_t esp_timer_get_time() { return now; }
void test_log(const char *, ...) {}

int main()
{
    size_t length = 123;
    assert(!positioning_preview_snapshot(&length) && length == 0);
    positioning_preview_init();
    assert(!positioning_preview_frame_due(now));
    assert(!positioning_preview_snapshot(&length)); // Never active outside setup.
    positioning_preview_set_active(true);
    assert(!positioning_preview_frame_due(now)); // No work before HTTP demand.
    assert(!positioning_preview_snapshot(&length) && length == 0);
    assert(positioning_preview_frame_due(now));
    assert(!positioning_preview_frame_due(now)); // Failed encodes also throttle.
    const uint8_t jpeg[] = {0xff, 0xd8, 0x01, 0xff, 0xd9};
    positioning_preview_publish(jpeg, sizeof(jpeg));
    uint8_t *first = positioning_preview_snapshot(&length);
    assert(first && length == sizeof(jpeg) && memcmp(first, jpeg, length) == 0);
    now = 999999;
    assert(!positioning_preview_frame_due(now));
    now = 1000000;
    assert(positioning_preview_frame_due(now));
    uint8_t changed[] = {0xff, 0xd8, 0x02, 0xff, 0xd9};
    positioning_preview_publish(changed, sizeof(changed));
    changed[2] = 0x42; // Cache owns a copy; camera/encoder may free its buffer.
    uint8_t *second = positioning_preview_snapshot(&length);
    assert(second && second[2] == 0x02 && first[2] == 0x01);
    free(first); free(second);
    std::vector<uint8_t> oversized(65537, 0);
    positioning_preview_publish(oversized.data(), oversized.size());
    positioning_preview_publish(nullptr, 100);
    second = positioning_preview_snapshot(&length);
    assert(second && second[2] == 0x02);
    free(second);
    now = 3500001;
    assert(!positioning_preview_frame_due(now)); // Demand expires when tab closes.
    assert(!positioning_preview_snapshot(&length) && length == 0); // Never show stale frames.
    assert(positioning_preview_frame_due(now)); // New request can restart capture.
    positioning_preview_set_active(false);
    positioning_preview_publish(jpeg, sizeof(jpeg)); // In-flight encode after exit.
    assert(!positioning_preview_snapshot(&length));
    assert(!positioning_preview_frame_due(now));
    positioning_preview_set_active(true);
    assert(!positioning_preview_snapshot(&length)); // No frame from earlier session.
    assert(positioning_preview_frame_due(now));
    positioning_preview_set_active(false);
    puts("PASS: demand-only 1 FPS capture, owned JPEG copies, stale/oversized frames and setup teardown");
}
