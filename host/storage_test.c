/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host_hal.h"
#include "fw.h"
#include "libc.c"

static uint8_t flash[0x100000];
static int fail_after = -1;

static int st_read(uint32_t off, void *dst, uint32_t n)
{
    memcpy(dst, flash + off, n);
    return 0;
}

static int st_erase(uint32_t off)
{
    assert(off >= 0xF7000u && off < 0xFB000u && !(off & 0xFFFu));
    memset(flash + off, 0xFF, 0x1000);
    return 0;
}

static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    assert(off >= 0xF7000u && off + n <= 0xFB000u);
    if (fail_after == 0)
        return -1;
    if (fail_after > 0)
        fail_after--;
    for (i = 0; i < n; i++)
        flash[off + i] &= ((const uint8_t *)src)[i];
    return 0;
}

#include "storage.c"

int main(void)
{
    char a[64] = "first", b[64] = "second", out[64];
    memset(flash, 0xFF, sizeof flash);
    assert(st_load(0, out, sizeof out) == -1);
    assert(st_save(0, a, sizeof a) == 0);
    assert(st_save(1, b, sizeof b) == 0);
    assert(st_load(0, out, sizeof out) == (int)sizeof a && !strcmp(out, "first"));
    assert(st_load(1, out, sizeof out) == (int)sizeof b && !strcmp(out, "second"));
    strcpy(a, "third");
    fail_after = 0;
    assert(st_save(0, a, sizeof a) != 0);
    fail_after = -1;
    assert(st_load(0, out, sizeof out) > 0 && !strcmp(out, "first"));
    assert(st_save(0, a, sizeof a) == 0);
    assert(st_load(0, out, sizeof out) > 0 && !strcmp(out, "third"));
    assert(st_load(1, out, sizeof out) > 0 && !strcmp(out, "second"));
    assert(st_save(2, a, sizeof a) == -1);
    puts("storage_test: ok");
    return 0;
}
