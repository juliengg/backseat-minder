#pragma once

// Initializes and configures the DFRobot SEN0609/C4001 presence sensor on
// UART1 (GPIO 41 RX, GPIO 42 TX).
void mmwave_sensor_init();

// Processes any pending sensor reports and returns the latest presence state.
bool mmwave_sensor_person_detected();
