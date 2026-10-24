#include "CloudSyncPlugin.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <algorithm>
#include <display/core/Controller.h>
#include <display/core/TimeSync.h>
#include <display/core/process/BrewProcess.h>
#include <display/models/profile.h>
#include <display/models/shot_log_format.h>
#include <display/plugins/ShotHistoryPlugin.h>
#include <display/plugins/WebSocketHandler.h>
#include <display/plugins/cloudsync/CloudSyncHttp.h>
#include <display/util/PsramAllocator.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <version.h>

using cloudsync::ResponseOutcome;
using cloudsync::ShotAction;

namespace {
constexpr const char *LOG_TAG = "CloudSync";
constexpr const char *CURSOR_PATH = "/h/.cloudsync";
constexpr const char *CURSOR_TEMP_PATH = "/h/.cloudsync.tmp";
constexpr int CURSOR_FORMAT_VERSION = 1;
constexpr int CONTRACT_VERSION = 1;

// mbedTLS runs the handshake on the calling task's stack.
constexpr uint32_t WORKER_STACK_BYTES = 12 * 1024;
constexpr UBaseType_t WORKER_PRIORITY = 1;
constexpr BaseType_t WORKER_CORE = 0;
// Catches missed wake-ups (e.g. Wi-Fi returning without an event) and re-checks busy/offline states.
constexpr uint32_t POLL_INTERVAL_MS = 60 * 1000;

// The Worker caps request bodies at 64 KiB; base64 grows the log by 4/3, leaving room for notes
// and the profile. 40 KiB of samples is a shot of over five minutes.
constexpr size_t MAX_SHOT_LOG_BYTES = 40 * 1024;
constexpr size_t MAX_REQUEST_BYTES = 63 * 1024;
constexpr size_t MAX_NOTES_FILE_BYTES = 8 * 1024;
constexpr size_t MAX_PROFILE_SNAPSHOTS = 8;
constexpr size_t MAX_PENDING_NOTES = 16;

String padShotId(uint32_t shotId) {
    char padded[12];
    snprintf(padded, sizeof(padded), "%06u", static_cast<unsigned>(shotId));
    return String(padded);
}

String shotLogPath(uint32_t shotId) { return "/h/" + padShotId(shotId) + ".slog"; }
String notesPath(uint32_t shotId) { return "/h/" + padShotId(shotId) + ".json"; }

uint8_t *allocateLarge(size_t size) {
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return buffer != nullptr ? buffer : static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_DEFAULT));
}

String sha256Hex(const uint8_t *bytes, size_t size) {
    uint8_t digest[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), bytes, size, digest) != 0) {
        return "";
    }
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return String(hex);
}

bool isStationConnected() { return (WiFi.getMode() & WIFI_MODE_STA) != 0 && WiFi.status() == WL_CONNECTED; }

float randomUnit() { return static_cast<float>(esp_random() >> 8) / static_cast<float>(1u << 24); }

// Serializes into PSRAM; the caller frees the buffer with heap_caps_free.
char *serializeToBuffer(JsonDocument &doc, size_t &size) {
    size = measureJson(doc);
    char *buffer = reinterpret_cast<char *>(allocateLarge(size + 1));
    if (buffer != nullptr) {
        serializeJson(doc, buffer, size + 1);
    }
    return buffer;
}
} // namespace

void CloudSyncPlugin::setup(Controller *c, PluginManager *pm) {
    controller = c;
    pluginManager = pm;
    fs = controller->isSDCard() ? static_cast<fs::FS *>(&SD_MMC) : static_cast<fs::FS *>(&LittleFS);

    pm->on("webui:ws:register", [this](Event const &event) { registerWsHandlers(event.getPointer<WebSocketHandler>("ws")); });
    pm->on("settings:changed", [this](Event const &) {
        std::lock_guard<std::mutex> lock(mutex);
        isConfigReloadRequested = true;
    });
    pm->on("controller:brew:end", [this](Event const &) { captureProfileSnapshot(); });
    pm->on("evt:history-shot-saved", [this](Event const &) { wakeWorker(); });
    pm->on("history:notes:saved", [this](Event const &event) {
        const auto shotId = static_cast<uint32_t>(event.getInt("id"));
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (std::find(pendingNotesShotIds.begin(), pendingNotesShotIds.end(), shotId) == pendingNotesShotIds.end()) {
                if (pendingNotesShotIds.size() >= MAX_PENDING_NOTES) {
                    ESP_LOGW(LOG_TAG, "Notes queue full: dropped_shot_id=%u", pendingNotesShotIds.front());
                    pendingNotesShotIds.erase(pendingNotesShotIds.begin());
                }
                pendingNotesShotIds.push_back(shotId);
            }
        }
        wakeWorker();
    });
    pm->on("controller:wifi:connect", [this](Event const &event) {
        if (event.getInt("AP") == 0) {
            wakeWorker();
        }
    });

    // Low priority on core 0 next to the network stack, away from the UI and control loops on core 1.
    if (xTaskCreatePinnedToCore(workerTask, "CloudSync", WORKER_STACK_BYTES, this, WORKER_PRIORITY, &taskHandle, WORKER_CORE) !=
        pdPASS) {
        ESP_LOGE(LOG_TAG, "Failed to start the worker task: stack_bytes=%u", static_cast<unsigned>(WORKER_STACK_BYTES));
        taskHandle = nullptr;
    }
}

// Settings are plain members without locking, so they are only read here on the main loop and
// handed to the worker as a copy.
void CloudSyncPlugin::loop() {
    bool isReloadNeeded;
    {
        std::lock_guard<std::mutex> lock(mutex);
        isReloadNeeded = isConfigReloadRequested;
        isConfigReloadRequested = false;
    }
    if (!isReloadNeeded) {
        return;
    }
    Settings &settings = controller->getSettings();
    {
        std::lock_guard<std::mutex> lock(mutex);
        config.baseUrl = settings.getCloudSyncUrl();
        config.token = settings.getCloudSyncToken();
        config.generation++;
    }
    ESP_LOGI(LOG_TAG, "Configuration loaded: url=%s token_configured=%d", settings.getCloudSyncUrl().c_str(),
             !settings.getCloudSyncToken().isEmpty());
    wakeWorker();
}

void CloudSyncPlugin::wakeWorker() {
    if (taskHandle != nullptr) {
        xTaskNotifyGive(taskHandle);
    }
}

void CloudSyncPlugin::workerTask(void *arg) { static_cast<CloudSyncPlugin *>(arg)->runWorker(); }

void CloudSyncPlugin::runWorker() {
    uint32_t sleepMs = POLL_INTERVAL_MS;
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleepMs));
        Config snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = config;
        }
        sleepMs = runSyncPass(snapshot);
    }
}

uint32_t CloudSyncPlugin::runSyncPass(const Config &snapshot) {
    passGeneration = snapshot.generation;
    if (!snapshot.isComplete()) {
        setState(SyncState::NotConfigured);
        return POLL_INTERVAL_MS;
    }
    if (isAuthPaused && snapshot.generation != authFailedGeneration) {
        // New URL or token: worth trying again right away.
        isAuthPaused = false;
        isRetryScheduled = false;
    }
    if (isAuthPaused) {
        setState(SyncState::AuthenticationFailed);
        return POLL_INTERVAL_MS;
    }
    if (isRetryScheduled) {
        if (static_cast<int32_t>(retryAtMillis - millis()) > 0) {
            setState(SyncState::RetryScheduled);
            return sleepBeforeNextPassMs();
        }
        isRetryScheduled = false;
    }
    if (!isStationConnected()) {
        setState(SyncState::Offline);
        return POLL_INTERVAL_MS;
    }
    if (!TimeSync::isClockTrusted()) {
        setState(SyncState::WaitingForClock);
        return POLL_INTERVAL_MS;
    }
    if (controller->isActive() || controller->isUpdating()) {
        setState(SyncState::Busy);
        return POLL_INTERVAL_MS;
    }

    if (!syncPendingShots(snapshot) || !syncPendingNotes(snapshot)) {
        return sleepBeforeNextPassMs();
    }
    setState(SyncState::Idle);
    return POLL_INTERVAL_MS;
}

uint32_t CloudSyncPlugin::sleepBeforeNextPassMs() const {
    if (!isRetryScheduled) {
        return POLL_INTERVAL_MS;
    }
    // Signed difference: a deadline that already passed must not wrap into a huge delay.
    const auto remainingMs = static_cast<int32_t>(retryAtMillis - millis());
    return remainingMs <= 0 ? 1 : std::min(static_cast<uint32_t>(remainingMs), POLL_INTERVAL_MS);
}

// Returns false when the pass must stop (waiting, retry scheduled, auth failure, brew started).
bool CloudSyncPlugin::syncPendingShots(const Config &snapshot) {
    const auto historyIndex = static_cast<uint32_t>(controller->getSettings().getHistoryIndex());
    if (!isCursorLoaded) {
        const int64_t storedNextShotId = loadCursor();
        nextShotId = cloudsync::reconcileCursor(storedNextShotId, historyIndex);
        isCursorLoaded = true;
        if (storedNextShotId < 0) {
            ESP_LOGI(LOG_TAG, "Starting at the current shot counter, earlier shots are not uploaded: next_shot_id=%u",
                     nextShotId);
            storeCursor(nextShotId);
        }
    }
    const uint32_t reconciledShotId = cloudsync::reconcileCursor(nextShotId, historyIndex);
    if (reconciledShotId != nextShotId) {
        ESP_LOGW(LOG_TAG, "Shot counter went backwards (settings reset?): cursor=%u history_index=%u", nextShotId, historyIndex);
        nextShotId = reconciledShotId;
        storeCursor(nextShotId);
    }

    while (nextShotId < historyIndex) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status.pendingShots = historyIndex - nextShotId;
        }
        if (controller->isActive() || controller->isUpdating()) {
            setState(SyncState::Busy);
            return false;
        }

        const cloudsync::ShotFacts facts = readShotFacts(nextShotId);
        const ShotAction action = cloudsync::decideShotAction(facts);
        if (action == ShotAction::Wait) {
            setState(SyncState::WaitingForTimestamp);
            return false;
        }
        if (action == ShotAction::Skip) {
            ESP_LOGW(LOG_TAG, "Skipping shot: id=%u file_exists=%d header_valid=%d start_epoch=%u", nextShotId,
                     facts.doesFileExist, facts.isHeaderValid, facts.startEpochSeconds);
        } else {
            setState(SyncState::Uploading);
            const ResponseOutcome outcome = uploadShot(snapshot, nextShotId);
            if (outcome == ResponseOutcome::AuthenticationFailed || outcome == ResponseOutcome::RetryLater) {
                return false;
            }
        }
        nextShotId++;
        storeCursor(nextShotId);
    }
    std::lock_guard<std::mutex> lock(mutex);
    status.pendingShots = 0;
    return true;
}

bool CloudSyncPlugin::syncPendingNotes(const Config &snapshot) {
    while (true) {
        uint32_t shotId = 0;
        bool hasCandidate = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Shots at or past the cursor carry their notes in the shot upload itself.
            for (uint32_t pendingShotId : pendingNotesShotIds) {
                if (pendingShotId < nextShotId) {
                    shotId = pendingShotId;
                    hasCandidate = true;
                    break;
                }
            }
            status.pendingNotes = pendingNotesShotIds.size();
        }
        if (!hasCandidate) {
            return true;
        }
        if (controller->isActive() || controller->isUpdating()) {
            setState(SyncState::Busy);
            return false;
        }

        setState(SyncState::Uploading);
        bool isShotUnknown = false;
        const ResponseOutcome outcome = uploadNotes(snapshot, shotId, isShotUnknown);
        if (!isShotUnknown && (outcome == ResponseOutcome::AuthenticationFailed || outcome == ResponseOutcome::RetryLater)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        pendingNotesShotIds.erase(std::remove(pendingNotesShotIds.begin(), pendingNotesShotIds.end(), shotId),
                                  pendingNotesShotIds.end());
    }
}

cloudsync::ShotFacts CloudSyncPlugin::readShotFacts(uint32_t shotId) {
    cloudsync::ShotFacts facts;
    // Read before the header: a fix lands in the file before this flag clears, so a cleared flag
    // with an untrusted header really means no fix is coming.
    facts.hasTimestampFixPending = ShotHistory.hasPendingTimestamps();
    facts.isClockTrusted = TimeSync::isClockTrusted();
    File shotFile = fs->open(shotLogPath(shotId), "r");
    if (!shotFile) {
        return facts;
    }
    facts.doesFileExist = true;
    ShotLogHeader header{};
    facts.isHeaderValid = shotFile.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
                          header.magic == SHOT_LOG_MAGIC && header.headerSize == SHOT_LOG_HEADER_SIZE;
    shotFile.close();
    facts.startEpochSeconds = header.startEpoch;
    return facts;
}

bool CloudSyncPlugin::readShotLog(uint32_t shotId, uint8_t *&bytes, size_t &size) {
    bytes = nullptr;
    File shotFile = fs->open(shotLogPath(shotId), "r");
    if (!shotFile) {
        return false;
    }
    size = shotFile.size();
    if (size < sizeof(ShotLogHeader) || size > MAX_SHOT_LOG_BYTES) {
        ESP_LOGE(LOG_TAG, "Shot log has an unexpected size: id=%u bytes=%u max=%u", shotId, static_cast<unsigned>(size),
                 static_cast<unsigned>(MAX_SHOT_LOG_BYTES));
        shotFile.close();
        return false;
    }
    bytes = allocateLarge(size);
    const bool isComplete = bytes != nullptr && shotFile.read(bytes, size) == size;
    shotFile.close();
    if (!isComplete) {
        heap_caps_free(bytes);
        bytes = nullptr;
    }
    return isComplete;
}

ResponseOutcome CloudSyncPlugin::uploadShot(const Config &snapshot, uint32_t shotId) {
    uint8_t *shotLog = nullptr;
    size_t shotLogSize = 0;
    if (!readShotLog(shotId, shotLog, shotLogSize)) {
        recordFailure(ResponseOutcome::RejectedPermanently, 0, "shot_log_unreadable");
        return ResponseOutcome::RejectedPermanently;
    }
    ShotLogHeader header{};
    memcpy(&header, shotLog, sizeof(header));
    const String shotLogSha256 = sha256Hex(shotLog, shotLogSize);

    size_t base64Size = 0;
    mbedtls_base64_encode(nullptr, 0, &base64Size, shotLog, shotLogSize); // reports the size including the terminator
    auto *base64 = reinterpret_cast<char *>(allocateLarge(base64Size));
    const bool isEncoded = base64 != nullptr && mbedtls_base64_encode(reinterpret_cast<unsigned char *>(base64), base64Size,
                                                                      &base64Size, shotLog, shotLogSize) == 0;
    heap_caps_free(shotLog);
    if (!isEncoded || shotLogSha256.isEmpty()) {
        heap_caps_free(base64);
        recordFailure(ResponseOutcome::RetryLater, 0, "out_of_memory");
        return ResponseOutcome::RetryLater;
    }

    char *body = nullptr;
    size_t bodySize = 0;
    {
        JsonDocument doc(&psramAllocator);
        doc["contract_version"] = CONTRACT_VERSION;
        doc["device_shot_id"] = shotId;
        doc["started_at"] = header.startEpoch;
        doc["firmware_version"] = BUILD_GIT_VERSION;
        doc["slog_base64"] = serialized(base64, base64Size);
        doc["slog_sha256"] = shotLogSha256;

        File notesFile = fs->open(notesPath(shotId), "r");
        if (notesFile) {
            JsonDocument notes(&psramAllocator);
            const bool isNotesReadable =
                notesFile.size() <= MAX_NOTES_FILE_BYTES && deserializeJson(notes, notesFile) == DeserializationError::Ok;
            notesFile.close();
            // Unreadable notes must not block the shot itself; edits re-send them later.
            if (isNotesReadable && notes.is<JsonObject>()) {
                doc["notes"] = notes;
            } else {
                ESP_LOGW(LOG_TAG, "Notes not attached, file is not a JSON object: id=%u", shotId);
            }
        }
        const String profileJson = findProfileSnapshot(shotId);
        if (!profileJson.isEmpty()) {
            doc["profile"] = serialized(profileJson);
        }
        // serialized() only references base64, so it must stay alive until here.
        body = serializeToBuffer(doc, bodySize);
    }
    heap_caps_free(base64);
    if (body == nullptr) {
        recordFailure(ResponseOutcome::RetryLater, 0, "out_of_memory");
        return ResponseOutcome::RetryLater;
    }
    if (bodySize > MAX_REQUEST_BYTES) {
        heap_caps_free(body);
        ESP_LOGE(LOG_TAG, "Shot upload too large: id=%u bytes=%u max=%u", shotId, static_cast<unsigned>(bodySize),
                 static_cast<unsigned>(MAX_REQUEST_BYTES));
        recordFailure(ResponseOutcome::RejectedPermanently, 0, "request_too_large");
        return ResponseOutcome::RejectedPermanently;
    }

    const cloudsync::HttpResponse response =
        cloudsync::sendJsonRequest("POST", snapshot.baseUrl + "/v1/device/shots", snapshot.token, body, bodySize);
    heap_caps_free(body);

    const ResponseOutcome outcome = cloudsync::classifyResponse(response.status);
    if (outcome == ResponseOutcome::Acknowledged || outcome == ResponseOutcome::RejectedPermanently) {
        forgetProfileSnapshot(shotId); // kept across retries, but this shot won't be sent again
    }
    if (outcome == ResponseOutcome::Acknowledged) {
        ESP_LOGI(LOG_TAG, "Shot uploaded: id=%u status=%d bytes=%u start_epoch=%u", shotId, response.status,
                 static_cast<unsigned>(shotLogSize), header.startEpoch);
        recordSuccess(response.status);
        std::lock_guard<std::mutex> lock(mutex);
        status.lastUploadedShotId = shotId;
        status.hasUploadedShot = true;
    } else {
        ESP_LOGW(LOG_TAG, "Shot upload failed: id=%u status=%d error=%s", shotId, response.status, response.errorCode.c_str());
        recordFailure(outcome, response.status, response.errorCode);
    }
    return outcome;
}

ResponseOutcome CloudSyncPlugin::uploadNotes(const Config &snapshot, uint32_t shotId, bool &isShotUnknown) {
    isShotUnknown = false;
    uint8_t *shotLog = nullptr;
    size_t shotLogSize = 0;
    if (!readShotLog(shotId, shotLog, shotLogSize)) {
        isShotUnknown = true; // the shot is gone locally, so its notes have nothing to attach to
        return ResponseOutcome::RejectedPermanently;
    }
    const String shotLogSha256 = sha256Hex(shotLog, shotLogSize);
    heap_caps_free(shotLog);

    char *body = nullptr;
    size_t bodySize = 0;
    {
        JsonDocument doc(&psramAllocator);
        File notesFile = fs->open(notesPath(shotId), "r");
        if (!notesFile) {
            isShotUnknown = true;
            return ResponseOutcome::RejectedPermanently;
        }
        JsonDocument notes(&psramAllocator);
        const bool isNotesReadable =
            notesFile.size() <= MAX_NOTES_FILE_BYTES && deserializeJson(notes, notesFile) == DeserializationError::Ok;
        notesFile.close();
        if (!isNotesReadable || !notes.is<JsonObject>()) {
            ESP_LOGW(LOG_TAG, "Notes not sent, file is not a JSON object: id=%u", shotId);
            isShotUnknown = true;
            return ResponseOutcome::RejectedPermanently;
        }
        doc["contract_version"] = CONTRACT_VERSION;
        doc["device_shot_id"] = shotId;
        doc["slog_sha256"] = shotLogSha256;
        doc["notes"] = notes;
        body = serializeToBuffer(doc, bodySize);
    }
    if (body == nullptr) {
        recordFailure(ResponseOutcome::RetryLater, 0, "out_of_memory");
        return ResponseOutcome::RetryLater;
    }

    const cloudsync::HttpResponse response =
        cloudsync::sendJsonRequest("PUT", snapshot.baseUrl + "/v1/device/shots/notes", snapshot.token, body, bodySize);
    heap_caps_free(body);

    if (response.status == 404 && response.errorCode == "shot_not_found") {
        // Skipped shots never reached the cloud; their notes have nowhere to go.
        ESP_LOGI(LOG_TAG, "Notes dropped, shot unknown to the cloud: id=%u", shotId);
        isShotUnknown = true;
        return ResponseOutcome::RejectedPermanently;
    }
    const ResponseOutcome outcome = cloudsync::classifyResponse(response.status);
    if (outcome == ResponseOutcome::Acknowledged) {
        ESP_LOGI(LOG_TAG, "Notes uploaded: id=%u status=%d", shotId, response.status);
        recordSuccess(response.status);
    } else {
        ESP_LOGW(LOG_TAG, "Notes upload failed: id=%u status=%d error=%s", shotId, response.status, response.errorCode.c_str());
        recordFailure(outcome, response.status, response.errorCode);
        if (outcome == ResponseOutcome::RejectedPermanently) {
            isShotUnknown = true;
        }
    }
    return outcome;
}

void CloudSyncPlugin::captureProfileSnapshot() {
    String profileJson;
    uint32_t shotId = 0;
    {
        // Deref under the process lock — other tasks delete the process at any time (GM-147).
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *last = controller->getLastProcess();
        if (last == nullptr || last->getType() != MODE_BREW) {
            return;
        }
        auto *brewProcess = static_cast<BrewProcess *>(last);
        if (brewProcess->isUtility()) {
            return;
        }
        // ShotHistory names the shot being recorded after the counter it increments once saved.
        shotId = static_cast<uint32_t>(controller->getSettings().getHistoryIndex());
        JsonDocument doc(&psramAllocator);
        JsonObject profileObject = doc.to<JsonObject>();
        writeProfile(profileObject, brewProcess->profile);
        serializeJson(doc, profileJson);
    }
    std::lock_guard<std::mutex> lock(mutex);
    profileSnapshots[shotId] = profileJson; // a discarded short shot's id is reused, so overwrite
    while (profileSnapshots.size() > MAX_PROFILE_SNAPSHOTS) {
        profileSnapshots.erase(profileSnapshots.begin());
    }
}

String CloudSyncPlugin::findProfileSnapshot(uint32_t shotId) {
    std::lock_guard<std::mutex> lock(mutex);
    auto snapshot = profileSnapshots.find(shotId);
    return snapshot != profileSnapshots.end() ? snapshot->second : String("");
}

void CloudSyncPlugin::forgetProfileSnapshot(uint32_t shotId) {
    std::lock_guard<std::mutex> lock(mutex);
    profileSnapshots.erase(shotId);
}

void CloudSyncPlugin::recordSuccess(int httpStatus) {
    std::lock_guard<std::mutex> lock(mutex);
    status.lastSuccessEpoch = TimeSync::trustedNow();
    status.lastHttpStatus = httpStatus;
    status.lastErrorCode = "";
    status.consecutiveFailures = 0;
}

void CloudSyncPlugin::recordFailure(ResponseOutcome outcome, int httpStatus, const String &errorCode) {
    uint32_t failures;
    {
        std::lock_guard<std::mutex> lock(mutex);
        status.lastHttpStatus = httpStatus;
        status.lastErrorCode = errorCode;
        if (outcome == ResponseOutcome::RetryLater) {
            status.consecutiveFailures++;
        }
        failures = status.consecutiveFailures;
    }
    if (outcome == ResponseOutcome::AuthenticationFailed) {
        ESP_LOGE(LOG_TAG, "Device token rejected, pausing until the settings change: status=%d", httpStatus);
        isAuthPaused = true;
        authFailedGeneration = passGeneration; // the configuration that was actually rejected
    } else if (outcome == ResponseOutcome::RetryLater) {
        const uint32_t delayMs = cloudsync::retryDelayMs(failures, randomUnit());
        retryAtMillis = millis() + delayMs;
        isRetryScheduled = true;
        ESP_LOGW(LOG_TAG, "Retry scheduled: failures=%u delay_ms=%u", failures, delayMs);
    }
}

void CloudSyncPlugin::setState(SyncState state) {
    std::lock_guard<std::mutex> lock(mutex);
    status.state = state;
}

int64_t CloudSyncPlugin::loadCursor() {
    // The temp file only survives a crash between remove and rename on SD (FAT can't rename over a file).
    for (const char *path : {CURSOR_PATH, CURSOR_TEMP_PATH}) {
        File cursorFile = fs->open(path, "r");
        if (!cursorFile) {
            continue;
        }
        JsonDocument doc;
        const DeserializationError error = deserializeJson(doc, cursorFile);
        cursorFile.close();
        if (error == DeserializationError::Ok && doc["v"] == CURSOR_FORMAT_VERSION && doc["next"].is<uint32_t>()) {
            return doc["next"].as<uint32_t>();
        }
        ESP_LOGE(LOG_TAG, "Cursor file unreadable, ignoring it: path=%s error=%s", path, error.c_str());
    }
    return -1;
}

bool CloudSyncPlugin::storeCursor(uint32_t nextShotIdToStore) {
    JsonDocument doc;
    doc["v"] = CURSOR_FORMAT_VERSION;
    doc["next"] = nextShotIdToStore;
    File tempFile = fs->open(CURSOR_TEMP_PATH, FILE_WRITE);
    if (!tempFile) {
        ESP_LOGE(LOG_TAG, "Cannot write cursor: path=%s next_shot_id=%u", CURSOR_TEMP_PATH, nextShotIdToStore);
        return false;
    }
    const bool isWritten = serializeJson(doc, tempFile) > 0;
    tempFile.close();
    if (!isWritten) {
        ESP_LOGE(LOG_TAG, "Cannot write cursor: path=%s next_shot_id=%u", CURSOR_TEMP_PATH, nextShotIdToStore);
        return false;
    }
    if (fs->exists(CURSOR_PATH)) {
        fs->remove(CURSOR_PATH);
    }
    if (!fs->rename(CURSOR_TEMP_PATH, CURSOR_PATH)) {
        ESP_LOGE(LOG_TAG, "Cannot replace cursor: next_shot_id=%u", nextShotIdToStore);
        return false;
    }
    return true;
}

const char *CloudSyncPlugin::stateName(SyncState state) {
    switch (state) {
    case SyncState::Starting:
        return "starting";
    case SyncState::NotConfigured:
        return "not_configured";
    case SyncState::Offline:
        return "offline";
    case SyncState::WaitingForClock:
        return "waiting_for_clock";
    case SyncState::Busy:
        return "busy";
    case SyncState::WaitingForTimestamp:
        return "waiting_for_timestamp";
    case SyncState::Uploading:
        return "uploading";
    case SyncState::Idle:
        return "idle";
    case SyncState::RetryScheduled:
        return "retry_scheduled";
    case SyncState::AuthenticationFailed:
        return "authentication_failed";
    }
    return "unknown";
}

void CloudSyncPlugin::registerWsHandlers(WebSocketHandler *ws) {
    if (ws == nullptr) {
        return;
    }
    ws->registerRequestHandler("req:cloudsync:status", [this, ws](uint32_t clientId, JsonDocument &request) {
        JsonDocument response(&psramAllocator);
        response["tp"] = "res:cloudsync:status";
        if (request["rid"].is<const char *>()) {
            response["rid"] = request["rid"];
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            response["state"] = stateName(status.state);
            response["pendingShots"] = status.pendingShots;
            response["pendingNotes"] = status.pendingNotes;
            response["lastSuccessAt"] = status.lastSuccessEpoch;
            if (status.hasUploadedShot) {
                response["lastUploadedShotId"] = status.lastUploadedShotId;
            }
            response["lastHttpStatus"] = status.lastHttpStatus;
            response["lastError"] = status.lastErrorCode;
            response["consecutiveFailures"] = status.consecutiveFailures;
        }
        ws->sendJson(clientId, response);
    });
}
