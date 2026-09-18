#ifndef COUNTDOWN_CONTROL_H
#define COUNTDOWN_CONTROL_H

#include "ScreenManager.h"

struct CountdownConfig {
    String label = "";
    unsigned long durationSeconds = 0; // 0 = never configured (screen just became a countdown, no 'set' yet)
    uint32_t color = TFT_CYAN;
};

enum class CountdownRunState {
    IDLE,
    RUNNING,
    PAUSED,
    COMPLETED,
};

// Runtime behavior state for one screen's countdown. Not that it would matter across a reboot
// anyway - OrbItWidget::persistLayout() skips countdown slots entirely, so this (and
// OrbItSlot::countdownConfig) simply doesn't exist anymore once the device restarts.
struct CountdownRuntime {
    CountdownRunState state = CountdownRunState::IDLE;
    unsigned long totalMs = 0; // duration of the current/most recent run
    unsigned long remainingMsAtPause = 0; // snapshot taken on 'pause'
    unsigned long runStartedAtMs = 0; // virtual millis() start of the current RUNNING segment
    unsigned long completedAtMs = 0; // millis() when it hit zero - drives the flash animation's phase

    // "what was last actually drawn", mirroring GaugeState/SysMonitorState's own pattern so
    // CountdownControl only repaints what actually needs to change - the ring/text update once a
    // second while running, and a fillScreen() on every single one of those ticks was the whole
    // ring flashing visibly once a second. lastDrawnFraction < 0 means "nothing drawn this run yet".
    bool initialized = false;
    CountdownRunState lastDrawnState = CountdownRunState::IDLE;
    float lastDrawnFraction = -1.0f;
    uint32_t lastDrawnFillColor = 0;
    String lastDrawnTimeText = "";
    String lastDrawnLabelText = "";

    // Remaining time in ms as of `now`, given this runtime's current state - the single source of
    // truth both OrbItWidget (the RUNNING->COMPLETED transition, and the redraw-throttling stamp)
    // and CountdownControl (what to actually draw) compute from.
    unsigned long remainingMs(unsigned long now) const {
        switch (state) {
        case CountdownRunState::RUNNING: {
            unsigned long elapsed = now - runStartedAtMs;
            return elapsed >= totalMs ? 0 : totalMs - elapsed;
        }
        case CountdownRunState::PAUSED:
            return remainingMsAtPause;
        case CountdownRunState::COMPLETED:
            return 0;
        case CountdownRunState::IDLE:
        default:
            return totalMs;
        }
    }
};

// Renders a countdown timer as a depleting ring (full circle at start, shrinking to nothing as
// time runs out, rounded leading edge via drawSmoothArc) with the remaining MM:SS centered and an
// optional label below - same overall look and layout convention as GaugeControl's own ring style.
// When the countdown reaches zero, this instead flashes the
// whole screen (alternating between the accent color and black, plus "TIME'S UP") every ~500ms
// until the slot is stopped/restarted/re-set - there's no speaker on this hardware, so a purely
// visual, impossible-to-miss-on-a-glance signal is the whole point.
//
// A per-second tick only patches the delta (the newly-elapsed arc sliver going from fill to track
// color, plus the MM:SS text) rather than a full fillScreen()+redraw - same "erase just what
// changed" approach GaugeControl already uses for its own ring, for the same reason: an unconditional
// full repaint every second visibly flashed the whole screen once a second.
class CountdownControl {
public:
    explicit CountdownControl(ScreenManager &manager);

    // externalForce mirrors GaugeControl's own: "this screen may currently show something else
    // entirely" (e.g. OrbIt just became the active widget), independent of anything CountdownRuntime
    // remembers having last drawn.
    void draw(int displayIndex, const CountdownConfig &config, CountdownRuntime &runtime, bool externalForce);

private:
    void drawIdle(const CountdownConfig &config);
    void drawTimer(const CountdownConfig &config, CountdownRuntime &runtime, unsigned long remainingMs, bool paused, bool fullRedraw);
    void drawCompleted(const CountdownConfig &config, CountdownRuntime &runtime);

    ScreenManager &m_manager;
};
#endif // COUNTDOWN_CONTROL_H
