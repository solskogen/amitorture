/* CRC-32 (IEEE 802.3 / zlib), nibble table: 64 bytes, ~2x the speed of bitwise */
#include "rt.h"

static const ULONG crc_nib[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
};

ULONG crc32_update(ULONG crc, const void *buf, ULONG len)
{
    const UBYTE *p = buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nib[crc & 15];
        crc = (crc >> 4) ^ crc_nib[crc & 15];
    }
    return ~crc;
}
