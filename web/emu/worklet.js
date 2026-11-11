// The emulator in the audio thread: each 128-frame quantum runs the machine until its I2S
// DMA has produced 128 frames, so the audio device paces the emulation; and also, for an even
// amount of work per quantum, its share of emulated time while the queue holds less than one
// DMA half buffer beyond them (fm1_ahead: half the firmwares hand out 256 or 512 frames at
// once). The page gets the LCD and the LEDs about 30 times a second; keys, knobs and the pot
// come back as messages.
// The WebAssembly JIT's host side, as web/jit.js (a copy: an AudioWorklet does not import
// modules everywhere): compile a hot block's module, install its `run` in the function table.
function jitImports(exports, stats) {
  return {
    fm1_jit_compile(ptr, len) {
      const start = now();
      try {
        const x = exports();
        const bytes = new Uint8Array(x.memory.buffer, ptr, len).slice();
        const table = x.__indirect_function_table;
        const run = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env: { memory: x.memory, table } }).exports.run;
        const slot = table.grow(1);
        table.set(slot, run);
        stats.regions++;
        stats.bytes += len;
        stats.tableSlots = table.length;
        return slot;
      } catch (e) {
        stats.failed++;
        stats.error = String(e);
        return 0;
      } finally {
        const elapsed = now() - start;
        stats.compileMs += elapsed;
        stats.compileMaxMs = Math.max(stats.compileMaxMs, elapsed);
      }
    },
  };
}

// How much a quantum may emulate, in the emulator's run budget (cpu0's instructions, at most
// one per cycle): while the firmware boots, 20 ms of emulated time, so a boot is quick (there is
// no audio to keep yet); once audio plays, 1.5 quanta, so a firmware that stalls or falls silent
// does not hold the audio thread for 20 ms each quantum. The pace (fm1_ahead) needs one quantum
// when it may run a whole DMA half buffer ahead; fm1_render raises the cap by the rest of the half
// buffer when ahead is less (?ahead=256 with 512-frame halves). Without the pace (?ahead=0) a
// quantum may have to run a whole half buffer's time, so it keeps 20 ms.
const AHEAD_FRAMES = 1024;
function budgets(mhz, frames) {
  return { boot: mhz * 1e6 * 0.02, play: mhz * 1e6 * 1.5 * frames / sampleRate };
}

// The USB audio in: a computer playing a kalimba into the port (main.js decodes the samples, at
// this context's rate: web/kalimba, tools/kalimba.py). A note every 0.5-1.5x the mean interval,
// which the density (0..1) sets from 4 s down to 0.25 s: a random key, at a random velocity, in a
// random place in the stereo field. The velocity picks the zone (the sample's layer): most often
// another than the last note's, always when the key repeats, so no two notes in a row sound alike;
// within the zone it varies the level by +-2 dB (the layers themselves get louder).
// fm1_usb_push keeps the stream USB_AHEAD ahead of the machine's playback.
const USB_AHEAD = 0.03;
const ZONE_CHANGE = 0.7;
const pick = (a) => a[Math.floor(Math.random() * a.length)];
class Kalimba {
  constructor() {
    this.byKey = new Map();             // key -> its samples, one a zone
    this.keys = [];
    this.last = null;
    this.on = true;
    this.density = 0.35;
    this.voices = [];
    this.wait = 0;
  }
  set samples(list) {
    this.byKey = new Map();
    for (const s of list) {
      s.step = Math.pow(2, s.tune / 1200);
      if (!this.byKey.has(s.key)) this.byKey.set(s.key, []);
      this.byKey.get(s.key).push(s);
    }
    this.keys = [...this.byKey.keys()];
    this.last = null;
  }
  note() {
    let key = pick(this.keys);
    // (a key with one zone does not repeat: another key instead)
    if (key === this.last?.key && this.byKey.get(key).length < 2) key = pick(this.keys.filter((k) => k !== key));
    const zones = this.byKey.get(key);
    const other = zones.filter((s) => s.zone !== this.last?.zone);
    const s = other.length && (key === this.last?.key || Math.random() < ZONE_CHANGE) ? pick(other) : pick(zones);
    this.last = s;
    const pan = 0.2 + 0.6 * Math.random(), gain = 0.55 * Math.pow(10, (Math.random() * 4 - 2) / 20);
    return { s: s.data, step: s.step, at: 0, l: gain * Math.cos(pan * Math.PI / 2), r: gain * Math.sin(pan * Math.PI / 2) };
  }
  // `frames` stereo frames at `rate` into `out` (interleaved i16)
  render(out, frames, rate) {
    const step = sampleRate / rate;
    for (let i = 0; i < frames; i++) {
      if (--this.wait <= 0) {
        const mean = 4 * Math.pow(16, -this.density) * rate;
        this.wait = mean * (0.5 + Math.random());
        if (this.on && this.keys.length && this.voices.length < 8) this.voices.push(this.note());
      }
      let l = 0, r = 0;
      for (const v of this.voices) {
        const k = v.at | 0, f = v.at - k, s = v.s;
        const x = s[k] + ((s[k + 1] ?? 0) - s[k]) * f;
        l += x * v.l;
        r += x * v.r;
        v.at += step * v.step;
      }
      out[2 * i] = Math.max(-32767, Math.min(32767, l * 32767));
      out[2 * i + 1] = Math.max(-32767, Math.min(32767, r * 32767));
    }
    this.voices = this.voices.filter((v) => v.at < v.s.length - 1);
  }
  get busy() { return this.on || this.voices.length > 0; }
}

// the wall clock, for how long each quantum's emulation takes (where an AudioWorklet has no
// performance.now(), Date.now(): whole milliseconds, which still average out over a second)
const now = globalThis.performance?.now ? () => performance.now() : () => Date.now();

class Fm1 extends AudioWorkletProcessor {
  constructor() {
    super();
    this.x = null;
    this.calls = 0;
    this.bootToken = 0;
    this.snapshots = [];
    this.kalimba = new Kalimba();
    this.port.onmessageerror = () => this.port.postMessage({ type: "error", message: "a message could not be read" });
    this.port.onmessage = async (e) => {
      const m = e.data;
      if (m.type === "boot") {
        const token = ++this.bootToken;
        // Keep the previous (paused) instance until the replacement boots successfully.
        try {
          await this.boot(m, token);
        } catch (err) {
          if (token !== this.bootToken) return;
          this.paused = false;
          this.port.postMessage({ type: "error", seq: m.seq, message: `boot: ${err}` });
        }
      } else if (m.type === "kalimba-samples") {
        this.kalimba.samples = m.samples;
      } else if (m.type === "kalimba") {
        this.kalimba.on = m.on;
        this.kalimba.density = m.density;
      } else if (m.type === "cancel-boot") {
        ++this.bootToken;
        this.x = null;
        this.snapshots = [];
      } else if (m.type === "snapshot" && !this.x) {
        this.port.postMessage({ type: "snapshot", seq: m.seq, request: m.request, unavailable: true });
      } else if (this.x) {
        if (m.type === "snapshot") {
          if (m.seq !== this.seq) return;
          this.snapshots.push({ ...m, started: now() });
          this.flushSnapshots();
        } else if (m.type === "cancel-snapshot") this.snapshots = this.snapshots.filter(r => r.request !== m.request);
        else if (m.type === "resume") this.paused = false;
        else if (m.type === "key") this.x.fm1_key(m.id, m.down ? 1 : 0);
        else if (m.type === "enc") this.x.fm1_enc(m.enc, m.clicks);
        else if (m.type === "pot") this.x.fm1_pot(m.value);
        else if (m.type === "midi") for (const b of m.bytes) this.x.fm1_midi(b);
        else if (m.type === "analog") this.x.fm1_analog(m.idx, m.value);   // SMK-37: pads, faders, wheels
        else if (m.type === "vel") this.x.fm1_velocity(m.us);             // SMK-37: the next keys' velocity
      }
    };
  }

  async boot(m, token) {
    // the module arrives as bytes: a compiled WebAssembly.Module does not cross into the
    // AudioWorklet's scope (Chrome delivers it as a messageerror). Compiled once, here: on a
    // phone compiling it again on every firmware change takes a noticeable while
    if (!this.module) this.module = await WebAssembly.compile(m.wasm);
    let x;
    this.jit = { regions: 0, failed: 0, error: "", bytes: 0, tableSlots: 0, compileMs: 0, compileMaxMs: 0 };
    x = (await WebAssembly.instantiate(this.module, { env: jitImports(() => x, this.jit) })).exports;
    if (token !== this.bootToken) return;
    // hot code compiled to WebAssembly, a few blocks per quantum at most (?jit=0: interpreter only)
    x.fm1_jit(m.jit === false ? 0 : 1, 16);
    const ahead = x.fm1_ahead ? m.ahead ?? AHEAD_FRAMES : 0;
    x.fm1_ahead?.(ahead);               // (?ahead=0: each quantum runs for its own frames only)
    // the whole update package (.fwsc): the machine unpacks it, gets the flash chip and the
    // boot parameters from it (what M-VAVE's own firmware needs; the others boot the same)
    const copy = (buf) => {
      const p = x.fm1_alloc(buf.byteLength);
      new Uint8Array(x.memory.buffer, p, buf.byteLength).set(new Uint8Array(buf));
      return p;
    };
    // a firmware that clocks its codec at another rate than the stock 44.1 kHz (this context runs at it)
    x.fm1_audio_rate?.(sampleRate);
    x.fm1_audio_frame_ns?.(m.frameNs || 0);
    const pkg = copy(m.package);
    if (x.fm1_boot_fwsc(pkg, m.package.byteLength, 0, 0, m.mhz) !== 0) {
      throw new Error("not an FM-1 update package (or a DX7 bank it cannot use)");
    }
    if (m.sectors?.byteLength && x.fm1_flash_restore(copy(m.sectors), m.sectors.byteLength) !== 0) {
      throw new Error("invalid saved flash sectors");
    }
    if (m.syx && x.fm1_load_syx(copy(m.syx), m.syx.byteLength) === 1) {
      throw new Error("this firmware cannot load that DX7 bank");
    }
    // a computer on the USB port, its playback stream open from the start (silence until fed)
    this.usb = !!(m.usb && x.fm1_usb_plug);
    if (this.usb) {
      x.fm1_usb_plug();
      x.fm1_usb_push(0);
    }
    this.kalimba.voices = [];
    this.x = x;                         // (a reboot replaces the whole instance)
    this.mhz = m.mhz;
    this.ahead = ahead;
    this.budget = budgets(m.mhz, 128).boot;
    this.playing = false;               // the firmware's audio has started (the budget is capped)
    this.lastTiming = null;
    this.views = null;
    this.lcdRevision = null;
    this.reportFrames = 0;
    this.reportCalls = 0;
    this.short = 0;
    this.calls = 0;
    this.busy = 0;                      // ms spent emulating since the last frame message
    this.worst = 0;                     // the longest quantum among them (once audio plays)
    this.timedMs = 0;                   // the emulated ms when the timed interval began
    this.timedIdle = 0;                 // and the idle ms skipped by then
    this.board = x.fm1_board();         // 0 the FM-1, 1 the SMK-37 (the package says which)
    this.seq = m.seq;
    this.paused = false;
    this.snapshots = [];
    this.flashRevision = x.fm1_flash_revision();
    this.observedRevision = this.flashRevision;
    this.flashChangedAt = this.lastSnapshot = now();
    const base = new Uint8Array(x.memory.buffer, x.fm1_flash_base(), x.fm1_flash_len()).slice();
    const flash = new Uint8Array(x.memory.buffer, x.fm1_flash(), x.fm1_flash_len()).slice();
    this.port.postMessage({ type: "booted", seq: m.seq, board: this.board, revision: this.flashRevision, base, flash }, [base.buffer, flash.buffer]);
  }

  observeFlash() {
    const revision = this.x.fm1_flash_revision();
    if (revision !== this.observedRevision) {
      this.observedRevision = revision;
      this.flashChangedAt = now();
      this.port.postMessage({ type: "flash-changed", seq: this.seq, revision });
    }
    return revision;
  }

  flushSnapshots() {
    const x = this.x;
    if (!x) return;
    this.observeFlash();
    const quiet = now() - this.flashChangedAt >= 250 || x.fm1_halted();
    const busy = x.fm1_flash_busy();
    for (const request of this.snapshots.splice(0)) {
      if (busy || !quiet) {
        if (request.request !== undefined && now() - request.started >= 1500) {
          // A constantly writing firmware must not prevent switching away. Keep its
          // last stable save instead of persisting a possibly half-written sector.
          if (request.pause) this.paused = true;
          this.port.postMessage({ type: "snapshot", seq: this.seq, request: request.request,
            unavailable: true, error: request.pause ? null : "flash did not become quiet; try saving again" });
        } else this.snapshots.push(request);
        continue;
      }
      const flash = new Uint8Array(x.memory.buffer, x.fm1_flash(), x.fm1_flash_len()).slice();
      this.flashRevision = x.fm1_flash_revision();
      this.observedRevision = this.flashRevision;
      this.lastSnapshot = now();
      if (request.pause) this.paused = true;
      this.port.postMessage({ type: "snapshot", seq: this.seq, request: request.request,
        revision: this.flashRevision, flash }, [flash.buffer]);
    }
  }

  // top the computer's playback up to USB_AHEAD at its stream's rate
  feedUsb(x) {
    const rate = x.fm1_usb_play_rate();
    if (!rate || !this.kalimba.busy) return;
    const frames = Math.min(1024, Math.round(rate * USB_AHEAD) - x.fm1_usb_play_queued());
    if (frames <= 0) return;
    const buf = new Int16Array(x.memory.buffer, x.fm1_usb_buf(), 2048);
    this.kalimba.render(buf, frames, rate);
    x.fm1_usb_push(frames);
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const x = this.x;
    if (!x || this.paused) return true;
    const n = out[0].length;
    const playing = this.playing;       // (the worst quantum: of those once audio played)
    const t0 = now();
    let have;
    try {
      if (this.usb) this.feedUsb(x);
      have = x.fm1_render(n, this.budget);
    } catch (err) {                       // a trap in the emulator: stop and say so
      this.x = null;
      for (const request of this.snapshots.splice(0)) {
        this.port.postMessage({ type: "snapshot", seq: this.seq, request: request.request, unavailable: true });
      }
      this.port.postMessage({ type: "error", seq: this.seq, fatal: true, message: `render: ${err}` });
      return true;
    }
    const buffer = x.memory.buffer, lp = x.fm1_out_l(), rp = x.fm1_out_r();
    if (!this.views || this.views.l.buffer !== buffer || this.views.l.length !== n ||
        this.views.l.byteOffset !== lp || this.views.r.byteOffset !== rp) {
      this.views = { l: new Float32Array(buffer, lp, n), r: new Float32Array(buffer, rp, n) };
    }
    const { l, r } = this.views;
    out[0].set(l);
    if (out[1]) out[1].set(r);
    if (have > 0 && !this.playing) {
      this.playing = true;
      if (this.ahead > 0) this.budget = budgets(this.mhz, n).play;
    }
    this.short = (this.short || 0) + (have < n ? 1 : 0);
    this.reportFrames += n;
    this.reportCalls++;
    if (this.reportFrames >= sampleRate / 30) {
      const flashRevision = this.observeFlash();
      if (flashRevision !== this.flashRevision && now() - this.lastSnapshot >= 1000 &&
          now() - this.flashChangedAt >= 250 && !this.snapshots.length) {
        this.snapshots.push({});
      }
      this.flushSnapshots();
      const revision = x.fm1_lcd_revision();
      const lcd = revision !== this.lcdRevision ? new Uint16Array(x.memory.buffer, x.fm1_lcd(), 240 * 240).slice() : null;
      this.lcdRevision = revision;
      const leds = new Float32Array(x.memory.buffer, x.fm1_leds(), 41).slice();
      const pads = this.board ? new Uint32Array(x.memory.buffer, x.fm1_pads(), 16).slice() : null;
      const transfer = [leds.buffer];
      if (lcd) transfer.push(lcd.buffer);
      if (pads) transfer.push(pads.buffer);
      // Report the preceding interval: its final postMessage is included in its timing.
      const ms = x.fm1_ms(), idle = x.fm1_idle_ms?.() ?? 0;
      this.port.postMessage({ type: "frame", seq: this.seq, lcd, leds, ms,
        short: this.short, halted: x.fm1_halted(), jit: this.jit, pads, playing: this.playing,
        busy: this.lastTiming?.busy ?? 0, worst: this.lastTiming?.worst ?? 0,
        emulated: this.lastTiming?.emulated ?? 0, idle: this.lastTiming?.idle ?? 0,
        quanta: this.lastTiming?.quanta ?? 0, frames: this.lastTiming?.frames ?? n }, transfer);
      const took = now() - t0;
      // busy: ms of wall clock the interval's quanta took; emulated: ms of emulated time they ran,
      // idle: of which idle skipping jumped over (cost nothing)
      this.lastTiming = { busy: this.busy + took, worst: playing ? Math.max(this.worst, took) : this.worst,
        emulated: ms - this.timedMs, idle: idle - this.timedIdle,
        quanta: this.reportCalls, frames: this.reportFrames / this.reportCalls };
      this.timedMs = ms;
      this.timedIdle = idle;
      this.busy = 0;
      this.worst = 0;
      this.reportFrames = 0;
      this.reportCalls = 0;
    } else {
      const took = now() - t0;
      this.busy += took;
      if (playing) this.worst = Math.max(this.worst, took);
    }
    return true;
  }
}
registerProcessor("fm1", Fm1);
