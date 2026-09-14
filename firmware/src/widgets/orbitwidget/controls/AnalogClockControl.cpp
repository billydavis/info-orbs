#include "AnalogClockControl.h"

#include "config_helper.h"
#include <math.h>

namespace {
// angleDegrees measured clockwise from 12 o'clock, matching a normal clock face.
// DEG_TO_RAD is already defined by the Arduino core (Arduino.h) - reuse it rather than
// redeclaring a constant of the same name, which the preprocessor would silently mangle.
void endpoint(int centreX, int centreY, float angleDegrees, int length, int &outX, int &outY) {
    float radians = angleDegrees * DEG_TO_RAD;
    outX = centreX + (int) (length * sinf(radians));
    outY = centreY - (int) (length * cosf(radians));
}
} // namespace

AnalogClockControl::AnalogClockControl(ScreenManager &manager) : m_manager(manager) {
}

void AnalogClockControl::drawFace(int displayIndex, const AnalogClockColors &colors) {
    const int centre = SCREEN_SIZE / 2;
    const int faceRadius = 110; // stays within the round bezel's visible area

    m_manager.selectScreen(displayIndex);
    m_manager.fillScreen(colors.background);
    m_manager.drawCircle(centre, centre, faceRadius, colors.tick);

    for (int i = 0; i < 12; i++) {
        int x1, y1, x2, y2;
        float angle = i * 30.0f;
        endpoint(centre, centre, angle, faceRadius, x1, y1);
        endpoint(centre, centre, angle, faceRadius - 10, x2, y2);
        m_manager.drawLine(x1, y1, x2, y2, colors.tick);
    }
}

void AnalogClockControl::computeHands(GlobalTime *time, AnalogClockHands &out) {
    const int centre = SCREEN_SIZE / 2;
    int hour24 = time->getHour24();
    int minute = time->getMinute();
    int second = time->getSecond();

    // Minute/hour hands move smoothly (fractional degrees from seconds/minutes) rather than
    // jumping once a minute/hour, matching how a real analog clock's hands actually move.
    float secondAngle = second * 6.0f;
    float minuteAngle = minute * 6.0f + second * 0.1f;
    float hourAngle = (hour24 % 12) * 30.0f + minute * 0.5f;

    endpoint(centre, centre, hourAngle, 55, out.hourX, out.hourY);
    endpoint(centre, centre, minuteAngle, 85, out.minuteX, out.minuteY);
    endpoint(centre, centre, secondAngle, 95, out.secondX, out.secondY);
}

void AnalogClockControl::draw(int displayIndex, GlobalTime *time, const AnalogClockColors &colors, AnalogClockHands &hands, bool fullRedraw) {
    const int centre = SCREEN_SIZE / 2;

    m_manager.selectScreen(displayIndex);

    if (fullRedraw || !hands.initialized) {
        drawFace(displayIndex, colors);
        computeHands(time, hands);
        hands.initialized = true;
    } else {
        // Erase just the old hands (redraw them in the background color) instead of clearing the
        // whole screen - hand lengths (55/85/95) stay well short of the tick marks (radius
        // 100-110), so this can't accidentally erase the face/ticks. Any incidental overlap
        // between hands near the center self-heals below since all three are always redrawn fresh
        // afterward regardless of which one actually moved.
        m_manager.drawLine(centre, centre, hands.hourX, hands.hourY, colors.background);
        m_manager.drawLine(centre, centre, hands.minuteX, hands.minuteY, colors.background);
        m_manager.drawLine(centre, centre, hands.secondX, hands.secondY, colors.background);
        computeHands(time, hands);
    }

    m_manager.drawLine(centre, centre, hands.hourX, hands.hourY, colors.hourHand);
    m_manager.drawLine(centre, centre, hands.minuteX, hands.minuteY, colors.minuteHand);
    m_manager.drawLine(centre, centre, hands.secondX, hands.secondY, colors.secondHand);
    m_manager.fillCircle(centre, centre, 4, colors.hourHand);
}
