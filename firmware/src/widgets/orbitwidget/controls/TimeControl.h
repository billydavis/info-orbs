#ifndef TIME_CONTROL_H
#define TIME_CONTROL_H

#include "GlobalTime.h"
#include "ScreenManager.h"

// Renders the full current time (HH:MM) on one screen, with optional date and day-of-week -
// mirrors WeatherWidget's own clock-face screen (WeatherWidget::displayClock) but is
// self-contained: no dependency on WeatherWidget, just the same GlobalTime accessors any widget
// already uses. Caller must set the desired text font (m_manager.setFont(...)) beforehand, same
// precondition as WeatherControl.
class TimeControl {
public:
    explicit TimeControl(ScreenManager &manager);

    // format24Hour is independent of GlobalTime's own device-wide FORMAT_24_HOUR setting/
    // setFormat24Hour() - it's computed here from GlobalTime::getHour24() (always the true 24-hour
    // value) so each slot can choose its own display format without mutating shared global state.
    void drawFullTime(int displayIndex, GlobalTime *time, bool showDate, bool showDay, bool format24Hour, uint32_t foregroundColor, uint32_t backgroundColor);

private:
    ScreenManager &m_manager;
};
#endif // TIME_CONTROL_H
