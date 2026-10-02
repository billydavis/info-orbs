#ifndef MATRIX_EFFECT_H
#define MATRIX_EFFECT_H

#include "ScreenManager.h"

// "Digital rain": columns of glyphs falling down the screen, each drop a white head trailing a
// colored tail that fades out at its end.
//
// No default member initializers: this lives in ScreensaverState's union with the other effects'
// states, and seed() sets every field before anything reads it.
struct MatrixState {
    static const int COLS = 20;
    static const int ROWS = 15;
    // Which glyph each cell last drew, so a cell can be redrawn in another color as the drop moves on.
    uint8_t glyph[COLS][ROWS];
    // Row of each column's drop's head, -1 while the column waits for its next drop. Runs on past
    // the bottom row until the tail has left the screen too.
    int8_t head[COLS];
    uint8_t length[COLS]; // cells from head to end of tail
    uint16_t stepMs[COLS]; // time per row - each drop falls at its own speed
    unsigned long nextStepMs[COLS];
};

class MatrixEffect {
public:
    static const uint32_t DEFAULT_COLOR = TFT_GREEN;

    explicit MatrixEffect(ScreenManager &manager);

    // Draws to whichever screen is currently selected; `color` is the rain's. fullRedraw clears the
    // screen and starts the rain afresh; otherwise this moves each drop that's due down a row,
    // drawing only the few cells whose glyph or color changed.
    void draw(MatrixState &state, uint32_t color, unsigned long now, bool fullRedraw);

private:
    void seed(MatrixState &state, unsigned long now);
    void scheduleDrop(MatrixState &state, int col, unsigned long now, int maxDelayMs);
    void stepDrop(MatrixState &state, int col, uint32_t color, unsigned long now);
    void drawGlyph(const MatrixState &state, int col, int row, uint32_t color);
    void clearCell(int col, int row);

    ScreenManager &m_manager;
};
#endif // MATRIX_EFFECT_H
