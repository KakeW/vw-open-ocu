#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/twai.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#if __has_include("gateway_config.h")
#include "gateway_config.h"
#else
#include "gateway_config.example.h"
#endif

#define WIFI_READY BIT0
#define MQTT_READY BIT1
#define BATCH_MAX 48
#define QUEUE_DEPTH 256
#define HEADER_SIZE 12
#define FRAME_MAX_SIZE 29
#define HEARTBEAT_US 5000000

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_us;
    uint32_t flags_id;
    uint8_t dlc;
    uint8_t data[8];
} frame_t;

static const struct {
    uint32_t id;
    bool extended;
} allowlist[] = {{.id = UINT32_MAX} OCU_ALLOWLIST};

static EventGroupHandle_t connections;
static QueueHandle_t frames;
static atomic_uint dropped_queue;
static atomic_uint dropped_transport;
static atomic_uint filtered;
static atomic_uint unsupported_dlc;
static atomic_uint queue_peak;
static atomic_uint connection_epoch;

static bool allowed(const twai_message_t *message)
{
    if (OCU_CAPTURE_MODE) {
        return true;
    }
    for (size_t i = 1; i < sizeof(allowlist) / sizeof(allowlist[0]); ++i) {
        if (allowlist[i].id == message->identifier && allowlist[i].extended == message->extd) {
            return true;
        }
    }
    return false;
}

static void receive_frames(void *unused)
{
    uint64_t sequence = 0;
    for (;;) {
        twai_message_t message;
        if (twai_receive(&message, portMAX_DELAY) != ESP_OK) {
            vTaskDelay(1);
            continue;
        }
        uint64_t timestamp = esp_timer_get_time();
        if (message.data_length_code > 8) {
            atomic_fetch_add(&unsupported_dlc, 1);
            continue;
        }
        if (!allowed(&message)) {
            atomic_fetch_add(&filtered, 1);
            continue;
        }
        frame_t frame = {
            .sequence = sequence++,
            .timestamp_us = timestamp,
            .flags_id = message.identifier | (message.extd ? BIT31 : 0) |
                        (message.rtr ? BIT30 : 0),
            .dlc = message.data_length_code,
        };
        if (!message.rtr) {
            memcpy(frame.data, message.data, frame.dlc);
        }
        if (xQueueSend(frames, &frame, 0) != pdTRUE) {
            atomic_fetch_add(&dropped_queue, 1);
            continue;
        }
        unsigned depth = uxQueueMessagesWaiting(frames);
        if (depth > atomic_load(&queue_peak)) {
            atomic_store(&queue_peak, depth);
        }
    }
}

static uint8_t *put_be(uint8_t *dest, uint64_t value, unsigned width)
{
    for (unsigned i = width; i > 0; --i) {
        *dest++ = (uint8_t)(value >> ((i - 1) * 8));
    }
    return dest;
}

static uint8_t *encode_frame(uint8_t *dest, const frame_t *frame)
{
    dest = put_be(dest, frame->sequence, 8);
    dest = put_be(dest, frame->timestamp_us, 8);
    dest = put_be(dest, frame->flags_id, 4);
    *dest++ = frame->dlc;
    if (!(frame->flags_id & BIT30)) {
        memcpy(dest, frame->data, frame->dlc);
        dest += frame->dlc;
    }
    return dest;
}

static void discard_queued(void)
{
    // Take a finite snapshot so a busy bus cannot keep this loop running.
    unsigned queued = uxQueueMessagesWaiting(frames);
    frame_t discarded;
    for (unsigned i = 0; i < queued; ++i) {
        if (xQueueReceive(frames, &discarded, 0) == pdTRUE) {
            atomic_fetch_add(&dropped_transport, 1);
        }
    }
}

static bool heartbeat(esp_mqtt_client_handle_t client, uint64_t session,
                      unsigned epoch, const uint64_t *last_published_sequence)
{
    twai_status_info_t info;
    ESP_ERROR_CHECK(twai_get_status_info(&info));
    char payload[512];
    char published[21] = "null";
    if (last_published_sequence != NULL) {
        snprintf(published, sizeof(published), "%" PRIu64, *last_published_sequence);
    }
    int length = snprintf(payload, sizeof(payload),
        "{\"version\":1,\"session\":\"%016" PRIx64 "\",\"capture_mode\":%s,"
        "\"connection_epoch\":%u,\"last_published_sequence\":%s,"
        "\"queue_depth\":%u,\"queue_capacity\":%u,\"queue_peak\":%u,"
        "\"queue_dropped\":%u,\"transport_dropped\":%u,\"filtered\":%u,"
        "\"unsupported_dlc\":%u,\"driver_missed\":%" PRIu32 ",\"driver_overrun\":%" PRIu32 ","
        "\"bus_errors\":%" PRIu32 ",\"rx_errors\":%" PRIu32 ",\"controller_state\":%d}",
        session, OCU_CAPTURE_MODE ? "true" : "false", epoch, published,
        (unsigned)uxQueueMessagesWaiting(frames), QUEUE_DEPTH, atomic_load(&queue_peak),
        atomic_load(&dropped_queue), atomic_load(&dropped_transport), atomic_load(&filtered),
        atomic_load(&unsupported_dlc), info.rx_missed_count, info.rx_overrun_count,
        info.bus_error_count, info.rx_error_counter, (int)info.state);
    return length > 0 && length < sizeof(payload) &&
        esp_mqtt_client_publish(client, "vw_open_ocu/" OCU_GATEWAY_ID "/v1/status",
                                payload, length, 0, 0) >= 0;
}

static void publish_frames(void *argument)
{
    esp_mqtt_client_handle_t client = argument;
    uint64_t session = ((uint64_t)esp_random() << 32) | esp_random();
    uint8_t batch[HEADER_SIZE + BATCH_MAX * FRAME_MAX_SIZE] = {'O', 'C', 'U', 1};
    put_be(batch + 4, session, 8);
    int64_t next_heartbeat = 0;
    unsigned epoch = 0;
    uint64_t last_published_sequence = 0;
    bool published_frames = false;

    for (;;) {
        unsigned current_epoch = atomic_load(&connection_epoch);
        EventBits_t bits = xEventGroupGetBits(connections);
        if ((bits & (WIFI_READY | MQTT_READY)) != (WIFI_READY | MQTT_READY)) {
            discard_queued();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (current_epoch != epoch) {
            discard_queued();
            epoch = current_epoch;
            next_heartbeat = 0;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_heartbeat) {
            if (!heartbeat(client, session, epoch,
                           published_frames ? &last_published_sequence : NULL)) {
                discard_queued();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            next_heartbeat = now + HEARTBEAT_US;
        }

        uint8_t *end = batch + HEADER_SIZE;
        unsigned count = 0;
        frame_t frame;
        // Wait briefly for the first frame and allow a short coalescing window.
        if (xQueueReceive(frames, &frame, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }
        end = encode_frame(end, &frame);
        uint64_t batch_last_sequence = frame.sequence;
        ++count;
        if (uxQueueMessagesWaiting(frames) < BATCH_MAX - 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        while (count < BATCH_MAX && xQueueReceive(frames, &frame, 0) == pdTRUE) {
            end = encode_frame(end, &frame);
            batch_last_sequence = frame.sequence;
            ++count;
        }
        if (epoch != atomic_load(&connection_epoch) ||
            esp_mqtt_client_publish(client, "vw_open_ocu/" OCU_GATEWAY_ID "/v1/frames",
                                    (const char *)batch, end - batch, 0, 0) < 0) {
            atomic_fetch_add(&dropped_transport, count);
        } else {
            last_published_sequence = batch_last_sequence;
            published_frames = true;
        }
    }
}

static void wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(connections, WIFI_READY);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(connections, WIFI_READY | MQTT_READY);
        atomic_fetch_add(&connection_epoch, 1);
    }
}

static void connect_wifi(void *unused)
{
    for (;;) {
        if (!(xEventGroupGetBits(connections) & WIFI_READY)) {
            esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGD("ocu", "Wi-Fi connect: %s", esp_err_to_name(result));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void mqtt_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    if (id == MQTT_EVENT_CONNECTED) {
        atomic_fetch_add(&connection_epoch, 1);
        xEventGroupSetBits(connections, MQTT_READY);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(connections, MQTT_READY);
        atomic_fetch_add(&connection_epoch, 1);
    }
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    connections = xEventGroupCreate();
    frames = xQueueCreate(QUEUE_DEPTH, sizeof(frame_t));
    ESP_ERROR_CHECK(connections && frames ? ESP_OK : ESP_ERR_NO_MEM);

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(0, 1, TWAI_MODE_LISTEN_ONLY);
    general.tx_queue_len = 0;
    general.rx_queue_len = 128;
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&general, &timing, &filter));
    ESP_ERROR_CHECK(twai_start());
    ESP_ERROR_CHECK(xTaskCreate(receive_frames, "can_rx", 3072, NULL, 12, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t config = {.sta = {.ssid = OCU_WIFI_SSID, .password = OCU_WIFI_PASSWORD,
                                   .threshold.authmode = WIFI_AUTH_WPA2_PSK}};
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(xTaskCreate(connect_wifi, "wifi_retry", 3072, NULL, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);

    xEventGroupWaitBits(connections, WIFI_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(OCU_SNTP_SERVER);
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW("ocu", "Waiting for SNTP before MQTT TLS");
    }

    const esp_mqtt_client_config_t mqtt = {
        // The mqtts:// URI selects TLS. ESP-MQTT rejects a second transport selector.
        .broker.address.uri = OCU_MQTT_URI,
        .broker.verification.certificate = OCU_MQTT_CA_PEM,
        .broker.verification.crt_bundle_attach = OCU_MQTT_CA_PEM ? NULL : esp_crt_bundle_attach,
        .credentials.username = OCU_MQTT_USERNAME,
        .credentials.authentication.password = OCU_MQTT_PASSWORD,
        .credentials.client_id = "vw-open-ocu-" OCU_GATEWAY_ID,
        .network.timeout_ms = 2000,
    };
    ESP_ERROR_CHECK(strncmp(OCU_MQTT_URI, "mqtts://", 8) == 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt);
    ESP_ERROR_CHECK(client ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    ESP_ERROR_CHECK(xTaskCreate(publish_frames, "can_mqtt", 6144, client, 5, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}
