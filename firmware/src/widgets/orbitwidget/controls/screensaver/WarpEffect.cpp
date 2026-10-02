#include "WarpEffect.h"

#include "ScreensaverCommon.h"
#include "Utils.h"
#include <math.h>

using namespace Screensaver;

namespace {
// Everything moves outward at BASE_SPEED + distance * ACCELERATION pixels per second, so it crawls
// out of the far end and rushes past the edge - a little under two seconds from one to the other.
const float BASE_SPEED = 10.0f;
const float ACCELERATION = 1.5f;
// A streak is as long as its star travels in this many seconds: a dot at the far end, a line by
// the edge.
const float STREAK_SECONDS = 0.06f;
// Just past the bezel. A ring or star this far from the tunnel's axis is out of sight and starts
// again, and it's also where the tunnel's bend has straightened out completely - see bendAt().
const int EDGE_RADIUS = 122;
const int RING_START_RADIUS = 2;
// Rings a quarter of their lifetime apart, for a tunnel that's already under way when it first shows.
const float RING_SEED_RADIUS[] = {2, 10, 27, 59};
const int STAR_SPAWN_MIN_RADIUS = 3;
const int STAR_SPAWN_MAX_RADIUS = 25;

// A turn moves the tunnel's far end this many pixels off the screen's centre, in a random direction.
const int MIN_TURN = 20;
const int MAX_TURN = 55;
const int MIN_TURN_SECONDS = 2;
const int MAX_TURN_SECONDS = 6;
// Fraction of the way to the new turn the tunnel swings each second.
const float TURN_RATE = 1.5f;

float speedAt(float radius) {
    return BASE_SPEED + radius * ACCELERATION;
}

// How much of its bend something `radius` from the tunnel's axis is drawn with: all of it at the
// far end, less the closer it comes, none by the time it passes the edge of the screen.
int bendAt(int8_t bend, int radius) {
    return radius >= EDGE_RADIUS ? 0 : bend * (EDGE_RADIUS - radius) / EDGE_RADIUS;
}

// Stars and rings both start dim at the far end and brighten as they approach.
uint32_t starColor(int radius) {
    return Utils::rgb565dim(TFT_WHITE, min(70 + radius * 2, 255));
}

uint32_t ringColor(uint32_t color, int radius) {
    return Utils::rgb565dim(color, min(45 + radius * 2, 255));
}

// The streak a star at `radius` shows as, in the form WarpState stores it.
void streakFor(float radius, uint8_t &inner, uint8_t &outer) {
    float length = speedAt(radius) * STREAK_SECONDS;
    inner = (uint8_t) constrain(radius - length, 0.0f, 255.0f);
    outer = (uint8_t) constrain(radius, 0.0f, 255.0f);
}
} // namespace

WarpEffect::WarpEffect(ScreenManager &manager) : m_manager(manager) {
}

void WarpEffect::newTurn(WarpState &state) {
    float angle = random(0, 360) * DEG_TO_RAD;
    float size = random(MIN_TURN, MAX_TURN + 1);
    state.bendTargetX = size * cosf(angle);
    state.bendTargetY = size * sinf(angle);
    state.turnTimer = random(MIN_TURN_SECONDS * 1000, MAX_TURN_SECONDS * 1000) / 1000.0f;
}

void WarpEffect::spawnStar(WarpState &state, int i, int minRadius, int maxRadius) {
    float angle = random(0, 360) * DEG_TO_RAD;
    state.starDirX[i] = (int8_t) roundf(127.0f * cosf(angle));
    state.starDirY[i] = (int8_t) roundf(127.0f * sinf(angle));
    state.starBendX[i] = (int8_t) roundf(state.bendX);
    state.starBendY[i] = (int8_t) roundf(state.bendY);
    state.starRadius[i] = random(minRadius, maxRadius);
    streakFor(state.starRadius[i], state.starInner[i], state.starOuter[i]);
}

void WarpEffect::seed(WarpState &state) {
    // Starts already part-way round a turn, rather than dead straight.
    newTurn(state);
    state.bendX = state.bendTargetX;
    state.bendY = state.bendTargetY;
    for (int i = 0; i < WarpState::NUM_STARS; i++) {
        // Anywhere along the tunnel, so the first frame is already full of stars.
        spawnStar(state, i, STAR_SPAWN_MIN_RADIUS, PLAYFIELD_RADIUS);
    }
    for (int r = 0; r < WarpState::NUM_RINGS; r++) {
        state.ringRadius[r] = RING_SEED_RADIUS[r];
        state.ringBendX[r] = (int8_t) roundf(state.bendX);
        state.ringBendY[r] = (int8_t) roundf(state.bendY);
        state.ringDrawn[r] = (int16_t) state.ringRadius[r];
    }
}

void WarpEffect::drawStreak(const WarpState &state, int i, uint32_t color) {
    int inner = state.starInner[i];
    int outer = state.starOuter[i];
    int x1 = CENTRE + bendAt(state.starBendX[i], inner) + state.starDirX[i] * inner / 127;
    int y1 = CENTRE + bendAt(state.starBendY[i], inner) + state.starDirY[i] * inner / 127;
    int x2 = CENTRE + bendAt(state.starBendX[i], outer) + state.starDirX[i] * outer / 127;
    int y2 = CENTRE + bendAt(state.starBendY[i], outer) + state.starDirY[i] * outer / 127;
    if (x1 == x2 && y1 == y2) {
        m_manager.fillRect(x1, y1, 1, 1, color);
    } else {
        m_manager.drawLine(x1, y1, x2, y2, color);
    }
}

void WarpEffect::drawRing(const WarpState &state, int ring, uint32_t color) {
    int radius = state.ringDrawn[ring];
    m_manager.drawCircle(CENTRE + bendAt(state.ringBendX[ring], radius), CENTRE + bendAt(state.ringBendY[ring], radius), radius, color);
}

// Everything is redrawn every frame, moved or not: erasing whatever did move cuts across the rest.
void WarpEffect::drawAll(WarpState &state, uint32_t color) {
    for (int r = 0; r < WarpState::NUM_RINGS; r++) {
        state.ringDrawn[r] = (int16_t) state.ringRadius[r];
        drawRing(state, r, ringColor(color, state.ringDrawn[r]));
    }
    for (int i = 0; i < WarpState::NUM_STARS; i++) {
        drawStreak(state, i, starColor(state.starOuter[i]));
    }
}

void WarpEffect::draw(WarpState &state, uint32_t color, float dt, bool fullRedraw) {
    if (fullRedraw) {
        seed(state);
        m_manager.fillScreen(TFT_BLACK);
        drawAll(state, color);
        return;
    }

    state.turnTimer -= dt;
    if (state.turnTimer <= 0) {
        newTurn(state);
    }
    float swing = min(dt * TURN_RATE, 1.0f);
    state.bendX += (state.bendTargetX - state.bendX) * swing;
    state.bendY += (state.bendTargetY - state.bendY) * swing;

    for (int r = 0; r < WarpState::NUM_RINGS; r++) {
        state.ringRadius[r] += speedAt(state.ringRadius[r]) * dt;
        bool gone = state.ringRadius[r] > EDGE_RADIUS;
        if (gone || (int16_t) state.ringRadius[r] != state.ringDrawn[r]) {
            drawRing(state, r, TFT_BLACK);
        }
        if (gone) {
            state.ringRadius[r] = RING_START_RADIUS;
            state.ringBendX[r] = (int8_t) roundf(state.bendX);
            state.ringBendY[r] = (int8_t) roundf(state.bendY);
        }
    }

    for (int i = 0; i < WarpState::NUM_STARS; i++) {
        state.starRadius[i] += speedAt(state.starRadius[i]) * dt;
        uint8_t inner, outer;
        streakFor(state.starRadius[i], inner, outer);
        if (inner > EDGE_RADIUS) {
            drawStreak(state, i, TFT_BLACK);
            spawnStar(state, i, STAR_SPAWN_MIN_RADIUS, STAR_SPAWN_MAX_RADIUS);
        } else if (inner != state.starInner[i] || outer != state.starOuter[i]) {
            drawStreak(state, i, TFT_BLACK);
            state.starInner[i] = inner;
            state.starOuter[i] = outer;
        }
    }

    drawAll(state, color);
}
