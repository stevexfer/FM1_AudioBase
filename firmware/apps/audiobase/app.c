/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1_AudioBase: the computer's audio, over USB, through the FM-1 to its outputs (the codec and, back to the
 * computer, the USB audio input), MASTER the volume of both; GLO the visualizer. The platform's USB audio is
 * sys/usb.c (FW_USB_PLAY in, FELUCCA_UAC out; build.txt). */
#include "ab_fw.h"
#include "avis.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "audiobase: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

enum { PAGE_MAIN, PAGE_VIS, PAGE_COUNT }; /* GLO steps through them */

#define METER_W 180
#define METER_DB_MIN (-600)                 /* the meters' left end, tenths of a dB */

/* the audio interrupt's (the main loop only reads them, but the gain's target) */
static volatile int32_t ab_gain_target;     /* Q15: the main loop's, from MASTER */
static int32_t ab_gain;                     /* Q15: ramped to the target over a block */
static int16_t ab_in[2u * FW_BLOCK];
static int16_t ab_scope[AB_SCOPE];
static volatile uint32_t ab_scope_w;
static volatile uint32_t ab_meter[2];       /* the peak before the volume, 0..32767, falling ~24 dB/s */

/* the main loop's */
static int page = PAGE_MAIN;
static uint32_t dirty = 1;
static int shown_play = -1, shown_rec = -1;
static int32_t shown_gain = -1;
static uint32_t midi_seen_ms, status_ms;

/* ---- for avis.c (ab_fw.h) */
uint16_t* ui_fb(void) { return cv_px; }
void ui_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t* px) { lcd_blit(x, y, w, h, px); }
void ui_lcd_sync(void) { lcd_sync(); }
void ui_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { lcd_fill(x, y, w, h, c); }

void ab_scope_read(int16_t* dst, int n)
{
    const uint32_t w = ab_scope_w;
    int i;
    for (i = 0; i < n; i++)
        dst[i] = ab_scope[(w - (uint32_t)n + (uint32_t)i) & (AB_SCOPE - 1u)];
}

/* MASTER (0..1023) as a gain, Q15: squared, so the knob's travel is near even in dB; the bottom is silence */
static int32_t master_gain(uint32_t m)
{
    return m < 4u ? 0 : (int32_t)(m * m >> 5);
}

/* 0..32768 (32768 = 0 dB) in tenths of a dB, from -960; within about 0.1 dB */
static int32_t db10(uint32_t v)
{
    uint32_t b = 0, f;
    int32_t l;
    if (!v)
        return -960;
    while (b < 31u && (v >> (b + 1u)))
        b++;
    f = b >= 16u ? (v >> (b - 16u)) & 0xFFFFu : (v << (16u - b)) & 0xFFFFu;   /* the mantissa past 1, Q16 */
    f += ((f * (65536u - f)) >> 16) * 22u >> 6;                                /* log2(1 + f), near enough */
    l = ((int32_t)b - 15) * 65536 + (int32_t)f;                                 /* log2(v / 32768), Q16 */
    l = (l * 602 >> 16) / 10;
    return l < -960 ? -960 : l;
}

/* ---- the main page */
static void draw_row(uint32_t y, const char* label, const char* value, uint16_t vc)
{
    cv_begin(240, 18, C_BLACK);
    cv_text(12, 1, &FONT_S, label, C_GRAY);
    cv_text(228 - text_w(&FONT_S, value), 1, &FONT_S, value, vc);
    cv_blit(0, y);
}

static uint16_t meter_colour(int32_t db)
{
    return db > -30 ? RGB(255, 60, 40) : db > -120 ? RGB(250, 200, 40) : RGB(30, 204, 112);
}

static void draw_meters(void)
{
    uint32_t ch;
    cv_begin(240, 26, C_BLACK);
    for (ch = 0; ch < 2u; ch++) {
        const int32_t db = db10(ab_meter[ch]);
        const int32_t y = 3 + 12 * (int32_t)ch;
        int32_t w = (db - METER_DB_MIN) * METER_W / -METER_DB_MIN;
        w = w < 0 ? 0 : w > METER_W ? METER_W : w;
        cv_text(12, y - 3, &FONT_S, ch ? "R" : "L", C_DIM);
        cv_rect(36, y, METER_W, 8, RGB(24, 24, 30));
        cv_rect(36, y, w, 8, meter_colour(db));
    }
    cv_blit(0, 62);
}

static void draw_volume(int32_t gain)
{
    char b[16];
    const int32_t db = db10((uint32_t)gain);
    if (gain)
        fmt_fix(b, db, 1);
    else
        str_cpy(b, "OFF", sizeof b);
    if (gain)
        str_cpy(b + str_len(b), " dB", sizeof b - str_len(b));
    cv_begin(240, 34, C_BLACK);
    cv_text(12, 1, &FONT_S, "VOLUME", C_GRAY);
    cv_text(228 - text_w(&FONT_S, b), 1, &FONT_S, b, C_HI);
    cv_rect(12, 24, 216, 6, C_DIM);
    cv_rect(12, 24, gain * 216 / 32768, 6, C_ACCENT);
    cv_blit(0, 140);
}

static void draw_status(void)
{
    char b[16], n[8];
    const int midi = fm1_ms - midi_seen_ms < 300u;
    cv_begin(240, 18, C_BLACK);
    fmt_int(n, (int32_t)(fw_stats.cpu_q8 * 100u >> 8));
    str_cpy(b, "CPU ", sizeof b);
    str_cpy(b + 4, n, sizeof b - 4);
    cv_text(12, 1, &FONT_S, b, C_GRAY);
    cv_text(110, 1, &FONT_S, fw_stats.usb_up ? "USB" : "---", fw_stats.usb_up ? C_ACCENT : C_DIM);
    cv_text(170, 1, &FONT_S, "MIDI", midi ? C_HI : C_DIM);
    cv_blit(0, 222);
}

static void draw_main(int play, int rec, int32_t gain)
{
    static const char* const PLAY_TXT[3] = { "OFF", "WAITING", "PLAYING" };
    static const char* const REC_TXT[3] = { "OFF", "OPEN", "RECORDING" };
    if (dirty) {
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 10, 240, &FONT_S, FW_NAME, C_WHITE, 1);
        draw_text_box(0, 196, 240, &FONT_S, "GLO: VISUALIZER", C_DIM, 1);
        shown_play = shown_rec = -1;
        shown_gain = -1;
        status_ms = 0;
        dirty = 0;
    }
    if (play != shown_play) {
        draw_row(40, "USB IN", PLAY_TXT[play], play == 2 ? C_ACCENT : C_HI);
        shown_play = play;
    }
    if (rec != shown_rec) {
        draw_row(104, "USB OUT", REC_TXT[rec], rec == 2 ? C_ACCENT : C_HI);
        shown_rec = rec;
    }
    if (gain != shown_gain) {
        draw_volume(gain);
        shown_gain = gain;
    }
    draw_meters();
    if (fm1_ms - status_ms >= 250u || !status_ms) {
        draw_status();
        status_ms = fm1_ms | 1u;
    }
}

static void app_init(void)
{
    usb_play_enable(1);
}

static void app_frame(const fw_input_t* in)
{
    const int play = usb_play_state(), rec = usb_rec_state();
    const int32_t gain = master_gain(in->master);
    fw_midi_t m;
    ab_gain_target = gain;
    while (fw_midi_read(&m))   /* (drained: a full ring would hold the host's SysEx back too) */
        midi_seen_ms = in->ms;
    if (in->pressed & FW_BIT(FW_BTN_GLO)) {
        page = (page + 1) % PAGE_COUNT;
        if (page == PAGE_VIS)
            avis_start(in->ms);
        else
            dirty = 1;
    }
    fw_led_button(FW_BTN_GLO, page == PAGE_VIS);
    fw_led_button(FW_BTN_PLAY, play == 2);
    fw_led_button(FW_BTN_REC, rec == 2);
    if (page == PAGE_VIS)
        avis_frame(in->ms);
    else
        draw_main(play, rec, gain);
}

/* the computer's audio (or silence), its scope and meters, then the volume */
static void app_audio(int32_t* lr, uint32_t frames)
{
    const int32_t g1 = ab_gain_target, step = (g1 - ab_gain) / (int32_t)frames;
    int32_t g = ab_gain;
    uint32_t i, pk[2] = { 0, 0 }, w = ab_scope_w;
    if (!(usb_play_begin() && usb_play_read(ab_in, frames)))
        memset(ab_in, 0, frames * 2u * sizeof ab_in[0]);
    for (i = 0; i < frames; i++) {
        const int32_t l = ab_in[2u * i], r = ab_in[2u * i + 1u];
        const uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);
        pk[0] = al > pk[0] ? al : pk[0];
        pk[1] = ar > pk[1] ? ar : pk[1];
        ab_scope[w++ & (AB_SCOPE - 1u)] = (int16_t)((l + r) >> 1);
        g += step;
        lr[2u * i] = l * g >> 15;
        lr[2u * i + 1u] = r * g >> 15;
    }
    ab_gain = g1;
    ab_scope_w = w;
    for (i = 0; i < 2u; i++) {
        const uint32_t m = ab_meter[i] - (ab_meter[i] >> 6);
        ab_meter[i] = pk[i] > m ? pk[i] : m;
    }
}
