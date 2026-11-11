/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 npi3pak */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "host_hal.h"
#include "fw.h"
#include "fw_tables.h"
#include "fw_logo.h"

#ifndef FW_NAME
#define FW_NAME "FM-1 PLAYGROUND"
#endif
#ifndef FW_VERSION
#define FW_VERSION "host"
#endif

static fw_stats_t fw_stats;
static uint8_t host_store[FW_STORE_SLOTS][FW_STORE_MAX];
static uint32_t host_store_len[FW_STORE_SLOTS];

static int fw_store_load(uint32_t slot, void *dst, uint32_t max)
{
    const uint32_t n = slot < FW_STORE_SLOTS && host_store_len[slot] < max ? host_store_len[slot] : max;
    if (slot >= FW_STORE_SLOTS || !host_store_len[slot])
        return -1;
    memcpy(dst, host_store[slot], n);
    return (int)n;
}

static int fw_store_save(uint32_t slot, const void *src, uint32_t len)
{
    if (slot >= FW_STORE_SLOTS || len > FW_STORE_MAX)
        return -1;
    memcpy(host_store[slot], src, len);
    host_store_len[slot] = len;
    return 0;
}

/* the USB audio (sys/usb.c) as the script sets it: usbtone HZ [DBFS] is the computer playing a sine, usbrec 0|1|2
   the computer's recording (no stream, selected, taken) */
static uint32_t host_usb_hz, host_usb_phase;
static int32_t host_usb_amp;
static int host_usb_rec;

static void usb_play_enable(int on) { (void)on; }
static int usb_play_state(void) { return host_usb_hz ? 2 : 0; }
static int usb_play_begin(void) { return host_usb_hz != 0; }
static int usb_rec_state(void) { return host_usb_rec; }

static int usb_play_read(int16_t *d, uint32_t n)
{
    uint32_t i;
    if (!host_usb_hz)
        return 0;
    for (i = 0; i < n; i++) {
        const int16_t s = (int16_t)(host_usb_amp * (int32_t)FW_SINE[host_usb_phase >> 22] >> 15);
        d[2u * i] = s;
        d[2u * i + 1u] = (int16_t)-s;
        host_usb_phase += (uint32_t)((uint64_t)host_usb_hz * 4294967296ull / FW_FS);
    }
    return 1;
}

#include "libc.c"
#include "lcd.c"
#include "gfx.c"
#include "input.c"
#include "app.c"

#define HOST_MAX_SAMPLES (FW_FS * 120u)

static int16_t *host_wav;
static uint32_t host_wav_n;
static uint64_t host_samples;

static void host_render_until(uint32_t ms)
{
    int32_t block[2u * FW_BLOCK];
    while (host_samples * 1000u < (uint64_t)ms * FW_FS) {
        uint32_t i;
        app_audio(block, FW_BLOCK);
        for (i = 0; i < 2u * FW_BLOCK && host_wav_n < 2u * HOST_MAX_SAMPLES; i++)
            host_wav[host_wav_n++] = (int16_t)(block[i] > 32767 ? 32767 : block[i] < -32768 ? -32768 : block[i]);
        host_samples += FW_BLOCK;
    }
}

static void host_frames(uint32_t n)
{
    fw_input_t in;
    while (n--) {
        fm1_ms += 15u;
        host_render_until(fm1_ms);
        fw_input_read(&in);
        app_frame(&in);
    }
}

static void host_put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void host_put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void host_write_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fwrite("RIFF", 1, 4, f);
    host_put32(f, 36u + host_wav_n * 2u);
    fwrite("WAVEfmt ", 1, 8, f);
    host_put32(f, 16);
    host_put16(f, 1);
    host_put16(f, 2);
    host_put32(f, FW_FS);
    host_put32(f, FW_FS * 4u);
    host_put16(f, 4);
    host_put16(f, 16);
    fwrite("data", 1, 4, f);
    host_put32(f, host_wav_n * 2u);
    fwrite(host_wav, 2, host_wav_n, f);
    fclose(f);
    printf("wav %s %u frames\n", path, host_wav_n / 2u);
}

static void host_write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        const uint16_t c = host_fb[i];
        const uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u),
                                (uint8_t)((c & 31u) * 255u / 31u)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("screen %s\n", path);
}

static int host_find(const char *const *names, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (str_eq(names[i], name))
            return (int)i;
    }
    return -1;
}

static void host_button(int label, int down)
{
    const uint32_t bit = FW_BIT(FW_BTN_MATRIX[label]);
    if (down && !(fm1_in.buttons & bit))
        fm1_in.pressed |= bit;
    if (!down && (fm1_in.buttons & bit))
        fm1_in.released |= bit;
    if (down)
        fm1_in.buttons |= bit;
    if (!down)
        fm1_in.buttons &= ~bit;
}

static void host_key(uint32_t k, int down)
{
    const uint32_t bit = FW_BIT(k);
    if (down && !(fm1_in.notes & bit))
        fm1_in.notes_pressed |= bit;
    if (down)
        fm1_in.notes |= bit;
    if (!down)
        fm1_in.notes &= ~bit;
}

static void host_midi(uint32_t status, uint32_t d1, uint32_t d2)
{
    const uint32_t cin = status >= 0xF0u ? 0xFu : status >> 4;
    if (mi_w - mi_r < MQ)
        midi_in_q[mi_w++ % MQ] = cin | status << 8 | d1 << 16 | d2 << 24;
}

static void host_bench(uint32_t ms)
{
    int32_t block[2u * FW_BLOCK];
    const uint32_t blocks = ms * FW_FS / 1000u / FW_BLOCK;
    const clock_t t0 = clock();
    uint32_t i;
    for (i = 0; i < blocks; i++)
        app_audio(block, FW_BLOCK);
    printf("bench %u blocks %.1f us/block\n", blocks, (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / blocks);
}

static int host_command(char *line)
{
    char cmd[32] = {0}, a[256] = {0}, b[32] = {0}, c[32] = {0};
    const int n = sscanf(line, "%31s %255s %31s %31s", cmd, a, b, c);
    const int down = !str_eq(b, "up");
    if (n < 1 || cmd[0] == '#')
        return 0;
    if (str_eq(cmd, "frames"))
        host_frames((uint32_t)atoi(a));
    if (str_eq(cmd, "key"))
        host_key((uint32_t)atoi(a) % FW_KEYS, down);
    if (str_eq(cmd, "btn") && host_find(FW_BTN_NAME, FW_BTN_COUNT, a) >= 0)
        host_button(host_find(FW_BTN_NAME, FW_BTN_COUNT, a), down);
    if (str_eq(cmd, "knob") && host_find(FW_KNOB_NAME, FW_KNOB_COUNT, a) >= 0)
        fm1_in.enc_steps[FW_KNOB_MATRIX[host_find(FW_KNOB_NAME, FW_KNOB_COUNT, a)]] += (int16_t)atoi(b);
    if (str_eq(cmd, "master"))
        host_adc_master = atoi(a);
    if (str_eq(cmd, "midi"))
        host_midi((uint32_t)strtoul(a, 0, 0), (uint32_t)atoi(b), (uint32_t)atoi(c));
    if (str_eq(cmd, "screen"))
        host_write_ppm(a);
    if (str_eq(cmd, "wav"))
        host_write_wav(a);
    if (str_eq(cmd, "bench"))
        host_bench((uint32_t)atoi(a));
    if (str_eq(cmd, "echo"))
        printf("%s %s %s\n", a, b, c);
    if (str_eq(cmd, "usbtone")) {
        host_usb_hz = (uint32_t)atoi(a);
        host_usb_amp = (int32_t)(32767.0 * pow(10.0, (n >= 3 ? atof(b) : 0.0) / 20.0));
    }
    if (str_eq(cmd, "usbrec"))
        host_usb_rec = atoi(a);
    if (str_eq(cmd, "logo"))
        draw_logo();
    if (str_eq(cmd, "leds"))
        printf("leds %016llx\n", (unsigned long long)host_leds);
    return str_eq(cmd, "quit");
}

int main(void)
{
    char line[512];
    host_wav = calloc(2u * HOST_MAX_SAMPLES, sizeof *host_wav);
    lcd_init();
    app_init();
    while (fgets(line, sizeof line, stdin)) {
        if (host_command(line))
            break;
    }
    free(host_wav);
    return 0;
}
