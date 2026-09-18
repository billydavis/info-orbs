#include "CountdownControl.h"

#include "config_helper.h"
#include <math.h>

namespace {
const int CENTRE = SCREEN_SIZE / 2;
const int OUTER_RADIUS = 108; // matches GaugeControl/AnalogClockControl's own round-bezel margin
const int RING_THICKNESS = 16;
const int INNER_RADIUS = OUTER_RADIUS - RING_THICKNESS;
const uint32_t TRACK_COLOR = TFT_DARKGREY;
const uint32_t PAUSED_COLOR = TFT_DARKGREY;
const uint32_t WARNING_COLOR = TFT_RED;
const int TIME_TEXT_Y = CENTRE - 8;
const int TIME_TEXT_SIZE = 34;
const int LABEL_TEXT_Y = CENTRE + 24;
const int LABEL_TEXT_SIZE = 13;
const unsigned long FLASH_PERIOD_MS = 500;

// Same "this class's own 0deg=12 o'clock clockwise convention" -> ScreenManager::drawArc's native
// "0deg=6 o'clock clockwise" conversion GaugeControl already worked out and confirmed against the
// TFT_eSPI source - duplicated rather than shared since each control stays self-contained (matches
// how every other OrbIt control already does its own thing rather than reaching into a sibling).
uint32_t toNativeAngle(float myAngleDeg) {
    float native = fmodf(myAngleDeg + 180.0f, 360.0f);
    if (native < 0) {
        native += 360.0f;
    }
    return (uint32_t) roundf(native);
}

String formatTime(unsigned long ms) {
    // Ceiling, not floor - so the displayed count doesn't visibly touch 0 a moment before the
    // countdown actually completes (the transition to the flashing COMPLETED screen is what
    // signals "actually done", not this text hitting 00:00).
    unsigned long totalSeconds = (ms + 999) / 1000;
    unsigned long mm = totalSeconds / 60;
    unsigned long ss = totalSeconds % 60;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", mm, ss);
    return String(buf);
}
} // namespace

CountdownControl::CountdownControl(ScreenManager &manager) : m_manager(manager) {
}

void CountdownControl::draw(int displayIndex, const CountdownConfig &config, CountdownRuntime &runtime, bool externalForce) {
    m_manager.selectScreen(displayIndex);
    unsigned long now = millis();

    bool stateChanged = externalForce || !runtime.initialized || runtime.state != runtime.lastDrawnState;

    if (runtime.state == CountdownRunState::COMPLETED) {
        drawCompleted(config, runtime);
    } else if (runtime.state == CountdownRunState::IDLE) {
        if (stateChanged) {
            drawIdle(config);
        }
    } else {
        // RUNNING or PAUSED - always fully repaints the ring/text on every call (this only gets
        // called about once a second while running, or once on the transition into/out of pause,
        // so the extra draw cost of a full repaint over a delta-only patch is negligible).
        drawTimer(config, runtime, runtime.remainingMs(now), runtime.state == CountdownRunState::PAUSED);
    }

    runtime.initialized = true;
    runtime.lastDrawnState = runtime.state;
}

void CountdownControl::drawIdle(const CountdownConfig &config) {
    m_manager.fillScreen(TFT_BLACK);
    m_manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, 0, 360, TRACK_COLOR, TFT_BLACK);
    m_manager.setFont(DEFAULT_FONT);
    m_manager.drawString("READY", CENTRE, TIME_TEXT_Y, TIME_TEXT_SIZE - 8, Align::MiddleCenter, TFT_SILVER, TFT_BLACK);
    if (config.label.length() > 0) {
        m_manager.drawString(config.label, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter, TFT_SILVER, TFT_BLACK);
    }
}

void CountdownControl::drawTimer(const CountdownConfig &config, CountdownRuntime &runtime, unsigned long remainingMs, bool paused) {
    m_manager.fillScreen(TFT_BLACK);

    float fraction = runtime.totalMs > 0 ? (float) remainingMs / (float) runtime.totalMs : 0.0f;
    if (fraction < 0.0f) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }

    bool warning = !paused && runtime.totalMs > 0 && remainingMs <= min((unsigned long) (runtime.totalMs / 10), 30000UL);
    uint32_t fillColor = paused ? PAUSED_COLOR : (warning ? WARNING_COLOR : config.color);

    float fillSweep = 360.0f * fraction;
    if (fillSweep > 0.0f) {
        if (fillSweep >= 360.0f) {
            m_manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, 0, 360, fillColor, TFT_BLACK);
        } else {
            m_manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, toNativeAngle(0), toNativeAngle(fillSweep), fillColor, TFT_BLACK);
        }
    }
    if (fillSweep < 360.0f) {
        m_manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, toNativeAngle(fillSweep), toNativeAngle(360.0f), TRACK_COLOR, TFT_BLACK);
    }

    m_manager.setFont(DEFAULT_FONT);
    m_manager.drawString(formatTime(remainingMs), CENTRE, TIME_TEXT_Y, TIME_TEXT_SIZE, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);

    String label = paused ? (config.label.length() > 0 ? config.label + " - PAUSED" : "PAUSED") : config.label;
    if (label.length() > 0) {
        m_manager.drawString(label, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter, TFT_SILVER, TFT_BLACK);
    }
}

void CountdownControl::drawCompleted(const CountdownConfig &config, CountdownRuntime &runtime) {
    unsigned long now = millis();
    bool flashOn = ((now - runtime.completedAtMs) / FLASH_PERIOD_MS) % 2 == 0;

    uint32_t backgroundColor = flashOn ? config.color : TFT_BLACK;
    uint32_t textColor = flashOn ? TFT_BLACK : config.color;

    m_manager.fillScreen(backgroundColor);
    m_manager.setFont(DEFAULT_FONT);
    m_manager.drawString("TIME'S UP", CENTRE, TIME_TEXT_Y, 20, Align::MiddleCenter, textColor, backgroundColor);
    if (config.label.length() > 0) {
        m_manager.drawString(config.label, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter, textColor, backgroundColor);
    }
}
