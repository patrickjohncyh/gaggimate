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
   → *Decided:* every uploaded shot must carry a verified UTC epoch (see §7). Restart SNTP on
   every STA connect. A shot that starts before sync keeps its `millis()` start and is converted
   once the clock syncs. Shots that can't be converted are not uploaded.
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
8. **Certificate bundle (checked 2026-10-24):** the bundle linked into `firmware.elf`
   (`_binary_x509_crt_bundle_start`, 136 roots) contains GTS Root R4, the root of
   `*.workers.dev` (leaf → WE1 → GTS Root R4), and ISRG Root X1. **Still unconfirmed:**
   ArduinoJson float formatting, and the LittleFS size.

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
- On boot or reconnect, reconcile `index.bin` against the cursor, but only for shots recorded
  since CloudSync was enabled. Legacy shots are never uploaded (see §7).
- Command polling and profile install come later (plan Phase 6). Install-only first, idle-only,
  read-back hash, never overwrite the original.

## 7. Decisions (2026-10-23)

- **Clean slate.** Legacy shots (ids 0–170 on the machine) are ignored. Gaggi starts with the
  first shot CloudSync uploads. No backfill. The cursor is initialised to the current
  `historyIndex` when CloudSync is first enabled.
- **UTC timestamps.** `startEpoch` is Unix time, so it's UTC; display time zones are derived in
  the cloud. Firmware requirements:
  - re-init SNTP on every STA connect (today it only starts at boot and never in AP fallback);
  - treat an epoch as valid only when SNTP reports synced and the epoch is ≥ 1700000000;
  - convert pre-sync shots from their `millis()` start once synced;
  - never upload a shot without a valid UTC start.
- **Cloud repo:** private `patrickjohncyh/gaggi` (Gaggi is the coach's name). Fixtures live in
  its `tests/fixtures/firmware/`, not in this public fork.
- **Still open:** device authentication (proposed: per-device bearer token over HTTPS) and the
  Telegram bot (not created yet; needed from Phase 3).

## 8. Phase 2 design (firmware cloud sync)

Exit test (from the plan): brew several shots with intermittent Wi-Fi; after reconnection all are
in the cloud exactly once with complete native curves, and machine control and UI are unaffected.

### Trusted UTC time — `core/TimeSync`

- `TimeSync::begin()` installs the SNTP sync-notification callback before SNTP starts (no other
  code uses it). A shared atomic flag records that SNTP has set the clock at least once.
- `TimeSync::isClockTrusted()` is true only when SNTP has synced **and** `time()` ≥ 1700000000.
  The RTC keeps running across soft resets, so a plausible-looking `time()` after a reboot is not
  trusted until SNTP confirms it.
- `controller:wifi:connect` with `AP=0` calls `sntp_restart()`, so a reconnect syncs right away
  instead of waiting out lwIP's retry backoff.

### Shot timestamps — `ShotHistoryPlugin`

- At file creation the plugin records whether `startEpoch` came from a trusted clock.
- **Sync during the shot:** when the header is patched at the end, an untrusted start is replaced
  by `now − elapsed`, using `millis()` (monotonic within a boot).
- **Sync after the shot:** the shot id and its `millis()` start are kept in memory. Once the clock
  is trusted, the ShotHistory task patches the `.slog` header and the index entry, then fires
  `evt:history-shot-saved` again so the dashboard and CloudSync pick up the corrected time.
- A reboot before sync loses the `millis()` reference. Those shots keep their untrusted epoch and
  are never uploaded (decision §7).
- `hasPendingTimestamps()` (atomic) tells CloudSync to wait rather than skip.
- `req:history:notes:save` now fires `history:notes:saved` (int `id`).

### CloudSync plugin

Settings (NVS keys ≤ 15 chars): `cs_a` enabled, `cs_url` base URL, `cs_tok` device token.
- The token is **write-only**. `/api/settings` returns `cloudSyncTokenConfigured`, never the
  token. An empty submitted value leaves the stored token unchanged.
- Registered only when enabled, like the other optional plugins (toggling needs a restart).
  URL/token changes apply on `settings:changed` without a restart.

**Cursor** `/h/.cloudsync`, JSON `{"v":1,"next":N}`:
- Lives on the same filesystem as the shots, so it moves with an SD card. The file is hidden, so
  history cleanup ignores it.
- Ids below `next` are done. On first enable it is created at the current `historyIndex`, so
  legacy shots are never sent (clean slate).
- If `historyIndex` < `next` (NVS reset), the cursor is pulled back to `historyIndex`. The cloud
  sees the reused ids, flags them `id_reused`, and never overwrites.
- Written via tmp file + rename, and only after a 2xx ack.

**Worker task** (core 0, priority 1, 12 KB stack for mbedTLS). It runs only when all of these hold:
- enabled, URL and token set;
- station Wi-Fi up;
- clock trusted;
- not brewing (`Controller::isActive()`) and not updating (`isUpdating()`).

It wakes on `evt:history-shot-saved`, `history:notes:saved`, `settings:changed`, Wi-Fi connect,
or a 60 s poll. Event handlers only notify the task.

For `id = next`:
| Situation | Action |
|---|---|
| `.slog` missing | Skip (deleted or cleaned up), advance |
| Header unreadable | Skip, log error, advance |
| Start epoch untrusted, fix pending or clock not trusted | Wait |
| Start epoch untrusted, no fix possible | Skip, log warning, advance |
| Otherwise | Upload |

**Upload:** `POST {url}/v1/device/shots`, contract v1 (gaggi `docs/dev-notes/plan-phase-1-foundation.md`).
- Raw `.slog` as base64 with its sha256 (mbedTLS), plus notes `/h/<id>.json` if present, plus the
  brew-time profile snapshot.
- Body built in PSRAM, capped at 60 KiB (a 120 s shot is ≈20 KB).

| Response | Meaning | Action |
|---|---|---|
| 200/201 | acked | advance, reset backoff |
| 401/403 | auth failed | pause until settings change; status shows it |
| 400/413/422 | rejected permanently | log the error code, advance (the file stays on the device) |
| other / network | transient | exponential backoff 30 s → 30 min, ±20 % jitter |

**Profile snapshot:**
- On `controller:brew:end` (non-utility brews), the process's profile copy is serialized with
  `writeProfile` under the process lock. It's keyed by the shot id being recorded
  (`historyIndex` at that moment).
- Kept in memory, at most 8 entries, and attached on upload.
- A reboot before upload loses it, and the shot uploads without a profile. That's acceptable for
  Phase 2, because the header still carries profile id and name.

**Notes edited later:**
- `history:notes:saved` for an id below `next` queues a notes update:
  `PUT {url}/v1/device/shots/notes` with `{device_shot_id, slog_sha256, notes}`. The new gaggi
  endpoint looks the shot up by content hash.
- The queue is in memory, at most 16 ids. A reboot loses pending notes updates (known gap).

**Status:** `req:cloudsync:status` over the WebSocket returns:
- state;
- pending shot count (`historyIndex − next`);
- last success time, last error code and HTTP status.

The plugin card shows them.

**HTTPS:** a small POST/PUT client on `esp_http_client` with the Arduino certificate bundle,
HTTPS-only and a 15 s timeout, mirroring `EspHttpTransport`, which stays GET-only for OTA.

**Pure logic** (`plugins/cloudsync/CloudSyncLogic`, native-tested): time trust, start-epoch
correction, the shot decision table, response classification, backoff, cursor reconciliation.

### Not in Phase 2

- Command polling and profile install (Phase 6).
- Full profile-library snapshots.
- Warning when unsynced shots near cleanup.
- Persisting pending notes updates and profile snapshots across reboots.
