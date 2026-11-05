#ifndef AMT_CPU_CHECKS_H
#define AMT_CPU_CHECKS_H

/* WB1D is memory-aligned; WB2D/WB3D already contain right-justified operands.
 * Rotate first, including across the longword boundary, then justify size. */
static inline ULONG cpu_wb1_data(ULONG data, ULONG addr, ULONG size)
{
    ULONG bits = (addr & 3) * 8;
    if (bits)
        data = (data << bits) | (data >> (32 - bits));
    if (size == 1)
        return data >> 24;
    if (size == 2)
        return data >> 16;
    return data;
}

#endif
