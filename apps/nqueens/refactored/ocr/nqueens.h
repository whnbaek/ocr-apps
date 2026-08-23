
#ifndef NQUEENS_H
#define NQUEENS_H

#include <ocr.h>
#include <timer.h>

struct nqueens_args
{
    ocrGuid_t find_template;   /* every task spawns children from it */
    ocrGuid_t parent_event;    /* receives this subtree's solution count */
    u32       max_set;
    u32       all;
    u32       ldiag;
    u32       cols;
    u32       rdiag;
    u32       rr_levels;      /* tree levels scattered before pinning */
};

struct shutdown_args
{
    ocrGuid_t find_template;
    ocrGuid_t shutdown_template;
    u32 n;
    u32 cutoff;
    u32 rounds_left;
    u32 rr_levels;
    timestamp_t start;
};

// Computes Hamming-weight for an arbitrary integer
static inline u32 NumberOfSetBits( u32 i )
{
    i = i - ((i >> 1) & 0x55555555);
    i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
    return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

#endif

