// Host shim for esp_sntp.h: time is taken from the host clock, so these are no-ops.
#pragma once

#include <sys/time.h>

typedef enum { SNTP_SYNC_MODE_IMMED = 0, SNTP_SYNC_MODE_SMOOTH = 1 } sntp_sync_mode_t;

static inline void sntp_init(void) {}
static inline void sntp_stop(void) {}
static inline void sntp_set_sync_mode(sntp_sync_mode_t mode) { (void)mode; }
static inline void sntp_setoperatingmode(int mode) { (void)mode; }
static inline void sntp_setservername(int idx, const char *server) {
    (void)idx;
    (void)server;
}

typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);

// The host clock is always synced, so the callback fires as soon as it is installed.
static inline void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    if (callback)
        callback(&now);
}
static inline bool sntp_restart(void) { return true; }
