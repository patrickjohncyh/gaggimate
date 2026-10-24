#ifndef CLOUDSYNCLOGIC_H
#define CLOUDSYNCLOGIC_H

// Pure decision logic for CloudSync: clock trust, shot timestamp correction, the per-shot upload
// decision, server response handling and retry backoff. No Arduino/ESP dependencies so it can be
// unit tested natively (test/test_cloud_sync_logic).

#include <cstdint>

namespace cloudsync {

// Anything earlier (Nov 2023) is a boot-relative or unset clock, never a real time. Matches the
// gaggi Worker, which rejects shots that start earlier.
constexpr uint32_t MIN_TRUSTED_EPOCH = 1700000000;

constexpr uint32_t INITIAL_RETRY_DELAY_MS = 30 * 1000;
constexpr uint32_t MAX_RETRY_DELAY_MS = 30 * 60 * 1000;
// Spreads retries so a fleet (or one device after a long outage) doesn't hit the Worker in lockstep.
constexpr float RETRY_JITTER_FRACTION = 0.2f;

/**
 * Whether a wall-clock reading can be stored as a real UTC time.
 *
 * @param epochSeconds   value of time() at the moment of interest
 * @param hasSntpSynced  SNTP has set the clock at least once since boot
 * @return true only when both hold; the RTC survives soft resets, so a plausible epoch alone is
 *         not proof the clock was ever synced
 *
 * Example: isTrustedEpoch(1792734864, true) == true; isTrustedEpoch(1792734864, false) == false
 */
bool isTrustedEpoch(uint32_t epochSeconds, bool hasSntpSynced);

/**
 * Reconstructs the UTC start of a shot that began before the clock was synced.
 *
 * @param nowEpochSeconds  trusted current UTC time
 * @param nowMillis        millis() read together with nowEpochSeconds
 * @param startMillis      millis() when the shot started (same boot)
 * @return the start time, rounded to the nearest second
 *
 * Example: correctedStartEpoch(1792734900, 50000, 14000) == 1792734864
 */
uint32_t correctedStartEpoch(uint32_t nowEpochSeconds, uint32_t nowMillis, uint32_t startMillis);

/** What the worker does with the shot at the cursor. */
enum class ShotAction : uint8_t {
    Upload,
    Wait, // a timestamp fix may still arrive; retry later without advancing
    Skip, // can never be uploaded; advance the cursor
};

/** Facts about the shot at the cursor, gathered by the plugin from the filesystem. */
struct ShotFacts {
    bool doesFileExist = false;
    bool isHeaderValid = false;
    uint32_t startEpochSeconds = 0;
    bool hasTimestampFixPending = false; // ShotHistory still holds a millis() reference for a shot
    bool isClockTrusted = false;
};

/**
 * Decides whether the shot at the cursor is uploaded, waited for, or skipped.
 *
 * A shot is uploaded only with a trusted UTC start (decision 2026-10-23). An untrusted start is
 * worth waiting for while the clock isn't synced yet or ShotHistory still has a fix queued;
 * otherwise the millis() reference was lost to a reboot and the shot is skipped.
 *
 * Example: decideShotAction({.doesFileExist = false}) == ShotAction::Skip
 */
ShotAction decideShotAction(const ShotFacts &facts);

/** How the worker reacts to a server response. */
enum class ResponseOutcome : uint8_t {
    Acknowledged,         // stored (or already stored); advance
    AuthenticationFailed, // token wrong or revoked; pause until the settings change
    RejectedPermanently,  // the server will never accept this payload; advance, keep the local file
    RetryLater,           // network or server trouble; back off and retry the same shot
};

/**
 * Maps an HTTP status to the worker's reaction.
 *
 * @param httpStatus  response status, or <= 0 when no response arrived (connect/TLS/timeout)
 *
 * Example: classifyResponse(422) == ResponseOutcome::RejectedPermanently
 */
ResponseOutcome classifyResponse(int httpStatus);

/**
 * Delay before retry number `failureCount` (1-based), doubling from 30 s up to 30 min.
 *
 * @param failureCount  consecutive failures so far (>= 1)
 * @param jitterUnit    uniform random value in [0, 1); 0.5 means no jitter
 * @return delay in ms, within ±20 % of the un-jittered value
 *
 * Example: retryDelayMs(1, 0.5f) == 30000; retryDelayMs(20, 0.5f) == 1800000
 */
uint32_t retryDelayMs(uint32_t failureCount, float jitterUnit);

/**
 * Brings a persisted cursor back in line with the firmware's shot counter.
 *
 * @param storedNextShotId  first id not yet handled, or a negative value when no cursor exists
 * @param historyIndex      next id ShotHistory will assign
 * @return the cursor to use: historyIndex on first enable (legacy shots are never sent), and
 *         historyIndex when the counter went backwards after an NVS reset (the reused ids are new
 *         shots the cloud flags as id_reused)
 *
 * Example: reconcileCursor(-1, 171) == 171; reconcileCursor(180, 3) == 3; reconcileCursor(168, 171) == 168
 */
uint32_t reconcileCursor(int64_t storedNextShotId, uint32_t historyIndex);

} // namespace cloudsync

#endif // CLOUDSYNCLOGIC_H
