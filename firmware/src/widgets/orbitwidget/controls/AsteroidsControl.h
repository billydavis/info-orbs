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
    // Sub-pixel positions - the field drifts so slowly that a star only crosses a pixel boundary
    // (and so only costs a redraw) about once a second.
    float starX[NUM_STARS];
    float starY[NUM_STARS];
    uint8_t starTier[NUM_STARS]; // 0 = brightest/nearest (fastest drift) .. 2 = dimmest/farthest
    float starDriftX = 0, starDriftY = 0; // unit direction the whole field drifts in
    int8_t blinkStar = -1; // index of the star currently twinkling, -1 if none
    unsigned long blinkUntilMs = 0;
    unsigned long nextBlinkMs = 0;

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
    // physics frame and only repaints what moved, mirroring the erase-just-what-moved approach
    // AnalogClockControl/GaugeControl already use elsewhere in OrbIt. Rocks pass in front of the
    // planet: they're wireframes, so the planet stays visible through them.
    void draw(int displayIndex, AsteroidsState &state, bool fullRedraw);

private:
    void seed(AsteroidsState &state);
    void drawStars(const AsteroidsState &state);
    void drawPlanet(int x, int y);
    void drawRock(const AsteroidsState &state, int rockIndex, int x, int y, uint32_t color);
    // Drifts/twinkles the stars and repaints any that this frame's erasing (planetMoved's ring
    // around the planet, each rockMoved's old outline) may have wiped out.
    void updateStars(AsteroidsState &state, float dt, unsigned long now, int planetX, int planetY, bool planetMoved, const bool *rockMoved);

    ScreenManager &m_manager;
};
#endif // ASTEROIDS_CONTROL_H
