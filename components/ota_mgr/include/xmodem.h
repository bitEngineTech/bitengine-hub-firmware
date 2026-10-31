#pragma once

/*
 * XMODEM-CRC sender for the Silicon Labs Gecko Bootloader.
 *
 * Every constant below is copied from the bootloader source that will parse
 * our packets — not from memory, not from a generic XMODEM description:
 *
 *   sdk/platform/bootloader/communication/xmodem-parser/btl_xmodem.h
 *     XMODEM_DATA_SIZE 128, XMODEM_CMD_SOH/EOT/ACK/NAK/CAN/C,
 *     packet layout { header, packetNumber, packetNumberC, data[128],
 *                     crcH, crcL }  → 133 bytes, CRC big-endian in frame.
 *
 *   sdk/platform/bootloader/security/btl_crc16.c
 *     CRC-16/XMODEM (poly 0x1021, MSB-first, no reflection),
 *     seeded with 0 at btl_xmodem.c:34 and computed over the 128 data
 *     bytes only (not the header/sequence bytes).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Control bytes (btl_xmodem.h XMODEM_CMD_*) ---- */
#define XMODEM_SOH          0x01
#define XMODEM_EOT          0x04
#define XMODEM_ACK          0x06
#define XMODEM_NAK          0x15
#define XMODEM_CAN          0x18
#define XMODEM_C            0x43

/* ---- Frame geometry ---- */
#define XMODEM_DATA_SIZE    128
#define XMODEM_PACKET_SIZE  133   /* SOH + seq + ~seq + 128 data + crcH + crcL */

/**
 * CRC-16/XMODEM over @p len bytes, seeded with @p seed.
 * Bit-exact port of btl_crc16Stream(); seed 0 for a fresh block.
 */
uint16_t xmodem_crc16(const uint8_t *data, size_t len, uint16_t seed);

/**
 * Build one 133-byte XMODEM-CRC packet into @p out.
 *
 * @param out    Destination, must hold XMODEM_PACKET_SIZE bytes.
 * @param seq    Packet sequence number. XMODEM numbers the first data block 1
 *               and wraps 255 → 0; the caller supplies the already-wrapped
 *               value (uint8_t arithmetic does this for free).
 * @param data   Payload source.
 * @param len    Bytes available at @p data, 1..XMODEM_DATA_SIZE. A short final
 *               block is padded with @p pad_byte.
 * @param pad_byte Filler for the final short block.
 *
 * @return false if out/data is NULL or len is 0 or > XMODEM_DATA_SIZE.
 */
bool xmodem_build_packet(uint8_t *out, uint8_t seq,
                         const uint8_t *data, size_t len, uint8_t pad_byte);

/**
 * Verify the CRC port against the CRC-16/XMODEM reference vector
 * ("123456789" → 0x31C3). Cheap; call once at init so a bad port fails
 * loudly at boot instead of silently corrupting every upload.
 */
bool xmodem_crc16_selftest(void);

#ifdef __cplusplus
}
#endif
