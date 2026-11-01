/* MQTT over SSL Example with RGB LED Control

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "freertos/task.h"
#include "led_strip.h" // <-- Added for RGB LED control
#include "cJSON.h"


static const char *TAG = "mqtts_example";
static esp_mqtt_client_handle_t mqtt_client;

// --- RGB LED Configuration ---
#define BLINK_GPIO GPIO_NUM_38
static led_strip_handle_t led_strip;

static void configure_led(void)
{
    /* LED strip general initialization, according to your LED board design */
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    
    /* Set all LED off initially */
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "RGB LED initialized on GPIO %d", BLINK_GPIO);
}

static void set_led_color(uint8_t red, uint8_t green, uint8_t blue)
{
    /* Set the LED color and refresh */
    led_strip_set_pixel(led_strip, 0, red, green, blue);
    led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "LED Color set to: R=%d, G=%d, B=%d", red, green, blue);
}

#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipseprojects_io_pem_start[]  = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipseprojects_io_pem_start[]   asm("_binary_mqtt_eclipseprojects_io_pem_start");
#endif
extern const uint8_t mqtt_eclipseprojects_io_pem_end[]   asm("_binary_mqtt_eclipseprojects_io_pem_end");

static void send_binary(esp_mqtt_client_handle_t client)
{
    esp_partition_mmap_handle_t out_handle;
    const void *binary_address;
    const esp_partition_t *partition = esp_ota_get_running_partition();
    esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &binary_address, &out_handle);
    int binary_size = MIN(CONFIG_BROKER_BIN_SIZE_TO_SEND, partition->size);
    int msg_id = esp_mqtt_client_publish(client, "/topic/binary", binary_address, binary_size, 0, 0);
    ESP_LOGI(TAG, "binary sent with msg_id=%d", msg_id);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        
        // --- NEW: Subscribe to LED control topics ---
        msg_id = esp_mqtt_client_subscribe(client, "esp32s3/led/control", 1);
        ESP_LOGI(TAG, "Subscribed to LED control, msg_id=%d", msg_id);
        
        msg_id = esp_mqtt_client_subscribe(client, "esp32s3/rgb", 1);
        ESP_LOGI(TAG, "Subscribed to RGB control, msg_id=%d", msg_id);
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        //msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
        
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        
        if (strncmp(event->data, "send binary please", event->data_len) == 0) {
            ESP_LOGI(TAG, "Sending the binary");
            send_binary(client);
        }
        
        // Handle LED control commands (supports both plain text and JSON)
        if (strncmp(event->topic, "esp32s3/led/control", event->topic_len) == 0) {
            char data_str[64] = {0};
            memcpy(data_str, event->data, event->data_len);
            
            bool turn_on = false;
            
            // Try to parse as JSON first: {"msg": "ON"}
            cJSON *json = cJSON_Parse(data_str);
            if (json != NULL) {
                cJSON *msg = cJSON_GetObjectItem(json, "msg");
                if (cJSON_IsString(msg) && (strcmp(msg->valuestring, "ON") == 0)) {
                    turn_on = true;
                }
                cJSON_Delete(json);
            }
            // Fall back to plain text
            else if (strncmp(event->data, "ON", event->data_len) == 0) {
                turn_on = true;
            }
            
            if (turn_on) {
                ESP_LOGI(TAG, "Turning LED ON (White)");
                set_led_color(255, 255, 255);
                esp_mqtt_client_publish(client, "esp32s3/led/status", "ON", 0, 1, 0);
            } else {
                ESP_LOGI(TAG, "Turning LED OFF");
                set_led_color(0, 0, 0);
                esp_mqtt_client_publish(client, "esp32s3/led/status", "OFF", 0, 1, 0);
            }
        }
        
        // Handle RGB color commands (supports both plain text and JSON)
        if (strncmp(event->topic, "esp32s3/rgb", event->topic_len) == 0) {
            // Make a safe, null-terminated copy of the payload
            char data_str[64] = {0};
            size_t len = event->data_len;
            if (len > sizeof(data_str) - 1) {
                len = sizeof(data_str) - 1;
            }
            memcpy(data_str, event->data, len);
            data_str[len] = '\0';   // <-- Guarantee termination

            int r = 0, g = 0, b = 0;
            bool color_set = false;

            // Try JSON first: {"r":255,"g":0,"b":0}
            cJSON *json = cJSON_Parse(data_str);
            if (json != NULL) {
                cJSON *r_json = cJSON_GetObjectItem(json, "r");
                cJSON *g_json = cJSON_GetObjectItem(json, "g");
                cJSON *b_json = cJSON_GetObjectItem(json, "b");
                if (cJSON_IsNumber(r_json) && cJSON_IsNumber(g_json) && cJSON_IsNumber(b_json)) {
                    r = r_json->valueint;
                    g = g_json->valueint;
                    b = b_json->valueint;
                    color_set = true;
                    ESP_LOGI(TAG, "Parsed JSON RGB: r=%d, g=%d, b=%d", r, g, b);
                }
                cJSON_Delete(json);
            }

            // Fall back to plain text: "255,0,0"
            // FIX: parse data_str (null-terminated), NOT event->data (raw buffer)!
            if (!color_set) {
                if (sscanf(data_str, "%d,%d,%d", &r, &g, &b) == 3) {
                    color_set = true;
                    ESP_LOGI(TAG, "Parsed plain text RGB: r=%d, g=%d, b=%d", r, g, b);
                }
            }

            if (color_set) {
                // Clamp to 0-255
                r = (r < 0) ? 0 : (r > 255) ? 255 : r;
                g = (g < 0) ? 0 : (g > 255) ? 255 : g;
                b = (b < 0) ? 0 : (b > 255) ? 255 : b;

                set_led_color(r, g, b);

                char status[32];
                snprintf(status, sizeof(status), "%d,%d,%d", r, g, b);
                esp_mqtt_client_publish(client, "esp32s3/rgb/status", status, 0, 1, 0);
            } else {
                ESP_LOGW(TAG, "Failed to parse RGB data: %s", data_str);
            }
        }
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
        
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_publish_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    int counter = 0;
    char payload[64];

    while (1) {
        counter++;
        snprintf(payload, sizeof(payload), "%d", counter);  //Hello Ubuntu! Count: 
        
        int msg_id = esp_mqtt_client_publish(mqtt_client, "/topic/qos0", payload, 0, 1, 0);
        ESP_LOGI(TAG, "Published: %s (msg_id=%d)", payload, msg_id);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void mqtt_app_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "esp32s3_%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Using Unique Client ID: %s", client_id);

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_BROKER_URI,
            .verification.crt_bundle_attach = esp_crt_bundle_attach, 
        },
        .credentials = {
            .client_id = client_id, 
        },
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1. Initialize RGB LED first
    configure_led();

    // 2. Connect to WiFi
    ESP_ERROR_CHECK(example_connect());

    // 3. Start MQTT
    mqtt_app_start();
}