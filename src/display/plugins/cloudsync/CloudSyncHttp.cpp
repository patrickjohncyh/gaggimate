#include "CloudSyncHttp.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

// Arduino replaces the IDF bundle with its own copy; attach only works after the bundle was set.
extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");

namespace cloudsync {

namespace {
constexpr const char *LOG_TAG = "CloudSyncHttp";
constexpr int TIMEOUT_MS = 15000;
// Worker replies are small JSON objects; anything longer is not worth reading.
constexpr size_t MAX_RESPONSE_BYTES = 512;
constexpr size_t WRITE_CHUNK_BYTES = 4096;

String readErrorCode(esp_http_client_handle_t client) {
    char responseBody[MAX_RESPONSE_BYTES + 1];
    int totalRead = 0;
    while (totalRead < static_cast<int>(MAX_RESPONSE_BYTES)) {
        const int bytesRead = esp_http_client_read(client, responseBody + totalRead, MAX_RESPONSE_BYTES - totalRead);
        if (bytesRead <= 0) {
            break;
        }
        totalRead += bytesRead;
    }
    responseBody[totalRead] = '\0';
    JsonDocument doc;
    if (totalRead == 0 || deserializeJson(doc, responseBody) != DeserializationError::Ok) {
        return "";
    }
    return doc["error"].is<const char *>() ? String(doc["error"].as<const char *>()) : String("");
}
} // namespace

HttpResponse sendJsonRequest(const char *method, const String &url, const String &token, const char *body, size_t bodySize) {
    HttpResponse response;
    if (!url.startsWith("https://")) {
        response.errorCode = "https_required";
        return response;
    }

    arduino_esp_crt_bundle_set(x509_crt_imported_bundle_bin_start);
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = strcmp(method, "PUT") == 0 ? HTTP_METHOD_PUT : HTTP_METHOD_POST;
    config.crt_bundle_attach = arduino_esp_crt_bundle_attach;
    config.timeout_ms = TIMEOUT_MS;
    config.user_agent = "GaggiMate-CloudSync";
    config.disable_auto_redirect = true; // a redirect would re-send the bearer token to another host
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.errorCode = "client_init_failed";
        return response;
    }

    const String authorization = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(bodySize));
    if (err != ESP_OK) {
        ESP_LOGW(LOG_TAG, "Connection failed: method=%s error=%s", method, esp_err_to_name(err));
        response.errorCode = "connect_failed";
        esp_http_client_cleanup(client);
        return response;
    }

    size_t written = 0;
    while (written < bodySize) {
        const size_t chunkBytes = std::min(WRITE_CHUNK_BYTES, bodySize - written);
        const int sent = esp_http_client_write(client, body + written, static_cast<int>(chunkBytes));
        if (sent <= 0) {
            break;
        }
        written += static_cast<size_t>(sent);
    }
    if (written < bodySize) {
        ESP_LOGW(LOG_TAG, "Request body truncated: method=%s sent=%u of=%u", method, static_cast<unsigned>(written),
                 static_cast<unsigned>(bodySize));
        response.errorCode = "send_failed";
    } else if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGW(LOG_TAG, "No response headers: method=%s", method);
        response.errorCode = "no_response";
    } else {
        response.status = esp_http_client_get_status_code(client);
        if (response.status >= 300) {
            response.errorCode = readErrorCode(client);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return response;
}

} // namespace cloudsync
