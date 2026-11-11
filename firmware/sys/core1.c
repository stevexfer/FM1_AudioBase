/* SPDX-License-Identifier: GPL-3.0-only */
/* The second core for the app (hal/fm1_cpu1.h): one job at a time, from the audio interrupt. fw_core1_run hands it
 * fn(arg): 1 = taken, and fw_core1_wait before using what it works on; 0 = not running (one core): do the work here.
 * fw_core1_wait: 0 done, -1 it gave up (a unit whose core 1 misreads, or a job that did not end): one core from now
 * on, and the caller does that job's work itself. */

int fw_core1_run(void (*fn)(uint32_t), uint32_t arg)
{
#if FM1_CPU1_USED
    return fm1_cpu1_run(fn, arg);
#else
    (void)fn;
    (void)arg;
    return 0;
#endif
}

int fw_core1_wait(void)
{
#if FM1_CPU1_USED
    return fm1_cpu1_wait();
#else
    return 0;
#endif
}

uint32_t fw_ticks(void) { return fm1_ticks(); }   /* TIMER4, 24 a microsecond */
