#include "TimeSync.h"

#include <atomic>
#include <ctime>
#include <display/plugins/cloudsync/CloudSyncLogic.h>
#include <esp_log.h>
#include <esp_sntp.h>

namespace {
constexpr const char *LOG_TAG = "TimeSync";

// Written from the lwIP (tcpip) task, read from every task that timestamps something.
std::atomic<bool> hasSntpSynced{false};

void onSntpSync(struct timeval *tv) {
    const bool wasSynced = hasSntpSynced.exchange(true);
    if (!wasSynced) {
        ESP_LOGI(LOG_TAG, "Clock synced via SNTP: epoch=%ld", static_cast<long>(tv != nullptr ? tv->tv_sec : time(nullptr)));
    }
}
} // namespace

namespace TimeSync {

void begin() { sntp_set_time_sync_notification_cb(onSntpSync); }

bool requestSync() {
    const bool isRestarted = sntp_restart();
    if (!isRestarted) {
        ESP_LOGW(LOG_TAG, "SNTP sync requested but SNTP is not running");
    }
    return isRestarted;
}

bool isClockTrusted() { return cloudsync::isTrustedEpoch(static_cast<uint32_t>(time(nullptr)), hasSntpSynced.load()); }

uint32_t trustedNow() {
    const auto now = static_cast<uint32_t>(time(nullptr));
    return cloudsync::isTrustedEpoch(now, hasSntpSynced.load()) ? now : 0;
}

} // namespace TimeSync
