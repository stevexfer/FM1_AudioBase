/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit) */
static uint8_t flash_ok;
bootguard_t bootguard __attribute__((section(".noinit")));

static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        const uint32_t k = n > 256u ? 256u : n;
        const uint32_t f = irq_save();
        const int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}

static int fl_erase4k_quiet(uint32_t off, uint32_t *took)
{
    const uint32_t f = irq_save();
    int rc;
    audio_silence();
    rc = FL_FAR(fl_erase4k_ram)(off, took);
    irq_restore(f);
    return rc;
}

static int st_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_STORE_OK(off, 0x1000u))
        return -8;
    return fl_erase4k_quiet(off, &took);
}

static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!FL_STORE_OK(off, n))
        return -8;
    return fl_write(off, src, n);
}

#include "storage.c"

static int fw_store_load(uint32_t slot, void *dst, uint32_t max)
{
    if (!flash_ok || slot >= FW_STORE_SLOTS)
        return -1;
    return st_load(slot, dst, max);
}

static int fw_store_save(uint32_t slot, const void *src, uint32_t len)
{
    if (!flash_ok || slot >= FW_STORE_SLOTS)
        return -1;
    return st_save(slot, src, len);
}

static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#define OTA_POOL                                    /* the update's buffers in .bss: the pool keeps its headroom */
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }

static void ota_idle(void)
{
    fm1_wdt_feed();
    if (recovery_active)
        recovery_poll();
}

static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }

static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k_quiet(off, &took);
}

static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}

static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }

static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    char b[24];
    uint32_t k = 0;
    const uint32_t v = (uint32_t)-code;
    if (recovery_active)
        return;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u) {
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_HI, 1);
        return;
    }
    if (code == 1) {
        draw_text_box(0, 124, 240, &FONT_S, "DRY RUN OK", C_HI, 1);
        fm1_delay_ms(1500);
        return;
    }
    str_cpy(b, "FAILED  -", sizeof b);
    while (b[k])
        k++;
    if (v >= 10u)
        b[k++] = (char)('0' + v / 10u);
    b[k++] = (char)('0' + v % 10u);
    b[k] = 0;
    draw_text_box(0, 124, 240, &FONT_S, b, C_HI, 1);
    fm1_delay_ms(1500);
}

static void ota_commit(const uint8_t *parm)
{
    bootguard_clear(&bootguard);
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
