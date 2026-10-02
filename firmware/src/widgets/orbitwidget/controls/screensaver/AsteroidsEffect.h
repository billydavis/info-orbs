#ifndef ASTEROIDS_EFFECT_H
#define ASTEROIDS_EFFECT_H

#include "ScreenManager.h"

// A starfield with a small ringed planet (labeled "Orb-It") and a few wireframe rocks drifting and
// bouncing inside the round bezel's visible area, evoking the classic vector-graphics Asteroids
// arcade screen. This is the screensaver OrbIt puts on screen 2 by default.
//
// No default member initializers: this lives in ScreensaverState's union with the other effects'
// states, and seed() sets every field before anything reads it.
struct AsteroidsState {
    static const int NUM_STARS = 40;
    // Sub-pixel positions - the field drifts so slowly that a star only crosses a pixel boundary
    // (and so only costs a redraw) about once a second.
    float starX[NUM_STARS];
    float starY[NUM_STARS];
    uint8_t starTier[NUM_STARS]; // 0 = brightest/nearest (fastest drift) .. 2 = dimmest/farthest
    float starDriftX, starDriftY; // unit direction the whole field drifts in
    int8_t blinkStar; // index of the star currently twinkling, -1 if none
    unsigned long blinkUntilMs;
    unsigned long nextBlinkMs;

    float planetX, planetY;
    float planetVX, planetVY;
    int lastPlanetX, lastPlanetY;

    static const int NUM_ROCKS = 4;
    static const int ROCK_POINTS = 6;
    float rockX[NUM_ROCKS], rockY[NUM_ROCKS];
    float rockVX[NUM_ROCKS], rockVY[NUM_ROCKS];
    int8_t rockOffsetX[NUM_ROCKS][ROCK_POINTS];
    int8_t rockOffsetY[NUM_ROCKS][ROCK_POINTS];
    int rockBoundRadius[NUM_ROCKS];
    int lastRockX[NUM_ROCKS], lastRockY[NUM_ROCKS];
};

class AsteroidsEffect {
public:
    static const uint32_t DEFAULT_COLOR = 0xFD20; // orange (RGB565)

    explicit AsteroidsEffect(ScreenManager &manager);

    // Draws to whichever screen is currently selected; `color` is the planet's. fullRedraw seeds a
    // fresh starfield/planet/rocks and paints everything from scratch; otherwise this steps one
    // physics frame of `dt` seconds and only repaints what moved, mirroring the erase-just-what-moved
    // approach AnalogClockControl/GaugeControl already use elsewhere in OrbIt. Rocks pass in front
    // of the planet: they're wireframes, so the planet stays visible through them.
    void draw(AsteroidsState &state, uint32_t color, float dt, unsigned long now, bool fullRedraw);

private:
    void seed(AsteroidsState &state, unsigned long now);
    void drawStars(const AsteroidsState &state);
    void drawPlanet(int x, int y, uint32_t color);
    void drawRock(const AsteroidsState &state, int rockIndex, int x, int y, uint32_t color);
    // Drifts/twinkles the stars and repaints any that this frame's erasing (planetMoved's ring
    // around the planet, each rockMoved's old outline) may have wiped out.
    void updateStars(AsteroidsState &state, float dt, unsigned long now, int planetX, int planetY, bool planetMoved, const bool *rockMoved);

    ScreenManager &m_manager;
};
#endif // ASTEROIDS_EFFECT_H
