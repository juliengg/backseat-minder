#pragma once

struct BeaconStatus {
    bool available;
    bool driver_present;
    bool rssi_valid;
    int rssi_dbm;
    bool checking = false;
};

// Call after NVS initialization. On a scheduled deep-sleep wake, preserve the
// previous NEAR decision for reporting/hysteresis while checking. A checking
// status must never qualify for sleep; three fresh advertisements are required.
bool beacon_tracker_init(bool previously_near = false);
bool beacon_tracker_stop();
BeaconStatus beacon_tracker_status();
bool beacon_tracker_driver_near();
int beacon_tracker_rssi(); // 0 when there is no fresh, usable RSSI.
