#ifndef FEATURE_DEFAULTS_H
#define FEATURE_DEFAULTS_H

// Defaults for the build-time feature switches, for a config.h that doesn't set them (one written
// by hand from an older template, say) - everything defaults to built in, which is how the firmware
// behaved before these switches existed. setup/setup.py writes every one of them explicitly.
// Switching something off drops its code (and any images it alone uses) from the firmware.

// Widgets that used to always be built
#ifndef USE_CLOCK_WIDGET
    #define USE_CLOCK_WIDGET true
#endif
#ifndef USE_WEATHER_WIDGET
    #define USE_WEATHER_WIDGET true
#endif

// OrbIt controls - only matter with ORBIT_WIDGET_ENABLED. "blank" is always built.
#ifndef ORBIT_CONTROL_TIME
    #define ORBIT_CONTROL_TIME true
#endif
#ifndef ORBIT_CONTROL_ANALOG_CLOCK
    #define ORBIT_CONTROL_ANALOG_CLOCK true
#endif
#ifndef ORBIT_CONTROL_GAUGE
    #define ORBIT_CONTROL_GAUGE true
#endif
#ifndef ORBIT_CONTROL_SYS_MONITOR
    #define ORBIT_CONTROL_SYS_MONITOR true
#endif
#ifndef ORBIT_CONTROL_SCREENSAVER
    #define ORBIT_CONTROL_SCREENSAVER true
#endif
#ifndef ORBIT_CONTROL_COUNTDOWN
    #define ORBIT_CONTROL_COUNTDOWN true
#endif
#ifndef ORBIT_CONTROL_WEATHER
    #define ORBIT_CONTROL_WEATHER true
#endif
#ifndef ORBIT_CONTROL_TICKER
    #define ORBIT_CONTROL_TICKER true
#endif
#ifndef ORBIT_CONTROL_CUSTOM
    #define ORBIT_CONTROL_CUSTOM true
#endif

// The screensaver control's effects - only matter with ORBIT_CONTROL_SCREENSAVER
#ifndef ORBIT_EFFECT_ASTEROIDS
    #define ORBIT_EFFECT_ASTEROIDS true
#endif
#ifndef ORBIT_EFFECT_MATRIX
    #define ORBIT_EFFECT_MATRIX true
#endif
#ifndef ORBIT_EFFECT_WARP
    #define ORBIT_EFFECT_WARP true
#endif
#ifndef ORBIT_EFFECT_ORRERY
    #define ORBIT_EFFECT_ORRERY true
#endif
#ifndef ORBIT_EFFECT_RADAR
    #define ORBIT_EFFECT_RADAR true
#endif

#if ORBIT_CONTROL_SCREENSAVER && !(ORBIT_EFFECT_ASTEROIDS || ORBIT_EFFECT_MATRIX || ORBIT_EFFECT_WARP || ORBIT_EFFECT_ORRERY || ORBIT_EFFECT_RADAR)
    #error "ORBIT_CONTROL_SCREENSAVER needs at least one ORBIT_EFFECT_* set to true (or set ORBIT_CONTROL_SCREENSAVER to false)"
#endif

#endif // FEATURE_DEFAULTS_H
