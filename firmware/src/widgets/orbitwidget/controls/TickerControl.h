#ifndef TICKER_CONTROL_H
#define TICKER_CONTROL_H

#include "ScreenManager.h"
#include "stockwidget/StockDataModel.h"

// Renders one stock/crypto/forex ticker card to whichever screen is selected - extracted from
// StockWidget so the same rendering can be reused by a widget that only wants one ticker on one
// screen (e.g. OrbIt) rather than owning a whole row of tickers. Covers crypto/forex too: a
// StockDataModel's symbol is just whatever twelvedata-format string it was given (e.g. "BTC/USD"),
// there's no separate crypto rendering path.
class TickerControl {
public:
    explicit TickerControl(ScreenManager &manager);

    void draw(int displayIndex, StockDataModel &stock, uint32_t backgroundColor, uint32_t textColor);

private:
    ScreenManager &m_manager;
};
#endif // TICKER_CONTROL_H
