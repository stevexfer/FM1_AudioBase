/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit)
 * Modified for FM1_AudioBase, 2026: the USB audio input's tap (FELUCCA_UAC) */
#define FW_HALF_WORDS (FW_BLOCK * 2u)
#define FW_OUT_SHIFT 7

static int32_t abuf[2u * FW_HALF_WORDS] __attribute__((aligned(4)));
static volatile uint32_t fw_in_audio;
static volatile uint32_t fw_audio_hold;
static volatile uint32_t fw_t5_nested_ticks;
static fw_stats_t fw_stats;

static int32_t fw_clip16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return v;
}

/* the app's block (or silence while held), and what goes out of the codec goes to the USB audio input too */
static void fw_audio_render(int32_t *out)
{
    uint32_t i;
#if FELUCCA_UAC
    uac_render_start();
#endif
    if (fw_audio_hold) {
        for (i = 0; i < FW_HALF_WORDS; i++)
            out[i] = 0;
    } else {
        app_audio(out, FW_BLOCK);
    }
#if FELUCCA_UAC
    uac_tap(out, FW_BLOCK);
#endif
    for (i = 0; i < FW_HALF_WORDS; i++)
        out[i] = fw_clip16(out[i]) << FW_OUT_SHIFT;
}

void fm1_alnk0_irq(void)
{
    uint8_t p;
    uint32_t t0, half, us;
    fw_in_audio = 1;
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    fw_t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (!(p & FM1_AUDIO_HALF)) {
        fw_in_audio = 0;
        return;
    }
    /* acknowledged before the render (ChoralRoot 0.18, heard on the unit): one pending bit, so a render that ends
     * after the next boundary would clear that boundary's interrupt and the DMA would replay a stale half */
    fm1_audio_ack_half();
    half = fm1_audio_free_half();
    fw_audio_render(&abuf[half * FW_HALF_WORDS]);
    us = (fm1_ticks() - t0 - fw_t5_nested_ticks) / FM1_TICKS_PER_US;
    if (us > fw_stats.audio_max_us)
        fw_stats.audio_max_us = us;
    fw_stats.cpu_q8 = (fw_stats.cpu_q8 * 15u + (us * 256u) / (FW_BLOCK * 1000000u / FW_FS)) / 16u;
    if (fm1_audio_free_half() != half)
        fw_stats.audio_late++;
    fw_in_audio = 0;
}
extern void isr_alnk0(void);

static void fw_audio_start(void)
{
    uint32_t i;
    for (i = 0; i < FW_NELEM(abuf); i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, FW_HALF_WORDS, isr_alnk0, 3);
}

static void audio_silence(void)
{
    uint32_t i;
    for (i = 0; i < FW_NELEM(abuf); i++)
        abuf[i] = 0;
}
