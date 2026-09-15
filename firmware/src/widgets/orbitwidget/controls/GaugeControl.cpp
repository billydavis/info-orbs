#include "GaugeControl.h"

#include "config_helper.h"
#include <math.h>

namespace {
const int CENTRE = SCREEN_SIZE / 2;
const int OUTER_RADIUS = 108; // stays within the round bezel's visible area, matches AnalogClockControl's margin
const int RING_THICKNESS = 16;
const int INSTRUMENT_THICKNESS = 10;
const int VALUE_TEXT_Y = CENTRE - 8;
const int VALUE_TEXT_SIZE = 34;
const int LABEL_TEXT_Y = CENTRE + 24;
const int LABEL_TEXT_SIZE = 13;

struct StyleGeometry {
    float startDeg; // this class's own convention: 0 = 12 o'clock, clockwise
    float sweepDeg;
    int thickness;
    bool ticks;
};

StyleGeometry geometryFor(GaugeStyle style) {
    switch (style) {
    case GaugeStyle::SPEEDOMETER:
        return {-135.0f, 270.0f, RING_THICKNESS, false};
    case GaugeStyle::INSTRUMENT:
        return {-150.0f, 300.0f, INSTRUMENT_THICKNESS, true};
    case GaugeStyle::RING:
    default:
        return {0.0f, 360.0f, RING_THICKNESS, false};
    }
}

// Converts an angle in this class's convention (0 = 12 o'clock, clockwise, may be negative or
// >360) to ScreenManager::drawArc's native convention (0 = 6 o'clock, clockwise) - see the class
// comment in GaugeControl.h for how this was confirmed against the actual TFT_eSPI source.
uint32_t toNativeAngle(float myAngleDeg) {
    float native = fmodf(myAngleDeg + 180.0f, 360.0f);
    if (native < 0) {
        native += 360.0f;
    }
    return (uint32_t) roundf(native);
}

// x,y offset for a point at `lengthFromCentre` along `angleDeg` (this class's own convention),
// used only for the instrument style's tick marks - drawLine doesn't need the native-angle
// conversion above, that's purely a drawArc/drawSmoothArc quirk.
void endpoint(float angleDeg, int lengthFromCentre, int &outX, int &outY) {
    float radians = angleDeg * DEG_TO_RAD;
    outX = CENTRE + (int) (lengthFromCentre * sinf(radians));
    outY = CENTRE - (int) (lengthFromCentre * cosf(radians));
}

// Draws a filled arc sector in this class's own angle convention, handling the one degenerate
// case ScreenManager::drawArc/drawSmoothArc can't: a full 360deg sweep converts to native
// start==end (both 180), which both functions treat as "nothing to draw". StockWidget already
// proves passing (0,360) directly to drawArc is the correct way to get a full circle - rounded
// ends are meaningless for a complete circle anyway, so that path ignores roundEnds.
void drawArcSegment(ScreenManager &manager, int outerRadius, int innerRadius, float myStartDeg, float mySweepDeg, uint32_t color, uint32_t backgroundColor, bool roundEnds) {
    if (mySweepDeg <= 0.0f) {
        return;
    }
    if (mySweepDeg >= 360.0f) {
        manager.drawArc(CENTRE, CENTRE, outerRadius, innerRadius, 0, 360, color, backgroundColor);
        return;
    }
    uint32_t nativeStart = toNativeAngle(myStartDeg);
    uint32_t nativeEnd = toNativeAngle(myStartDeg + mySweepDeg);
    if (roundEnds) {
        manager.drawSmoothArc(CENTRE, CENTRE, outerRadius, innerRadius, nativeStart, nativeEnd, color, backgroundColor, true);
    } else {
        manager.drawArc(CENTRE, CENTRE, outerRadius, innerRadius, nativeStart, nativeEnd, color, backgroundColor);
    }
}

void drawTicks(ScreenManager &manager, const StyleGeometry &geometry, int innerRadius, uint32_t color) {
    if (!geometry.ticks) {
        return;
    }
    for (int i = 0; i <= 10; i++) {
        float angle = geometry.startDeg + geometry.sweepDeg * (i / 10.0f);
        int x1, y1, x2, y2;
        endpoint(angle, innerRadius - 2, x1, y1);
        endpoint(angle, innerRadius - 10, x2, y2);
        manager.drawLine(x1, y1, x2, y2, color);
    }
}

float clampedPercent(const GaugeConfig &config) {
    float range = config.max - config.min;
    float percent = range != 0.0f ? (config.value - config.min) / range : 0.0f;
    if (percent < 0.0f) {
        return 0.0f;
    }
    if (percent > 1.0f) {
        return 1.0f;
    }
    return percent;
}

// Show a percentage only for the common 0-100 case - an arbitrary min/max range (e.g. a
// temperature) shouldn't get a misleading "%" suffix.
String valueTextFor(const GaugeConfig &config) {
    bool isPercentRange = (config.min == 0.0f && config.max == 100.0f);
    return String((int) roundf(config.value)) + (isPercentRange ? "%" : "");
}
} // namespace

GaugeControl::GaugeControl(ScreenManager &manager) : m_manager(manager) {
}

void GaugeControl::draw(int displayIndex, const GaugeConfig &config, uint32_t backgroundColor, GaugeState &state, bool externalForce) {
    m_manager.selectScreen(displayIndex);

    StyleGeometry geometry = geometryFor(config.style);
    int innerRadius = OUTER_RADIUS - geometry.thickness;
    float percent = clampedPercent(config);

    // An already-drawn arc segment can't be cheaply recolored in place, and a style change moves
    // the whole geometry - either needs a full repaint, not just an updated fill. externalForce
    // covers "this screen might currently show something else entirely" (e.g. OrbIt just became
    // the active widget), independent of anything GaugeState remembers.
    bool stylingChanged = config.style != state.lastStyle || config.trackColor != state.lastTrackColor;
    bool needsFullRedraw = externalForce || !state.initialized || stylingChanged;

    if (needsFullRedraw) {
        m_manager.fillScreen(backgroundColor);
        drawArcSegment(m_manager, OUTER_RADIUS, innerRadius, geometry.startDeg, geometry.sweepDeg, config.trackColor, backgroundColor, false);
        drawTicks(m_manager, geometry, innerRadius, config.trackColor);
    } else if (percent < state.lastPercent) {
        // Shrinking: erase the now-empty tail back to the track color first. The previous fill's
        // leading edge was drawn with roundEnds=true, and TFT_eSPI's own drawSmoothArc doc comment
        // warns "rounded ends extend the arc angle so can overlap" - the rounded cap bulges past
        // its nominal angle by roughly its own radius (half the ring's thickness). Erasing exactly
        // up to the old angle leaves a sliver of that cap behind, so pad the erase by a safe
        // overestimate of that overshoot (never past the track's own natural end, or this would
        // bleed track color into a style's gap).
        const float ROUND_CAP_MARGIN_DEG = 6.0f;
        float eraseStart = geometry.startDeg + geometry.sweepDeg * percent;
        float rawErase = geometry.sweepDeg * (state.lastPercent - percent);
        float maxErase = geometry.sweepDeg * (1.0f - percent);
        float erase = fminf(rawErase + ROUND_CAP_MARGIN_DEG, maxErase);
        drawArcSegment(m_manager, OUTER_RADIUS, innerRadius, eraseStart, erase, config.trackColor, backgroundColor, false);
        drawTicks(m_manager, geometry, innerRadius, config.trackColor);
    }

    // Redraw the full current fill (not just the delta) whenever the percent or color actually
    // changed - this keeps the leading edge's rounded cap correct on every update rather than only
    // after a full redraw, while still avoiding the expensive fillScreen()/track/tick work above.
    if (needsFullRedraw || percent != state.lastPercent || config.color != state.lastColor) {
        drawArcSegment(m_manager, OUTER_RADIUS, innerRadius, geometry.startDeg, geometry.sweepDeg * percent, config.color, backgroundColor, true);
    }

    String valueText = valueTextFor(config);
    bool valueTextChanged = valueText != state.lastValueText;
    if (valueTextChanged || config.color != state.lastColor) {
        if (!needsFullRedraw && valueTextChanged && state.lastValueText.length() > 0) {
            // Erase the old text exactly (same position/size, background color) rather than a
            // generic rect clear - matches the same erase-old-value/draw-new-value technique
            // ClockWidget's displayDigit() already uses for its digits.
            m_manager.setFontColor(backgroundColor, backgroundColor);
            m_manager.drawString(state.lastValueText, CENTRE, VALUE_TEXT_Y, VALUE_TEXT_SIZE, Align::MiddleCenter);
        }
        m_manager.setFontColor(config.color, backgroundColor);
        m_manager.drawString(valueText, CENTRE, VALUE_TEXT_Y, VALUE_TEXT_SIZE, Align::MiddleCenter);
    }

    String upperLabel = config.label;
    upperLabel.toUpperCase();
    if (upperLabel != state.lastLabelText) {
        if (!needsFullRedraw && state.lastLabelText.length() > 0) {
            m_manager.setFontColor(backgroundColor, backgroundColor);
            m_manager.drawString(state.lastLabelText, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter);
        }
        if (upperLabel.length() > 0) {
            m_manager.setFontColor(TFT_SILVER, backgroundColor);
            m_manager.drawString(upperLabel, CENTRE, LABEL_TEXT_Y, LABEL_TEXT_SIZE, Align::MiddleCenter);
        }
    }

    state.initialized = true;
    state.lastPercent = percent;
    state.lastColor = config.color;
    state.lastTrackColor = config.trackColor;
    state.lastStyle = config.style;
    state.lastValueText = valueText;
    state.lastLabelText = upperLabel;
}
