/* SPDX-License-Identifier: GPL-3.0-only */
/* What the app's FPU C (avis.c) reaches of the platform, through app.c: ints and pointers only. */
#ifndef AB_FW_H
#define AB_FW_H
#include <stdint.h>

#define UI_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* the canvas's own pixels: 240 x 124, RGB565 with its bytes swapped. ui_lcd_blit waits for the transfer that runs,
   then starts one of w x h pixels at px and returns: px must stay as it is until ui_lcd_sync or the next blit */
uint16_t* ui_fb(void);
void ui_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t* px);
void ui_lcd_sync(void);
void ui_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c);

/* the scope: the last n (up to AB_SCOPE) samples heard, mono, before the volume (+-32767) */
#define AB_SCOPE 512
void ab_scope_read(int16_t* dst, int n);

#endif
