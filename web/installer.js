// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 npi3pak
import { Updater } from "./fm1ota.js";
import { productOf, logicalImage, sha256hex, validateStockPackage } from "./fm1pkg.js";

const MSG = {
    stockok: "That is the official FM-1 V15. Ready to write.",
    stockbad: "That is not the official FM-1 V15 file.",
    stockdone: "Done: the FM-1 started with its official firmware.",
    nomidi: "This browser has no Web MIDI. Open the page in Chrome or Edge.",
    manifestfail: "The firmware list did not load: ",
    ready: "Connect the FM-1 and press Install.",
    fetching: "Loading the package…",
    badsha: "The package on the site is damaged (SHA-256 mismatch). Reload the page.",
    notours: "Not an FM1_AudioBase package: ",
    start: "Talking to the FM-1…",
    verify: "The FM-1 checks the package…",
    loader: "Switching to update mode…",
    write: "Writing… do not unplug",
    reboot: "Restarting…",
    done: "Done: ",
    resumed: "Write finished, the FM-1 restarts.",
    stopped: "The write stopped. Reconnect the FM-1 and press Install again.",
    error: "Error: ",
    denied: "MIDI (SysEx) access was not allowed. Allow MIDI in the site settings (address bar) and press again.",
    notfound: "FM-1 not found. Check the cable and close other MIDI apps.",
    model: "The connected device is not an FM-1: ",
    lost: "The USB connection was lost. Reconnect and press Install again.",
    noloader: "The FM-1 in update mode did not appear. Replug the cable and press Install again.",
    noreturn: "The FM-1 did not come back after the write. Power-cycle it.",
    mismatch: "Written, but the FM-1 reports another version: ",
    foreign: "The FM-1 is in another firmware's update mode. Finish that install with its own updater: ",
    badreq: "The FM-1 asked for data outside the package: ",
    leave: "Writing. Do not close the page.",
};

const LOADER_MARK = "FELUCCA-LOADER-1";
const OUR_PRODUCT = /^FM-1_9\d\d$/;
const $ = (id) => document.getElementById(id);

const state = {
    manifest: [],
    stock: null,
    writing: false,
};

const say = (key, arg = "", tone = "") => {
    $("status").textContent = (MSG[key] ?? key) + arg;
    $("status").className = tone;
};
const log = (line) => {
    $("log").textContent += `${new Date().toISOString().slice(11, 19)} ${line}\n`;
};
const setBar = (v) => {
    $("bar").value = Math.max(0, Math.min(100, v));
};

const hasLoader = (raw) => new TextDecoder("latin1").decode(raw).includes(LOADER_MARK);

const checkOurs = (raw) => {
    const product = productOf(raw);
    if (!OUR_PRODUCT.test(product) || !hasLoader(raw))
        throw Object.assign(new Error(product || "?"), { code: "notours", detail: product || "?" });
    return { product, image: logicalImage(raw) };
};

const selected = () => document.querySelector("input[name=pkg]:checked")?.value ?? null;

const refresh = () => {
    $("go").disabled = state.writing || !state.manifest.some((p) => p.id === selected());
    $("stockgo").disabled = state.writing || state.stock === null;
};

const pkgCard = (p, first) => {
    const label = document.createElement("label");
    label.className = "pkg";
    const input = Object.assign(document.createElement("input"), { type: "radio", name: "pkg", value: p.id, checked: first });
    const title = document.createElement("b");
    title.textContent = `${p.title} ${p.version}`;
    const tagEl = Object.assign(document.createElement("span"), { className: "tag", textContent: p.tag });
    const small = Object.assign(document.createElement("small"), { textContent: p.desc });
    if (p.tag)
        title.append(tagEl);
    label.append(input, title, small);
    return label;
};

const fetchPackage = async (p) => {
    say("fetching");
    const r = await fetch(`firmware/${p.file}`, { cache: "no-cache" });
    if (!r.ok)
        throw new Error(`${p.file}: HTTP ${r.status}`);
    const raw = new Uint8Array(await r.arrayBuffer());
    if (await sha256hex(raw) !== p.sha256)
        throw Object.assign(new Error("sha256"), { code: "badsha" });
    return checkOurs(raw);
};

const progress = (total) => (k, a) => {
    log(`${k} ${a ?? ""}`);
    const bar = {
        start: () => setBar(2),
        verify: () => setBar(2 + Math.min(8, a / 10)),
        loader: () => setBar(11),
        write: () => setBar(12 + Math.min(86, (a / total) * 86)),
        reboot: () => setBar(98),
        done: () => setBar(100),
    }[k];
    if (bar)
        bar();
    if (k !== "done" && MSG[k])
        say(k);
};

const wakeLock = async () => {
    try {
        return await navigator.wakeLock?.request("screen");
    } catch (_) {
        return null;
    }
};

const explain = (e, isStock) => {
    if (isStock && e?.code === "mismatch") {
        setBar(100);
        say("stockdone", "", "ok");
        return;
    }
    if (e?.code && MSG[e.code]) {
        say(e.code, e.detail || "", "bad");
        return;
    }
    say("error", e?.message || String(e), "bad");
};

const run = async (pkg, isStock) => {
    const midi = await navigator.requestMIDIAccess({ sysex: true }).catch(() => null);
    if (!midi) {
        say("denied", "", "bad");
        return;
    }
    state.writing = true;
    refresh();
    setBar(0);
    const lock = await wakeLock();
    const up = new Updater(midi, log);
    const step = progress(Math.ceil(pkg.image.length / 512));
    try {
        const ota = await up.find((id) => /^ota-/i.test(id.model));
        if (ota) {
            ota.link.close();
            log(`resume: ${ota.id.text}`);
            const finished = await up.resume(pkg.image, step, isStock ? { product: pkg.product } : {});
            setBar(finished ? 100 : $("bar").value);
            say(finished ? (isStock ? "stockdone" : "resumed") : "stopped", "", finished ? "ok" : "bad");
        }
        if (!ota) {
            const id = await up.install(pkg.image, pkg.product, step);
            say(isStock ? "stockdone" : "done", isStock ? "" : id, "ok");
        }
    } catch (e) {
        explain(e, isStock);
        log(String(e?.stack || e));
    }
    await lock?.release?.().catch(() => null);
    state.writing = false;
    refresh();
};

const onStockFile = async (e) => {
    state.stock = null;
    const f = e.target.files?.[0];
    refresh();
    if (!f)
        return;
    try {
        state.stock = await validateStockPackage(new Uint8Array(await f.arrayBuffer()));
        say("stockok", "", "ok");
    } catch (_) {
        say("stockbad", "", "bad");
    }
    refresh();
};

const onInstall = async () => {
    try {
        const pkg = await fetchPackage(state.manifest.find((p) => p.id === selected()));
        log(`package ${pkg.product}, ${pkg.image.length} B`);
        await run(pkg, false);
    } catch (e) {
        explain(e, false);
        state.writing = false;
        refresh();
    }
};

const loadManifest = async () => {
    try {
        const r = await fetch("firmware/manifest.json", { cache: "no-cache" });
        if (!r.ok)
            throw new Error(`manifest.json: HTTP ${r.status}`);
        const manifest = await r.json();
        state.manifest = manifest.packages;
        if (manifest.source)
            $("source").href = manifest.source;
        $("pkgs").replaceChildren(...state.manifest.map((p, i) => pkgCard(p, i === 0)));
        return true;
    } catch (e) {
        say("manifestfail", e.message, "bad");
        return false;
    }
};

const main = async () => {
    $("stockfile").addEventListener("change", onStockFile);
    $("pkgs").addEventListener("change", refresh);
    $("go").addEventListener("click", onInstall);
    $("stockgo").addEventListener("click", () => run(state.stock, true));
    window.addEventListener("beforeunload", (e) => {
        if (!state.writing)
            return;
        e.preventDefault();
        e.returnValue = MSG.leave;
    });
    if (!navigator.requestMIDIAccess) {
        say("nomidi", "", "bad");
        return;
    }
    const loaded = await loadManifest();
    refresh();
    if (loaded)
        say("ready");
};

main();
