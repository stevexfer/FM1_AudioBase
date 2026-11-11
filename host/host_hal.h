/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <string.h>

#define FW_HOST 1
#define FM1_ADC_BATT 3
#define FM1_ADC_MASTER 4
#define MQ 64u

static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static void fm1_delay_ms(uint32_t ms) { (void)ms; }

static uint16_t host_fb[240 * 240];
static struct {
    uint8_t cmd;
    uint32_t x0, x1, y0, y1, x, y;
} host_lcd;

static void fm1_lcd_hw_init(void) {}
static void fm1_lcd_baud(uint32_t b) { (void)b; }
static void fm1_lcd_wait(void) {}
static void fm1_lcd_deselect(void) {}
static void fm1_lcd_send_cmd(uint8_t c)
{
    host_lcd.cmd = c;
    if (c == 0x2Cu) {
        host_lcd.x = host_lcd.x0;
        host_lcd.y = host_lcd.y0;
    }
}

static void host_lcd_pixel(uint16_t c)
{
    if (host_lcd.x < 240u && host_lcd.y < 240u)
        host_fb[host_lcd.y * 240u + host_lcd.x] = c;
    host_lcd.x++;
    if (host_lcd.x <= host_lcd.x1)
        return;
    host_lcd.x = host_lcd.x0;
    host_lcd.y++;
}

static void fm1_lcd_send_data(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t i;
    if (host_lcd.cmd == 0x2Au && n == 4u) {
        host_lcd.x0 = (uint32_t)b[0] << 8 | b[1];
        host_lcd.x1 = (uint32_t)b[2] << 8 | b[3];
    }
    if (host_lcd.cmd == 0x2Bu && n == 4u) {
        host_lcd.y0 = (uint32_t)b[0] << 8 | b[1];
        host_lcd.y1 = (uint32_t)b[2] << 8 | b[3];
    }
    if (host_lcd.cmd != 0x2Cu)
        return;
    for (i = 0; i + 1u < n; i += 2u)
        host_lcd_pixel((uint16_t)((uint32_t)b[i] << 8 | b[i + 1u]));
}

static struct {
    uint32_t notes, buttons, pressed, released, notes_pressed;
    int16_t enc_steps[7];
} fm1_in;
static uint8_t fm1_led[11];
static uint64_t host_leds;
static int32_t host_adc_master = 1023;

static uint32_t fm1_input_edges(uint32_t *released)
{
    const uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = 0;
    fm1_in.released = 0;
    return p;
}

static uint32_t fm1_input_note_edges(void)
{
    const uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    return p;
}

static int32_t fm1_enc_take(uint32_t e)
{
    const int32_t s = fm1_in.enc_steps[e];
    fm1_in.enc_steps[e] = 0;
    return s;
}

static void fm1_led_key(uint32_t id, int on)
{
    if (on)
        host_leds |= (uint64_t)1 << id;
    if (!on)
        host_leds &= ~((uint64_t)1 << id);
}

static int32_t fm1_adc_read(uint32_t ch)
{
    if (ch == FM1_ADC_MASTER)
        return host_adc_master;
    return 700;
}

static uint32_t midi_in_q[MQ], midi_out_q[MQ];
static volatile uint32_t mi_w, mi_r, mo_w, mo_r;
static void midi_out_event(uint32_t pkt)
{
    if (mo_w - mo_r < MQ)
        midi_out_q[mo_w++ % MQ] = pkt;
}
