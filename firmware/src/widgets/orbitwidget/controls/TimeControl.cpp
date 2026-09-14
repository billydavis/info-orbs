#include "TimeControl.h"

#include "config_helper.h"

TimeControl::TimeControl(ScreenManager &manager) : m_manager(manager) {
}

// Layout mirrors WeatherWidget::displayClock() exactly (same y-coordinates), since that's the
// existing "full clock face on one screen" reference this project already has.
void TimeControl::drawFullTime(int displayIndex, GlobalTime *time, bool showDate, bool showDay, bool format24Hour, uint32_t foregroundColor, uint32_t backgroundColor) {
    const int centre = SCREEN_SIZE / 2;
    const int clockY = 120;
    const int dateY = 50;
    const int dayOfWeekY = 190;

    m_manager.selectScreen(displayIndex);
    m_manager.fillScreen(backgroundColor);
    m_manager.setFontColor(foregroundColor);

    if (showDate) {
        m_manager.drawCentreString(time->getDayAndMonth(), centre, dateY, 18);
    }
    if (showDay) {
        m_manager.drawCentreString(time->getWeekday(), centre, dayOfWeekY, 22);
    }

    // getHour24() is always the true 24-hour value regardless of GlobalTime's own format setting -
    // convert to 12-hour here ourselves so this slot's format choice doesn't depend on (or mutate)
    // that shared global state. Hour deliberately not zero-padded - "9:30" reads better than
    // "09:30" on a screen this small. Drawn as one centered string (not separately-anchored
    // hour/colon/minute pieces) so the whole cluster stays visually centered regardless of whether
    // the hour is one or two digits.
    int hour24 = time->getHour24();
    int displayHour = format24Hour ? hour24 : (hour24 % 12 == 0 ? 12 : hour24 % 12);
    String timeString = String(displayHour) + ":" + time->getMinutePadded();
    m_manager.drawString(timeString, centre, clockY, 66, Align::MiddleCenter);
}
