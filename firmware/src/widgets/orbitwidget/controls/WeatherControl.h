#ifndef WEATHER_CONTROL_H
#define WEATHER_CONTROL_H

#include "ScreenManager.h"
#include "Utils.h"
#include "weatherwidget/WeatherDataModel.h"
#include <TJpg_Decoder.h>

// Renders individual pieces of the weather panel (icon / current temperature+high/low / city+condition
// text) to whichever screen is selected - extracted from WeatherWidget so the same rendering can be
// reused by a widget that only wants one of these elements on one screen (e.g. OrbIt) rather than
// owning all 5 screens of the full weather layout. Colors are derived from ScreenMode (Light/Dark)
// on each call rather than cached, mirroring WeatherWidget::configureColors() - cheap, and avoids
// needing 4 extra color parameters on every call.
//
// Callers must set the desired text font (m_manager.setFont(...)) before calling drawTemperature()/
// drawCondition(), same precondition WeatherWidget::draw() already followed.
class WeatherControl {
public:
    explicit WeatherControl(ScreenManager &manager);

    // Draws the icon for a given Visual Crossing condition string (e.g. "clear-day"), scaled/positioned
    // at (x,y). Picks the light- or dark-mode icon variant per screenMode.
    void drawIcon(int displayIndex, ScreenMode screenMode, const String &condition, int x, int y, int scale);

    // Draws the current temperature plus today's high/low, matching WeatherWidget's "single degree" panel.
    void drawTemperature(int displayIndex, ScreenMode screenMode, WeatherDataModel &model);

    // Draws the city name plus a word-wrapped current-conditions description, matching WeatherWidget's
    // "weather text" panel.
    void drawCondition(int displayIndex, ScreenMode screenMode, WeatherDataModel &model);

private:
    void showJPG(int displayIndex, int x, int y, const byte jpgData[], int jpgDataSize, int scale);

    ScreenManager &m_manager;

    static const int centre = 120; // Centre location of the screen (240x240)
};
#endif // WEATHER_CONTROL_H
