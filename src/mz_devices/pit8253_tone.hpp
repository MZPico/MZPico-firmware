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
// Schedules timestamped bus writes onto exact sample positions. The
// playhead advances exactly one buffer per audio buffer regardless of
// WHEN the fill happens - core0's poll loop jitters by milliseconds
// (WiFi/USB share it), so anchoring to wall-clock at fill time smears
// event offsets and turns 1-bit music into popping. Events beyond the
// current buffer window are left unconsumed for the next buffer.
struct ToneTimeline {
    // Pulse-train beeper engines emit one event per edge at up to ~100k
    // edges/s; melody is a few per second. Size for the engines.
    static constexpr uint32_t QUEUE_SIZE = 512;
    static constexpr uint32_t BUFFER_FRAMES = AUDIO_BUFFER_SIZE / 2;
    static constexpr uint32_t BUFFER_US = (BUFFER_FRAMES * 1000000ull) / AUDIO_SAMPLE_RATE;

    struct Ev {
        uint16_t offUs;   // microseconds into the buffer window
        uint8_t a;
        uint8_t b;
    };
    Ev q[QUEUE_SIZE];
    uint32_t len = 0, pos = 0, sample = 0;
    uint32_t playheadTs = 0;
    bool inited = false;

    // Call at buffer start, with the newest pending event's timestamp (if any)
    void beginBuffer(uint32_t newestTs, bool haveNewest) {
        len = 0;
        pos = 0;
        sample = 0;
        if (!inited) {
            if (!haveNewest) return;
            playheadTs = newestTs - 2 * BUFFER_US;
            inited = true;
        } else {
            playheadTs += BUFFER_US;
            // The sample clock and the us timer drift ~100ppm apart; if the
            // backlog grows past a few buffers, resnap instead of lagging
            if (haveNewest && (int32_t)(newestTs - playheadTs) > (int32_t)(4 * BUFFER_US)) {
                playheadTs = newestTs - 2 * BUFFER_US;
            }
        }
    }

    // Does this timestamp fall inside the current buffer window?
    bool accepts(uint32_t ts) const {
        return inited && (int32_t)(ts - (playheadTs + BUFFER_US)) < 0;
    }

    // Returns false when full - the caller should apply the write
    // immediately instead (coarse beats dropped)
    bool push(uint32_t ts, uint8_t a, uint8_t b) {
        if (len >= QUEUE_SIZE) return false;
        int32_t rel = (int32_t)(ts - playheadTs);
        if (rel < 0) rel = 0;
        if ((uint32_t)rel > BUFFER_US) rel = BUFFER_US;
        q[len].offUs = (uint16_t)rel;
        q[len].a = a;
        q[len].b = b;
        len++;
        return true;
    }

    // Per output sample: invoke f(a, b, cycleOffset) for every event due at
    // this sample. cycleOffset locates the event WITHIN the sample (given
    // the sample spans `cycles` input-clock cycles) so the integrator can
    // honor pulses narrower than one sample - the substance of pulse-train
    // beeper engines (10-30us pulses vs 22.7us samples).
    template <typename F>
    void applyDue(uint32_t cycles, F&& f) {
        while (pos < len) {
            uint32_t evScaled = (uint32_t)q[pos].offUs * 441u;   // us -> samples x10000
            uint32_t evSample = evScaled / 10000u;
            if (evSample > sample) break;
            uint32_t cyc = 0;
            if (evSample == sample) {
                cyc = ((evScaled - sample * 10000u) * cycles) / 10000u;
            }
            f(q[pos].a, q[pos].b, cyc);
            pos++;
        }
        sample++;
    }
};

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

    // ---- sliced sample rendering ----
    // The output is the AREA AVERAGE of the speaker line across the sample
    // window (a box filter): hard-edge sampling of a square wave aliases
    // audibly, and write-driven pulses narrower than one sample would
    // vanish entirely. A sample renders as:
    //   cycles = beginSample();
    //   for each due write: advanceTo(itsCycleOffset); apply the write;
    //   out = finishSample(amplitude);
    uint32_t curCycles = 0;
    uint32_t curDone = 0;
    int32_t curArea = 0;

    uint32_t beginSample() {
        cycleResid += INPUT_CLOCK;
        curCycles = cycleResid / AUDIO_SAMPLE_RATE;
        cycleResid %= AUDIO_SAMPLE_RATE;
        curDone = 0;
        curArea = 0;
        return curCycles;
    }

    void advanceTo(uint32_t cycleOff) {
        if (cycleOff > curCycles) cycleOff = curCycles;
        if (cycleOff > curDone) {
            integrate(cycleOff - curDone);
            curDone = cycleOff;
        }
    }

    int32_t finishSample(int32_t amplitude) {
        if (curCycles > curDone) {
            integrate(curCycles - curDone);
            curDone = curCycles;
        }
        int32_t x = (int32_t)(((int64_t)amplitude * curArea) / (int32_t)curCycles);
        // One-pole DC blocker (~27 Hz at 44.1 kHz): static levels decay to
        // silence; transitions - the actual 1-bit audio - pass through
        int32_t y = x - dcPrevIn + dcPrevOut - (dcPrevOut >> 8);
        dcPrevIn = x;
        dcPrevOut = y;
        if (y > 32767) y = 32767;
        if (y < -32767) y = -32767;
        return y;
    }

private:
    void integrate(uint32_t run) {
        bool g = gate;
        if (running && mode == 3 && reloadValue >= 2 && reloadValue <= MAX_AUDIBLE_RELOAD) {
            uint32_t reload = reloadValue;
            if (counter == 0 || counter > reload) {
                counter = (reload + 1) >> 1;  // reload raced in mid-render
            }
            while (run) {
                uint32_t chunk = (counter < run) ? counter : run;
                curArea += (g && outLevel) ? (int32_t)chunk : -(int32_t)chunk;
                counter -= chunk;
                run -= chunk;
                if (counter == 0) {
                    outLevel = !outLevel;
                    counter = outLevel ? (reload + 1) >> 1 : reload >> 1;
                }
            }
        } else if (running && mode == 0) {
            // one-shot: low while counting, high from terminal count on
            uint32_t low = (counter < run) ? counter : run;
            uint32_t high = run - low;
            if (counter > run) {
                counter -= run;
            } else {
                counter = 0;
                outLevel = true;
                running = false;
            }
            curArea += g ? ((int32_t)high - (int32_t)low) : -(int32_t)run;
        } else {
            // static (or sub-audible mode 3) level
            curArea += (g && outLevel) ? (int32_t)run : -(int32_t)run;
        }
    }

public:
};
