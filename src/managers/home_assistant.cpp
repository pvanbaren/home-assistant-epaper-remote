#include "config.h"
#include "constants.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "managers/home_assistant.h"
#include "store.h"
#include <cJSON.h>
#include <cstdio>
#include <cstring>

static const char* TAG = "home_assistant";

// HA's REST API health endpoint; appended to config->home_assistant_url
// (which must be the scheme+host+port base, no trailing slash and no
// path component).
constexpr const char* HASS_PROBE_PATH = "/api/";

// Set by home_assistant_task at startup; used by hass_request_probe() to
// nudge the next reconnect attempt. Both fields are written from one task
// and read from the task itself plus optionally hass_request_probe(); on
// ESP32 a 32-bit aligned TickType_t / pointer load-store is atomic, so no
// extra synchronisation is needed for the nudge path.
static EntityStore* s_hass_store = nullptr;
static volatile TickType_t s_next_retry = 0;

struct home_assistant_context {
    EntityStore* store;
    const Configuration* config;
    esp_http_client_handle_t client;
    ConnState state;
    char url_buf[256];
    char auth_header[512];
};

static void hass_set_state(home_assistant_context* hass, ConnState state) {
    if (state == hass->state) {
        return;
    }
    ConnState prev = hass->state;
    hass->state = state;
    if (state == ConnState::ConnectionError && prev == ConnState::InvalidCredentials) {
        // Keep the louder "invalid credentials" surface in the UI even if
        // a follow-up request can't reach the host. Token won't get less
        // wrong on its own.
        return;
    }
    store_set_hass_state(hass->store, state);
}

static void hass_map_response(home_assistant_context* hass, esp_err_t err, int status) {
    if (err != ESP_OK) {
        hass_set_state(hass, ConnState::ConnectionError);
        return;
    }
    if (status == 401 || status == 403) {
        hass_set_state(hass, ConnState::InvalidCredentials);
    } else if (status >= 200 && status < 300) {
        hass_set_state(hass, ConnState::Up);
    } else {
        hass_set_state(hass, ConnState::ConnectionError);
    }
}

static bool hass_build_url(home_assistant_context* hass, const char* path) {
    int n = snprintf(hass->url_buf, sizeof(hass->url_buf), "%s%s",
                     hass->config->home_assistant_url, path);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(hass->url_buf)) {
        ESP_LOGW(TAG, "URL too long for buffer (%d bytes)", n);
        return false;
    }
    return true;
}

static void hass_probe(home_assistant_context* hass) {
    if (!hass_build_url(hass, HASS_PROBE_PATH)) {
        return;
    }
    esp_http_client_set_url(hass->client, hass->url_buf);
    esp_http_client_set_method(hass->client, HTTP_METHOD_GET);
    esp_http_client_set_post_field(hass->client, nullptr, 0);

    esp_err_t err = esp_http_client_perform(hass->client);
    int status = esp_http_client_get_status_code(hass->client);
    ESP_LOGI(TAG, "Probe %s: err=%s status=%d", hass->url_buf, esp_err_to_name(err), status);
    hass_map_response(hass, err, status);
}

// Takes ownership of service_data and frees it before returning. When
// enqueued_ms is non-zero, transport errors retry every 200 ms until the
// command's MEDIA_COMMAND_MAX_AGE_MS lifetime elapses or Wi-Fi drops —
// masks brief TLS/TCP hiccups so the user doesn't see "lost connection"
// for a one-off blip on an otherwise healthy network.
static void hass_post_service(home_assistant_context* hass, const char* domain, const char* service, cJSON* service_data, uint32_t enqueued_ms = 0) {
    char path[128];
    int n = snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) {
        ESP_LOGW(TAG, "Service path too long for %s.%s", domain, service);
        cJSON_Delete(service_data);
        return;
    }
    if (!hass_build_url(hass, path)) {
        cJSON_Delete(service_data);
        return;
    }

    char* body = cJSON_PrintUnformatted(service_data);
    cJSON_Delete(service_data);
    if (!body) {
        ESP_LOGW(TAG, "Failed to serialize service_data");
        return;
    }

    esp_http_client_set_url(hass->client, hass->url_buf);
    esp_http_client_set_method(hass->client, HTTP_METHOD_POST);
    esp_http_client_set_header(hass->client, "Content-Type", "application/json");
    esp_http_client_set_post_field(hass->client, body, strlen(body));

    ESP_LOGI(TAG, "POST %s body=%s", hass->url_buf, body);

    esp_err_t err = ESP_FAIL;
    int status = 0;
    const bool retry_allowed = (enqueued_ms != 0);
    while (true) {
        err = esp_http_client_perform(hass->client);
        status = esp_http_client_get_status_code(hass->client);

        // Server responded (even with 4xx/5xx): transport is healthy, stop
        // retrying — this is HA telling us something, not the network.
        if (err == ESP_OK) break;
        if (!retry_allowed) break;

        const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - enqueued_ms >= MEDIA_COMMAND_MAX_AGE_MS) {
            ESP_LOGW(TAG, "POST %s: command expired, giving up", hass->url_buf);
            break;
        }
        // No point hammering retries over a dead radio — the Wi-Fi UI will
        // surface its own disconnect screen.
        const EventBits_t bits = xEventGroupGetBits(hass->store->event_group);
        if (!(bits & BIT_WIFI_UP)) {
            ESP_LOGW(TAG, "POST %s: Wi-Fi down, giving up", hass->url_buf);
            break;
        }
        ESP_LOGW(TAG, "POST %s: transport err=%s, retrying in 200 ms", hass->url_buf, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    cJSON_free(body);

    if (err != ESP_OK || status >= 400) {
        ESP_LOGW(TAG, "POST %s failed: err=%s status=%d", hass->url_buf, esp_err_to_name(err), status);
    }
    hass_map_response(hass, err, status);
}

static void hass_send_command(home_assistant_context* hass, Command* cmd) {
    if (!cmd->entity_id) {
        ESP_LOGW(TAG, "Command missing entity_id, skipping");
        return;
    }

    switch (cmd->type) {
    case CommandType::MediaVolumeUp: {
        cJSON* sd = cJSON_CreateObject();
        cJSON_AddStringToObject(sd, "entity_id", cmd->entity_id);
        hass_post_service(hass, "media_player", "volume_up", sd, cmd->enqueued_ms);
        break;
    }
    case CommandType::MediaVolumeDown: {
        cJSON* sd = cJSON_CreateObject();
        cJSON_AddStringToObject(sd, "entity_id", cmd->entity_id);
        hass_post_service(hass, "media_player", "volume_down", sd, cmd->enqueued_ms);
        break;
    }
    case CommandType::MediaVolumeMute: {
        cJSON* sd = cJSON_CreateObject();
        cJSON_AddStringToObject(sd, "entity_id", cmd->entity_id);
        cJSON_AddBoolToObject(sd, "is_volume_muted", true);
        hass_post_service(hass, "media_player", "volume_mute", sd, cmd->enqueued_ms);
        break;
    }
    case CommandType::MediaSelectSource: {
        if (!cmd->command_name) {
            ESP_LOGW(TAG, "MediaSelectSource missing source name");
            break;
        }
        cJSON* sd = cJSON_CreateObject();
        cJSON_AddStringToObject(sd, "entity_id", cmd->entity_id);
        cJSON_AddStringToObject(sd, "source", cmd->command_name);
        hass_post_service(hass, "media_player", "select_source", sd, cmd->enqueued_ms);
        break;
    }
    case CommandType::RemoteSendCommand: {
        if (!cmd->command_name) {
            ESP_LOGW(TAG, "RemoteSendCommand missing command_name");
            break;
        }
        cJSON* sd = cJSON_CreateObject();
        cJSON_AddStringToObject(sd, "entity_id", cmd->entity_id);
        cJSON_AddStringToObject(sd, "command", cmd->command_name);
        hass_post_service(hass, "remote", "send_command", sd, cmd->enqueued_ms);
        break;
    }
    case CommandType::CallService: {
        if (!cmd->action || !cmd->action->domain || !cmd->action->service) {
            ESP_LOGW(TAG, "CallService missing domain/service");
            break;
        }
        cJSON* sd = cJSON_CreateObject();
        if (cmd->action->entity_id) {
            cJSON_AddStringToObject(sd, "entity_id", cmd->action->entity_id);
        }
        hass_post_service(hass, cmd->action->domain, cmd->action->service, sd, cmd->enqueued_ms);
        break;
    }
    default:
        ESP_LOGI(TAG, "Service type not supported");
        break;
    }
}

void home_assistant_task(void* arg) {
    HomeAssistantTaskArgs* ctx = static_cast<HomeAssistantTaskArgs*>(arg);
    EntityStore* store = ctx->store;
    s_hass_store = store;

    ESP_LOGI(TAG, "Waiting for wifi...");
    store_wait_for_wifi_up(store);
    ESP_LOGI(TAG, "Wifi is up, connecting...");

    auto* hass = new home_assistant_context{};
    hass->store = store;
    hass->config = ctx->config;
    hass->state = ConnState::Initializing;

    int n = snprintf(hass->auth_header, sizeof(hass->auth_header), "Bearer %s",
                     ctx->config->home_assistant_token);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(hass->auth_header)) {
        ESP_LOGE(TAG, "Token too long for auth header buffer");
        hass_set_state(hass, ConnState::InvalidCredentials);
        vTaskDelete(nullptr);
    }

    esp_http_client_config_t cfg = {};
    cfg.url = ctx->config->home_assistant_url; // overridden per request via set_url
    cfg.cert_pem = ctx->config->root_ca;
    // home_assistant_url may use a numeric IP rather than a hostname (e.g.
    // when bypassing DNS for a private LAN HA instance). The cert chain of
    // trust is still verified against root_ca; only the SAN/CN match
    // against the host string is skipped.
    cfg.skip_cert_common_name_check = true;
    cfg.keep_alive_enable = true;
    cfg.timeout_ms = 5000;

    hass->client = esp_http_client_init(&cfg);
    esp_http_client_set_header(hass->client, "Authorization", hass->auth_header);

    hass_probe(hass);

    Command command;
    s_next_retry = xTaskGetTickCount() + pdMS_TO_TICKS(HASS_RECONNECT_DELAY_MS);
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        if (hass->state == ConnState::ConnectionError && xTaskGetTickCount() >= s_next_retry) {
            store_wait_for_wifi_up(store);
            hass_probe(hass);
            s_next_retry = xTaskGetTickCount() + pdMS_TO_TICKS(HASS_RECONNECT_DELAY_MS);
        }

        if (hass->state == ConnState::Up) {
            while (store_get_pending_command(store, &command)) {
                hass_send_command(hass, &command);
                vTaskDelay(pdMS_TO_TICKS(HASS_TASK_SEND_DELAY_MS));
                if (hass->state != ConnState::Up) {
                    // Last request bumped us off Up — let the stale-command
                    // drop in store_get_pending_command discard the rest of
                    // the queue rather than firing them at a broken endpoint.
                    break;
                }
            }
        }
    }
}

void hass_request_probe() {
    if (!s_hass_store || !s_hass_store->home_assistant_task) {
        return;
    }
    // Pull the next probe forward to "now" so the task's retry-gate fires
    // on its next wake, and notify the task so that wake happens immediately
    // instead of after the 1 s ulTaskNotifyTake timeout.
    s_next_retry = xTaskGetTickCount();
    xTaskNotifyGive(s_hass_store->home_assistant_task);
}
