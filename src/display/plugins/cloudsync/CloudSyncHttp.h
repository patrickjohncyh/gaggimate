#ifndef CLOUDSYNCHTTP_H
#define CLOUDSYNCHTTP_H

#include <Arduino.h>
#include <cstddef>

namespace cloudsync {

/** Outcome of one request: the HTTP status (<= 0 when none arrived) and the server's error code. */
struct HttpResponse {
    int status = 0;
    String errorCode; // "error" field of a JSON error body, or a local transport failure
};

/**
 * Sends one JSON request to the gaggi Worker over HTTPS with a device bearer token.
 *
 * Uses esp_http_client with the Arduino certificate bundle (the same chain validation as OTA) and
 * refuses plain HTTP, so the token never leaves the device unencrypted. Blocks for up to the 15 s
 * timeout; call it from the CloudSync worker task only.
 *
 * @param method    "POST" or "PUT"
 * @param url       full https:// URL
 * @param token     device bearer token (never logged)
 * @param body      JSON request body
 * @param bodySize  bytes in body
 * @return status and error code; status <= 0 means no response (DNS, connect, TLS, timeout)
 *
 * Example:
 *   HttpResponse response = sendJsonRequest("POST", baseUrl + "/v1/device/shots", token, json, jsonSize);
 *   if (classifyResponse(response.status) == ResponseOutcome::Acknowledged) { advanceCursor(); }
 */
HttpResponse sendJsonRequest(const char *method, const String &url, const String &token, const char *body, size_t bodySize);

} // namespace cloudsync

#endif // CLOUDSYNCHTTP_H
