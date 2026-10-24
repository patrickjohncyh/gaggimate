#ifndef CLOUDSYNCPLUGIN_H
#define CLOUDSYNCPLUGIN_H

#include <Arduino.h>
#include <FS.h>
#include <display/core/Plugin.h>
#include <display/plugins/cloudsync/CloudSyncLogic.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <map>
#include <mutex>
#include <vector>

class WebSocketHandler;

/**
 * Uploads every completed shot to the gaggi Worker (Coffee Coach) exactly once.
 *
 * The local history stays the source of truth: a persisted cursor (/h/.cloudsync) names the first
 * shot id not yet handled, and it only moves after the Worker acknowledges a shot or rejects it
 * for good. All network work runs on a dedicated low-priority task that pauses while brewing or
 * updating, so brewing behaves the same with the Worker unreachable. Design:
 * docs/dev-notes/plan-cloud-sync.md §8.
 */
class CloudSyncPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    enum class SyncState : uint8_t {
        Starting,
        NotConfigured,
        Offline,
        WaitingForClock,
        Busy,
        WaitingForTimestamp,
        Uploading,
        Idle,
        RetryScheduled,
        AuthenticationFailed
    };

    struct Config {
        String baseUrl;
        String token;
        uint32_t generation = 0; // bumped on every settings change, clears an auth pause
        bool isComplete() const { return !baseUrl.isEmpty() && !token.isEmpty(); }
    };

    struct Status {
        SyncState state = SyncState::Starting;
        uint32_t pendingShots = 0;
        uint32_t pendingNotes = 0;
        uint32_t lastSuccessEpoch = 0;
        uint32_t lastUploadedShotId = 0;
        bool hasUploadedShot = false;
        int lastHttpStatus = 0;
        String lastErrorCode;
        uint32_t consecutiveFailures = 0;
    };

    static void workerTask(void *arg);
    void runWorker();
    // Returns how long the worker may sleep before the next pass.
    uint32_t runSyncPass(const Config &config);
    uint32_t sleepBeforeNextPassMs() const;
    bool syncPendingShots(const Config &config);
    bool syncPendingNotes(const Config &config);

    cloudsync::ShotFacts readShotFacts(uint32_t shotId);
    cloudsync::ResponseOutcome uploadShot(const Config &config, uint32_t shotId);
    cloudsync::ResponseOutcome uploadNotes(const Config &config, uint32_t shotId, bool &isShotUnknown);
    bool readShotLog(uint32_t shotId, uint8_t *&bytes, size_t &size);
    String findProfileSnapshot(uint32_t shotId);
    void forgetProfileSnapshot(uint32_t shotId);

    void captureProfileSnapshot();
    void recordFailure(cloudsync::ResponseOutcome outcome, int httpStatus, const String &errorCode);
    void recordSuccess(int httpStatus);
    void setState(SyncState state);

    int64_t loadCursor();
    bool storeCursor(uint32_t nextShotId);

    void registerWsHandlers(WebSocketHandler *ws);
    static const char *stateName(SyncState state);
    void wakeWorker();

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    fs::FS *fs = nullptr;
    TaskHandle_t taskHandle = nullptr;

    // Shared between the main loop, event emitters (any task), the AsyncTCP task and the worker.
    std::mutex mutex;
    Config config;
    bool isConfigReloadRequested = true;
    Status status;
    std::map<uint32_t, String> profileSnapshots; // brew-time profile JSON by shot id
    std::vector<uint32_t> pendingNotesShotIds;

    // Worker task only.
    uint32_t nextShotId = 0;
    bool isCursorLoaded = false;
    uint32_t retryAtMillis = 0;
    bool isRetryScheduled = false;
    uint32_t passGeneration = 0;
    uint32_t authFailedGeneration = 0;
    bool isAuthPaused = false;
};

#endif // CLOUDSYNCPLUGIN_H
