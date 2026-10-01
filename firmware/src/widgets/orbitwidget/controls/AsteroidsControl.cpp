#include "AsteroidsControl.h"

#include "config_helper.h"
#include <math.h>

namespace {
const int CENTRE = SCREEN_SIZE / 2;
// Keeps every moving object's centre within this radius of CENTRE so nothing drifts under the
// round bezel - same margin reasoning as GaugeControl's OUTER_RADIUS.
const int PLAYFIELD_RADIUS = 100;
const int PLANET_RADIUS = 24;
const uint32_t PLANET_COLOR = 0xFD20; // orange (RGB565)
const uint32_t PLANET_RING_COLOR = TFT_SILVER;
const uint32_t LABEL_COLOR = TFT_BLACK;
const int LABEL_FONT_SIZE = 13;
// When the planet moves, a black ring this much wider than the planet is painted around its new
// position to wipe whatever the old position left sticking out. It has to cover more than the
// planet's own per-frame travel (under 3px even at MAX_FRAME_DT): the "Orb-It" label can render a
// few pixels wider than the planet's diameter, and its anti-aliased edge pixels (black blended
// into the orange fill) would otherwise be left behind as a trailing streak.
const int PLANET_ERASE_MARGIN = 8;
const uint32_t ROCK_COLOR = TFT_LIGHTGREY;
const float MAX_FRAME_DT = 0.2f; // clamp so a stall (e.g. first frame, widget switch) doesn't fling anything across the screen
// Pixels per second, per star tier - brighter stars read as nearer, so they drift a little faster.
const float STAR_DRIFT_SPEED[] = {1.2f, 0.8f, 0.5f};
const uint32_t STAR_COLOR[] = {TFT_WHITE, TFT_LIGHTGREY, TFT_DARKGREY};

// A twinkling star swaps to the far end of the brightness range so the blink reads at any tier.
uint32_t starColor(const AsteroidsState &state, int i) {
    if (i == state.blinkStar) {
        return state.starTier[i] == 2 ? TFT_WHITE : TFT_DARKGREY;
    }
    return STAR_COLOR[state.starTier[i]];
}

bool underPlanet(int x, int y, int planetX, int planetY) {
    int dx = x - planetX;
    int dy = y - planetY;
    return dx * dx + dy * dy <= (PLANET_RADIUS + 1) * (PLANET_RADIUS + 1);
}

bool withinBox(int x, int y, int boxX, int boxY, int halfSize) {
    return abs(x - boxX) <= halfSize && abs(y - boxY) <= halfSize;
}

// Bounces a circular object of `radius` off the play field's boundary, reflecting velocity about
// the radial normal (elastic bounce off the inside of a circle).
void stepAndBounce(float &x, float &y, float &vx, float &vy, float radius, float dt) {
    x += vx * dt;
    y += vy * dt;

    float dx = x - CENTRE;
    float dy = y - CENTRE;
    float dist = sqrtf(dx * dx + dy * dy);
    float limit = PLAYFIELD_RADIUS - radius;
    if (dist > limit && dist > 0.001f) {
        float nx = dx / dist;
        float ny = dy / dist;
        float dot = vx * nx + vy * ny;
        vx -= 2.0f * dot * nx;
        vy -= 2.0f * dot * ny;
        float overlap = dist - limit;
        x -= nx * overlap;
        y -= ny * overlap;
    }
}
} // namespace

AsteroidsControl::AsteroidsControl(ScreenManager &manager) : m_manager(manager) {
}

void AsteroidsControl::seed(AsteroidsState &state) {
    for (int i = 0; i < AsteroidsState::NUM_STARS; i++) {
        // Random point anywhere in the square framebuffer is fine for stars - unlike the planet/
        // rocks they're single pixels, so a few landing outside the visible circle just get
        // clipped by the physical bezel like everything else near the corners already does.
        state.starX[i] = random(0, SCREEN_SIZE);
        state.starY[i] = random(0, SCREEN_SIZE);
        state.starTier[i] = random(0, 3);
    }
    float driftAngle = random(0, 360) * DEG_TO_RAD;
    state.starDriftX = cosf(driftAngle);
    state.starDriftY = sinf(driftAngle);
    state.blinkStar = -1;
    state.nextBlinkMs = millis() + random(500, 2500);

    float planetAngle = random(0, 360) * DEG_TO_RAD;
    float planetSpeed = 14.0f;
    float planetStartRadius = 30.0f;
    state.planetX = CENTRE + planetStartRadius * cosf(planetAngle);
    state.planetY = CENTRE + planetStartRadius * sinf(planetAngle);
    float planetDir = random(0, 360) * DEG_TO_RAD;
    state.planetVX = planetSpeed * cosf(planetDir);
    state.planetVY = planetSpeed * sinf(planetDir);
    state.lastPlanetX = (int) state.planetX;
    state.lastPlanetY = (int) state.planetY;

    for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
        float angle = random(0, 360) * DEG_TO_RAD;
        float radiusFromCentre = random(30, PLAYFIELD_RADIUS - 10);
        state.rockX[r] = CENTRE + radiusFromCentre * cosf(angle);
        state.rockY[r] = CENTRE + radiusFromCentre * sinf(angle);
        float dir = random(0, 360) * DEG_TO_RAD;
        float speed = random(18, 34);
        state.rockVX[r] = speed * cosf(dir);
        state.rockVY[r] = speed * sinf(dir);

        // A fixed, once-only jittered hexagon so each rock keeps a consistent "irregular rock"
        // silhouette as it drifts, rather than a perfect hexagon - it never rotates, only translates.
        int baseRadius = random(6, 10);
        int maxOffset = 0;
        for (int p = 0; p < AsteroidsState::ROCK_POINTS; p++) {
            float pointAngle = (360.0f / AsteroidsState::ROCK_POINTS) * p * DEG_TO_RAD;
            int jitter = random(-3, 4);
            int pointRadius = baseRadius + jitter;
            int ox = (int) roundf(pointRadius * cosf(pointAngle));
            int oy = (int) roundf(pointRadius * sinf(pointAngle));
            state.rockOffsetX[r][p] = (int8_t) ox;
            state.rockOffsetY[r][p] = (int8_t) oy;
            maxOffset = max(maxOffset, max(abs(ox), abs(oy)));
        }
        state.rockBoundRadius[r] = maxOffset + 1;
        state.lastRockX[r] = (int) state.rockX[r];
        state.lastRockY[r] = (int) state.rockY[r];
    }

    state.lastFrameMs = millis();
}

void AsteroidsControl::drawStars(const AsteroidsState &state) {
    for (int i = 0; i < AsteroidsState::NUM_STARS; i++) {
        m_manager.fillRect((int) state.starX[i], (int) state.starY[i], 1, 1, starColor(state, i));
    }
}

void AsteroidsControl::updateStars(AsteroidsState &state, float dt, unsigned long now, int planetX, int planetY, bool planetMoved, const bool *rockMoved) {
    // At most one star changes twinkle state per frame: the current one ends, or a new one starts.
    int blinkChanged = -1;
    if (state.blinkStar >= 0) {
        if (now >= state.blinkUntilMs) {
            blinkChanged = state.blinkStar;
            state.blinkStar = -1;
            state.nextBlinkMs = now + random(500, 2500);
        }
    } else if (now >= state.nextBlinkMs) {
        state.blinkStar = random(0, AsteroidsState::NUM_STARS);
        state.blinkUntilMs = now + random(120, 300);
        blinkChanged = state.blinkStar;
    }

    for (int i = 0; i < AsteroidsState::NUM_STARS; i++) {
        int oldX = (int) state.starX[i];
        int oldY = (int) state.starY[i];
        float step = STAR_DRIFT_SPEED[state.starTier[i]] * dt;
        state.starX[i] += state.starDriftX * step;
        state.starY[i] += state.starDriftY * step;
        if (state.starX[i] < 0) {
            state.starX[i] += SCREEN_SIZE;
        } else if (state.starX[i] >= SCREEN_SIZE) {
            state.starX[i] -= SCREEN_SIZE;
        }
        if (state.starY[i] < 0) {
            state.starY[i] += SCREEN_SIZE;
        } else if (state.starY[i] >= SCREEN_SIZE) {
            state.starY[i] -= SCREEN_SIZE;
        }
        int x = (int) state.starX[i];
        int y = (int) state.starY[i];

        bool moved = x != oldX || y != oldY;
        bool redraw = moved || i == blinkChanged || (planetMoved && withinBox(x, y, planetX, planetY, PLANET_RADIUS + PLANET_ERASE_MARGIN));
        for (int r = 0; r < AsteroidsState::NUM_ROCKS && !redraw; r++) {
            redraw = rockMoved[r] && withinBox(x, y, state.lastRockX[r], state.lastRockY[r], state.rockBoundRadius[r]);
        }

        // Stars sit behind the planet, so never touch a pixel it covers - the planet repaints
        // itself only when it has to, and wouldn't clean up after a star drawn on top of it.
        if (moved && !underPlanet(oldX, oldY, planetX, planetY)) {
            m_manager.fillRect(oldX, oldY, 1, 1, TFT_BLACK);
        }
        if (redraw && !underPlanet(x, y, planetX, planetY)) {
            m_manager.fillRect(x, y, 1, 1, starColor(state, i));
        }
    }
}

void AsteroidsControl::drawPlanet(int x, int y) {
    m_manager.fillCircle(x, y, PLANET_RADIUS, PLANET_COLOR);
    m_manager.drawCircle(x, y, PLANET_RADIUS - 7, PLANET_RING_COLOR);
    m_manager.drawString("Orb-It", x, y, LABEL_FONT_SIZE, Align::MiddleCenter, LABEL_COLOR, PLANET_COLOR);
}

void AsteroidsControl::drawRock(const AsteroidsState &state, int rockIndex, int x, int y, uint32_t color) {
    for (int p = 0; p < AsteroidsState::ROCK_POINTS; p++) {
        int nextP = (p + 1) % AsteroidsState::ROCK_POINTS;
        int x1 = x + state.rockOffsetX[rockIndex][p];
        int y1 = y + state.rockOffsetY[rockIndex][p];
        int x2 = x + state.rockOffsetX[rockIndex][nextP];
        int y2 = y + state.rockOffsetY[rockIndex][nextP];
        m_manager.drawLine(x1, y1, x2, y2, color);
    }
}

void AsteroidsControl::draw(int displayIndex, AsteroidsState &state, bool fullRedraw) {
    m_manager.selectScreen(displayIndex);

    if (fullRedraw || !state.initialized) {
        seed(state);
        m_manager.fillScreen(TFT_BLACK);
        drawStars(state);
        drawPlanet((int) state.planetX, (int) state.planetY);
        for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
            drawRock(state, r, (int) state.rockX[r], (int) state.rockY[r], ROCK_COLOR);
        }
        state.initialized = true;
        return;
    }

    unsigned long now = millis();
    float dt = (now - state.lastFrameMs) / 1000.0f;
    if (dt > MAX_FRAME_DT) {
        dt = MAX_FRAME_DT;
    }
    state.lastFrameMs = now;

    stepAndBounce(state.planetX, state.planetY, state.planetVX, state.planetVY, PLANET_RADIUS, dt);
    int planetX = (int) state.planetX;
    int planetY = (int) state.planetY;
    bool planetMoved = planetX != state.lastPlanetX || planetY != state.lastPlanetY;
    bool planetDirty = planetMoved;

    // Painted back to front: erase, then stars, planet, rocks. Rocks are erased by blacking out
    // just their old outline rather than a filled box, so one crossing the planet no longer
    // punches a black square into it - the planet only needs repainting to heal those few lines.
    bool rockMoved[AsteroidsState::NUM_ROCKS];
    for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
        stepAndBounce(state.rockX[r], state.rockY[r], state.rockVX[r], state.rockVY[r], (float) state.rockBoundRadius[r], dt);
        rockMoved[r] = (int) state.rockX[r] != state.lastRockX[r] || (int) state.rockY[r] != state.lastRockY[r];
        if (rockMoved[r]) {
            drawRock(state, r, state.lastRockX[r], state.lastRockY[r], TFT_BLACK);
            if (withinBox(state.lastRockX[r], state.lastRockY[r], state.lastPlanetX, state.lastPlanetY, PLANET_RADIUS + PLANET_ERASE_MARGIN + state.rockBoundRadius[r])) {
                planetDirty = true;
            }
        }
    }

    if (planetMoved) {
        // The new disc covers most of the old one, so only the sliver left sticking out (plus any
        // label overhang) needs erasing - far fewer pixels, and far less flicker, than blanking
        // the whole planet first. The ring overlaps the disc's edge by a pixel so no gap is left.
        m_manager.drawArc(planetX, planetY, PLANET_RADIUS + PLANET_ERASE_MARGIN, PLANET_RADIUS - 1, 0, 360, TFT_BLACK, TFT_BLACK, false);
    }

    updateStars(state, dt, now, planetX, planetY, planetMoved, rockMoved);

    if (planetDirty) {
        drawPlanet(planetX, planetY);
    }
    state.lastPlanetX = planetX;
    state.lastPlanetY = planetY;

    // Every rock is redrawn every frame, moved or not - the planet's erase ring or repaint above
    // may have clipped one that's sitting still.
    for (int r = 0; r < AsteroidsState::NUM_ROCKS; r++) {
        state.lastRockX[r] = (int) state.rockX[r];
        state.lastRockY[r] = (int) state.rockY[r];
        drawRock(state, r, state.lastRockX[r], state.lastRockY[r], ROCK_COLOR);
    }
}
