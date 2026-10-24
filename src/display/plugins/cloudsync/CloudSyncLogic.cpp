#include "CloudSyncLogic.h"

namespace cloudsync {

bool isTrustedEpoch(uint32_t epochSeconds, bool hasSntpSynced) { return hasSntpSynced && epochSeconds >= MIN_TRUSTED_EPOCH; }

uint32_t correctedStartEpoch(uint32_t nowEpochSeconds, uint32_t nowMillis, uint32_t startMillis) {
    // Unsigned subtraction stays correct across the 49-day millis() wrap.
    const uint32_t elapsedMs = nowMillis - startMillis;
    return nowEpochSeconds - (elapsedMs + 500) / 1000;
}

ShotAction decideShotAction(const ShotFacts &facts) {
    if (!facts.doesFileExist || !facts.isHeaderValid) {
        return ShotAction::Skip;
    }
    if (facts.startEpochSeconds >= MIN_TRUSTED_EPOCH) {
        return ShotAction::Upload;
    }
    if (!facts.isClockTrusted || facts.hasTimestampFixPending) {
        return ShotAction::Wait;
    }
    return ShotAction::Skip;
}

ResponseOutcome classifyResponse(int httpStatus) {
    if (httpStatus >= 200 && httpStatus < 300) {
        return ResponseOutcome::Acknowledged;
    }
    if (httpStatus == 401 || httpStatus == 403) {
        return ResponseOutcome::AuthenticationFailed;
    }
    // 404 is a wrong base URL or a Worker without this route: fixable, so it is retried.
    if (httpStatus == 400 || httpStatus == 413 || httpStatus == 422) {
        return ResponseOutcome::RejectedPermanently;
    }
    return ResponseOutcome::RetryLater;
}

uint32_t retryDelayMs(uint32_t failureCount, float jitterUnit) {
    uint32_t delayMs = INITIAL_RETRY_DELAY_MS;
    for (uint32_t i = 1; i < failureCount && delayMs < MAX_RETRY_DELAY_MS; i++) {
        delayMs *= 2;
    }
    if (delayMs > MAX_RETRY_DELAY_MS) {
        delayMs = MAX_RETRY_DELAY_MS;
    }
    if (jitterUnit < 0.0f) {
        jitterUnit = 0.0f;
    } else if (jitterUnit >= 1.0f) {
        jitterUnit = 0.999999f;
    }
    const float factor = 1.0f + RETRY_JITTER_FRACTION * (2.0f * jitterUnit - 1.0f);
    return static_cast<uint32_t>(static_cast<float>(delayMs) * factor);
}

uint32_t reconcileCursor(int64_t storedNextShotId, uint32_t historyIndex) {
    if (storedNextShotId < 0 || storedNextShotId > historyIndex) {
        return historyIndex;
    }
    return static_cast<uint32_t>(storedNextShotId);
}

} // namespace cloudsync
