#include "ms3_decoder.h"

#include "can_config.h"

bool ms3_decode_oil_pressure(const can_rx_frame_t *frame, float *pressure_psi)
{
    if (frame->extended || frame->rtr || frame->id != CAN_MS_DASH_SENSOR_ID ||
        frame->dlc != 8 || frame->data_length != 8) {
        return false;
    }

    int32_t raw = ((uint32_t)frame->data[2] << 8) | frame->data[3];
    if (raw >= 0x8000) {
        raw -= 0x10000;
    }
    *pressure_psi = (float)raw / 10.0f;
    return true;
}
