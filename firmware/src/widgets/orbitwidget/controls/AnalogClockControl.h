#ifndef ANALOG_CLOCK_CONTROL_H
#define ANALOG_CLOCK_CONTROL_H

#include "GlobalTime.h"
#include "ScreenManager.h"

struct AnalogClockColors {
    uint32_t background = TFT_BLACK;
    uint32_t tick = TFT_WHITE;
    uint32_t hourHand = TFT_WHITE;
    uint32_t minuteHand = TFT_WHITE;
    uint32_t secondHand = TFT_RED;
};

// Tracks the previously-drawn hand endpoints for one screen, so AnalogClockControl can erase just
// the old hands instead of redrawing the whole face every second (a full fillScreen()+redraw every
// second caused a visible once-a-second flash). Caller (OrbItWidget) owns one of these per screen
// and passes it in by reference - AnalogClockControl itself stays stateless/reusable, matching how
// TimeControl::displayDigit() takes lastDigit/digit explicitly rather than owning per-screen state.
struct AnalogClockHands {
    bool initialized = false;
    int hourX = 0, hourY = 0;
    int minuteX = 0, minuteY = 0;
    int secondX = 0, secondY = 0;
};

// Renders a classic analog clock face (circle, 12 hour ticks, hour/minute/second hands) to
// whichever screen is selected. Unlike TimeControl/WeatherControl/TickerControl, this isn't
// adapted from an existing widget's rendering - none of ClockWidget/WeatherWidget/StockWidget draw
// an analog face, so this is new. The round face happens to fit these round (GC9A01) screens
// better than any rectangular content does.
class AnalogClockControl {
public:
    explicit AnalogClockControl(ScreenManager &manager);

    // fullRedraw draws the face/ticks and all hands from scratch (needed the first time a screen
    // shows this control, or after a force redraw); otherwise only the hands are erased-and-redrawn,
    // which is what keeps the once-a-second update flicker-free.
    void draw(int displayIndex, GlobalTime *time, const AnalogClockColors &colors, AnalogClockHands &hands, bool fullRedraw);

private:
    void drawFace(int displayIndex, const AnalogClockColors &colors);
    void computeHands(GlobalTime *time, AnalogClockHands &out);

    ScreenManager &m_manager;
};
#endif // ANALOG_CLOCK_CONTROL_H
