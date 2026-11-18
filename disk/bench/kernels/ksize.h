/* SPDX-License-Identifier: 0BSD
 *
 * Input sizes per kernel set. One loop of a kernel should take about
 * 100 ms on the slowest machine of its set (k000: a 7 MHz 68000 A500;
 * k020: a 14 MHz 68020 A1200 without fast RAM), so that even there a block
 * finishes in a few hundred milliseconds. Faster machines run more loops
 * per repetition, so they lose nothing. The sets are never compared with
 * each other, so their inputs may differ. KSET_REF is benchwork's own
 * sizes, for a host build to compare checksums with benchwork.
 */
#ifndef AMT_KSIZE_H
#define AMT_KSIZE_H

#if defined(KSET_REF)
#define KS_DHRY_RUNS     20000UL
#define KS_SCENE_W       640
#define KS_SCENE_H       200
#define KS_CORPUS        (64UL * 1024)
#define KS_ZWBITS        15
#define KS_ZMEMLEVEL     8
#define KS_INFLATES      3
#define KS_PAGE_W        640
#define KS_PAGE_H        480
#define KS_FT_SIZES      12, 24, 48
#define KS_FT_PASSES     2
#define KS_FT_STRIDE     1
#define KS_MEMCPY_REPS   64
#define KS_AEAD_RECORDS  4
#define KS_AEAD_RECORD   (16UL * 1024)
#define KS_NFS3_VERTICES 50000
#define KS_NFS3_TRIANGLES 6000
#elif defined(KSET_000)
#define KS_DHRY_RUNS     100UL
#define KS_SCENE_W       24
#define KS_SCENE_H       12
#define KS_CORPUS        (2UL * 1024)
#define KS_ZWBITS        11
#define KS_ZMEMLEVEL     6
#define KS_INFLATES      1
#define KS_PAGE_W        160
#define KS_PAGE_H        32
#define KS_FT_SIZES      12
#define KS_FT_PASSES     1
#define KS_FT_STRIDE     6
#define KS_MEMCPY_REPS   1
#define KS_AEAD_RECORDS  1
#define KS_AEAD_RECORD   512UL
#define KS_NFS3_VERTICES 32
#define KS_NFS3_TRIANGLES 8
#else /* KSET_020 */
#define KS_DHRY_RUNS     200UL
#define KS_SCENE_W       64
#define KS_SCENE_H       24
#define KS_CORPUS        (4UL * 1024)
#define KS_ZWBITS        12
#define KS_ZMEMLEVEL     7
#define KS_INFLATES      1
#define KS_PAGE_W        320
#define KS_PAGE_H        64
#define KS_FT_SIZES      12, 24
#define KS_FT_PASSES     1
#define KS_FT_STRIDE     4
#define KS_MEMCPY_REPS   1
#define KS_AEAD_RECORDS  1
#define KS_AEAD_RECORD   (2UL * 1024)
#define KS_NFS3_VERTICES 128
#define KS_NFS3_TRIANGLES 24
#endif

#endif
