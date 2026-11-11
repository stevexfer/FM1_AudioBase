/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit)
 * Modified for MOOD_FM1, 2026: FW_UBOOT_LABELS, FW_USB_PLAY
 * Modified for FM1_AudioBase, 2026: the USB audio input's packets nested in the render (FELUCCA_UAC) */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
#if FM1_CPU1_USED
extern uint32_t _c1_start[], _c1_load[];   /* (_c1_end: fm1_guard.h) */
#endif

#define FW_UBOOT_HOLD_MS 5000u
#define FW_FRAME_MS 15u
#define FW_SPLASH_MS 1000u          /* the splash stays this long, counted from drawing it */
#ifndef FW_UBOOT_COMBO
#define FW_UBOOT_COMBO 1
#endif
#ifndef FW_UBOOT_LABELS             /* the buttons held together for FW_UBOOT_HOLD_MS (FW_BTN_* bits) */
#define FW_UBOOT_LABELS (FW_BIT(FW_BTN_OCT_DOWN) | FW_BIT(FW_BTN_OCT_UP))
#endif

void fm1_timer5_irq(void)
{
    static uint32_t sub, owed, last, acc;
    const uint32_t t0 = fm1_ticks();
    uint32_t usb_due;
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    usb_due = sub % 5u == 0u;
    if (usb_due)
        owed |= 1u;
    if (FW_UART && sub % 5u == 2u)
        owed |= 2u;
    sub = sub + 1u == 10u ? 0u : sub + 1u;
    if (fw_in_audio) {
        /* the USB audio streams move a packet a ms: not left to the render's end */
#if FW_USB_PLAY
        if (usb_due)
            usb_play_nested();
#endif
#if FELUCCA_UAC
        if (usb_due)
            uac_service();
#endif
        (void)usb_due;
        fw_t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
#if FW_UART
    if (owed & 2u)
        uart_midi_poll();
#endif
    owed = 0;
}
extern void isr_timer5(void);

static void fw_hex(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    const uint32_t vals[5] = {c->vec, c->pc, c->emu, c->dbg, c->rets};
    static const char *const names[5] = {"VEC", "PC", "EMU", "DBG", "RETS"};
    char b[12];
    uint32_t i, t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "CRASH", C_WHITE, 1);
    for (i = 0; i < 5u; i++) {
        fw_hex(b, vals[i]);
        draw_text_box(10, 40 + 20 * i, 60, &FONT_S, names[i], C_WHITE, 0);
        draw_text_box(80, 40 + 20 * i, 150, &FONT_S, b, C_WHITE, 0);
    }
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void fw_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (FW_LOGO) {
        draw_logo();
        draw_text_box(0, 1, 236, &FONT_S, FW_VERSION, C_GRAY, 2);
        return;
    }
    draw_text_box(0, 96, 240, &FONT_S, FW_NAME, C_WHITE, 1);
    draw_text_box(0, 120, 240, &FONT_S, FW_VERSION, C_GRAY, 1);
}

static void fw_enter_uboot(const char *why)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, why, C_ACCENT, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
}

static void fw_uboot_combo(uint32_t now)
{
    static uint32_t t0, shown;
    const uint32_t held = now - t0;
    uint32_t both = 0, b;
    char d[4] = {0, '.', '.', 0};
    if (!FW_UBOOT_COMBO)
        return;
    for (b = 0; b < FW_BTN_COUNT; b++) {
        if ((FW_UBOOT_LABELS) & FW_BIT(b))
            both |= FW_BIT(fw_matrix_of(b));
    }
    if ((fm1_in.buttons & both) != both) {
        t0 = now;
        shown = 0;
        return;
    }
    if (held > FW_UBOOT_HOLD_MS)
        fw_enter_uboot("UBOOT");
    if (held <= 2000u)
        return;
    d[0] = (char)('0' + (FW_UBOOT_HOLD_MS - held + 999u) / 1000u);
    if (d[0] == (char)shown)
        return;
    shown = (uint32_t)d[0];
    lcd_fill(0, 100, 240, 40, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, "UBOOT IN", C_ACCENT, 1);
    draw_text_box(0, 128, 240, &FONT_S, d, C_ACCENT, 1);
}

static void fw_service_usb(void)
{
#if defined(APP_SYSEX) && FELUCCA_OTA
    {
        /* an update frame starts pack7(00 59 ..), so any other first byte is the app's (app.c app_sysex). One look
           at the pending frame decides: ota_service() drops frames that are not its own */
        const uint8_t *p;
        uint32_t n;
        if (ota_frame_get(&p, &n)) {
            if (n && p[0] != 0) {
                app_sysex(p, n);
                ota_frame_done();
            } else
                ota_service();
        }
    }
#else
    ota_service();
#endif
    if (usb.ota_req) {
        usb.ota_req = 0;
        fw_audio_hold = 1;                  /* the app's render off: the update gets the CPU */
        if (flash_ok)
            ota_session();
        fw_audio_hold = 0;
        lcd_fill(0, 0, 240, 240, C_BLACK);
    }
    if (usb.uboot_req)
        fw_enter_uboot("UBOOT (USB)");
}

static void fw_main(void)
{
    const uint32_t boot_ms = fm1_ms;
    fw_input_t in;
    uint32_t splash_t, f = irq_save();
    flash_ok = fl_id_known(FL_FAR(fl_jedec_ram)());
    irq_restore(f);
    if (flash_ok)
        ota_boot_cleanup();
    lcd_init();
    fw_splash();
    splash_t = fm1_ticks();
    fm1_input_init();
    fm1_adc_init();
    app_init();
#if FM1_CPU1_USED
    if (!bootguard.failed) {
        /* the second core (hal/fm1_cpu1.h; the app hands it work with fw_core1_run), before the audio and the
         * timers (started later it never reaches its entry). It starts in the chip's ROM: the PC limits open
         * meanwhile. A failed boot before this one, or no answer: never started, one core */
        fm1_guard_pc_open();
        fm1_cpu1_start();                      /* (the top of RAM is locked later, fm1_guard_lock_top) */
        fm1_guard_enable(FM1_GUARD_PC);
    }
#endif
    fw_audio_start();
    usb_start();
#if FW_UART
    uart_midi_init();
#endif
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    while ((uint32_t)(fm1_ticks() - splash_t) < FW_SPLASH_MS * 1000u * FM1_TICKS_PER_US)
        fm1_wdt_feed();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    for (;;) {
        const uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(m);
        if (bootguard.pending && m - boot_ms >= 30000u)
            bootguard_clear(&bootguard);
        fw_uboot_combo(m);
        fw_service_usb();
        fw_stats.usb_up = usb.config && !usb.suspended;
        fw_input_read(&in);
        app_frame(&in);
        while (fm1_ms - m < FW_FRAME_MS)
            fw_service_usb();
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt, boot_mode;
#if FM1_CPU1_USED
    fm1_cpu1_halt();                           /* (after Jangada, X0X): a second core still running from before this
                                                * reset (a watchdog, a fault, another firmware) would read the RAM
                                                * cleared below */
#endif
    fm1_time_init();
    fm1_supplies_stock();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if ((p3 & 1u) && !(p3 & (4u | 0x40u)))
        bootguard_clear(&bootguard);
    boot_mode = bootguard_begin(&bootguard);
    if (boot_mode == BOOT_ROM)
        fm1_enter_uboot();
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
#if FM1_CPU1_USED
    for (s = _c1_load, d = _c1_start; d < _c1_end; s++, d++)
        *d = *s;
#endif
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    if (boot_mode == BOOT_RECOVERY || recovery_key())
        recovery_main();
    fw_main();
    for (;;)
        ;
}
