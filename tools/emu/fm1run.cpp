// SPDX-License-Identifier: GPL-3.0-only
// Headless FM-1 runner for FM1_AudioBase on FM1Ception's engine (fm1-emu).
//
//   fm1run FIRMWARE.fwsc SCRIPT [OUTDIR]
//
// Script lines (time advances only with `wait`):
//   wait MS                     run the emulator for MS milliseconds of audio
//   btn NAME down|up|tap        FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+
//   key N down|up|tap           key 0..26 (F3..G5)
//   enc NAME CLICKS             SELECT ALGO K1 K2 K3 K4 PRESET
//   master V                    MASTER pot 0..1
//   reboot                      boot the firmware again, keeping its flash
//   upgrade FILE.fwsc           boot another firmware package, keeping the flash (a firmware update)
//   midi B0 [B1 [B2]]           a MIDI message (hex or decimal)
//   clock BPM MS                MIDI clock (F8) at BPM for MS milliseconds (as wait), at the firmware's 48 kHz
//   screen FILE.ppm             the LCD
//   wav FILE.wav                start writing audio to FILE (until the end)
//   status                      booted / speed / peak since last status
#include "fm1ception_engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::vector<uint8_t> readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    return std::vector<uint8_t> ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
}

struct Wav
{
    FILE* f = nullptr;
    uint32_t frames = 0;

    void open (const std::string& path)
    {
        f = std::fopen (path.c_str(), "wb");
        uint8_t hdr[44] = {};
        std::fwrite (hdr, 1, 44, f);
    }

    void write (const float* lr)
    {
        if (f == nullptr)
            return;
        int16_t s[2];
        for (int c = 0; c < 2; ++c)
            s[c] = (int16_t) std::lround (std::fmax (-1.0f, std::fmin (1.0f, lr[c])) * 32767.0f);
        std::fwrite (s, 2, 2, f);
        ++frames;
    }

    void close()
    {
        if (f == nullptr)
            return;
        auto put32 = [] (uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t) (v >> (8 * i)); };
        auto put16 = [] (uint8_t* p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); };
        uint8_t h[44];
        std::memcpy (h, "RIFF", 4); put32 (h + 4, 36 + frames * 4);
        std::memcpy (h + 8, "WAVEfmt ", 8); put32 (h + 16, 16); put16 (h + 20, 1); put16 (h + 22, 2);
        put32 (h + 24, FM1E_RATE); put32 (h + 28, FM1E_RATE * 4); put16 (h + 32, 4); put16 (h + 34, 16);
        std::memcpy (h + 36, "data", 4); put32 (h + 40, frames * 4);
        std::fseek (f, 0, SEEK_SET);
        std::fwrite (h, 1, 44, f);
        std::fclose (f);
        f = nullptr;
    }
};

const std::map<std::string, int> kButtons = {
    { "OCT-", 0 }, { "OCT+", 1 }, { "FX", 2 }, { "SCL", 3 }, { "ENV", 4 }, { "LFO", 5 }, { "EDIT", 6 },
    { "GLO", 7 }, { "HOME", 8 }, { "SAVE", 9 }, { "ARP", 10 }, { "SEQ", 11 }, { "PLAY", 12 }, { "REC", 13 },
};

const std::map<std::string, int> kEncoders = {
    { "SELECT", 0 }, { "ALGO", 1 }, { "K1", 2 }, { "K2", 3 }, { "K3", 4 }, { "K4", 5 }, { "PRESET", 6 },
};
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: fm1run FIRMWARE.fwsc SCRIPT [OUTDIR]\n");
        return 2;
    }
    const std::string outDir = argc > 3 ? std::string (argv[3]) + "/" : std::string();
    auto pkg = readFile (argv[1]);
    if (pkg.empty())
    {
        std::fprintf (stderr, "fm1run: cannot read %s\n", argv[1]);
        return 2;
    }

    Fm1e* e = fm1e_create();
    fm1e_set_lead (e, 2048);
    if (fm1e_boot (e, "FM1_AudioBase", pkg.data(), pkg.size(), nullptr, 0) != 0)
    {
        std::fprintf (stderr, "fm1run: boot failed\n");
        return 1;
    }

    Wav wav;
    uint64_t frame = 0;
    float peak = 0.0f;
    double sumSq = 0.0;
    uint64_t sumN = 0;

    auto runFrames = [&] (uint64_t n)
    {
        float lr[2];
        for (uint64_t i = 0; i < n;)
        {
            if (! fm1e_pop (e, lr))
            {
                std::this_thread::sleep_for (std::chrono::microseconds (200));
                continue;
            }
            peak = std::fmax (peak, std::fmax (std::fabs (lr[0]), std::fabs (lr[1])));
            sumSq += (double) lr[0] * lr[0];
            ++sumN;
            wav.write (lr);
            ++frame;
            ++i;
        }
    };
    auto run = [&] (double ms) { runFrames ((uint64_t) (ms * FM1E_RATE / 1000.0)); };

    std::ifstream script (argv[2]);
    std::string line;
    int lineNo = 0;
    while (std::getline (script, line))
    {
        ++lineNo;
        std::istringstream in (line);
        std::string cmd;
        if (! (in >> cmd) || cmd[0] == '#')
            continue;

        if (cmd == "wait")
        {
            double ms = 0;
            in >> ms;
            run (ms);
        }
        else if (cmd == "btn" || cmd == "key")
        {
            std::string name, how;
            in >> name >> how;
            int id = -1;
            if (cmd == "btn")
            {
                const auto it = kButtons.find (name);
                id = it == kButtons.end() ? -1 : it->second;
            }
            else
            {
                id = 14 + std::atoi (name.c_str());
            }
            if (id < 0 || id > 40)
            {
                std::fprintf (stderr, "line %d: unknown %s %s\n", lineNo, cmd.c_str(), name.c_str());
                return 2;
            }
            if (how == "down" || how == "tap")
                fm1e_key (e, (uint8_t) id, true);
            if (how == "tap")
                run (120);
            if (how == "up" || how == "tap")
                fm1e_key (e, (uint8_t) id, false);
        }
        else if (cmd == "enc")
        {
            std::string name;
            int clicks = 0;
            in >> name >> clicks;
            const auto it = kEncoders.find (name);
            if (it == kEncoders.end())
            {
                std::fprintf (stderr, "line %d: unknown encoder %s\n", lineNo, name.c_str());
                return 2;
            }
            fm1e_enc (e, (uint8_t) it->second, clicks);
        }
        else if (cmd == "master")
        {
            float v = 0;
            in >> v;
            fm1e_set_master (e, v);
        }
        else if (cmd == "midi")
        {
            std::string bytes[3];
            int len = 0;
            while (len < 3 && in >> bytes[len])
                ++len;
            auto num = [] (const std::string& s) { return (uint8_t) std::strtol (s.c_str(), nullptr, 0); };
            fm1e_push_midi (e, frame + 2048, num (bytes[0]), len > 1 ? num (bytes[1]) : 0, len > 2 ? num (bytes[2]) : 0,
                            (uint8_t) len);
        }
        else if (cmd == "clock")
        {
            // a frame is one firmware sample (48 kHz) whatever FM1E_RATE says
            double bpm = 120, ms = 0;
            in >> bpm >> ms;
            const double spacing = 48000.0 * 60.0 / (bpm * 24.0);
            const uint64_t end = frame + (uint64_t) (ms * FM1E_RATE / 1000.0);
            double next = (double) frame;
            while (frame < end)
            {
                fm1e_push_midi (e, (uint64_t) next + 2048, 0xF8, 0, 0, 1);
                next += spacing;
                runFrames (std::min<uint64_t> ((uint64_t) next, end) - frame);
            }
        }
        else if (cmd == "screen")
        {
            std::string file;
            in >> file;
            std::vector<uint16_t> lcd (FM1E_LCD_W * FM1E_LCD_H);
            fm1e_view (e, lcd.data(), nullptr, nullptr);
            FILE* f = std::fopen ((outDir + file).c_str(), "wb");
            std::fprintf (f, "P6\n%d %d\n255\n", FM1E_LCD_W, FM1E_LCD_H);
            for (uint16_t px : lcd)
            {
                const uint8_t rgb[3] = { (uint8_t) ((px >> 11) << 3), (uint8_t) (((px >> 5) & 63) << 2), (uint8_t) ((px & 31) << 3) };
                std::fwrite (rgb, 1, 3, f);
            }
            std::fclose (f);
        }
        else if (cmd == "reboot" || cmd == "upgrade")
        {
            if (cmd == "upgrade")
            {
                std::string file;
                in >> file;
                pkg = readFile (file.c_str());
                if (pkg.empty())
                {
                    std::fprintf (stderr, "line %d: cannot read %s\n", lineNo, file.c_str());
                    return 1;
                }
            }
            char msg[256] = {};
            fm1e_capture_flash (e, msg, sizeof msg);
            std::vector<uint8_t> flash (fm1e_flash (e, nullptr, 0));
            fm1e_flash (e, flash.data(), flash.size());
            fm1e_unload (e);
            if (fm1e_boot (e, "FM1_AudioBase", pkg.data(), pkg.size(), flash.data(), flash.size()) != 0)
            {
                std::fprintf (stderr, "line %d: reboot failed\n", lineNo);
                return 1;
            }
        }
        else if (cmd == "wav")
        {
            std::string file;
            in >> file;
            wav.close();
            wav.open (outDir + file);
        }
        else if (cmd == "status")
        {
            Fm1eStatus st {};
            float leds[FM1E_LEDS] {};
            fm1e_view (e, nullptr, leds, &st);
            std::string ledStr;
            for (float l : leds)
                ledStr += l > 0.5f ? '#' : (l > 0.05f ? '+' : '.');
            std::printf ("t=%.2fs booted=%d halted=%d jit=%d speed=%.2fx late=%llu peak=%.3f rms=%.4f leds=%s\n",
                         (double) frame / FM1E_RATE, st.booted, st.halted, st.jit, st.speed,
                         (unsigned long long) st.late, peak, sumN ? std::sqrt (sumSq / (double) sumN) : 0.0,
                         ledStr.c_str());
            std::fflush (stdout);
            peak = 0.0f;
            sumSq = 0.0;
            sumN = 0;
        }
        else
        {
            std::fprintf (stderr, "line %d: unknown command %s\n", lineNo, cmd.c_str());
            return 2;
        }
    }

    wav.close();
    fm1e_destroy (e);
    return 0;
}
