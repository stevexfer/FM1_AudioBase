// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 npi3pak
// The installer's protocol against a fake FM-1 on a fake Web MIDI: the running firmware, its update loader and the
// restart, every byte served compared with the image tools/fm1_install.py derives from the same package.
//   node web/test_web.mjs [build/audiobase.fwsc]
import { readFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { webcrypto } from "node:crypto";
import { pack7, unpack7, Updater, FM1_PORT } from "./fm1ota.js";
import { productOf, logicalImage, validateStockPackage } from "./fm1pkg.js";

const realTimeout = globalThis.setTimeout;
globalThis.setTimeout = (f, ms = 0) => realTimeout(f, ms / 50);
globalThis.crypto ??= webcrypto;

const pkgPath = process.argv[2] ?? "build/audiobase.fwsc";
const raw = new Uint8Array(readFileSync(pkgPath));
const results = [];
const ok = (cond, what) => {
    results.push({ cond, what });
    console.log(`${cond ? "ok  " : "FAIL"} ${what}`);
};

const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
const pyImageSha = execFileSync("python3", ["-c", [
    "import sys, hashlib; sys.path.insert(0, 'tools'); import fm1_install as f",
    `raw = open(${JSON.stringify(pkgPath)}, 'rb').read()`,
    "print(hashlib.sha256(f.logical_image(raw)).hexdigest(), f.product_of(raw))",
].join("\n")]).toString().trim().split(" ");

const sysex = (body) => [0xF0, ...pack7(body), 0xF7];
const identity = (text) => {
    const txt = [...new TextEncoder().encode(text)];
    return sysex([0x00, 0x59, 0x11, 0, 0, 0, ...txt, ...new Array(27 - txt.length).fill(0), 0]);
};
const request = (addr, len) => {
    const u = [0x00, 0x59, 0x30, 8, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, addr >>> 24,
        len & 0xFF, (len >> 8) & 0xFF, 0];
    const sum = u.slice(6, 14).reduce((s, b) => s + b, 0);
    return sysex([...u, ~sum & 0xFF]);
};
const parseResponse = (pkt) => {
    const u = unpack7(pkt.slice(1, -1));
    const addr = (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0;
    const len = u[11] | (u[12] << 8);
    const sum = u.slice(6, u.length - 1).reduce((s, b) => s + b, 0);
    return { addr, len, data: u.slice(14, 14 + len), chkOk: (~sum & 0xFF) === u[u.length - 1] };
};
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].join();
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7].join();

class Port {
    constructor(name, onSend) {
        this.name = name;
        this.state = "connected";
        this.onmidimessage = null;
        this.onSend = onSend;
    }
    async open() { return this; }
    send(bytes) {
        if (this.state !== "connected")
            throw new Error("disconnected");
        this.onSend(Array.from(bytes));
    }
    emit(bytes) {
        if (this.state === "connected" && this.onmidimessage)
            this.onmidimessage({ data: Uint8Array.from(bytes) });
    }
}

// the device: "app" answers the handshake and, on UPGRADE, reads a few parts and asks 0xE0000000; then it leaves the
// bus and comes back as the loader, which reads the whole image (twice when `passes` is 2, as the CRC retry does),
// asks 0xF0000000 and comes back as the new firmware
class FakeFM1 {
    constructor(access, image, { appId, loaderId = "ota-FM-1_900", newId, passes = 1, dropAt = -1 }) {
        Object.assign(this, { access, image, appId, loaderId, newId, passes, dropAt });
        this.served = new Uint8Array(image.length);
        this.mismatch = 0;
        this.badChk = 0;
        this.reads = 0;
        this.pending = null;
        this.attach(appId, "app");
    }
    attach(id, role) {
        const name = role === "loader" ? "FMini-1 Update" : "FM-1 Midi";   /* the names macOS gave them (2026-11-11) */
        const out = new Port(name, (b) => this.rx(b));
        const inp = new Port(name, () => null);
        this.role = role;
        this.id = id;
        this.ports = { inp, out };
        this.access.inputs = new Map([[name, inp]]);
        this.access.outputs = new Map([[name, out]]);
    }
    detach(then, ms) {
        this.ports.inp.state = this.ports.out.state = "disconnected";
        this.access.inputs = new Map();
        this.access.outputs = new Map();
        realTimeout(then, ms);
    }
    tx(bytes) { realTimeout(() => this.ports.inp.emit(bytes), 0); }
    rx(bytes) {
        const key = bytes.join();
        if (key === HS) {
            this.tx(identity(this.id));
            return;
        }
        if (key === UPGRADE) {
            this.plan = this.role === "app" ? this.checkPlan() : this.writePlan();
            this.next();
            return;
        }
        if (bytes[0] === 0xF0)
            this.answer(parseResponse(bytes));
    }
    checkPlan() { return [[0, 512], [512, 512], [this.image.length - 512, 512], [0xE0000000, 8]]; }
    writePlan() {
        const one = Array.from({ length: Math.ceil(this.image.length / 512) },
            (_, i) => [i * 512, Math.min(512, this.image.length - i * 512)]);
        return [...Array.from({ length: this.passes }, () => one).flat(), [0xF0000000, 8]];
    }
    next() {
        const [addr, len] = this.plan.shift();
        this.pending = addr;
        this.tx(request(addr, len));
    }
    answer(r) {
        if (!r.chkOk)
            this.badChk++;
        if (r.addr !== this.pending)
            return;
        if (r.addr === 0xE0000000) {
            this.detach(() => this.attach(this.loaderId, "loader"), 300);
            return;
        }
        if (r.addr === 0xF0000000) {
            this.detach(() => this.attach(this.newId, "app"), 300);
            return;
        }
        this.reads++;
        if (this.reads === this.dropAt) {
            this.detach(() => this.attach(this.loaderId, "loader"), 300);
            return;
        }
        const want = this.image.subarray(r.addr, r.addr + r.len);
        this.mismatch += want.some((b, i) => b !== r.data[i]) || r.data.length !== r.len ? 1 : 0;
        this.served.set(r.data, r.addr);
        this.next();
    }
}

const quiet = () => null;
const image = logicalImage(raw);
const product = productOf(raw);

ok(sha(image) === pyImageSha[0], `logicalImage == fm1_install.logical_image (${pyImageSha[0].slice(0, 12)})`);
ok(product === pyImageSha[1], `productOf == fm1_install.product_of (${product})`);
const bytes = Uint8Array.from({ length: 300 }, (_, i) => (i * 37 + 11) & 0xFF);
ok(unpack7(pack7(bytes)).slice(0, 300).every((b, i) => b === bytes[i]), "pack7 / unpack7 round trip");

{
    const access = {};
    const dev = new FakeFM1(access, image, { appId: "FM-1_900", newId: product, passes: 2 });
    const steps = [];
    const id = await new Updater(access, quiet).install(image, product, (k) => steps.push(k));
    ok(id === product, `install: the FM-1 comes back as ${id}`);
    ok(dev.mismatch === 0 && dev.badChk === 0, `install: ${dev.reads} reads, every byte and checksum as the package`);
    ok(sha(dev.served) === sha(image), "install: the loader received the whole image");
    ok(["start", "verify", "loader", "write", "reboot", "done"].every((k) => steps.includes(k)), "install: every step reported");
}

{
    const access = {};
    const dev = new FakeFM1(access, image, { appId: "FM-1_015", newId: product });
    const id = await new Updater(access, quiet).install(image, product, quiet);
    ok(id === product && dev.mismatch === 0, "install over the stock identity FM-1_015");
}

{
    const access = {};
    const dev = new FakeFM1(access, image, { appId: "FM-1_900", newId: product, dropAt: 300 });
    const up = new Updater(access, quiet);
    const first = await up.install(image, product, quiet).then(() => "done", (e) => e.code);
    ok(first === "lost" || first === "stopped", `a drop mid-write fails as ${first}`);
    await new Promise((r) => realTimeout(r, 500));
    const resumed = await up.resume(image, quiet);
    ok(resumed === true && dev.mismatch === 0, "resume finishes the write from update mode");
}

{
    const access = {};
    new FakeFM1(access, image, { appId: "ota-FM-1_015", newId: product });
    const code = await new Updater(access, quiet).resume(image, quiet).then(() => "resumed", (e) => e.code);
    ok(code === "foreign", "another firmware's update mode (ota-FM-1_015) is not written");
}

ok(["FM-1 Midi", "FMini-1 Update", "Felucca Update", "FM-1"].every((n) => FM1_PORT.test(n)) &&
   !["IAC Driver Bus 1", "Network Session 1", "Launchkey Mini MK3"].some((n) => FM1_PORT.test(n)),
   "the FM-1's ports are probed under the names macOS gives them, other gear never");

{
    const access = {};
    new FakeFM1(access, image, { appId: "Other_123", newId: product });
    const code = await new Updater(access, quiet).install(image, product, quiet).then(() => "done", (e) => e.code);
    ok(code === "model", "a device that is not an FM-1 is refused");
}

{
    const code = await validateStockPackage(raw).then(() => "accepted", () => "refused");
    ok(code === "refused", "our package is not taken for the official V15");
}

const failed = results.filter((r) => !r.cond).length;
console.log(failed ? `${failed} of ${results.length} FAILED` : `all ${results.length} passed`);
process.exit(failed ? 1 : 0);
