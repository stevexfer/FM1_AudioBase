/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit)
 * Modified for MOOD_FM1, 2026: FW_USB_PLAY, FW_USB_PRODUCT
 * Modified for FM1_AudioBase, 2026: FELUCCA_UAC at the codec's rate beside FW_USB_PLAY (duplex), stereo playback */
/* USB full-speed device on USB0: a class-compliant USB-MIDI interface.
 * All SIE traffic happens in usb_poll(), called from the TIMER5 ISR at 2 kHz,
 * so INDEX is never shared and no USB IRQ is needed. MIDI in goes to a ring
 * the audio ISR drains; MIDI out comes from a ring the audio ISR fills.
 * SysEx F0 22 24 35 7D F7 (soft key) asks the main loop to enter UBOOT.
 * VID 0x1209 / PID 0x0001 is the pid.codes test id.
 * FELUCCA_CDC=1 adds a CDC-ACM serial function (IAD composite: EP2 notify,
 * EP3 bulk data) for the console in console.c.
 * FELUCCA_UAC=1 (an app's build.txt) adds a USB audio input (UAC1, class compliant): the master output as
 * 16-bit stereo on an isochronous asynchronous IN endpoint (EP4), so a computer can record the FM-1 over the
 * cable. One rate, the codec's family: 48 kHz (FW_FS48, the codec at 48,076.9 Hz) or 44.1 kHz (44,117.6 Hz):
 * the frames go out as rendered, bit for bit, and the packet sizes follow the codec's clock. audio.c fills a
 * ring (uac_render_start, uac_tap); uac_service sends one packet per USB frame from the TIMER5 ISR, nested in
 * the render too (main.c). The update loader leaves it off: MIDI only.
 * FW_USB_PLAY=1 (an app's build.txt; never the loader) adds a USB audio output (UAC1, class compliant): the
 * computer plays into the FM-1, 16-bit stereo at 48 kHz on an isochronous adaptive OUT endpoint (EP3), after
 * FM1_FjordLead's FELUCCA_UAC_OUT (the endpoint's setup, the read nested in the render, a reader that follows the
 * host's clock by the ring's fill). Nothing is read until the app asks for it (usb_play_enable) and the host
 * streams; the app takes the audio, stereo, at the codec's rate (usb_play_begin, usb_play_read).
 * Both together are one audio device, input and output, under the product name (FW_USB_PRODUCT): interface 2
 * the playback, 3 the recording, listed before the MIDI interface in the audio control collection. */
#include "../hal/fm1_usb.h"   /* registers; relative, so the loader and the host tests find it too */
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 0
#endif
#ifndef FW_USB_PLAY
#define FW_USB_PLAY 0
#endif
#if FW_USB_PLAY && (FELUCCA_CDC || defined(FELUCCA_LOADER))
#error "FW_USB_PLAY: an app without FELUCCA_CDC (EP3)"
#endif
enum { S_FADDR = 0, S_POWER = 1, S_INTRTX1 = 2, S_INTRTX2 = 3, S_INTRRX1 = 4, S_INTRRX2 = 5, S_INTRUSB = 6,
       S_INTRTX1E = 7, S_INTRTX2E = 8, S_INTRRX1E = 9, S_INTRRX2E = 10, S_INTRUSBE = 11, S_FRAME1 = 12,
       S_FRAME2 = 13, S_INDEX = 14,
       S_TXMAXP = 16, S_CSR0 = 17, S_TXCSR1 = 17, S_TXCSR2 = 18, S_RXMAXP = 19, S_RXCSR1 = 20, S_RXCSR2 = 21,
       S_COUNT0 = 22, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23 };

static uint8_t ep0buf[64 + 4] __attribute__((aligned(4)));
static uint8_t ep1tx[64] __attribute__((aligned(4)));
static uint8_t ep1rx[64 + 4] __attribute__((aligned(4)));
#if FELUCCA_CDC
static uint8_t ep2tx[8] __attribute__((aligned(4)));
static uint8_t ep3tx[64] __attribute__((aligned(4)));
static uint8_t ep3rx[64 + 4] __attribute__((aligned(4)));
/* serial rings: in = host -> console (ISR writes), out = console -> host (ISR reads) */
#define CI_N 256u
#define CO_N 2048u
static uint8_t cdc_in[CI_N], cdc_out[CO_N];
static volatile uint32_t ci_w, ci_r, co_w, co_r;
static struct {
    uint8_t line[7];             /* line coding: just stored and echoed back */
    volatile uint8_t dtr;        /* host has the port open */
    uint8_t e0_rx;               /* SET_LINE_CODING data stage pending */
    uint8_t rx_pend;             /* EP3 OUT packet seen, not yet taken (the ring was full) */
    uint32_t rx_pkts, tx_pkts;
} cdc = {{0x00, 0xC2, 0x01, 0x00, 0, 0, 8}};   /* 115200 8N1 */
#endif

#if FELUCCA_UAC
/* USB audio input. The I2S clock is not locked to USB (48,076.9 Hz against the host's 48,000, or 44,117.6 against
 * 44,100): the endpoint is asynchronous, so each 1 ms packet carries what the ring holds, the nominal 48 (or the
 * 44.1 pattern, 44 and a 45th every 10 ms) and one more or one fewer while the ring's fill drifts out of its band.
 * The render writes FW_BLOCK frames at its end, one block every 5.3 ms, and the packets go on draining the ring
 * while it runs (nested, main.c): the fill measured when a render starts is the saw tooth's top, and the ring must
 * still hold a packet when a render that took the whole block ends. So the band starts at a block and a packet:
 * a frame waits ~10 ms on average. */
#define UAC_RATE (FW_FS48 ? 48000u : 44100u)
#define UA_N 1024u               /* ring, frames (16-bit L | R << 16); a power of two */
#define UA_MAXF (UAC_RATE / 1000u + 1u)   /* frames in the largest packet: nominal (+ the 44.1 pattern's) + 1 */
#define UA_LO (FW_BLOCK + UA_MAXF + 16u)  /* a render may take its whole block: keep a packet and more in hand */
#define UA_HI (UA_LO + 64u)
#define UA_PRIME ((UA_LO + UA_HI) / 2u)   /* silence ahead of the first rendered frame */
#define UAC_MAXP (UA_MAXF * 4u)
_Static_assert(UA_HI + FW_BLOCK + UA_MAXF < UA_N, "the ring holds the band, a render and a packet");
static uint32_t ua_ring[UA_N];
static volatile uint32_t ua_w, ua_r;
static uint32_t ep4tx[UA_MAXF] __attribute__((aligned(4)));
static struct {
    volatile uint8_t alt;        /* streaming interface setting: 1 = the host records */
    volatile uint8_t go;         /* the ring is primed: the consumer reads it (0: silence packets) */
    volatile uint8_t flowing;    /* the host takes our packets (it may set alt 1 well before it reads) */
    uint8_t queued;              /* a packet was queued since the stream (re)started */
    uint8_t feed;                /* this render feeds the ring (sampled when it starts) */
    uint8_t wait;                /* polls the queued packet has waited */
    uint8_t e0_rx;               /* SET_CUR sampling frequency: data stage pending */
    volatile uint32_t fill_min;  /* ring fill when the last render started */
    uint32_t acc;                /* 44.1 frames per ms: 0.1 frame steps */
    uint32_t last;               /* the last frame sent, repeated on an underrun */
    uint32_t starts, pkts, frames;               /* frames in packets sent from the ring */
    uint32_t underruns, overruns, missed;        /* glitches: ring empty (repeat), ring full (drop), no packet ready */
    uint32_t stalls;                             /* the host stopped reading for > 20 ms (alt still 1) */
    uint32_t adj_up, adj_down;                   /* packets one frame longer / shorter than the pattern */
    uint32_t fill_lo, fill_hi;                   /* range of fill_min since the stream started */
    uint32_t rate_sets;                          /* SET_CUR sampling frequency requests (one rate: acknowledged) */
} uac;
#endif

#if FW_USB_PLAY
/* USB audio output: the host's packets (48 frames a ms, by its clock) go to a ring as stereo frames; the audio ISR
 * reads it at the codec's rate, which is not locked to USB (160 MHz / 13 / 256 = 48,076.9 Hz against the host's
 * 48,000), so the reader resamples: PB_STEP0 ring frames per output frame, a little more or less by how far the
 * ring's fill is from PB_TARGET (4 points, Catmull-Rom: no drop, no repeat). The fill is taken when a render
 * starts, its high point (a render reads FW_BLOCK frames in one go, the packets come one a ms), and smoothed over
 * 64 renders, so the packets' beat against the renders does not reach the pitch. The render must find its
 * FW_BLOCK frames and a late packet or two: the reader starts at PB_TARGET. */
#define PB_RATE 48000u
#define PB_MAXF 49u                                      /* frames in the largest packet: 48 + 1 */
#define PB_MAXP (PB_MAXF * 4u)
#define PB_N 1024u                                       /* the ring, frames; a power of two (21 ms) */
#define PB_TARGET 416u                                   /* the fill a render starts with (8.7 ms) */
#define PB_HIGH 832u                                     /* over this the reader was held up: back to PB_TARGET */
#define PB_K 2                                           /* the step: PB_STEP0 + PB_K * (fill - PB_TARGET), Q16 */
#define PB_STEP0 ((int32_t)(((uint64_t)PB_RATE * 65536u * FW_MCLK_DIV * 256u + FW_MCLK_HZ / 2u) / FW_MCLK_HZ))
_Static_assert(PB_TARGET > FW_BLOCK + 2u * PB_MAXF && PB_HIGH + 3u * PB_MAXF < PB_N, "the ring holds a render and the packets around it");
static uint32_t pb_ring[PB_N];                           /* frames: 16-bit L | R << 16 */
static uint8_t pbrx[PB_MAXP + 4] __attribute__((aligned(4)));   /* the EP3 OUT packet */
static volatile uint32_t pb_w, pb_r;                     /* writer: TIMER5 (pb_w); reader: the audio ISR (pb_r) */
static struct {
    volatile uint8_t alt;        /* the streaming interface's setting: 1 = the host plays */
    volatile uint8_t want;       /* the app takes the audio (usb_play_enable) */
    volatile uint8_t on;         /* EP3 is read: alt and want (TIMER5 sets it) */
    volatile uint8_t flush;      /* writer -> reader: start empty */
    uint8_t pend;                /* a packet waits on EP3 */
    uint8_t play;                /* the reader is primed */
    int16_t yl[4], yr[4];        /* the reader's frames: it is between [1] and [2] */
    uint32_t frac;               /* ... this far, Q16 */
    int32_t fill_q8;             /* the fill when a render starts, smoothed */
    int32_t step;                /* this render's step, Q16 ring frames per output frame */
    uint32_t starts, pkts, drops, ovr, dry, skips;   /* streams; packets; ring full; RXCSR1 overrun; dry; held up */
} pb;
#endif

static struct {
    uint8_t up, config, pend_addr, has_pend_addr, e0_tx, e0_zlp;
    const uint8_t *e0_src;
    uint16_t e0_left;
    uint32_t resets, setups, rx_pkts, tx_pkts, sof_seen, timeouts, no_sof;
    uint32_t suspends, max_gap, retries, frame_stalls;   /* diagnostics: suspend events, longest run of polls without SOF */
    uint16_t frame, frame_same;  /* last SOF frame number and how long it has not moved (x 10 ms) */
    uint8_t detached;            /* usb_detach() was called: no retry */
    volatile uint8_t suspended;  /* bus idle > 3 ms (unplugged or host asleep): the UI hides "USB" */
    uint8_t last_setup[8];
    uint8_t sysex[16];
    uint8_t sx_len, sx_on;
    volatile uint8_t uboot_req;
    volatile uint8_t ota_req;    /* F0 22 24 35 7F F7: M-UPGRADE upgrade command (FELUCCA_OTA) */
    uint8_t rx_pend;             /* EP1 OUT packet seen, not yet taken (the MIDI ring was too full) */
    uint32_t rx_held, rx_bad;    /* packets held back (NAK) for room, malformed events ignored */
} usb;

#if FELUCCA_OTA
/* M-UPGRADE SysEx (ota.c): one received frame at a time (7-bit bytes between
 * F0 and F7; frames arriving while one is pending are dropped, the host
 * retries), and a TX ring of SysEx event packets sent before any MIDI. */
static uint8_t sx_frame[640];
static uint32_t sx_pos;
static volatile uint32_t sx_frame_len;
static volatile uint8_t sx_ready, sx_collect, sx_busy;
#define SXQ 64u
static uint32_t sx_out_q[SXQ];
static volatile uint32_t so_w, so_r;
#endif

/* MIDI rings: 4-byte USB-MIDI event packets */
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_out_q[MQ];
static volatile uint32_t mi_w, mi_r, mo_w, mo_r;
static void midi_out_event(uint32_t pkt)            /* from the audio ISR */
{
    if (usb.config && mo_w - mo_r < MQ) {
        midi_out_q[mo_w % MQ] = pkt;
        RING_PUBLISH();
        mo_w++;
    }
}

/* ------------------------------------------------------- descriptors --- */
/* interfaces: 0 audio control, 1 MIDI streaming, then the audio streams: 2 the playback (FW_USB_PLAY), then the
 * recording (FELUCCA_UAC), then CDC's two. The audio streams are listed before MIDI in the audio control
 * collection: macOS up to 15 (AppleUSBAudio) drops a MIDI interface from its list while walking it, and gives up
 * before one listed after (FM1_FjordLead, its #67).
 * bcdDevice: 3.00 MIDI, +0.01 CDC, +0.18 the audio input (one rate, listed first), +0.40 the audio output (hosts
 * cache descriptors per version) */
#define PLAY_IF 2u                                       /* FW_USB_PLAY: the playback stream */
#define UAC_AS_IF (2u + FW_USB_PLAY)                     /* FELUCCA_UAC: the recording stream */
#define UAC_TERM (FW_USB_PLAY ? 4u : 2u)                 /* the recording's USB streaming terminal */
#define CDC_IF (2 + FELUCCA_UAC)
#define CFG_LEN (101 + 74 * FELUCCA_CDC + 74 * FELUCCA_UAC + 74 * FW_USB_PLAY)
#define USB_NIF (2 + FELUCCA_UAC + 2 * FELUCCA_CDC + FW_USB_PLAY)
#define USB_BCD_LO (0x18 * FELUCCA_UAC + 0x40 * FW_USB_PLAY)
#if FELUCCA_CDC
static const uint8_t DEV_DESC[18] = {18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0x09, 0x12, 0x01, 0x00,
                                     0x01 + USB_BCD_LO, 0x03, 1, 2, 0, 1};   /* misc/IAD */
static const uint8_t CFG_DESC[] = {
    9, 2, CFG_LEN & 0xFF, CFG_LEN >> 8, USB_NIF, 1, 0, 0x80, 50,
    8, 0x0B, 0, 2 + FELUCCA_UAC, 1, 1, 0, 0,            /* IAD: audio + MIDI (IF 0-1, 0-2) */
#else
#ifndef FELUCCA_USB_PID
#define FELUCCA_USB_PID 0x0001   /* the update loader is 0x0002 */
#endif
static const uint8_t DEV_DESC[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 64, 0x09, 0x12, FELUCCA_USB_PID & 0xFF,
                                     FELUCCA_USB_PID >> 8,
                                     USB_BCD_LO, 0x03, 1, 2, 0, 1};
static const uint8_t CFG_DESC[] = {
    9, 2, CFG_LEN & 0xFF, CFG_LEN >> 8, USB_NIF, 1, 0, 0x80, 50,
#endif
    9, 4, 0, 0, 0, 1, 1, 0, 0,
#if FELUCCA_UAC && FW_USB_PLAY
    11, 0x24, 1, 0x00, 0x01, 53, 0, 3, PLAY_IF, UAC_AS_IF, 1,   /* AC header 1.00: both audio streams, then MIDI */
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 1: USB streaming, 2 ch (L R) */
    9, 0x24, 3, 2, 0x01, 0x03, 0, 1, 0,                 /* output terminal 2: speaker, from 1 */
    12, 0x24, 2, 3, 0x03, 0x06, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 3: line, 2 ch (L R) */
    9, 0x24, 3, 4, 0x01, 0x01, 0, 3, 0,                 /* output terminal 4: USB streaming, from 3 */
#elif FELUCCA_UAC
    10, 0x24, 1, 0x00, 0x01, 31, 0, 2, UAC_AS_IF, 1,    /* AC header 1.00: audio + MIDI streaming */
    12, 0x24, 2, 1, 0x03, 0x06, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 1: line, 2 ch (L R) */
    9, 0x24, 3, 2, 0x01, 0x01, 0, 1, 0,                 /* output terminal 2: USB streaming, from 1 */
#elif FW_USB_PLAY
    10, 0x24, 1, 0x00, 0x01, 31, 0, 2, PLAY_IF, 1,      /* AC header 1.00: audio + MIDI streaming */
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 1: USB streaming, 2 ch (L R) */
    9, 0x24, 3, 2, 0x01, 0x03, 0, 1, 0,                 /* output terminal 2: speaker, from 1 */
#else
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 1,
#endif
    9, 4, 1, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0x00, 0x01, 0x41, 0x00,
    6, 0x24, 2, 1, 1, 0,
    6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 3,
#if FW_USB_PLAY
    9, 4, PLAY_IF, 0, 0, 1, 2, 0, 0,                    /* IF2 audio streaming, alt 0: no bandwidth */
    9, 4, PLAY_IF, 1, 1, 1, 2, 0, 0,                    /* alt 1: the stream */
    7, 0x24, 1, 1, 1, 0x01, 0x00,                       /* AS general: terminal 1, delay 1, PCM */
    11, 0x24, 2, 1, 2, 2, 16, 1, PB_RATE & 0xFF, (PB_RATE >> 8) & 0xFF, PB_RATE >> 16,
                                                        /* type I: 2 ch, 16 bit, 48 kHz only */
    9, 5, 0x03, 0x09, PB_MAXP & 0xFF, PB_MAXP >> 8, 1, 0, 0,   /* EP3 OUT isochronous adaptive, 1 ms */
    7, 0x25, 1, 0x00, 0, 0, 0,                          /* CS endpoint: one rate, no controls */
#endif
#if FELUCCA_UAC
    9, 4, UAC_AS_IF, 0, 0, 1, 2, 0, 0,                  /* audio streaming (recording), alt 0: no bandwidth */
    9, 4, UAC_AS_IF, 1, 1, 1, 2, 0, 0,                  /* alt 1: the stream */
    7, 0x24, 1, UAC_TERM, 1, 0x01, 0x00,                /* AS general: the USB streaming terminal, delay 1, PCM */
    11, 0x24, 2, 1, 2, 2, 16, 1, UAC_RATE & 0xFF, (UAC_RATE >> 8) & 0xFF, UAC_RATE >> 16,
                                                        /* type I: 2 ch, 16 bit, one rate */
    9, 5, 0x84, 0x05, UAC_MAXP & 0xFF, UAC_MAXP >> 8, 1, 0, 0,   /* EP4 IN isochronous async, 1 ms */
    7, 0x25, 1, 0x01, 0, 0, 0,                          /* CS endpoint: sampling frequency control */
#endif
#if FELUCCA_CDC
    8, 0x0B, CDC_IF, 2, 2, 2, 1, 0,                     /* IAD: CDC ACM (IF 2-3, 3-4) */
    9, 4, CDC_IF, 0, 1, 2, 2, 1, 0,                     /* communication */
    5, 0x24, 0x00, 0x10, 0x01,                          /* header 1.10 */
    5, 0x24, 0x01, 0x00, CDC_IF + 1,                    /* call management: the data IF */
    4, 0x24, 0x02, 0x02,                                /* ACM: line coding + state */
    5, 0x24, 0x06, CDC_IF, CDC_IF + 1,                  /* union */
    7, 5, 0x82, 3, 8, 0, 16,                            /* EP2 IN interrupt (never sent) */
    9, 4, CDC_IF + 1, 0, 2, 0x0A, 0, 0, 0,              /* data */
    7, 5, 0x03, 2, 64, 0, 0,                            /* EP3 OUT bulk */
    7, 5, 0x83, 2, 64, 0, 0,                            /* EP3 IN bulk */
#endif
};
typedef char cfg_len_ok[sizeof CFG_DESC == CFG_LEN ? 1 : -1];
#if FELUCCA_CDC
/* SLOOP 2.4, after Felucca 1.0.3 (#67): menu USB SERIAL. ON presents the serial console as before (the
 * descriptors above, byte for byte); OFF, the default, presents the device as a FELUCCA_CDC=0 build does: class 0,
 * no IAD, audio + MIDI only (the same bytes: tests/uac_test.c compares them). With the console, macOS 13-15 let
 * Apple's CDC composite driver take the device and the USB audio input never appears. The console is a developer
 * tool; the editor, the installer and the soft key use USB-MIDI. Read once, before usb_start (main.c: the settings
 * are loaded first), so the host sees one device from the start */
static uint8_t usb_cdc_on;
static const uint8_t DEV_DESC_PLAIN[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00,
                                           USB_BCD_LO, 0x03, 1, 2, 0, 1};
#define CFG_PLAIN_LEN (CFG_LEN - 74)                    /* without the audio IAD (8) and the CDC block (66) */
static uint8_t cfg_plain[CFG_PLAIN_LEN];
static const uint8_t *cfg_plain_get(void)
{
    if (!cfg_plain[0]) {
        memcpy(cfg_plain, CFG_DESC, 9);
        cfg_plain[2] = CFG_PLAIN_LEN & 0xFF;
        cfg_plain[3] = CFG_PLAIN_LEN >> 8;
        cfg_plain[4] = USB_NIF - 2;
        memcpy(cfg_plain + 9, CFG_DESC + 17, CFG_PLAIN_LEN - 9);
    }
    return cfg_plain;
}
static int usb_cdc_now(void) { return usb_cdc_on; }   /* (the menu: what this start presents) */
#else
static int usb_cdc_now(void) { return 0; }
#endif
static const uint8_t STR0[4] = {4, 3, 0x09, 0x04};
static const uint8_t STR1[] = {42, 3, 'H', 0, 0xFC, 0, 'g', 0, 'e', 0, 'l', 0, 't', 0, 'o', 0, 'n', 0, ' ', 0, 'I', 0,
                               'n', 0, 's', 0, 't', 0, 'r', 0, 'u', 0, 'm', 0, 'e', 0, 'n', 0, 't', 0, 's', 0};
#ifdef FELUCCA_LOADER
static const uint8_t STR2[] = {30, 3, 'F', 0, 'e', 0, 'l', 0, 'u', 0, 'c', 0, 'c', 0, 'a', 0, ' ', 0, 'U', 0, 'p', 0,
                               'd', 0, 'a', 0, 't', 0, 'e', 0};
#elif defined(FW_USB_PRODUCT)
/* an app's own product name (its build.txt: define FW_USB_PRODUCT="..."; ASCII): what a computer lists the MIDI
 * port and the audio output as. The installers find the FM-1 by "FM-1" or "Felucca" in it (tools/fm1_install.py,
 * web/fm1ota.js). Made at the first request. */
static uint8_t STR2[2 + 2 * (sizeof FW_USB_PRODUCT - 1)];
static void str2_make(void)
{
    uint32_t i;
    STR2[0] = sizeof STR2;
    STR2[1] = 3;
    for (i = 0; i < sizeof FW_USB_PRODUCT - 1; i++)
        STR2[2u + 2u * i] = (uint8_t)FW_USB_PRODUCT[i];
}
#else
static const uint8_t STR2[] = {16, 3, 'F', 0, 'e', 0, 'l', 0, 'u', 0, 'c', 0, 'c', 0, 'a', 0};
#endif

static int get_desc(uint32_t wvalue, const uint8_t **d, uint16_t *l)
{
    switch (wvalue >> 8) {
    case 1:
        *d = DEV_DESC;
        *l = sizeof DEV_DESC;
#if FELUCCA_CDC
        if (!usb_cdc_on)
            *d = DEV_DESC_PLAIN;
#endif
        return 1;
    case 2:
        *d = CFG_DESC;
        *l = sizeof CFG_DESC;
#if FELUCCA_CDC
        if (!usb_cdc_on) {
            *d = cfg_plain_get();
            *l = CFG_PLAIN_LEN;
        }
#endif
        return 1;
    case 3:
        switch (wvalue & 0xFFu) {
        case 0:
            *d = STR0;
            *l = sizeof STR0;
            return 1;
        case 1:
            *d = STR1;
            *l = sizeof STR1;
            return 1;
        case 2:
#if defined(FW_USB_PRODUCT) && !defined(FELUCCA_LOADER)
            str2_make();
#endif
            *d = STR2;
            *l = sizeof STR2;
            return 1;
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/* ---------------------------------------------------------- the SIE --- */
static void sie_wr(uint32_t r, uint32_t v)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return;
    fm1_usb_sie_wr_start(r, v);
    while (!fm1_usb_sie_done() && --n)
        ;
    if (n)
        usb.timeouts = 0;                               /* consecutive failures only */
    else if (++usb.timeouts > 50u)
        usb.up = 0;                                     /* SIE dead (no clock?): stop polling it */
}

static uint32_t sie_rd(uint32_t r)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return 0;
    fm1_usb_sie_rd_start(r);
    while (!fm1_usb_sie_done())
        if (!--n) {
            if (++usb.timeouts > 50u)
                usb.up = 0;
            return 0;
        }
    usb.timeouts = 0;
    return fm1_usb_sie_data();
}

#if FELUCCA_UAC
static void uac_ep4_reset(void)                         /* drop a queued packet; isochronous mode */
{
    sie_wr(S_INDEX, 4);
    sie_wr(S_TXCSR1, 0x48);                             /* ClrDataTog + FlushFIFO (SDK usb_g_ep_config) */
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_TXCSR1, 0);
    sie_wr(S_TXCSR2, 0x40);                             /* ISO */
}

static void uac_stream(uint32_t alt)                    /* SET_INTERFACE, configuration, bus reset */
{
    uac.go = 0;                                         /* the next render primes the ring again */
    uac.flowing = 0;                                    /* ... once the host has taken a packet */
    uac.queued = 0;
    uac.wait = 0;
    RING_PUBLISH();
    uac.alt = (uint8_t)alt;
    uac.acc = 0;
    uac.last = 0;
    if (alt) {                                          /* (the fill range of the last stream stays readable) */
        uac.starts++;
        uac.fill_lo = 0xFFFFFFFFu;
        uac.fill_hi = 0;
    }
}
#endif

#if FW_USB_PLAY
/* TIMER5: EP3 is read while the host streams (alt 1) and the app wants the audio. Turned on (or `again`: a new
 * SET_INTERFACE), the endpoint starts empty and isochronous, and the reader starts over; off, its packets are left
 * where they land (the next one overruns the last: no code runs for them). */
static void usb_play_gate(int again)
{
    const uint8_t on = (uint8_t)(pb.alt && pb.want);
    if (on == pb.on && !again)
        return;
    pb.pend = 0;
    pb.flush = 1;
    RING_PUBLISH();
    pb.on = on;
    if (on) {
        pb.starts++;
        sie_wr(S_INDEX, 3);
        sie_wr(S_RXMAXP, PB_MAXP);
        sie_wr(S_RXCSR1, 0x90);                         /* ClrDataTog + FlushFIFO (as EP1's RX) */
        sie_wr(S_RXCSR2, 0x40);                         /* ISO */
    }
    sie_wr(S_INTRRX1E, usb.config == 1u ? (on ? 0x0Au : 0x02u) : 0u);   /* EP1 RX, + EP3 RX */
}

static void usb_play_stream(uint32_t alt)               /* SET_INTERFACE, configuration, bus reset */
{
    pb.alt = (uint8_t)(alt && usb.config == 1u);
    usb_play_gate(1);
}
#endif

static void ep1_config(void)
{
    fm1_usb_ep_txbuf(1, ep1tx);
    fm1_usb_ep_rxbuf(1, ep1rx);
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x02);
    fm1_usb_ep_enable(1u << 1);
#if FELUCCA_CDC
    fm1_usb_ep_txbuf(2, ep2tx);
    sie_wr(S_INDEX, 2);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    fm1_usb_ep_txbuf(3, ep3tx);
    fm1_usb_ep_rxbuf(3, ep3rx);
    sie_wr(S_INDEX, 3);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x0A);
    fm1_usb_ep_enable((1u << 2) | (1u << 3));
    cdc.rx_pend = 1;                                    /* look once: a packet may already wait */
#endif
#if FELUCCA_UAC
    fm1_usb_ep4_txbuf(ep4tx);
    sie_wr(S_INDEX, 4);
    sie_wr(S_TXMAXP, 0xFF);
    uac_ep4_reset();
    fm1_usb_ep_enable(1u << 4);
    uac_stream(0);
#endif
#if FW_USB_PLAY
    fm1_usb_ep_rxbuf(3, pbrx);
    fm1_usb_ep_enable(1u << 3);
    usb_play_stream(0);
#endif
}

static void e0_chunk(void)
{
    uint32_t n = usb.e0_left > 64u ? 64u : usb.e0_left, i;
    int last;
    for (i = 0; i < n; i++)
        ep0buf[i] = usb.e0_src[i];
    fm1_usb_ep0_send(ep0buf, n);
    usb.e0_src += n;
    usb.e0_left = (uint16_t)(usb.e0_left - n);
    last = usb.e0_left == 0 && !(n == 64u && usb.e0_zlp);
    if (usb.e0_left == 0 && n == 64u)
        usb.e0_zlp = 0;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, last ? 0x0A : 0x02);
    if (last)
        usb.e0_tx = 0;
}

static void e0_send(const uint8_t *p, uint16_t len, uint16_t wlen)
{
    if (len > wlen)
        len = wlen;
    usb.e0_src = p;
    usb.e0_left = len;
    usb.e0_zlp = (len < wlen) && (len % 64u == 0);
    usb.e0_tx = 1;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x40);
    e0_chunk();
}

static void ep0_service(void)
{
    static const uint8_t zero2[2];
    uint32_t csr, i;
    uint16_t wvalue, wlength;
    uint8_t *s = usb.last_setup;
    if (usb.has_pend_addr) {
        sie_wr(S_FADDR, usb.pend_addr);
        usb.has_pend_addr = 0;
    }
    sie_wr(S_INDEX, 0);
    csr = sie_rd(S_CSR0);
    if (csr & 0x04u) {                                  /* SentStall */
        sie_wr(S_CSR0, 0);
        usb.e0_tx = 0;
#if FELUCCA_CDC
        cdc.e0_rx = 0;
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
#endif
        return;
    }
    if (csr & 0x10u) {                                  /* SetupEnd: the host abandoned the transfer */
        sie_wr(S_CSR0, 0x80);
        usb.e0_tx = 0;
#if FELUCCA_CDC
        cdc.e0_rx = 0;                                  /* else the next SETUP is taken as line coding */
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
#endif
    }
    if (usb.e0_tx) {
        if (!(csr & 0x02u))
            e0_chunk();
        return;
    }
    if (!(csr & 0x01u))
        return;
    fm1_usb_rx_sync();
#if FELUCCA_CDC
    if (cdc.e0_rx) {                                    /* SET_LINE_CODING data stage */
        uint32_t n = sie_rd(S_COUNT0);
        for (i = 0; i < 7u && i < n; i++)
            cdc.line[i] = ep0buf[i];
        cdc.e0_rx = 0;
        goto ack;
    }
#endif
#if FELUCCA_UAC
    if (uac.e0_rx) {                                    /* SET_CUR sampling frequency: 3 bytes, Hz (one rate) */
        uac.rate_sets++;
        uac.e0_rx = 0;
        goto ack;
    }
#endif
    for (i = 0; i < 8u; i++)
        s[i] = ep0buf[i];
    usb.setups++;
    wvalue = (uint16_t)(s[2] | s[3] << 8);
    wlength = (uint16_t)(s[6] | s[7] << 8);
    switch ((uint32_t)s[0] << 8 | s[1]) {
    case 0x0005:                                        /* SET_ADDRESS */
        usb.pend_addr = s[2] & 0x7Fu;
        usb.has_pend_addr = 1;
        goto ack;
    case 0x8006: {                                      /* GET_DESCRIPTOR */
        const uint8_t *d;
        uint16_t l;
        if (get_desc(wvalue, &d, &l)) {
            if (!wlength)
                goto ack;
            e0_send(d, l, wlength);
            return;
        }
        goto stall;
    }
    case 0x0009:                                        /* SET_CONFIGURATION: 0 or 1 only */
        if (s[2] > 1u)
            goto stall;
        usb.config = s[2];
        if (usb.config == 1u)
            ep1_config();
        else
            sie_wr(S_INTRRX1E, 0);
#if FELUCCA_UAC
        if (usb.config != 1u)
            uac_stream(0);
#endif
#if FW_USB_PLAY
        if (usb.config != 1u)
            usb_play_stream(0);
#endif
        goto ack;
    case 0x8008:
        e0_send(&usb.config, 1, wlength);
        return;
    case 0x8000:
    case 0x8100:
    case 0x8200:
        e0_send(zero2, 2, wlength);
        return;
    case 0x010B:                                        /* SET_INTERFACE: alt 0 (alt 1 for the audio stream) */
#if FELUCCA_UAC
        if (s[4] == UAC_AS_IF && wvalue <= 1u && usb.config) {
            uac_ep4_reset();
            uac_stream(wvalue);
            goto ack;
        }
#endif
#if FW_USB_PLAY
        if (s[4] == PLAY_IF && wvalue <= 1u && usb.config) {
            usb_play_stream(wvalue);
            goto ack;
        }
#endif
        if (wvalue == 0)
            goto ack;
        goto stall;
    case 0x810A:
#if FELUCCA_UAC
        if (s[4] == UAC_AS_IF) {
            static uint8_t alt;
            alt = uac.alt;
            e0_send(&alt, 1, wlength);
            return;
        }
#endif
#if FW_USB_PLAY
        if (s[4] == PLAY_IF) {
            static uint8_t alt;
            alt = pb.alt;
            e0_send(&alt, 1, wlength);
            return;
        }
#endif
        e0_send(zero2, 1, wlength);
        return;
    case 0x0201: {                                      /* CLEAR_FEATURE(ENDPOINT_HALT): data toggle reset */
#if FELUCCA_CDC
        uint32_t ep = s[4] & 0x0Fu, last = usb_cdc_on ? 3u : 1u;   /* (USB SERIAL OFF: EP2 / EP3 are not described) */
#else
        uint32_t ep = s[4] & 0x0Fu, last = 1u;
#endif
#if FELUCCA_UAC
        if (wvalue == 0 && s[4] == 0x84u)
            goto ack;                                   /* isochronous: no halt, no toggle */
#endif
#if FW_USB_PLAY
        if (wvalue == 0 && s[4] == 0x03u)
            goto ack;                                   /* isochronous: no halt, no toggle */
#endif
        if (wvalue != 0 || ep > last)
            goto stall;                                 /* not an endpoint we have */
        if (ep) {                                       /* (EP0 has no halt to clear) */
            sie_wr(S_INDEX, ep);
            if (s[4] & 0x80u)
                sie_wr(S_TXCSR1, 0x40);
            else
                sie_wr(S_RXCSR1, 0x80);
        }
        goto ack;
    }
#if FELUCCA_CDC
    case 0x2120:                                        /* SET_LINE_CODING: 7 bytes follow */
        if (wlength) {
            cdc.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* ServicedRxPktRdy, no DataEnd yet */
            return;
        }
        goto ack;
    case 0xA121:                                        /* GET_LINE_CODING */
        e0_send(cdc.line, 7, wlength);
        return;
    case 0x2122:                                        /* SET_CONTROL_LINE_STATE */
        cdc.dtr = s[2] & 1u;
        goto ack;
    case 0x2123:                                        /* SEND_BREAK */
        goto ack;
#endif
#if FELUCCA_UAC
    case 0x2201:                                        /* SET_CUR, endpoint: sampling frequency */
        if (s[4] != 0x84u || s[3] != 1u)
            goto stall;
        if (wlength) {
            uac.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* the 3 bytes follow */
            return;
        }
        goto ack;
    case 0xA281:                                        /* GET_CUR / MIN / MAX: the rate / the range */
    case 0xA282:
    case 0xA283: {
        static const uint8_t RATE[3] = {UAC_RATE & 0xFF, (UAC_RATE >> 8) & 0xFF, UAC_RATE >> 16};
        if (s[4] != 0x84u || s[3] != 1u)
            goto stall;
        e0_send(RATE, 3, wlength);
        return;
    }
#endif
    default:
        goto stall;
    }
ack:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x48);
    return;
stall:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x60);
}

/* SysEx assembly: only short commands matter here */
static void sysex_byte(uint8_t b)
{
    static const uint8_t UBOOT_KEY[6] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};
    if (b >= 0xF8u)
        return;                                        /* realtime may occur anywhere in SysEx */
    if ((b & 0x80u) && b != 0xF0u && b != 0xF7u) {
        usb.sx_on = 0;
#if FELUCCA_OTA
        sx_collect = 0;
#endif
        return;                                        /* another status aborts the unfinished frame */
    }
    if (b == 0xF0) {
        usb.sx_on = 1;
        usb.sx_len = 0;
#if FELUCCA_OTA
        sx_collect = !sx_ready;
        sx_pos = 0;
#endif
    }
    if (!usb.sx_on)
        return;
#if FELUCCA_OTA
    if (sx_collect && b != 0xF0 && b != 0xF7) {
        if (sx_pos < sizeof sx_frame)
            sx_frame[sx_pos++] = b;
        else
            sx_collect = 0;                             /* too long: not ours */
    }
#endif
    if (usb.sx_len < sizeof usb.sysex)
        usb.sysex[usb.sx_len++] = b;
    if (b == 0xF7) {
        uint32_t i, ok = usb.sx_len == 6u;
        for (i = 0; ok && i < 6u; i++)
            ok = usb.sysex[i] == UBOOT_KEY[i];
        if (ok)
            usb.uboot_req = 1;
#if FELUCCA_OTA
        else if (usb.sx_len == 6u && usb.sysex[1] == 0x22 && usb.sysex[2] == 0x24 && usb.sysex[3] == 0x35 &&
                 usb.sysex[4] == 0x7F)
            usb.ota_req = 1;
        else if (sx_collect && sx_pos) {
            sx_frame_len = sx_pos;
            RING_PUBLISH();
            sx_ready = 1;                               /* main loop: ota_take() / ed_service() */
        }
        sx_collect = 0;
#endif
        usb.sx_on = 0;
    }
}

/* one USB-MIDI event packet: SysEx bytes to sysex_byte, channel voice messages and the clock /
 * transport realtime ones to the MIDI ring; a malformed packet (status not matching its CIN, a data
 * byte with bit 7) is ignored (after Felucca 1.0) */
static void midi_in_event(uint32_t pkt)
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu;
    if (cin >= 4u && cin <= 7u) {                      /* SysEx */
        uint32_t k, nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
        for (k = 0; k < nb; k++)
            sysex_byte((uint8_t)(pkt >> (8u * (k + 1u))));
        return;
    }
    if (st >= 0x80u && st < 0xF8u) {                   /* channel / system common also ends a SysEx */
        usb.sx_on = 0;
#if FELUCCA_OTA
        sx_collect = 0;
#endif
    }
    if (cin == 0xFu && (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)) {   /* clock, transport */
        if (mi_w - mi_r < MQ) {
            midi_in_q[mi_w % MQ] = 0xFu | st << 8;
            RING_PUBLISH();
            mi_w++;
        }
        return;
    }
    if (cin >= 8u && cin <= 0xEu && (st >> 4) == cin && !((pkt >> 16) & 0x80u) &&
        (cin == 0xCu || cin == 0xDu || !(pkt & 0x80000000u))) {
        if (mi_w - mi_r < MQ) {
            midi_in_q[mi_w % MQ] = pkt;
            RING_PUBLISH();
            mi_w++;
        }
        return;
    }
    if (cin >= 8u)
        usb.rx_bad++;
}

/* one EP1 OUT packet (<= 16 events) into the MIDI ring, or 0: fewer than 16 + 8 slots free, so the
 * packet stays (the host is NAKed) and is taken on a later poll: a burst from a DAW never drops a
 * note-off (0.9 dropped what did not fit: hanging notes), and 8 slots stay for the TRS input, which
 * shares the ring and cannot wait. The update loader never drains the ring: no back-pressure there.
 * (After Felucca 1.0.) */
#define EP1_ROOM (16u + 8u)
static int ep1_take(const uint8_t *b, uint32_t n)
{
    uint32_t i;
#ifndef FELUCCA_LOADER
    if (MQ - (mi_w - mi_r) < EP1_ROOM)
        return 0;
#endif
    for (i = 0; i + 3u < n; i += 4u)
        midi_in_event((uint32_t)b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
    return 1;
}

static void ep1_rx(void)                                /* leaves the packet (NAK) while the ring is too full */
{
    uint32_t csr, n;
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        usb.rx_pend = 0;
        return;
    }
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    fm1_usb_rx_sync();
    if (!ep1_take(ep1rx, n)) {
        usb.rx_held++;
        return;                                         /* rx_pend stays: taken on a later poll */
    }
    usb.rx_pend = 0;
    usb.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

#if FELUCCA_OTA
/* ---- SysEx frames for the main loop (ota.c / editor.c hooks; felucca.c and the
 * update loader supply ota_now_ms / ota_idle) ---- */
static uint32_t ota_now_ms(void);
static void ota_idle(void);

static int ota_wire_send(const uint8_t *p, uint32_t n)   /* F0..F7 -> USB-MIDI SysEx packets */
{
    uint32_t i = 0, t0 = ota_now_ms();
    sx_busy = 1;
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, pkt;
        uint32_t cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;   /* 4 continues; 5/6/7 end */
        pkt = cin | (uint32_t)p[i] << 8 | (k > 1u ? (uint32_t)p[i + 1] << 16 : 0u) |
              (k > 2u ? (uint32_t)p[i + 2] << 24 : 0u);
        while (so_w - so_r >= SXQ) {
            if (!*(volatile uint8_t *)&usb.config || ota_now_ms() - t0 > 200u) {
                sx_busy = 0;
                return -1;
            }
            ota_idle();
        }
        sx_out_q[so_w % SXQ] = pkt;
        RING_PUBLISH();
        so_w++;
        i += k;
    }
    sx_busy = 0;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!sx_ready)
        return 0;
    RING_PUBLISH();                                     /* read the frame only after the flag */
    *p = sx_frame;
    *n = sx_frame_len;
    return 1;
}
static void ota_frame_done(void)
{
    RING_PUBLISH();                                     /* done with the frame before the ISR may refill it */
    sx_ready = 0;
}
#endif

static void ep1_tx(void)
{
    uint32_t csr, n = 0;
#if FELUCCA_OTA
    if (mo_w == mo_r && so_w == so_r)
        return;
#else
    if (mo_w == mo_r)
        return;
#endif
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;                                         /* previous packet still pending */
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
#if FELUCCA_OTA
    while (so_r != so_w && n < 64u) {                   /* SysEx first, never split by notes */
        uint32_t pkt = sx_out_q[so_r % SXQ];
        ep1tx[n] = (uint8_t)pkt;
        ep1tx[n + 1] = (uint8_t)(pkt >> 8);
        ep1tx[n + 2] = (uint8_t)(pkt >> 16);
        ep1tx[n + 3] = (uint8_t)(pkt >> 24);
        n += 4u;
        so_r++;
    }
    while (!sx_busy && so_r == so_w && mo_r != mo_w && n < 64u) {
#else
    while (mo_r != mo_w && n < 64u) {
#endif
        uint32_t pkt;
        RING_PUBLISH();                                 /* the audio ISR (producer) can preempt us: */
        pkt = midi_out_q[mo_r % MQ];                    /* slot read strictly between the index checks */
        ep1tx[n] = (uint8_t)pkt;
        ep1tx[n + 1] = (uint8_t)(pkt >> 8);
        ep1tx[n + 2] = (uint8_t)(pkt >> 16);
        ep1tx[n + 3] = (uint8_t)(pkt >> 24);
        n += 4u;
        RING_PUBLISH();
        mo_r++;
    }
    if (!n)
        return;
    fm1_usb_ep_send(1, ep1tx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    usb.tx_pkts++;
}

#if FELUCCA_CDC
static void ep3_rx(void)                                /* leaves the packet (NAK) while the ring is full */
{
    uint32_t csr, n, i;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        cdc.rx_pend = 0;
        return;
    }
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    if (CI_N - (ci_w - ci_r) < n)
        return;                                         /* rx_pend stays: retried next poll */
    cdc.rx_pend = 0;
    fm1_usb_rx_sync();
    for (i = 0; i < n; i++)
        cdc_in[(ci_w + i) % CI_N] = ep3rx[i];
    RING_PUBLISH();
    ci_w += n;
    cdc.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

static void ep3_tx(void)                                /* <= 63 bytes per packet: never needs a ZLP */
{
    uint32_t csr, n = 0;
    if (co_w == co_r)
        return;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
    while (co_r != co_w && n < 63u)
        ep3tx[n++] = cdc_out[co_r++ % CO_N];
    fm1_usb_ep_send(3, ep3tx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    cdc.tx_pkts++;
}
#endif

#if FELUCCA_UAC
/* ---- the audio input: audio.c produces (render start, then each block), TIMER5 consumes ---- */
static __attribute__((noinline)) void uac_render_start(void)   /* audio ISR, before a half buffer renders */
{
    uint32_t fill;
    uac.feed = uac.alt && usb.config && uac.flowing;
    if (!uac.feed) {
        uac.go = 0;                                     /* not read (yet / any more): prime when it is */
        return;
    }
    if (!uac.go) {                                      /* (re)start: silence ahead, then this render */
        uint32_t r = ua_r, i;
        for (i = 0; i < UA_PRIME; i++)
            ua_ring[(r + i) & (UA_N - 1u)] = 0;
        RING_PUBLISH();
        ua_w = r + UA_PRIME;
        uac.fill_min = UA_PRIME;
        RING_PUBLISH();
        uac.go = 1;
        return;
    }
    fill = ua_w - ua_r;
    uac.fill_min = fill;
    if (fill < uac.fill_lo)
        uac.fill_lo = fill;
    if (fill > uac.fill_hi)
        uac.fill_hi = fill;
}

static inline int32_t uac_s16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static __attribute__((noinline)) void uac_tap(const int32_t *out, uint32_t n)   /* audio ISR: one block (Q15) */
{
    uint32_t w = ua_w, i;
    if (!uac.feed)
        return;
    if (UA_N - (w - ua_r) < n) {                        /* nobody reads: drop the block */
        uac.overruns++;
        return;
    }
#if FELUCCA_UAC_TONE
    {   /* bench: triangles of period 100 frames instead of the music (L, R in antiphase): every
         * recorded frame must follow the one before, so a lost or repeated frame shows exactly */
        static uint32_t ph;
        for (i = 0; i < n; i++, ph = ph == 99u ? 0u : ph + 1u) {
            uint32_t q = ph < 50u ? ph : 100u - ph, q2 = ph < 50u ? 50u - ph : ph - 50u;
            ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)(int16_t)((int32_t)q * 1000 - 25000) |
                                             (uint32_t)(uint16_t)(int16_t)((int32_t)q2 * 1000 - 25000) << 16;
        }
        (void)out;
    }
#else
    for (i = 0; i < n; i++)
        ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)uac_s16(out[2u * i]) |
                                         (uint32_t)(uint16_t)uac_s16(out[2u * i + 1u]) << 16;
#endif
    RING_PUBLISH();
    ua_w = w + n;
}

/* the next packet into d: the nominal frames a ms (48, or the 44.1 pattern; +-1 to hold the ring's fill), from
 * the ring or, until it is primed, silence. An empty ring repeats the last frame. Returns the frame count. */
static uint32_t uac_packet(uint32_t *d)
{
    uint32_t n = UAC_RATE / 1000u, r, fill, k, i;
    uac.acc += UAC_RATE % 1000u;
    if (uac.acc >= 1000u) {
        uac.acc -= 1000u;
        n++;
    }
    if (!uac.go) {                                      /* not primed yet */
        for (i = 0; i < n; i++)
            d[i] = 0;
        return n;
    }
    if (uac.fill_min > UA_HI) {
        n++;
        uac.adj_up++;
    } else if (uac.fill_min < UA_LO) {
        n--;
        uac.adj_down++;
    }
    r = ua_r;
    fill = ua_w - r;
    RING_PUBLISH();                                     /* the slots after the index */
    k = fill < n ? fill : n;
    for (i = 0; i < k; i++)
        d[i] = ua_ring[(r + i) & (UA_N - 1u)];
    if (k < n) {
        uint32_t last = k ? d[k - 1u] : uac.last;
        uac.underruns++;
        for (; i < n; i++)
            d[i] = last;
    }
    uac.last = d[n - 1u];
    RING_PUBLISH();
    ua_r = r + k;
    uac.pkts++;
    uac.frames += n;
    return n;
}

/* TIMER5, 2 kHz, also nested in the render: the host takes one packet per 1 ms frame; queue the
 * next one as soon as the last has gone (TxPktRdy clear). Not paced on SOF-pending (see usb_poll).
 * The ring is fed only while the host reads: it often selects alt 1 long before its first IN token,
 * and a recorder may stop reading without going back to alt 0 (a packet waiting > 20 ms). */
static void uac_service(void)
{
    uint32_t csr, n;
    if (!usb.up || !usb.config || !uac.alt)
        return;
    sie_wr(S_INDEX, 4);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u) {                                  /* still queued */
        if (uac.wait < 255u && ++uac.wait == 40u && uac.flowing) {
            uac.flowing = 0;
            uac.stalls++;
        }
        return;
    }
    if (uac.queued) {
        if (csr & 0x04u)
            uac.missed++;                               /* UnderRun: an IN token found nothing queued */
        uac.flowing = 1;
    }
    uac.queued = 1;
    uac.wait = 0;
    n = uac_packet(ep4tx);
    fm1_usb_ep4_send(ep4tx, n * 4u);
    sie_wr(S_TXCSR1, 0x01);                             /* TxPktRdy (and UnderRun cleared) */
}

static int usb_rec_state(void) { return uac.alt && usb.config ? (uac.flowing ? 2 : 1) : 0; }   /* 0 no stream, 1 selected, 2 the host takes it */
#endif

#if FW_USB_PLAY
/* EP3 OUT: one packet (16-bit L, R, little endian) into the ring; dropped when the ring is full */
static void usb_play_rx(void)
{
    uint32_t csr, n, i, w = pb_w;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        pb.pend = 0;
        return;
    }
    if (csr & 0x04u)                                    /* the packet before was not read in time */
        pb.ovr++;
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    n = (n > PB_MAXP ? PB_MAXP : n) / 4u;
    fm1_usb_rx_sync();
    pb.pend = 0;
    pb.pkts++;
    if (PB_N - (w - pb_r) >= n) {
        for (i = 0; i < n; i++) {
            const uint8_t *q = pbrx + 4u * i;
            pb_ring[(w + i) & (PB_N - 1u)] = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 |
                                             (uint32_t)q[3] << 24;
        }
        RING_PUBLISH();
        pb_w = w + n;
    } else {
        pb.drops++;
    }
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

/* TIMER5 while the audio ISR runs (main.c): usb_poll waits for the render's end then, and a packet a ms comes
 * meanwhile, each over the one before. This takes the one that waits. */
static void usb_play_nested(void)
{
    if (!usb.up || !pb.on)
        return;
    sie_wr(S_INDEX, 3);
    if (sie_rd(S_RXCSR1) & 1u)
        usb_play_rx();
}

static void usb_play_enable(int on) { pb.want = (uint8_t)(on != 0); }   /* the main loop */
static int usb_play_state(void) { return pb.alt ? (pb.on && pb.play ? 2 : 1) : 0; }   /* 0 no stream, 1 the host plays, 2 heard */

static void usb_play_prime(void)                        /* the reader: PB_TARGET frames behind the writer */
{
    uint32_t i, r = pb_w - PB_TARGET;
    for (i = 0; i < 4u; i++) {
        const uint32_t f = pb_ring[r++ & (PB_N - 1u)];
        pb.yl[i] = (int16_t)f;
        pb.yr[i] = (int16_t)(f >> 16);
    }
    RING_PUBLISH();
    pb_r = r;
    pb.frac = 0;
    pb.fill_q8 = (int32_t)(PB_TARGET << 8);
}

/* audio ISR, before a render: 1 when this render has the host's audio (usb_play_read gives it) */
static int usb_play_begin(void)
{
    uint32_t fill;
    if (pb.flush) {                                     /* a new stream: start empty */
        pb.flush = 0;
        RING_PUBLISH();
        pb_r = pb_w;
        pb.play = 0;
    }
    if (!pb.on) {
        pb.play = 0;
        return 0;
    }
    fill = pb_w - pb_r;
    if (!pb.play) {
        if (fill < PB_TARGET)
            return 0;
        usb_play_prime();
        pb.play = 1;
    } else if (fill > PB_HIGH) {                        /* renders were skipped (a save, an overload) */
        pb.skips++;
        usb_play_prime();
    }
    fill = pb_w - pb_r + 4u;
    pb.fill_q8 += ((int32_t)(fill << 8) - pb.fill_q8) >> 6;
    pb.step = PB_STEP0 + PB_K * ((pb.fill_q8 - (int32_t)(PB_TARGET << 8)) >> 8);
    return 1;
}

/* Catmull-Rom between y1 and y2 at t (Q11), its coefficients doubled */
static inline int16_t pb_cubic(int32_t y0, int32_t y1, int32_t y2, int32_t y3, int32_t t)
{
    int32_t v = y3 - y0 + 3 * (y1 - y2);
    v = ((v * t) >> 11) + 2 * y0 - 5 * y1 + 4 * y2 - y3;
    v = ((v * t) >> 11) + y2 - y0;
    v = y1 + ((v * t) >> 12);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* audio ISR: the next n frames at the codec's rate into d (16-bit L, R interleaved). 0: the ring ran dry (the
 * host stopped, or its packets were lost); the reader waits for PB_TARGET frames again. */
static int usb_play_read(int16_t *d, uint32_t n)
{
    uint32_t i, r = pb_r, frac = pb.frac;
    const uint32_t w = pb_w, step = (uint32_t)pb.step;
    int32_t l0 = pb.yl[0], l1 = pb.yl[1], l2 = pb.yl[2], l3 = pb.yl[3];
    int32_t r0 = pb.yr[0], r1 = pb.yr[1], r2 = pb.yr[2], r3 = pb.yr[3];
    if (!pb.play)
        return 0;
    for (i = 0; i < n; i++) {
        const int32_t t = (int32_t)(frac >> 5);         /* Q11 */
        d[2u * i] = pb_cubic(l0, l1, l2, l3, t);
        d[2u * i + 1u] = pb_cubic(r0, r1, r2, r3, t);
        frac += step;
        if (frac >= 65536u) {                           /* (the step stays under 2) */
            uint32_t f;
            frac -= 65536u;
            if (r == w) {
                pb.dry++;
                pb.play = 0;
                return 0;
            }
            f = pb_ring[r++ & (PB_N - 1u)];
            l0 = l1;
            l1 = l2;
            l2 = l3;
            l3 = (int16_t)f;
            r0 = r1;
            r1 = r2;
            r2 = r3;
            r3 = (int16_t)(f >> 16);
        }
    }
    pb.yl[0] = (int16_t)l0;
    pb.yl[1] = (int16_t)l1;
    pb.yl[2] = (int16_t)l2;
    pb.yl[3] = (int16_t)l3;
    pb.yr[0] = (int16_t)r0;
    pb.yr[1] = (int16_t)r1;
    pb.yr[2] = (int16_t)r2;
    pb.yr[3] = (int16_t)r3;
    pb.frac = frac;
    RING_PUBLISH();
    pb_r = r;
    return 1;
}
#endif

static void usb_poll(void)                              /* TIMER5 ISR, 2 kHz */
{
    uint32_t iu, it, ir;
    if (!usb.up)
        return;
    if (fm1_usb_sof_take()) {                           /* SOF pending: not a reliable "host is there", */
                                                        /* it keeps firing with the cable out */
        usb.sof_seen++;
        usb.no_sof = 0;
    } else if (++usb.no_sof > usb.max_gap) {
        usb.max_gap = usb.no_sof;
    }
    if (usb.config) {                                   /* the frame number only advances with a real host */
        static uint32_t tick;
        if (++tick >= 20u) {                            /* every 20 polls = 10 ms */
            uint16_t f;
            tick = 0;
            f = (uint16_t)(sie_rd(S_FRAME1) | (sie_rd(S_FRAME2) & 7u) << 8);
            if (f != usb.frame) {
                usb.frame = f;
                usb.frame_same = 0;
                usb.suspended = 0;
            } else if (++usb.frame_same >= 10u && !usb.suspended) {   /* 100 ms frozen: unplugged / host asleep */
                usb.suspended = 1;
                usb.frame_stalls++;
            }
        }
    }
    iu = sie_rd(S_INTRUSB);
#ifdef FELUCCA_LOADER
    it = sie_rd(S_INTRTX1) | sie_rd(S_INTRTX2) << 8;    /* the update loader reads both */
    ir = sie_rd(S_INTRRX1) | sie_rd(S_INTRRX2) << 8;
#else
    it = sie_rd(S_INTRTX1);                             /* EP0..7 only: skipping INTR*2 saves */
    ir = sie_rd(S_INTRRX1);                             /* 2 SIE round trips per poll */
#endif
    if (iu & 0x01u) {                                   /* suspend: the bus went idle */
        usb.suspends++;
        usb.suspended = 1;
    }
    if (iu & 0x02u)                                     /* resume */
        usb.suspended = 0;
    if (iu & 0x04u) {                                   /* bus reset */
        usb.resets++;
        sie_wr(S_FADDR, 0);
        usb.config = 0;
        usb.suspended = 0;
        mo_r = mo_w;                                    /* nothing stale for the next host */
        usb.sx_on = 0;
#if FELUCCA_OTA
        so_r = so_w;
        sx_collect = 0;
#endif
        usb.e0_tx = 0;
        usb.has_pend_addr = 0;
        usb.rx_pend = 0;
#if FELUCCA_CDC
        cdc.dtr = 0;
        cdc.e0_rx = 0;
        cdc.rx_pend = 0;
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
        uac_stream(0);
#endif
#if FW_USB_PLAY
        usb_play_stream(0);
#endif
        fm1_usb_ep0_buf(ep0buf);
        sie_wr(S_INTRUSBE, 0x07);
        sie_wr(S_INTRTX1E, 0x01);
        sie_wr(S_INTRRX1E, 0);
    }
    if (it & 0x01u)
        ep0_service();
    if (ir & 0x02u)
        usb.rx_pend = 1;                                /* (the flag reads once: remember the packet) */
    if (usb.rx_pend)
        ep1_rx();
    if (usb.config)
        ep1_tx();
#if FELUCCA_CDC
    if (usb.config) {
        if (ir & 0x08u)
            cdc.rx_pend = 1;
        if (cdc.rx_pend)                                /* not every poll: 3 SIE round trips each */
            ep3_rx();
        if (cdc.dtr)
            ep3_tx();
    }
#endif
#if FELUCCA_UAC
    uac_service();
#endif
#if FW_USB_PLAY
    usb_play_gate(0);                                   /* the app's switch */
    if (pb.on) {
        if (ir & 0x08u)
            pb.pend = 1;
        if (pb.pend)
            usb_play_rx();
    }
#endif
}

static void usb_start(void)                             /* boot, or main-loop retry while usb.up == 0 */
{
    usb.timeouts = 0;
    fm1_usb_reset();                                    /* reset whatever the ROM left */
    fm1_delay_ms(25);
    fm1_usb_attach(ep0buf);
    sie_wr(S_POWER, 0x60);
    sie_wr(S_INTRUSBE, 0x07);                           /* suspend, resume, reset (polled) */
    sie_wr(S_INTRTX1E, 0x01);
    sie_wr(S_INTRTX2E, 0);
    sie_wr(S_INTRRX1E, 0);
    sie_wr(S_INTRRX2E, 0);
    fm1_usb_ep_enable(0x1Fu);
    usb.up = usb.timeouts < 3u;                         /* a dead SIE leaves USB off */
}

/* Powered on without a cable, the SIE does not answer and usb_start leaves
 * USB off: retry once a second from the main loop so a cable plugged in
 * later still enumerates. */
static void usb_retry(uint32_t now_ms)
{
    static uint32_t t;
    if (usb.up || usb.detached || now_ms - t < 1000u)
        return;
    t = now_ms;
    usb.retries++;
    usb_start();
}

static void usb_detach(void)
{
    usb.detached = 1;
    usb.up = 0;
    fm1_usb_off();
}

