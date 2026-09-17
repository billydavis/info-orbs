#ifndef ASTEROIDS_CONTROL_H
#define ASTEROIDS_CONTROL_H

#include "ScreenManager.h"

// Purely decorative "screensaver" control - no config, no data source. A starfield with a small
// ringed planet (labeled "Orb-It") and a few wireframe rocks drifting and bouncing inside the
// round bezel's visible area, evoking the classic vector-graphics Asteroids arcade screen. This is
// the default OrbIt puts on screen 2 instead of a plain text label.
struct AsteroidsState {
    bool initialized = false;

    static const int NUM_STARS = 40;
    int16_t starX[NUM_STARS];
    int16_t starY[NUM_STARS];
    uint32_t starColor[NUM_STARS];

    float planetX = 0, planetY = 0;
    float planetVX = 0, planetVY = 0;
    int lastPlanetX = 0, lastPlanetY = 0;

    static const int NUM_ROCKS = 4;
    static const int ROCK_POINTS = 6;
    float rockX[NUM_ROCKS], rockY[NUM_ROCKS];
    float rockVX[NUM_ROCKS], rockVY[NUM_ROCKS];
    int8_t rockOffsetX[NUM_ROCKS][ROCK_POINTS];
    int8_t rockOffsetY[NUM_ROCKS][ROCK_POINTS];
    int rockBoundRadius[NUM_ROCKS];
    int lastRockX[NUM_ROCKS], lastRockY[NUM_ROCKS];

    unsigned long lastFrameMs = 0;
};

class AsteroidsControl {
public:
    explicit AsteroidsControl(ScreenManager &manager);

    // fullRedraw seeds a fresh starfield/planet/rocks and paints everything from scratch (first
    // time this screen shows the control, or after a force redraw); otherwise this steps one
    // physics frame and only repaints the small regions that moved, mirroring the
    // erase-just-what-moved approach AnalogClockControl/GaugeControl already use elsewhere in OrbIt.
    void draw(int displayIndex, AsteroidsState &state, bool fullRedraw);

private:
    void seed(AsteroidsState &state);
    void drawStars(AsteroidsState &state);
    void drawPlanet(int x, int y);
    void drawRock(const AsteroidsState &state, int rockIndex, int x, int y);
    void eraseAndRestoreStars(AsteroidsState &state, int x, int y, int radius);

    ScreenManager &m_manager;
};
#endif // ASTEROIDS_CONTROL_H
