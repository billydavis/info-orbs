#ifndef WEATHER_CONTROL_H
#define WEATHER_CONTROL_H

#include "ScreenManager.h"
#include "Utils.h"
#include "weatherwidget/WeatherDataModel.h"
#include <TJpg_Decoder.h>

// "Not set" for a WeatherStyle color - no RGB565 color is wider than 16 bits, so this can't be
// mistaken for one. An unset color falls back to a default that depends on ScreenMode (and, for
// some, on the other colors), which is why the defaults can't just be baked into the struct.
const uint32_t WEATHER_COLOR_DEFAULT = 0xFFFFFFFF;

// How a weather element is drawn - all settings, no live data.
struct WeatherStyle {
    uint32_t color = WEATHER_COLOR_DEFAULT; // main text; default white in Dark mode, black in Light
    uint32_t background = WEATHER_COLOR_DEFAULT; // default black in Dark mode, white in Light
    uint32_t highColor = WEATHER_COLOR_DEFAULT; // temperature only: today's high; default as the main text
    uint32_t lowColor = WEATHER_COLOR_DEFAULT; // temperature only: today's low; default as the main text
    uint32_t cityColor = WEATHER_COLOR_DEFAULT; // the city name; default as the main text
    bool showCity = false; // temperature only: the city name above the temperature
    bool showHighLow = true; // temperature only: false leaves just the current temperature, centered
};

// Renders individual pieces of the weather panel (icon / current temperature+high/low / city+condition
// text) to whichever screen is selected - extracted from WeatherWidget so the same rendering can be
// reused by a widget that only wants one of these elements on one screen (e.g. OrbIt) rather than
// owning all 5 screens of the full weather layout. Colors come from the WeatherStyle, falling back
// to ScreenMode (Light/Dark) defaults mirroring WeatherWidget::configureColors(), and are resolved
// and set on every call - the renderer's font color is shared with every other control, so nothing
// here may rely on what it was left at.
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
    void drawTemperature(int displayIndex, ScreenMode screenMode, WeatherDataModel &model, const WeatherStyle &style);

    // Draws the city name plus a word-wrapped current-conditions description, matching WeatherWidget's
    // "weather text" panel.
    void drawCondition(int displayIndex, ScreenMode screenMode, WeatherDataModel &model, const WeatherStyle &style);

private:
    void showJPG(int displayIndex, int x, int y, const byte jpgData[], int jpgDataSize, int scale);

    ScreenManager &m_manager;

    static const int centre = 120; // Centre location of the screen (240x240)
};
#endif // WEATHER_CONTROL_H
