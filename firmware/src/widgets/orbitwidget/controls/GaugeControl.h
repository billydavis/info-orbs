#ifndef GAUGE_CONTROL_H
#define GAUGE_CONTROL_H

#include "ScreenManager.h"

enum class GaugeStyle {
    RING, // full 360deg donut
    SPEEDOMETER, // 270deg sweep, gap at bottom
    INSTRUMENT, // 300deg sweep with 10% tick marks, thinner ring
};

struct GaugeConfig {
    String label = "";
    float value = 0;
    float min = 0;
    float max = 100;
    uint32_t color = TFT_CYAN;
    uint32_t trackColor = TFT_DARKGREY;
    GaugeStyle style = GaugeStyle::RING;
};

// Tracks what was actually last drawn for one screen, so GaugeControl can update just the fill
// arc and text instead of a full fillScreen()+redraw every time the value changes (which flickers
// - the same problem AnalogClockControl solved for its hands). Caller (OrbItWidget) owns one of
// these per screen, matching the AnalogClockHands pattern.
struct GaugeState {
    bool initialized = false;
    float lastPercent = 0.0f;
    uint32_t lastColor = 0;
    uint32_t lastTrackColor = 0;
    GaugeStyle lastStyle = GaugeStyle::RING;
    String lastValueText = "";
    String lastLabelText = "";
};

// Renders an arbitrary 0..100%-style value as a circular gauge (track + partial fill arc, value
// centered as text) - mocked up first at the linked artifact in
// firmware/src/widgets/orbitwidget/docs/orbit-api.md before writing any of this. A genuinely new
// rendering, like AnalogClockControl, not adapted from an existing widget.
//
// Angle convention note: this class works in its own "0deg = 12 o'clock, clockwise" convention
// (matching AnalogClockControl and the mockup) and converts to ScreenManager::drawArc's native
// convention (confirmed from TFT_eSPI.cpp's own doc comment: "Draw an arc clockwise from 6 o'clock
// position") only at the point of calling it - see toNativeAngle() in the .cpp. Do not pass raw
// angles from here into drawArc/drawSmoothArc elsewhere without going through that conversion.
class GaugeControl {
public:
    explicit GaugeControl(ScreenManager &manager);

    // externalForce is for "this screen may currently be showing something else entirely" (e.g.
    // OrbIt just became the active widget) - always triggers a full repaint regardless of what
    // `state` remembers. Absent that, GaugeControl decides for itself: a pure value change (same
    // style/colors) only touches the fill arc + text; a style or color change repaints everything,
    // since an already-drawn arc segment can't be cheaply recolored in place.
    void draw(int displayIndex, const GaugeConfig &config, uint32_t backgroundColor, GaugeState &state, bool externalForce);

private:
    ScreenManager &m_manager;
};
#endif // GAUGE_CONTROL_H
