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
// A rounded leading edge (drawSmoothArc's roundEnds) bulges slightly past its nominal angle - same
// overshoot GaugeControl's own shrinking-fill erase has to pad for, and the same fixed margin value.
const float ROUND_CAP_MARGIN_DEG = 6.0f;

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

// Draws the ring's arc from 0deg to `sweepDeg` (this class's own convention) with a rounded leading
// edge, mirroring GaugeControl's own drawArcSegment() - a full 360deg sweep degenerates to native
// start==end (both 180), which drawSmoothArc treats as "nothing to draw", so that case goes through
// plain drawArc instead (same fix StockWidget's own full-circle ring already relies on).
void drawFillArc(ScreenManager &manager, float sweepDeg, uint32_t color) {
    if (sweepDeg <= 0.0f) {
        return;
    }
    if (sweepDeg >= 360.0f) {
        manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, 0, 360, color, TFT_BLACK);
        return;
    }
    manager.drawSmoothArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, toNativeAngle(0), toNativeAngle(sweepDeg), color, TFT_BLACK, true);
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
        // RUNNING or PAUSED. stateChanged covers "just entered this state" (IDLE/COMPLETED ->
        // running, or paused <-> running); drawTimer() separately detects "fraction jumped back up"
        // (a 'restart' while already running, which doesn't change the enum state at all) to also
        // force a full repaint in that case - see its own fullRedraw handling.
        drawTimer(config, runtime, runtime.remainingMs(now), runtime.state == CountdownRunState::PAUSED, stateChanged);
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

void CountdownControl::drawTimer(const CountdownConfig &config, CountdownRuntime &runtime, unsigned long remainingMs, bool paused, bool fullRedraw) {
    float fraction = runtime.totalMs > 0 ? (float) remainingMs / (float) runtime.totalMs : 0.0f;
    if (fraction < 0.0f) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }

    bool warning = !paused && runtime.totalMs > 0 && remainingMs <= min((unsigned long) (runtime.totalMs / 10), 30000UL);
    uint32_t fillColor = paused ? PAUSED_COLOR : (warning ? WARNING_COLOR : config.color);

    // A full ring repaint is needed whenever the fill color changed (an already-drawn arc segment
    // can't be cheaply recolored in place - same reasoning GaugeControl's own style/color change
    // uses) or the fraction went UP instead of down (a 'restart' while already running/paused jumps
    // straight back to ~1.0 - not something the per-tick "shrink the fill" patch below can express).
    // Ordinary running ticks only ever shrink the fraction by a small amount, which the delta patch
    // below handles without touching the rest of the ring at all.
    bool ringNeedsFullRepaint = fullRedraw || runtime.lastDrawnFraction < 0.0f || fillColor != runtime.lastDrawnFillColor || fraction > runtime.lastDrawnFraction;

    float fillSweep = 360.0f * fraction;

    if (ringNeedsFullRepaint) {
        m_manager.fillScreen(TFT_BLACK);
        if (fillSweep < 360.0f) {
            m_manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, toNativeAngle(fillSweep), toNativeAngle(360.0f), TRACK_COLOR, TFT_BLACK);
        }
    } else if (fraction < runtime.lastDrawnFraction) {
        // The common case: one more second elapsed, nothing else changed. Only the newly-elapsed
        // sliver (between the old and new fraction) needs to flip from fill color to track color -
        // everything else on screen is already correct and untouched. Padded past the exact delta
        // by ROUND_CAP_MARGIN_DEG (clamped to the track's own natural end) so the fill's rounded
        // leading edge - which bulges past its nominal angle - never leaves a stale sliver of its
        // own color behind, the same stale-pixel bug AsteroidsControl had to fix for its own erases.
        float eraseStart = fillSweep;
        float rawErase = 360.0f * (runtime.lastDrawnFraction - fraction);
        float maxErase = 360.0f - fillSweep;
        float erase = fminf(rawErase + ROUND_CAP_MARGIN_DEG, maxErase);
        if (erase > 0.0f) {
            m_manager.drawArc(CENTRE, CENTRE, OUTER_RADIUS, INNER_RADIUS, toNativeAngle(eraseStart), toNativeAngle(eraseStart + erase), TRACK_COLOR, TFT_BLACK);
        }
    }

    // Redraws the current fill whenever it actually changed (fraction or color) - cheap (one bounded
    // arc draw, not a fillScreen), and the simplest way to keep the rounded leading edge correct at
    // its new position every tick, matching GaugeControl's own "always redraw the full current fill"
    // approach rather than trying to patch just the moving rounded cap in place.
    if (ringNeedsFullRepaint || fraction != runtime.lastDrawnFraction || fillColor != runtime.lastDrawnFillColor) {
        drawFillArc(m_manager, fillSweep, fillColor);
    }

    m_manager.setFont(DEFAULT_FONT);
    String timeText = formatTime(remainingMs);
    if (ringNeedsFullRepaint || timeText != runtime.lastDrawnTimeText) {
        if (!ringNeedsFullRepaint && runtime.lastDrawnTimeText.length() > 0) {
            // Erase just the old text (same position/size, background color) rather than relying on
            // a fillScreen() that didn't happen this tick - same erase-old/draw-new technique
            // GaugeControl already uses for its own value text.
            m_manager.drawString(runtime.lastDrawnTimeText, CENTRE, TIME_TEXT_Y, TIME_TEXT_SIZE, Align::MiddleCenter, TFT_BLACK, TFT_BLACK);
        }
        m_manager.drawString(timeText, CENTRE, TIME_TEXT_Y, TIME_TEXT_SIZE, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
    }

    String label = paused ? (config.label.length() > 0 ? config.label + " - PAUSED" : "PAUSED") : config.label;
    if (ringNeedsFullRepaint || label != runtime.lastDrawnLabelText) {
        if (!ringNeedsFullRepaint && runtime.lastDrawnLabelText.length() > 0) {
            m_manager.drawString(runtime.lastDrawnLabelText, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter, TFT_BLACK, TFT_BLACK);
        }
        if (label.length() > 0) {
            m_manager.drawString(label, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter, TFT_SILVER, TFT_BLACK);
        }
    }

    runtime.lastDrawnFraction = fraction;
    runtime.lastDrawnFillColor = fillColor;
    runtime.lastDrawnTimeText = timeText;
    runtime.lastDrawnLabelText = label;
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
