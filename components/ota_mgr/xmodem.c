#include "xmodem.h"

#include <string.h>

/* Bit-exact port of btl_crc16() from
 * sdk/platform/bootloader/security/btl_crc16.c — the routine that will
 * recompute our CRC on the MG24 side. Kept structurally identical to the
 * original rather than rewritten as a table lookup: a mismatch here fails
 * every block with a NAK and the cause would be invisible from the C5 log. */
static uint16_t crc16_byte(uint8_t new_byte, uint16_t prev)
{
    prev = (uint16_t)((prev >> 8) | (prev << 8));
    prev ^= new_byte;
    prev ^= (uint16_t)((prev & 0xff) >> 4);
    prev ^= (uint16_t)((prev << 8) << 4);
    prev ^= (uint16_t)(((uint8_t)((uint8_t)(prev & 0xff) << 5))
                       | ((uint16_t)((uint8_t)((uint8_t)(prev & 0xff) >> 3)) << 8));
    return prev;
}

uint16_t xmodem_crc16(const uint8_t *data, size_t len, uint16_t seed)
{
    if (!data) return seed;

    uint16_t crc = seed;
    for (size_t i = 0; i < len; i++) {
        crc = crc16_byte(data[i], crc);
    }
    return crc;
}

bool xmodem_build_packet(uint8_t *out, uint8_t seq,
                         const uint8_t *data, size_t len, uint8_t pad_byte)
{
    if (!out || !data) return false;
    if (len == 0 || len > XMODEM_DATA_SIZE) return false;

    out[0] = XMODEM_SOH;
    out[1] = seq;
    out[2] = (uint8_t)(0xFF - seq);   /* complement, per XmodemPacket_t */

    memcpy(&out[3], data, len);
    if (len < XMODEM_DATA_SIZE) {
        memset(&out[3 + len], pad_byte, XMODEM_DATA_SIZE - len);
    }

    /* btl_xmodem.c:65 — CRC covers the 128 data bytes only, seeded with 0,
       and is placed high byte first (crcH at offset 131, crcL at 132). */
    uint16_t crc = xmodem_crc16(&out[3], XMODEM_DATA_SIZE, 0);
    out[131] = (uint8_t)(crc >> 8);
    out[132] = (uint8_t)(crc & 0xFF);

    return true;
}

bool xmodem_crc16_selftest(void)
{
    static const uint8_t vec[] = "123456789";
    return xmodem_crc16(vec, 9, 0) == 0x31C3;
}
