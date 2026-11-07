#pragma once

#include "can_receiver.h"

/* Simulator mapping: simplified dash generic sensor 1 is signed PSI * 10. */
bool ms3_decode_oil_pressure(const can_rx_frame_t *frame, float *pressure_psi);
