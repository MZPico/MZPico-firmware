#pragma once

#include <stdint.h>
#include "i2s_audio.hpp"

// Shared 8253 counter-0 sound model for the CTC (I/O-mapped) and melody
// (memory-snooped) devices.
//
// The OUT pin is modeled as a LEVEL, not just a running square wave:
// a control word halts the counter and parks OUT (mode 0 low, others
// high), loading a count starts mode 3 as a square wave or mode 0 as a
// one-shot (low until terminal count, then high). The speaker line is
// OUT AND gate. This makes 1-bit "beeper engine" techniques audible -
// ZX Spectrum ports toggle the level at audio rate via alternating
// control words, gate flips, or mode-0 PWM - while classic mode-3
// melody/beep behavior is unchanged. A DC blocker turns static levels
// into silence, like the real AC-coupled amplifier, so only transitions
// are heard.
//
// Bus-side methods (ctrlWrite/countWrite/setGate) are small and inline
// so they fold into the callers' RAM_FUNC handlers. render() runs on
// core0 only.
struct Pit8253Tone {
    // GDG master clock 17.734475 MHz / 16 (per the MZ-800 GDG; matches
    // mz800emu's GDGCLK_REAL_BASE / GDGCLK_CTC0_DIVIDER)
    static constexpr uint32_t INPUT_CLOCK = 1108405;
    // Mode-3 reloads slower than ~25 Hz (e.g. the divisor-0 idle value
    // 0x10000 -> 16.9 Hz) are inaudible on the real speaker; rendering
    // them would turn rests into audible clicking through the DC blocker
    static constexpr uint32_t MAX_AUDIBLE_RELOAD = INPUT_CLOCK / 25;

    enum class LoadMode : uint8_t { None = 0, LSB, MSB, LSB_MSB };

    // ---- bus-side state ----
    volatile bool gate = false;
    volatile bool outLevel = true;
    volatile bool running = false;
    volatile uint8_t mode = 3;            // normalized M bits of control word
    volatile uint32_t reloadValue = 0;
    LoadMode loadMode = LoadMode::None;
    bool waitingMsb = false;
    uint8_t latchedLsb = 0;

    // ---- render-side state (core0) ----
    uint32_t counter = 0;
    uint32_t cycleResid = 0;
    int32_t dcPrevIn = 0;
    int32_t dcPrevOut = 0;

    void setGate(bool open) { gate = open; }

    // 8253 control word (0xD7 / 0xE007). Counter 1/2 selects and latch
    // commands are ignored; a real control word halts the counter and
    // parks the output until a full count is loaded.
    void ctrlWrite(uint8_t dt) {
        if (((dt >> 6) & 0x03) != 0) {
            return;  // counters 1/2
        }
        uint8_t rl = (dt >> 4) & 0x03;
        if (rl == 0) {
            return;  // latch for reading; write mode persists
        }
        switch (rl) {
            case 1: loadMode = LoadMode::LSB; break;
            case 2: loadMode = LoadMode::MSB; break;
            case 3: loadMode = LoadMode::LSB_MSB; break;
        }
        waitingMsb = false;

        uint8_t m = (dt >> 1) & 0x07;
        if ((m & 0x03) == 0x02) m = 2;    // x10 -> mode 2
        if ((m & 0x03) == 0x03) m = 3;    // x11 -> mode 3
        mode = m;
        running = false;
        outLevel = (m != 0);              // mode 0 parks low, others high
    }

    // 8253 counter 0 data (0xD4 / 0xE004)
    void countWrite(uint8_t dt) {
        switch (loadMode) {
            case LoadMode::LSB:
                applyReload((uint16_t)((reloadValue & 0xFF00) | dt));
                break;
            case LoadMode::MSB:
                applyReload((uint16_t)(((uint16_t)dt << 8) | (reloadValue & 0x00FF)));
                break;
            case LoadMode::LSB_MSB:
                if (!waitingMsb) {
                    latchedLsb = dt;
                    waitingMsb = true;
                } else {
                    waitingMsb = false;
                    applyReload((uint16_t)(((uint16_t)dt << 8) | latchedLsb));
                }
                break;
            default:
                break;
        }
    }

    void applyReload(uint16_t value) {
        uint32_t effective = value ? value : 0x10000;
        reloadValue = effective;
        running = true;
        if (mode == 3) {
            // square wave: high for ceil(N/2), low for floor(N/2)
            outLevel = true;
            counter = (effective + 1) >> 1;
        } else if (mode == 0) {
            // one-shot: low while counting, high from terminal count on
            outLevel = false;
            counter = effective;
        } else {
            // modes 1/2/4/5: strobes/pulses too short to render; park high
            outLevel = true;
            running = false;
        }
    }

    // Render one output sample (mono, DC-blocked, +-amplitude swing).
    // The output is the AREA AVERAGE of the speaker line across the
    // sample window (a box filter), not the level at the sample instant:
    // hard-edge sampling of a square wave aliases audibly - heard as
    // detune/roughness against the real speaker playing in unison.
    int32_t render(int32_t amplitude) {
        cycleResid += INPUT_CLOCK;
        uint32_t cycles = cycleResid / AUDIO_SAMPLE_RATE;
        cycleResid %= AUDIO_SAMPLE_RATE;
        bool g = gate;
        int32_t area;

        if (running && mode == 3 && reloadValue >= 2 && reloadValue <= MAX_AUDIBLE_RELOAD) {
            uint32_t reload = reloadValue;
            if (counter == 0 || counter > reload) {
                counter = (reload + 1) >> 1;  // reload raced in mid-render
            }
            area = 0;
            uint32_t left = cycles;
            while (left) {
                uint32_t run = (counter < left) ? counter : left;
                area += (g && outLevel) ? (int32_t)run : -(int32_t)run;
                counter -= run;
                left -= run;
                if (counter == 0) {
                    outLevel = !outLevel;
                    counter = outLevel ? (reload + 1) >> 1 : reload >> 1;
                }
            }
        } else {
            if (running && mode == 0) {
                if (counter > cycles) {
                    counter -= cycles;
                } else {
                    counter = 0;
                    outLevel = true;   // terminal count reached
                    running = false;
                }
            }
            // static (or sub-audible) level for the whole sample
            area = (g && outLevel) ? (int32_t)cycles : -(int32_t)cycles;
        }

        int32_t x = (int32_t)(((int64_t)amplitude * area) / (int32_t)cycles);
        // One-pole DC blocker (~27 Hz at 44.1 kHz): static levels decay to
        // silence; transitions - the actual 1-bit audio - pass through
        int32_t y = x - dcPrevIn + dcPrevOut - (dcPrevOut >> 8);
        dcPrevIn = x;
        dcPrevOut = y;
        if (y > 32767) y = 32767;
        if (y < -32767) y = -32767;
        return y;
    }
};
