#ifndef SCREENSAVER_COMMON_H
#define SCREENSAVER_COMMON_H

#include "config_helper.h"

// Geometry every screensaver effect shares - see ScreensaverControl.
namespace Screensaver {
const int CENTRE = SCREEN_SIZE / 2;
// Keeps every moving object's centre within this radius of CENTRE so nothing drifts under the
// round bezel - same margin reasoning as GaugeControl's OUTER_RADIUS.
const int PLAYFIELD_RADIUS = 100;
// Clamp on a frame's elapsed time, so a stall (e.g. a widget switch) doesn't fling anything across the screen.
const float MAX_FRAME_DT = 0.2f;
} // namespace Screensaver
#endif // SCREENSAVER_COMMON_H
