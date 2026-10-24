// Unit tests: CloudSync decisions (clock trust, timestamp correction, shot upload decision,
// response handling, backoff, cursor reconciliation). Host-side — pio test -e native -f test_cloud_sync_logic.

#include <unity.h>

#include "display/plugins/cloudsync/CloudSyncLogic.cpp"

using namespace cloudsync;

void setUp() {}
void tearDown() {}

constexpr uint32_t SYNCED_EPOCH = 1792734864; // 2026-10-22, a real shot's start

static ShotFacts uploadableShot() {
    ShotFacts facts;
    facts.doesFileExist = true;
    facts.isHeaderValid = true;
    facts.startEpochSeconds = SYNCED_EPOCH;
    facts.isClockTrusted = true;
    return facts;
}

static void test_epoch_is_trusted_only_after_sntp_sync() {
    TEST_ASSERT_TRUE(isTrustedEpoch(SYNCED_EPOCH, true));
    // RTC time carried over a soft reset looks plausible but was never confirmed this boot.
    TEST_ASSERT_FALSE(isTrustedEpoch(SYNCED_EPOCH, false));
}

static void test_boot_relative_epoch_is_never_trusted() {
    TEST_ASSERT_FALSE(isTrustedEpoch(791440, true)); // seen on shots 0-96 before 2026-09-04
    TEST_ASSERT_FALSE(isTrustedEpoch(MIN_TRUSTED_EPOCH - 1, true));
    TEST_ASSERT_TRUE(isTrustedEpoch(MIN_TRUSTED_EPOCH, true));
}

static void test_corrected_start_subtracts_elapsed_time() {
    TEST_ASSERT_EQUAL_UINT32(SYNCED_EPOCH, correctedStartEpoch(SYNCED_EPOCH + 36, 50000, 14000));
}

static void test_corrected_start_rounds_to_nearest_second() {
    TEST_ASSERT_EQUAL_UINT32(SYNCED_EPOCH - 2, correctedStartEpoch(SYNCED_EPOCH, 10000, 8400)); // 1.6 s
    TEST_ASSERT_EQUAL_UINT32(SYNCED_EPOCH - 1, correctedStartEpoch(SYNCED_EPOCH, 10000, 8600)); // 1.4 s
}

static void test_corrected_start_survives_millis_wrap() {
    // Shot started 2 s before millis() wrapped, clock read 3 s after.
    TEST_ASSERT_EQUAL_UINT32(SYNCED_EPOCH - 5, correctedStartEpoch(SYNCED_EPOCH, 3000, UINT32_MAX - 1999));
}

static void test_shot_with_trusted_start_is_uploaded() {
    TEST_ASSERT_EQUAL(ShotAction::Upload, decideShotAction(uploadableShot()));
}

static void test_missing_or_corrupt_shot_is_skipped() {
    ShotFacts missing = uploadableShot();
    missing.doesFileExist = false;
    TEST_ASSERT_EQUAL(ShotAction::Skip, decideShotAction(missing));

    ShotFacts corrupt = uploadableShot();
    corrupt.isHeaderValid = false;
    TEST_ASSERT_EQUAL(ShotAction::Skip, decideShotAction(corrupt));
}

static void test_untrusted_start_waits_for_pending_fix() {
    ShotFacts facts = uploadableShot();
    facts.startEpochSeconds = 120;
    facts.hasTimestampFixPending = true;
    TEST_ASSERT_EQUAL(ShotAction::Wait, decideShotAction(facts));
}

static void test_untrusted_start_waits_while_clock_unsynced() {
    ShotFacts facts = uploadableShot();
    facts.startEpochSeconds = 120;
    facts.isClockTrusted = false;
    TEST_ASSERT_EQUAL(ShotAction::Wait, decideShotAction(facts));
}

static void test_untrusted_start_without_possible_fix_is_skipped() {
    // Synced clock, nothing pending: the millis() reference was lost to a reboot.
    ShotFacts facts = uploadableShot();
    facts.startEpochSeconds = 120;
    TEST_ASSERT_EQUAL(ShotAction::Skip, decideShotAction(facts));
}

static void test_success_statuses_acknowledge() {
    TEST_ASSERT_EQUAL(ResponseOutcome::Acknowledged, classifyResponse(200)); // duplicate replay
    TEST_ASSERT_EQUAL(ResponseOutcome::Acknowledged, classifyResponse(201));
}

static void test_auth_failures_pause() {
    TEST_ASSERT_EQUAL(ResponseOutcome::AuthenticationFailed, classifyResponse(401));
    TEST_ASSERT_EQUAL(ResponseOutcome::AuthenticationFailed, classifyResponse(403));
}

static void test_validation_failures_are_permanent() {
    TEST_ASSERT_EQUAL(ResponseOutcome::RejectedPermanently, classifyResponse(400));
    TEST_ASSERT_EQUAL(ResponseOutcome::RejectedPermanently, classifyResponse(413));
    TEST_ASSERT_EQUAL(ResponseOutcome::RejectedPermanently, classifyResponse(422));
}

static void test_transient_failures_retry() {
    TEST_ASSERT_EQUAL(ResponseOutcome::RetryLater, classifyResponse(0)); // no response
    TEST_ASSERT_EQUAL(ResponseOutcome::RetryLater, classifyResponse(-1));
    TEST_ASSERT_EQUAL(ResponseOutcome::RetryLater, classifyResponse(404)); // wrong URL, fixable
    TEST_ASSERT_EQUAL(ResponseOutcome::RetryLater, classifyResponse(429));
    TEST_ASSERT_EQUAL(ResponseOutcome::RetryLater, classifyResponse(500));
    TEST_ASSERT_EQUAL(ResponseOutcome::RetryLater, classifyResponse(503));
}

static void test_backoff_doubles_from_thirty_seconds() {
    TEST_ASSERT_EQUAL_UINT32(30000, retryDelayMs(1, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(60000, retryDelayMs(2, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(120000, retryDelayMs(3, 0.5f));
}

static void test_backoff_caps_at_thirty_minutes() {
    TEST_ASSERT_EQUAL_UINT32(MAX_RETRY_DELAY_MS, retryDelayMs(7, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(MAX_RETRY_DELAY_MS, retryDelayMs(1000, 0.5f));
}

static void test_backoff_jitter_stays_within_twenty_percent() {
    TEST_ASSERT_EQUAL_UINT32(24000, retryDelayMs(1, 0.0f));
    TEST_ASSERT_UINT32_WITHIN(1, 36000, retryDelayMs(1, 0.999999f));
    TEST_ASSERT_UINT32_WITHIN(1, 36000, retryDelayMs(1, 5.0f)); // out-of-range input is clamped
    TEST_ASSERT_EQUAL_UINT32(24000, retryDelayMs(1, -1.0f));
}

static void test_first_enable_starts_at_current_shot_counter() {
    TEST_ASSERT_EQUAL_UINT32(171, reconcileCursor(-1, 171)); // legacy shots 0-170 are never sent
}

static void test_cursor_behind_counter_is_kept() {
    TEST_ASSERT_EQUAL_UINT32(168, reconcileCursor(168, 171));
    TEST_ASSERT_EQUAL_UINT32(171, reconcileCursor(171, 171));
}

static void test_cursor_ahead_of_reset_counter_follows_it_back() {
    TEST_ASSERT_EQUAL_UINT32(3, reconcileCursor(180, 3)); // NVS reset restarted ids at 0
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_epoch_is_trusted_only_after_sntp_sync);
    RUN_TEST(test_boot_relative_epoch_is_never_trusted);
    RUN_TEST(test_corrected_start_subtracts_elapsed_time);
    RUN_TEST(test_corrected_start_rounds_to_nearest_second);
    RUN_TEST(test_corrected_start_survives_millis_wrap);
    RUN_TEST(test_shot_with_trusted_start_is_uploaded);
    RUN_TEST(test_missing_or_corrupt_shot_is_skipped);
    RUN_TEST(test_untrusted_start_waits_for_pending_fix);
    RUN_TEST(test_untrusted_start_waits_while_clock_unsynced);
    RUN_TEST(test_untrusted_start_without_possible_fix_is_skipped);
    RUN_TEST(test_success_statuses_acknowledge);
    RUN_TEST(test_auth_failures_pause);
    RUN_TEST(test_validation_failures_are_permanent);
    RUN_TEST(test_transient_failures_retry);
    RUN_TEST(test_backoff_doubles_from_thirty_seconds);
    RUN_TEST(test_backoff_caps_at_thirty_minutes);
    RUN_TEST(test_backoff_jitter_stays_within_twenty_percent);
    RUN_TEST(test_first_enable_starts_at_current_shot_counter);
    RUN_TEST(test_cursor_behind_counter_is_kept);
    RUN_TEST(test_cursor_ahead_of_reset_counter_follows_it_back);
    return UNITY_END();
}
