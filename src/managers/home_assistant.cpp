#include <IPAddress.h> // fixes compilation issues with esp_websocket_client

#include "config.h"
#include "constants.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "managers/home_assistant.h"
#include "store.h"
#include <cJSON.h>
#include <cstring>

typedef struct home_assistant_context {
    EntityStore* store;
    const Configuration* config;
    esp_websocket_client_handle_t client;
    ConnState state;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;

    uint16_t event_id;
    char* json_buffer;
    size_t json_buffer_len;
    size_t json_buffer_cap;
    bool dropping_oversized_payload;
} home_assistant_context_t;

static const char* TAG = "home_assistant";

// Live websocket handle, captured once the task has connected. Used by
// home_assistant_shutdown() to send a clean close frame before deep sleep
// tears down WiFi underneath us. nullptr until launch_hass has progressed
// far enough to construct the client.
static esp_websocket_client_handle_t s_hass_client = nullptr;

static void hass_update_state(home_assistant_context_t* hass, ConnState state) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    ConnState previous_state = hass->state;
    hass->state = state;
    xSemaphoreGive(hass->mutex);

    if (previous_state == state) {
        return;
    }

    if (state == ConnState::Initializing) {
        // initial state at boot time, do nothing
    } else if (state == ConnState::ConnectionError && previous_state == ConnState::InvalidCredentials) {
        // keep invalid credentials in the UI, do nothing
    } else {
        store_set_hass_state(hass->store, state);
    }

    xTaskNotifyGive(hass->task);
}

static uint16_t hass_generate_event_id(home_assistant_context_t* hass) {
    xSemaphoreTake(hass->mutex, portMAX_DELAY);
    uint16_t event_id = hass->event_id++;
    xSemaphoreGive(hass->mutex);
    return event_id;
}

static void hass_cmd_authenticate(home_assistant_context_t* hass) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "auth");
    cJSON_AddStringToObject(root, "access_token", hass->config->home_assistant_token);
    char* request = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

static void hass_send_call_service(home_assistant_context_t* hass, const char* domain, const char* service, cJSON* service_data) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", hass_generate_event_id(hass));
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", domain);
    cJSON_AddStringToObject(root, "service", service);
    cJSON_AddItemToObject(root, "service_data", service_data);

    char* request = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending %s", request);
    esp_websocket_client_send_text(hass->client, request, strlen(request), portMAX_DELAY);
    cJSON_free(request);
    cJSON_Delete(root);
}

static void hass_send_command(home_assistant_context_t* hass, Command* cmd) {
    if (!cmd->entity_id) {
        ESP_LOGW(TAG, "Command missing entity_id, skipping");
        return;
    }

    switch (cmd->type) {
    case CommandType::MediaVolumeUp: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        hass_send_call_service(hass, "media_player", "volume_up", service_data);
        break;
    }
    case CommandType::MediaVolumeDown: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        hass_send_call_service(hass, "media_player", "volume_down", service_data);
        break;
    }
    case CommandType::MediaVolumeMute: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        cJSON_AddBoolToObject(service_data, "is_volume_muted", true);
        hass_send_call_service(hass, "media_player", "volume_mute", service_data);
        break;
    }
    case CommandType::MediaSelectSource: {
        if (!cmd->command_name) {
            ESP_LOGW(TAG, "MediaSelectSource missing source name");
            break;
        }
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        cJSON_AddStringToObject(service_data, "source", cmd->command_name);
        hass_send_call_service(hass, "media_player", "select_source", service_data);
        break;
    }
    case CommandType::RemoteSendCommand: {
        if (!cmd->command_name) {
            ESP_LOGW(TAG, "RemoteSendCommand missing command_name");
            break;
        }
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        cJSON_AddStringToObject(service_data, "command", cmd->command_name);
        hass_send_call_service(hass, "remote", "send_command", service_data);
        break;
    }
    case CommandType::ScriptTurnOn: {
        cJSON* service_data = cJSON_CreateObject();
        cJSON_AddStringToObject(service_data, "entity_id", cmd->entity_id);
        hass_send_call_service(hass, "script", "turn_on", service_data);
        break;
    }
    case CommandType::CallService: {
        if (!cmd->action || !cmd->action->domain || !cmd->action->service) {
            ESP_LOGW(TAG, "CallService missing domain/service");
            break;
        }
        cJSON* service_data = cJSON_CreateObject();
        if (cmd->action->entity_id) {
            cJSON_AddStringToObject(service_data, "entity_id", cmd->action->entity_id);
        }
        hass_send_call_service(hass, cmd->action->domain, cmd->action->service, service_data);
        break;
    }
    default:
        ESP_LOGI(TAG, "Service type not supported");
        break;
    }
}

static void hass_handle_server_payload(home_assistant_context_t* hass, cJSON* json) {
    cJSON* type_item = cJSON_GetObjectItem(json, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == nullptr) {
        return;
    }

    if (strcmp(type_item->valuestring, "auth_required") == 0) {
        ESP_LOGI(TAG, "Logging in to home assistant...");
        hass_cmd_authenticate(hass);
    } else if (strcmp(type_item->valuestring, "auth_invalid") == 0) {
        ESP_LOGI(TAG, "Auth invalid, marking InvalidCredentials");
        hass_update_state(hass, ConnState::InvalidCredentials);
    } else if (strcmp(type_item->valuestring, "auth_ok") == 0) {
        ESP_LOGI(TAG, "Authentication successful");
        hass_update_state(hass, ConnState::Up);
    } else if (strcmp(type_item->valuestring, "result") == 0) {
        // Best-effort: log non-success results. We don't track request IDs.
        cJSON* success_item = cJSON_GetObjectItem(json, "success");
        if (cJSON_IsBool(success_item) && !cJSON_IsTrue(success_item)) {
            ESP_LOGW(TAG, "HA result was not successful");
        }
    }
}

static void hass_ws_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    home_assistant_context_t* hass = static_cast<home_assistant_context_t*>(handler_args);
    esp_websocket_event_data_t* data = static_cast<esp_websocket_event_data_t*>(event_data);
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_DISCONNECTED");
        hass_update_state(hass, ConnState::ConnectionError);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "Received WEBSOCKET_EVENT_ERROR");
        hass_update_state(hass, ConnState::ConnectionError);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0 || data->op_code == 1) {
            xSemaphoreTake(hass->mutex, portMAX_DELAY);

            if (data->payload_offset == 0) {
                hass->json_buffer_len = 0;
                hass->dropping_oversized_payload = false;
            }

            if (hass->dropping_oversized_payload) {
                xSemaphoreGive(hass->mutex);
                return;
            }

            const size_t chunk_end = data->payload_offset + data->data_len;
            if (hass->json_buffer == nullptr || chunk_end > hass->json_buffer_cap) {
                ESP_LOGE(TAG, "JSON buffer overflow, discarding message payload_len=%d", data->payload_len);
                hass->dropping_oversized_payload = true;
                hass->json_buffer_len = 0;
                xSemaphoreGive(hass->mutex);
                return;
            }

            memcpy(hass->json_buffer + data->payload_offset, data->data_ptr, data->data_len);
            if (chunk_end > hass->json_buffer_len) {
                hass->json_buffer_len = chunk_end;
            }

            cJSON* json = nullptr;
            if (hass->json_buffer_len == data->payload_len && hass->json_buffer_len > 0) {
                json = cJSON_ParseWithLength(hass->json_buffer, hass->json_buffer_len);
                if (!json) {
                    ESP_LOGE(TAG, "JSON parsing failed");
                }
            }
            xSemaphoreGive(hass->mutex);

            if (json) {
                hass_handle_server_payload(hass, json);
                cJSON_Delete(json);
            }
        } else if (data->op_code == 8) {
            ESP_LOGI(TAG, "Received Connection Close frame");
            hass_update_state(hass, ConnState::ConnectionError);
        }
        break;
    default:
        ESP_LOGI(TAG, "Unknown event type %d", event_id);
    }
}

void home_assistant_task(void* arg) {
    HomeAssistantTaskArgs* ctx = static_cast<HomeAssistantTaskArgs*>(arg);
    EntityStore* store = ctx->store;

    ESP_LOGI(TAG, "Waiting for wifi...");
    store_wait_for_wifi_up(store);
    ESP_LOGI(TAG, "Wifi is up, connecting...");

    esp_websocket_client_config_t client_config = {
        .uri = ctx->config->home_assistant_url,
        .disable_auto_reconnect = true,
        .task_stack = 8192,
        .cert_pem = ctx->config->root_ca,
    };

    home_assistant_context_t* hass = new home_assistant_context_t{};
    hass->store = store;
    hass->config = ctx->config;
    hass->client = esp_websocket_client_init(&client_config);
    s_hass_client = hass->client;
    hass->mutex = xSemaphoreCreateMutex();
    hass->task = xTaskGetCurrentTaskHandle();
    hass->json_buffer_cap = HASS_MAX_JSON_BUFFER;
    hass->json_buffer = static_cast<char*>(heap_caps_malloc(hass->json_buffer_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (hass->json_buffer == nullptr) {
        ESP_LOGW(TAG, "PSRAM allocation failed, falling back to regular heap for JSON buffer");
        hass->json_buffer = static_cast<char*>(malloc(hass->json_buffer_cap));
    }
    if (hass->json_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer, cannot start Home Assistant client");
        hass_update_state(hass, ConnState::ConnectionError);
        vTaskDelete(nullptr);
    }
    hass->event_id = 1;

    esp_websocket_register_events(hass->client, WEBSOCKET_EVENT_ANY, hass_ws_event_handler, static_cast<void*>(hass));
    esp_err_t err = esp_websocket_client_start(hass->client);
    ESP_LOGI(TAG, "esp_websocket_client_start returned: %s", esp_err_to_name(err));

    Command command;
    bool previous_connect_failed = false;
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        xSemaphoreTake(hass->mutex, portMAX_DELAY);
        ConnState state = hass->state;
        xSemaphoreGive(hass->mutex);

        if (state == ConnState::InvalidCredentials || state == ConnState::ConnectionError) {
            ESP_LOGI(TAG, "Client is no longer connected, reconnecting...");

            err = esp_websocket_client_close(hass->client, portMAX_DELAY);
            ESP_LOGI(TAG, "esp_websocket_client_close returned %s", esp_err_to_name(err));

            store_wait_for_wifi_up(store);

            if (previous_connect_failed) {
                ESP_LOGI(TAG, "Waiting 10 seconds");
                vTaskDelay(pdMS_TO_TICKS(HASS_RECONNECT_DELAY_MS));
            }
            previous_connect_failed = true;

            ESP_LOGI(TAG, "Attempting to reconnect to home assistant");
            xSemaphoreTake(hass->mutex, portMAX_DELAY);
            hass->state = ConnState::Initializing;
            hass->event_id = 1;
            xSemaphoreGive(hass->mutex);
            store_flush_pending_commands(hass->store);

            err = esp_websocket_client_start(hass->client);
            ESP_LOGI(TAG, "esp_websocket_client_start returned %s", esp_err_to_name(err));
        }

        if (state == ConnState::Up) {
            previous_connect_failed = false;
            while (store_get_pending_command(store, &command)) {
                hass_send_command(hass, &command);
                vTaskDelay(pdMS_TO_TICKS(HASS_TASK_SEND_DELAY_MS));
            }
        }
    }
}

void home_assistant_shutdown() {
    if (s_hass_client == nullptr) {
        return;
    }
    // Send the WebSocket close frame and wait briefly for the server to
    // ack. Without this the TCP socket dies abruptly on WiFi teardown and
    // HA's server holds the half-open session until its keepalive expires.
    esp_err_t err = esp_websocket_client_close(s_hass_client, pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "home_assistant_shutdown: close returned %s", esp_err_to_name(err));
}
