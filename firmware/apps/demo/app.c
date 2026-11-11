/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 npi3pak */
#define NVOICES 6u
#define ENV_MAX 0x7FFFFFFFu
#define SCOPE_W 200u

enum { WAVE_SAW, WAVE_SQUARE, WAVE_TRI, WAVE_SINE, WAVE_COUNT };
static const char *const WAVE_NAME[WAVE_COUNT] = {"SAW", "SQUARE", "TRI", "SINE"};

typedef struct {
    uint32_t phase, inc;
    uint32_t env;
    int32_t lp;
    uint8_t note, gate, key;
} voice_t;

typedef struct {
    int32_t wave, cutoff, attack, release, octave;
} params_t;

static voice_t voices[NVOICES];
static volatile params_t params = {WAVE_SAW, 40, 4, 24, 0};
static volatile uint32_t master_q8 = 256;
static int16_t scope[SCOPE_W];
static volatile uint32_t scope_ready;
static uint32_t screen_dirty = 1;
static fw_midi_t last_midi;

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static uint32_t env_rate(int32_t step)
{
    return 0x7FFFFFFFu / ((uint32_t)(step * step) * 64u + 32u);
}

static int32_t osc(uint32_t wave, uint32_t phase)
{
    const int32_t saw = (int32_t)(phase >> 16) - 32768;
    if (wave == WAVE_SQUARE)
        return phase < 0x80000000u ? 24000 : -24000;
    if (wave == WAVE_TRI)
        return (saw < 0 ? -saw : saw) * 2 - 32768;
    if (wave == WAVE_SINE)
        return FW_SINE[phase >> 22];
    return saw;
}

static void voice_on(uint32_t note, uint32_t key)
{
    uint32_t i, pick = 0, best = 0xFFFFFFFFu;
    for (i = 0; i < NVOICES; i++) {
        const uint32_t score = voices[i].env >> 1 | (uint32_t)voices[i].gate << 31;
        if (score < best) {
            best = score;
            pick = i;
        }
    }
    fm1_irq_off();
    voices[pick].note = (uint8_t)note;
    voices[pick].key = (uint8_t)key;
    voices[pick].inc = FW_NOTE_INC[note & 127u];
    voices[pick].gate = 1;
    fm1_irq_on();
}

static void voice_off(uint32_t note, uint32_t key)
{
    uint32_t i;
    for (i = 0; i < NVOICES; i++) {
        if (voices[i].gate && voices[i].note == note && voices[i].key == key)
            voices[i].gate = 0;
    }
}

static uint32_t key_note(uint32_t k)
{
    return (uint32_t)clampi((int32_t)(FW_KEY_BASE_NOTE + k) + params.octave * 12, 0, 127);
}

static void handle_keys(const fw_input_t *in)
{
    uint32_t k;
    for (k = 0; k < FW_KEYS; k++) {
        if (in->keys_released & FW_BIT(k))
            voice_off(key_note(k), 1);
        if (in->keys_pressed & FW_BIT(k))
            voice_on(key_note(k), 1);
    }
}

static void handle_midi(void)
{
    fw_midi_t m;
    while (fw_midi_read(&m)) {
        const uint32_t kind = m.status & 0xF0u;
        last_midi = m;
        screen_dirty = 1;
        if (kind == 0x90u && m.d2)
            voice_on(m.d1, 0);
        if (kind == 0x80u || (kind == 0x90u && !m.d2))
            voice_off(m.d1, 0);
    }
}

static void handle_knobs(const fw_input_t *in)
{
    params_t p = params;
    p.wave = (p.wave + in->knob[FW_KNOB_1] + WAVE_COUNT * 64) % WAVE_COUNT;
    p.cutoff = clampi(p.cutoff + in->knob[FW_KNOB_2], 1, 64);
    p.attack = clampi(p.attack + in->knob[FW_KNOB_3], 0, 64);
    p.release = clampi(p.release + in->knob[FW_KNOB_4], 0, 64);
    if (in->pressed & FW_BIT(FW_BTN_OCT_DOWN))
        p.octave = clampi(p.octave - 1, -3, 3);
    if (in->pressed & FW_BIT(FW_BTN_OCT_UP))
        p.octave = clampi(p.octave + 1, -3, 3);
    if (in->knob[FW_KNOB_1] || in->knob[FW_KNOB_2] || in->knob[FW_KNOB_3] || in->knob[FW_KNOB_4] ||
        (in->pressed & (FW_BIT(FW_BTN_OCT_DOWN) | FW_BIT(FW_BTN_OCT_UP))))
        screen_dirty = 1;
    params = p;
    master_q8 = in->master * in->master >> 12;
}

static void draw_param(uint32_t y, const char *label, const char *value, int32_t bar, int32_t bar_max, uint16_t c)
{
    cv_begin(240, 18, C_BLACK);
    cv_text(12, 1, &FONT_S, label, C_GRAY);
    cv_text(70, 1, &FONT_S, value, C_HI);
    cv_rect(150, 6, 78, 6, C_DIM);
    cv_rect(150, 6, bar * 78 / bar_max, 6, c);
    cv_blit(0, y);
}

static void draw_params(void)
{
    char b[12];
    draw_param(40, "WAVE", WAVE_NAME[params.wave], params.wave + 1, WAVE_COUNT, RGB(40, 124, 255));
    fmt_int(b, params.cutoff);
    draw_param(60, "TONE", b, params.cutoff, 64, RGB(30, 204, 112));
    fmt_int(b, params.attack);
    draw_param(80, "ATK", b, params.attack, 64, C_WHITE);
    fmt_int(b, params.release);
    draw_param(100, "REL", b, params.release, 64, RGB(255, 98, 26));
    fmt_int(b, params.octave);
    draw_param(120, "OCT", b, params.octave + 4, 7, C_GRAY);
}

static void draw_status(void)
{
    char b[16], n[8];
    cv_begin(240, 18, C_BLACK);
    fmt_int(n, (int32_t)(fw_stats.cpu_q8 * 100u >> 8));
    str_cpy(b, "CPU ", sizeof b);
    str_cpy(b + 4, n, sizeof b - 4);
    cv_text(12, 1, &FONT_S, b, C_GRAY);
    cv_text(110, 1, &FONT_S, fw_stats.usb_up ? "USB" : "---", fw_stats.usb_up ? C_ACCENT : C_DIM);
    fmt_int(n, last_midi.d1);
    cv_text(170, 1, &FONT_S, "MIDI", last_midi.status ? C_HI : C_DIM);
    cv_text(212, 1, &FONT_S, n, C_GRAY);
    cv_blit(0, 222);
}

static void draw_scope(void)
{
    uint32_t x;
    int32_t prev = 40;
    if (!scope_ready)
        return;
    cv_begin(SCOPE_W, 80, C_BLACK);
    cv_rect(0, 40, SCOPE_W, 1, C_DIM);
    for (x = 0; x < SCOPE_W; x++) {
        const int32_t y = 40 - scope[x] / 820;
        cv_line((int32_t)x - 1, prev, (int32_t)x, y, C_ACCENT);
        prev = y;
    }
    cv_blit(20, 140);
    scope_ready = 0;
}

static void app_init(void)
{
}

static void app_frame(const fw_input_t *in)
{
    uint32_t i, lit = 0;
    handle_knobs(in);
    handle_keys(in);
    handle_midi();
    for (i = 0; i < NVOICES; i++) {
        const int32_t k = (int32_t)voices[i].note - (int32_t)FW_KEY_BASE_NOTE - params.octave * 12;
        if (voices[i].gate && voices[i].key && k >= 0 && k < (int32_t)FW_KEYS)
            lit |= FW_BIT(k);
    }
    fw_leds_keys(lit);
    fw_led_button(FW_BTN_PLAY, (in->ms / 500u) & 1u);
    if (screen_dirty) {
        draw_text_box(0, 10, 240, &FONT_S, FW_NAME, C_WHITE, 1);
        draw_params();
        screen_dirty = 0;
    }
    draw_scope();
    if ((in->ms & 255u) < 16u)
        draw_status();
}

static void app_audio(int32_t *lr, uint32_t frames)
{
    const uint32_t wave = (uint32_t)params.wave;
    const int32_t k = params.cutoff * params.cutoff * 16;
    const uint32_t up = env_rate(params.attack), down = env_rate(params.release);
    const int32_t gain = (int32_t)master_q8;
    uint32_t i, v;
    for (i = 0; i < frames; i++) {
        int32_t mix = 0;
        for (v = 0; v < NVOICES; v++) {
            voice_t *s = &voices[v];
            if (s->gate)
                s->env = s->env > ENV_MAX - up ? ENV_MAX : s->env + up;
            if (!s->gate)
                s->env = s->env < down ? 0 : s->env - down;
            if (!s->env)
                continue;
            s->phase += s->inc;
            s->lp += (int32_t)(((int64_t)(osc(wave, s->phase) - s->lp) * k) >> 16);
            mix += (int32_t)(((int64_t)s->lp * (int32_t)(s->env >> 16)) >> 15);
        }
        mix = (mix >> 2) * gain >> 8;
        lr[2u * i] = mix;
        lr[2u * i + 1u] = mix;
        if (!scope_ready && i < SCOPE_W)
            scope[i] = (int16_t)clampi(mix, -32768, 32767);
    }
    if (!scope_ready)
        scope_ready = 1;
}
