#pragma once
#include "alert_workflow.h"
#include "beacon_tracker.h"
void alert_runtime_init();
// Explicit STOP cancellation; persists until confirmed driver return.
void alert_runtime_cancel();
// Temporary setup pause. On exit confirm driver/occupancy to choose readiness.
void alert_runtime_enter_setup();
// Also recheck readiness if setup startup fails without rebooting.
void alert_runtime_exit_setup();
void alert_runtime_poll(bool presence, const BeaconStatus &driver);
// Run after the escalation poll so real alerts get first access to the link.
void alert_runtime_poll_tests(uint8_t requests);
bool alert_runtime_test_sms_pending();
void alert_runtime_sample(float temperature_f, bool valid);
bool alert_runtime_active();
bool alert_runtime_suppressed();
const alert::Workflow &alert_runtime_workflow();
