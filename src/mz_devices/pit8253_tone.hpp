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
    static constexpr uint32_t INPUT_CLOCK = 1108590;  // 1.10859 MHz

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

    // Render one output sample (mono, DC-blocked, +-amplitude swing)
    int32_t render(int32_t amplitude) {
        cycleResid += INPUT_CLOCK;
        uint32_t cycles = cycleResid / AUDIO_SAMPLE_RATE;
        cycleResid %= AUDIO_SAMPLE_RATE;

        if (running) {
            if (mode == 3) {
                uint32_t reload = reloadValue;
                if (reload >= 2) {
                    if (counter == 0 || counter > reload) {
                        counter = (reload + 1) >> 1;  // reload raced in mid-render
                    }
                    for (uint32_t i = 0; i < cycles; i++) {
                        if (--counter == 0) {
                            outLevel = !outLevel;
                            counter = outLevel ? (reload + 1) >> 1 : reload >> 1;
                        }
                    }
                }
            } else if (mode == 0) {
                if (counter > cycles) {
                    counter -= cycles;
                } else {
                    counter = 0;
                    outLevel = true;   // terminal count reached
                    running = false;
                }
            }
        }

        int32_t x = (gate && outLevel) ? amplitude : -amplitude;
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
