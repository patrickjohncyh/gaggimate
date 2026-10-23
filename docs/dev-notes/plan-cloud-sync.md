# Plan: CloudSync (Coffee Coach firmware side) — Phase 0 reconnaissance

Branch: `feat/cloud-sync` (from the fork's `master` @ `25209a75`, upstream `b78beee4` merged).

This is Phase 0 of the Coffee Coach plan: confirm the firmware integration surface before any
code. The cloud side (Cloudflare Worker, D1, R2, Telegram, model) lives in a separate repository.
This repository only gets the on-device **CloudSync** module: shot upload, profile snapshot, and
command polling.

## 1. Firmware compatibility record

| Item | Value | Source |
|---|---|---|
| Controller hardware | GaggiMate Pro Rev 1.1 | `res:ota-settings` |
| Display hardware | LilyGo T-RGB (ESP32-S3, 16 MB flash, OPI PSRAM), env `display` | `platformio.ini`, `boards/LilyGo-T-RGB.json` |
| Display firmware on the machine (2026-10-23) | `v1.8.1-214-g3e08be3d` (HomeKit work, before the upstream merge) | `res:ota-settings` |
| Controller firmware on the machine | `v1.8.1-210-g6dff0448`, protocol 5 | `res:ota-settings`, boot log |
| Integration base | fork `master` @ `25209a75`, protocol 8 | `lib/NanoPbComm/src/Protocol.h` |
| Matching controller build | official nightly `v1.9.0-135-gb78beee4` (2026-10-19), same upstream commit as merged | `releases/download/nightly/version.txt` |
| Timemore Dot | Supported upstream since `d94803b3` (2026-05-26), protocol fixes through 2026-09-23, scales library v1.0.7. Already in the base, no reproduction needed. | git history |
| Partition table | `default_16MB.csv`; history and profiles on LittleFS, or SD if present (`/h`, `/p`) | board json, `ShotHistoryPlugin.cpp:93-96` |
| History storage | Binary `.slog` per shot plus `/h/index.bin` plus per-shot notes `/h/<id>.json` | `shot_log_format.h` |

**Before flashing the merged firmware**, the controller must be updated to protocol 8 (System &
Updates → Nightly → Update Controller), or the display locks control.

## 2. Data findings from the machine (fixtures)

Captured on 2026-10-23 via the local API (`/api/history/index.bin`, `/api/history/<id>.slog`,
`req:profiles:list`):

- **171 shots, ids 0–170.** No gaps, deletions or rollover so far.
- **Shots 0–96 have relative timestamps.** `startEpoch` holds seconds since the display booted
  (35 s – 791,440 s) because the clock never synced before 2026-09-04. Several are out of order.
  Shots 97+ have real Unix time. **Timestamps can't be assumed valid.**
- **Format v7 on the machine, v8 in the merged firmware** (`SHOT_LOG_VERSION = 8`, from upstream
  `c17416d2`). The cloud parser must accept both. Both use 512-byte headers and 250 ms samples
  with 14 fields (`fieldsMask 0x3FFF`). The sample is 30 bytes in v7.
- **Size:** 512 + 30 × samples, ≈5.2 KB for a 40 s shot and ≈7.7 KB for 60 s. One bounded POST
  per shot is enough; no chunking or compression needed.
- **7 profiles,** exported as full JSON (`res:profiles:list`).

The fixtures are held locally, not committed (see §7).

## 3. Integration surface (patch points)

All references are to this branch.

| Need | Where | Notes |
|---|---|---|
| Shot fully persisted | `evt:history-shot-saved` (int `id`), `ShotHistoryPlugin.cpp:334-339` | Fires after the file is closed, `hi` is incremented, the dose is auto-filled into notes, and the index is upserted. Not fired for shots ≤7.5 s (deleted). Runs **on the ShotHistory task** (core 0, prio 1, 250 ms cadence), so the handler may only enqueue the id. Pattern to copy: `MahlkonigPlugin` (mutex + flag, work in `loop()`). |
| Not a persistence signal | `history:shot:recorded` (`:289`) | Fires before the ≤7.5 s discard. |
| Enumerate history | `/h/index.bin` (32-byte header, 128-byte entries: id, timestamp, duration, volume, rating, flags COMPLETED/DELETED/HAS_NOTES, profile id/name, averages) | `shot_log_format.h:154-193` |
| Shot telemetry | `/h/<6-digit id>.slog` | The header has `startEpoch`, profile id and a 47-char name, up to 12 phase transitions, exit reason, final weight. **No profile JSON, dose or notes.** |
| Notes (dose/yield/rating/beans/grind/taste) | `/h/<id>.json`, schema `schema/shot_notes.json`, WS `req:history:notes:save` | Mutable at any time. **No event on save or delete**, so one must be added (`ShotHistoryPlugin.cpp:600-640`). |
| Profile serialization | `parseProfile` / `writeProfile`, `models/profile.h:289-506` | Normalizes: drops unknown keys, fixed key order, fills defaults, drops unknown target types. Writes the volatile `favorite`/`selected` flags to disk. |
| Profile save/select | `ProfileManager::saveProfile` / `selectProfile` (`ProfileManager.cpp:199-304`) | Already called off the main task (AsyncTCP). ProfileManager has no mutex. |
| Busy/idle | `Controller::isActive()` (locks `processMutex`, safe from any task), `isUpdating()`, `getMode()` | `isActive()` goes false ≈3 s before ShotHistory finishes extended recording. Key on the saved event. |
| Wi-Fi | `controller:wifi:connect` (int `AP`) / `controller:wifi:disconnect`, dispatched on the main loop | AP fallback never starts SNTP. |
| Time validity | No "time synced" event. NTP only in `setupWifi()` (`Controller.cpp:513-518`) | Reuse the `MIN_VALID_TIME = 1700000000` check (`CleaningSchedulePlugin.cpp:11`). |
| HTTPS client | `EspHttpTransport` (`esp_http_client`, Arduino cert bundle, 15 s timeout, HTTPS only) | **GET-only**, so a POST path is needed. mbedTLS goes to PSRAM via `WebUIPlugin::setup`, so register CloudSync after WebUIPlugin. |
| Background network task | **None exists.** All outbound HTTPS runs on the main loop today | CloudSync needs its own pinned task (≥8–12 KB stack). |
| Settings / secrets | `Property<T>` registry (NVS namespace `controller`, keys ≤15 chars), exposed in `WebUIPlugin::handleSettings` | **No real secret masking precedent:** the HA password is returned in plaintext. The device token must be write-only (return a "configured" flag, or use the `---unchanged---` sentinel). |
| Existing off-device upload | None in firmware. Visualizer upload is browser-side; MQTT publishes no shot data | — |

## 4. Risks and required design responses

1. **Shot id reuse.** `hi` is flushed to NVS lazily (≤5 s). An NVS wipe, a display swap, or
   moving the SD card resets or collides ids, and the next shot overwrites `000000.slog` etc.
   → The cloud idempotency key is **(device id, shot id, startEpoch, durationMs, content
   sha256)**, never the bare id. A same-id/different-hash arrival is a new logical shot, flagged.
2. **Invalid timestamps** (seen: shots 0–96).
   → Upload `startEpoch` with an `epoch_valid` flag (≥ 1700000000) plus the device's boot id
   and uptime at shot end. CloudSync waits for a valid clock before uploading *new* shots, but
   backfill must still send relative-time shots, marked as such.
3. **The profile isn't in the shot.** The `.slog` has only id and name, and the profile can
   change before upload.
   → Snapshot the profile at `controller:brew:end` (under `getProcessLock()`) and send it, or
   its canonical hash, with the shot.
4. **Notes arrive later and change.** Dose, yield, rating and beans live in mutable notes.
   → Add `history:notes:saved` / `history:shot:deleted` events. Re-upload notes as a separate
   idempotent revision.
5. **Profile hashes aren't stable across a round-trip** (normalization, volatile flags).
   → Hash a **canonical form**: `writeProfile` output minus `favorite`/`selected`. Verify
   parse→write idempotency with a native test before Phase 6.
6. **Never block:** the ShotHistory task (sample gaps), AsyncTCP (watchdog), the controller
   logic task, or the main loop (OTA/WS). Pause during OTA (`isUpdating()`,
   `ota:update:start/end`). Run one TLS session at a time (≈32 KB+ each).
7. **History files can vanish** (low-space cleanup deletes the oldest; shots ≤7.5 s are deleted;
   the user can delete). → Upload promptly, tolerate missing files, and warn when unsynced shots
   approach cleanup.
8. **Unconfirmed:** whether `-DCONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` changes the linked
   bundle (verify the Cloudflare chain validates), ArduinoJson float formatting, and the LittleFS
   size.

## 5. Recovery procedure

- **Known-good builds:** official nightly via System & Updates (OTA), or USB-flash any tagged
  fork commit: `pio run -e display -t upload`. Use upstream's embed fix (sha256 stamp) and check
  that `firmware.bin` contains the current `web_ui.bin` before flashing.
- **Never run `-t uploadfs`.** It replaces LittleFS and wipes profiles and history.
- **Back up before flashing:** `/api/history/index.bin`, every `/api/history/<id>.slog` and
  notes, and `req:profiles:list`, using the scripts used for the fixtures.
- **If USB flashing fails** with "No serial data received": hold BOOT while plugging in or
  pressing RESET, then retry.
- **If the protocol mismatches** after a display flash: update the controller OTA from the
  display (Nightly channel).

## 6. Proposed CloudSync shape (for review, not built)

- `CloudSyncPlugin`, registered after WebUIPlugin and gated on a setting (`cs_en`, `cs_url`,
  write-only `cs_tok`).
- Event handlers only enqueue: `evt:history-shot-saved`, `history:notes:saved` (new),
  `controller:brew:end` (profile snapshot).
- A dedicated worker task. It waits for STA Wi-Fi, a valid clock, idle, and not updating. Then
  it uploads the raw `.slog` + notes + profile snapshot as one POST to `/v1/device/shots`, and
  advances a persisted cursor (`/h/.cloudsync`) only on a 2xx ack. Backoff with jitter.
- On boot or reconnect, reconcile `index.bin` against the cursor (backfill).
- Command polling and profile install come later (plan Phase 6). Install-only first, idle-only,
  read-back hash, never overwrite the original.

## 7. Open decisions

- Where the cloud repo lives (new `coffee-coach` repo?) and whether you have Cloudflare (Workers,
  D1, R2) and a Telegram bot token ready.
- Whether to commit the captured fixtures. This fork is **public**, and the shot fixtures reveal
  brewing times. They fit the cloud repo's `tests/fixtures/` better (keep that repo private).
- Device auth: an opaque per-device bearer token (simplest) vs HMAC-signed requests with a
  timestamp (replay protection without TLS pinning).
