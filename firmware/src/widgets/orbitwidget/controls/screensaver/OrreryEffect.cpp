#include "OrreryEffect.h"

#include "ScreensaverCommon.h"
#include "Utils.h"
#include <math.h>

using namespace Screensaver;

namespace {
struct Planet {
    int orbitRadius;
    int radius;
    uint32_t color;
    float speed; // radians per second
    bool ringed;
};

// Spaced so that no two bodies can ever touch, whatever their positions - the ringed planet's
// ring and the moon's orbit included. That's what lets a body be moved without a thought for the
// others: see moveBody(). Speeds fall off with distance the way real orbits do (radius ^ -1.5),
// from one lap in 8 seconds for the innermost to about a minute for the outermost.
const Planet PLANETS[OrreryState::NUM_PLANETS] = {
    {26, 3, TFT_SILVER, 0.785f, false},
    {38, 4, TFT_ORANGE, 0.444f, false},
    {57, 5, 0x3D9F, 0.242f, false}, // blue - the one with the moon
    {81, 7, 0xE5F0, 0.143f, true}, // tan
    {99, 4, TFT_CYAN, 0.106f, false},
};
const int MOON = OrreryState::NUM_PLANETS; // its index among the bodies
const int MOON_PLANET = 2;
const int MOON_ORBIT_RADIUS = 9;
const float MOON_SPEED = 2.1f;
const uint32_t MOON_COLOR = TFT_LIGHTGREY;

// The ring is a line through the planet, tilted a little, drawn in front of it.
const int RING_HALF_WIDTH = 10;
const uint32_t RING_COLOR = TFT_LIGHTGREY;

const int SUN_RADIUS = 13;
const int SUN_GLOW_RADIUS = 15;
const uint8_t SUN_GLOW_BRIGHTNESS = 90; // of 255
const uint32_t ORBIT_COLOR = 0x39E7; // very dark grey
const uint32_t STAR_COLOR[] = {TFT_LIGHTGREY, TFT_DARKGREY};
const int STAR_FIELD_RADIUS = CENTRE - 2;

// Every disc here is this set of pixels, drawn and tested alike - a pixel-exact test of what a
// body covers is what moveBody() depends on, so nothing is left to a library's own circle shape.
bool inDisc(int dx, int dy, int radius) {
    return dx * dx + dy * dy <= radius * radius + radius;
}

// Row of the ring `dx` pixels across from the planet's centre, relative to it.
int ringY(int dx) {
    return -(dx * 3 + (dx >= 0 ? 5 : -5)) / 10;
}

// Whether a body centred `dx`, `dy` away covers this pixel.
bool inBody(int body, int dx, int dy) {
    if (body == MOON) {
        return inDisc(dx, dy, 1);
    }
    if (inDisc(dx, dy, PLANETS[body].radius)) {
        return true;
    }
    return PLANETS[body].ringed && abs(dx) <= RING_HALF_WIDTH && dy == ringY(dx);
}

// How far a body reaches left/right and up/down of its centre.
int halfWidth(int body) {
    if (body == MOON) {
        return 1;
    }
    return PLANETS[body].ringed ? RING_HALF_WIDTH : PLANETS[body].radius;
}

int halfHeight(int body) {
    return body == MOON ? 1 : PLANETS[body].radius;
}

bool onOrbitLine(int x, int y) {
    int distanceSquared = (x - CENTRE) * (x - CENTRE) + (y - CENTRE) * (y - CENTRE);
    for (const Planet &planet : PLANETS) {
        int r = planet.orbitRadius;
        if (distanceSquared > r * r - r && distanceSquared <= r * r + r) {
            return true;
        }
    }
    return false;
}
} // namespace

OrreryEffect::OrreryEffect(ScreenManager &manager) : m_manager(manager) {
}

void OrreryEffect::position(const OrreryState &state, int body, int &x, int &y) {
    int planet = body == MOON ? MOON_PLANET : body;
    float fx = CENTRE + PLANETS[planet].orbitRadius * cosf(state.angle[planet]);
    float fy = CENTRE + PLANETS[planet].orbitRadius * sinf(state.angle[planet]);
    if (body == MOON) {
        fx += MOON_ORBIT_RADIUS * cosf(state.angle[MOON]);
        fy += MOON_ORBIT_RADIUS * sinf(state.angle[MOON]);
    }
    x = (int) roundf(fx);
    y = (int) roundf(fy);
}

void OrreryEffect::seed(OrreryState &state) {
    for (int body = 0; body < OrreryState::NUM_BODIES; body++) {
        state.angle[body] = random(0, 360) * DEG_TO_RAD;
    }
    for (int body = 0; body < OrreryState::NUM_BODIES; body++) {
        int x, y;
        position(state, body, x, y);
        state.drawnX[body] = x;
        state.drawnY[body] = y;
    }
    for (int i = 0; i < OrreryState::NUM_STARS; i++) {
        // Anywhere the bezel shows, except under the sun - which never moves to uncover one.
        float angle = random(0, 360) * DEG_TO_RAD;
        float distance = random(SUN_GLOW_RADIUS + 3, STAR_FIELD_RADIUS);
        state.starX[i] = (uint8_t) (CENTRE + distance * cosf(angle));
        state.starY[i] = (uint8_t) (CENTRE + distance * sinf(angle));
        state.starTier[i] = random(0, 2);
    }
}

// What a pixel shows when no body is over it: a star, an orbit line, or nothing.
uint32_t OrreryEffect::backgroundAt(const OrreryState &state, int x, int y) {
    for (int i = 0; i < OrreryState::NUM_STARS; i++) {
        if (state.starX[i] == x && state.starY[i] == y) {
            return STAR_COLOR[state.starTier[i]];
        }
    }
    return onOrbitLine(x, y) ? ORBIT_COLOR : TFT_BLACK;
}

void OrreryEffect::drawBackground(const OrreryState &state, uint32_t sunColor) {
    m_manager.fillScreen(TFT_BLACK);

    // The orbit lines, a row at a time, each run of line pixels as one fillRect.
    int reach = PLANETS[OrreryState::NUM_PLANETS - 1].orbitRadius + 1;
    for (int y = CENTRE - reach; y <= CENTRE + reach; y++) {
        int runStart = -1;
        for (int x = CENTRE - reach; x <= CENTRE + reach + 1; x++) {
            bool line = x <= CENTRE + reach && onOrbitLine(x, y);
            if (line && runStart < 0) {
                runStart = x;
            } else if (!line && runStart >= 0) {
                m_manager.fillRect(runStart, y, x - runStart, 1, ORBIT_COLOR);
                runStart = -1;
            }
        }
    }

    for (int i = 0; i < OrreryState::NUM_STARS; i++) {
        m_manager.fillRect(state.starX[i], state.starY[i], 1, 1, STAR_COLOR[state.starTier[i]]);
    }

    m_manager.fillCircle(CENTRE, CENTRE, SUN_GLOW_RADIUS, Utils::rgb565dim(sunColor, SUN_GLOW_BRIGHTNESS));
    m_manager.fillCircle(CENTRE, CENTRE, SUN_RADIUS, sunColor);
}

void OrreryEffect::drawBody(int body, int x, int y) {
    int radius = body == MOON ? 1 : PLANETS[body].radius;
    uint32_t color = body == MOON ? MOON_COLOR : PLANETS[body].color;
    for (int dy = -radius; dy <= radius; dy++) {
        int halfRow = radius;
        while (!inDisc(halfRow, dy, radius)) {
            halfRow--;
        }
        m_manager.fillRect(x - halfRow, y + dy, 2 * halfRow + 1, 1, color);
    }
    if (body != MOON && PLANETS[body].ringed) {
        for (int dx = -RING_HALF_WIDTH; dx <= RING_HALF_WIDTH; dx++) {
            m_manager.fillRect(x + dx, y + ringY(dx), 1, 1, RING_COLOR);
        }
    }
}

// Draws the body at its new position first, then restores the background on just the pixels its
// old position covered and its new one doesn't - usually a thin crescent. Nothing is ever blanked
// and redrawn, so there's no flicker. The background is all that needs putting back: no two bodies
// ever overlap (see PLANETS).
void OrreryEffect::moveBody(OrreryState &state, int body, int x, int y) {
    int oldX = state.drawnX[body];
    int oldY = state.drawnY[body];
    drawBody(body, x, y);
    for (int dy = -halfHeight(body); dy <= halfHeight(body); dy++) {
        for (int dx = -halfWidth(body); dx <= halfWidth(body); dx++) {
            if (inBody(body, dx, dy) && !inBody(body, oldX + dx - x, oldY + dy - y)) {
                m_manager.fillRect(oldX + dx, oldY + dy, 1, 1, backgroundAt(state, oldX + dx, oldY + dy));
            }
        }
    }
    state.drawnX[body] = x;
    state.drawnY[body] = y;
}

void OrreryEffect::draw(OrreryState &state, uint32_t color, float dt, bool fullRedraw) {
    if (fullRedraw) {
        seed(state);
        drawBackground(state, color);
        for (int body = 0; body < OrreryState::NUM_BODIES; body++) {
            drawBody(body, state.drawnX[body], state.drawnY[body]);
        }
        return;
    }

    for (int body = 0; body < OrreryState::NUM_BODIES; body++) {
        state.angle[body] += (body == MOON ? MOON_SPEED : PLANETS[body].speed) * dt;
        if (state.angle[body] >= TWO_PI) {
            state.angle[body] -= TWO_PI;
        }
    }
    for (int body = 0; body < OrreryState::NUM_BODIES; body++) {
        int x, y;
        position(state, body, x, y);
        if (x != state.drawnX[body] || y != state.drawnY[body]) {
            moveBody(state, body, x, y);
        }
    }
}
