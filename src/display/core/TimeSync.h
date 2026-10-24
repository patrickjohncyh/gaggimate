#ifndef TIMESYNC_H
#define TIMESYNC_H

#include <cstdint>

// Tracks whether the wall clock holds a real UTC time. SNTP is the only source trusted: the RTC
// keeps counting across soft resets, so time() alone can look valid without ever being synced.
namespace TimeSync {

/**
 * Installs the SNTP sync callback. Call once, before SNTP is started.
 *
 * Example: TimeSync::begin(); configTzTime(tz, NTP_SERVER);
 */
void begin();

/**
 * Asks SNTP for an immediate sync, e.g. after the station reconnects, instead of waiting out
 * lwIP's retry backoff.
 *
 * @return false when SNTP was never started (AP-only boot), so there is nothing to restart
 *
 * Example: pluginManager->on("controller:wifi:connect", [](Event const &) { TimeSync::requestSync(); });
 */
bool requestSync();

/**
 * Whether time() can be recorded as a UTC timestamp right now.
 *
 * @return true once SNTP has synced since boot and the clock reads >= cloudsync::MIN_TRUSTED_EPOCH
 *
 * Example: header.startEpoch = time(nullptr); isStartTrusted = TimeSync::isClockTrusted();
 */
bool isClockTrusted();

/**
 * The current UTC time in seconds, or 0 when the clock isn't trusted.
 *
 * Example: if (uint32_t now = TimeSync::trustedNow()) { lastSuccessAt = now; }
 */
uint32_t trustedNow();

} // namespace TimeSync

#endif // TIMESYNC_H
