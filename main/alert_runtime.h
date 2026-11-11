#pragma once
#include "alert_workflow.h"
#include "beacon_tracker.h"
void alert_runtime_init();
void alert_runtime_poll(bool presence, const BeaconStatus &driver);
void alert_runtime_sample(float temperature_f, bool valid);
bool alert_runtime_active();
bool alert_runtime_suppressed();
const alert::Workflow &alert_runtime_workflow();
