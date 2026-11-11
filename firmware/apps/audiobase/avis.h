/* SPDX-License-Identifier: GPL-3.0-only */
/* AudioBase's visualizer (GLO): the sound's waveform, fed back on itself. See avis.c. */
#ifndef AVIS_H
#define AVIS_H
#include <stdint.h>

/* before the first call: it takes the whole screen and the canvas. It draws the scope (ab_scope_read) */
void avis_start(uint32_t now);
/* draws a third of a frame (a strip of 180 x 60) */
void avis_frame(uint32_t now);

#endif
