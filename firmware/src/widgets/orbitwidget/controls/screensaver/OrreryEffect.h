#ifndef ORRERY_EFFECT_H
#define ORRERY_EFFECT_H

#include "ScreenManager.h"

// A small solar system: a sun in the middle of the screen and five planets circling it along faint
// orbit lines, the inner ones faster than the outer. One planet has a ring, one has a moon, and a
// few stars sit behind it all.
//
// No default member initializers: this lives in ScreensaverState's union with the other effects'
// states, and seed() sets every field before anything reads it.
struct OrreryState {
    static const int NUM_PLANETS = 5;
    static const int NUM_BODIES = NUM_PLANETS + 1; // the planets, then the moon
    static const int NUM_STARS = 25;

    float angle[NUM_BODIES]; // radians round its orbit - the moon's is round its planet
    int16_t drawnX[NUM_BODIES], drawnY[NUM_BODIES]; // where each body currently is on screen

    uint8_t starX[NUM_STARS], starY[NUM_STARS];
    uint8_t starTier[NUM_STARS]; // 0 = brighter, 1 = dimmer
};

class OrreryEffect {
public:
    static const uint32_t DEFAULT_COLOR = TFT_GOLD;

    explicit OrreryEffect(ScreenManager &manager);

    // Draws to whichever screen is currently selected; `color` is the sun's. fullRedraw starts the
    // planets at random points on their orbits and paints everything from scratch; otherwise this
    // advances `dt` seconds and, for each body that has reached a new pixel, draws it there and
    // puts back whatever its old position was covering.
    void draw(OrreryState &state, uint32_t color, float dt, bool fullRedraw);

private:
    void seed(OrreryState &state);
    void position(const OrreryState &state, int body, int &x, int &y);
    uint32_t backgroundAt(const OrreryState &state, int x, int y);
    void drawBackground(const OrreryState &state, uint32_t sunColor);
    void drawBody(int body, int x, int y);
    void moveBody(OrreryState &state, int body, int x, int y);

    ScreenManager &m_manager;
};
#endif // ORRERY_EFFECT_H
