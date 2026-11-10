#include "tpms_fbb0.h"
#include <string.h>

bool tpms_fbb0_id_valid(const char *id)
{
    if (!id) return false;
    for (unsigned i = 0; i < 6; ++i) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'F')))
            return false;
    }
    return id[6] == '\0';
}

bool tpms_fbb0_decode(const uint8_t *raw, size_t len, tpms_fbb0_data_t *out)
{
    static const uint8_t prefix[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xB0, 0xFB};
    if (!raw || !out || len < 26 || memcmp(raw, prefix, sizeof(prefix))) return false;
    uint16_t crc = 0; /* CRC16-XMODEM: poly 0x1021, init 0, no reflection/xorout. */
    for (unsigned i = 9; i < 26; ++i) {
        if (i == 16 || i == 18) continue;
        crc ^= (uint16_t)raw[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    if (crc != (((uint16_t)raw[16] << 8) | raw[18])) return false;
    tpms_fbb0_data_t data = {0};
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < 3; ++i) {
        data.sensor_id[i * 2] = hex[raw[25 - i] >> 4];
        data.sensor_id[i * 2 + 1] = hex[raw[25 - i] & 15];
    }
    data.pressure_pa = (raw[12] + (raw[17] == 1 ? 256u : 0u)) * 3144u;
    data.temperature_c = (int16_t)raw[13] - 55;
    data.voltage_mv = raw[11] * 10u + 1220u;
    data.state = raw[14];
    data.version = raw[15];
    data.ble_version_code = raw[19];
    *out = data;
    return true;
}
