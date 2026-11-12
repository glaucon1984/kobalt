#pragma once
#include <climits>

namespace kobalt::pocketbook {
// The firmware's timed power-off is a hard timer (an RTC alarm) that libinkview arms inside the
// foreground app's own process, from the Settings timeout and the time of the last activity it
// shares with the firmware's monitor. A device in standby is woken by it like by a press: the
// app gets EVT_FOREGROUND, and about 1.5 s later the timer fires in the app's event loop and
// asks the firmware to power off (EVT_BACKGROUND, then the power-off logo). Seconds until that
// timer is due, from libinkview's own state (ivstate.powerofftm, poweroffenable, and the last
// activity behind ivmpc, looked up by symbol at the field offsets of firmware 6.11's library and
// checked for plausibility); INT_MIN when unreadable, implausible, or the timed power-off is off.
// At a wake the firmware's timer caused, it is zero or less; at a press it is far off.
int secondsToTimedPowerOff();
// The same figure as a moment on the firmware's clock (hw_boottime seconds, which count the
// time asleep), to be taken at the standby, and the seconds left to such a moment at a later
// wake. The figure itself cannot be trusted at that wake: the last activity is shared with the
// rest of the firmware, and something moves it some ten minutes into a standby without arming
// the timer again (seen with the one-hour timeout: the timer fired an hour after the standby as
// armed, while the figure read 610 to 615 s). LONG_MIN and INT_MIN when not known.
long timedPowerOffMoment();
int secondsToMoment(long moment);
// The frontlight is switched by the firmware's monitor from a shared "enabled" flag and level
// (SetFrontlightEnabled / SetFrontlightState send it the message; the flag's message goes only
// when the flag changes, the level's always). Whether it is on now, and its level (0..100).
bool frontlightEnabled();
int frontlightLevel();
// Turns it on or off through the firmware; the level is kept.
void setFrontlightEnabled(bool enabled);
void setFrontlightLevel(int level);
// The light as Kobalt wants it: a level above zero switches the light on at that level, zero
// switches it off (the firmware's flag follows, so its own controls agree).
void applyFrontlight(int level);
// The firmware's automatic brightness: a light sensor, or the schedule in
// system/config/frontlight/autobrightness.cfg (support_fl_time in /ebrmain/config/device.cfg),
// run by the firmware itself from the FL_MODE_AUTO_BRIGHTNESS bit of the shared light state,
// through libinkview's hw_*_frontlight_auto calls (inkinternal.h only, looked up at run time;
// a firmware without them has no option). While it is on, the firmware stores a level set by
// the app without applying it, so a manual level must switch it off first.
bool hasAutomaticBrightness();
bool automaticBrightness();
// Returns whether the firmware reports the new state afterwards.
bool setAutomaticBrightness(bool enabled);
// The schedule holds back warmth too: SetFrontlightColor stores the colour in the same shared state
// and sends the same message as a level, and the monitor applies neither while the schedule runs
// (seen: a warmth swipe took effect only after the finger lifted, live once a brightness swipe had
// switched the schedule off). A warmth change lifts the schedule for its duration and resumes it
// afterwards. The shared level is the schedule's current one, so the brightness does not move.
// While lifted, automaticBrightness() still reports it on (the panel, the standby record), and
// setAutomaticBrightness() ends the lift. Both return whether they changed anything.
bool liftBrightnessSchedule();
bool resumeBrightnessSchedule();
// Kobalt switches the light off at this level: both flag and level are set to off/0 (the
// brightness schedule, which would hold the level, is switched off too), and the level and the
// schedule are recorded on disk, because the firmware keeps the off state across a power-off
// and Kobalt's next start has to switch the light back on (restoreStandbyLight). -1 when the
// light was off already. At a standby the firmware has usually switched the light off itself
// by the time Kobalt runs (nothing to do then); at the wake its timed power-off causes it has
// lit the panel again, and this is what keeps the power-off dark.
int darkenForStandby();
// Switches the light back on at the recorded level and forgets the record. False when none.
bool restoreStandbyLight(const char *why);
}
