//
// Created by wizche on 1/7/22.
//
#include <string.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "config.h"
#include "mqtt_client.h"
#include "cJSON.h"

static const char *TAG = "mesh_mqtt";
static esp_mqtt_client_handle_t s_client = NULL;

static char *build_json_message();

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            if (esp_mqtt_client_subscribe(s_client, TOPIC, 0) < 0) {
                // Disconnect to retry the subscribe after auto-reconnect timeout
                esp_mqtt_client_disconnect(s_client);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            //ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            //ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            //ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            //ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

void mqtt_app_publish(char *topic, char *publish_string) {
    if (s_client) {
        int msg_id = esp_mqtt_client_publish(s_client, topic, publish_string, 0, 1, 0);
        ESP_LOGI(TAG, "sent publish returned msg_id=%d", msg_id);
    }
}

static char *build_json_message() {
    char *string = NULL;
    cJSON *info = cJSON_CreateObject();

    // SSID
    char *id = NULL;
    uint8_t stamac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_STA, stamac);
    asprintf(&id, "ESPM_%2X%2X%2X", stamac[3], stamac[4], stamac[5]);
    if (cJSON_AddStringToObject(info, "id", id) == NULL) {
        free(id);
        goto end;
    }
    free(id);

    // LAYER
    if (cJSON_AddNumberToObject(info, "layer", esp_mesh_get_layer()) == NULL) {
        goto end;
    }
    // MESH TOPOLOGY
    if (cJSON_AddNumberToObject(info, "topology", esp_mesh_get_topology()) == NULL) {
        goto end;
    }
    // ROOT
    if (cJSON_AddBoolToObject(info, "root", esp_mesh_is_root()) == NULL) {
        goto end;
    }
    // STA RSSI
    wifi_ap_record_t stawifidata;
    if (esp_wifi_sta_get_ap_info(&stawifidata)) {
        goto end;
    }
    if (cJSON_AddStringToObject(info, "ssid", (const char *) stawifidata.ssid) == NULL) {
        goto end;
    }
    if (cJSON_AddNumberToObject(info, "rssi", stawifidata.rssi) == NULL) {
        goto end;
    }
    // IPs
    struct netif *netif;
    NETIF_FOREACH(netif) {
        if (strcmp(netif->name, "lo") == 0) continue;
        char *ipstr;
        char *intf_label;
        asprintf(&intf_label, "%c%c_ip", netif->name[0], netif->name[1]);
        asprintf(&ipstr, IPSTR, IP2STR(&netif->ip_addr.u_addr.ip4));
        if (cJSON_AddStringToObject(info, intf_label, ipstr) == NULL) {
            free(intf_label);
            free(ipstr);
            goto end;
        }
        free(intf_label);
        free(ipstr);
    }
    // mesh-id
    char *meshid;
    asprintf(&meshid, "%2X%2X%2X%2X%2X%2X",
             MESH_ID[0], MESH_ID[1], MESH_ID[2], MESH_ID[3], MESH_ID[4], MESH_ID[5]);
    if (cJSON_AddStringToObject(info, "mesh_id", meshid) == NULL) {
        free(meshid);
        goto end;
    }
    free(meshid);

    // routing table
    if (cJSON_AddNumberToObject(info, "routing_table_size", esp_mesh_get_routing_table_size()) == NULL) {
        goto end;
    }

    // HEAP free
    if (cJSON_AddNumberToObject(info, "heap_free", esp_get_free_heap_size()) == NULL) {
        goto end;
    }

    // Serialize
    string = cJSON_Print(info);
    if (string == NULL) {
        fprintf(stderr, "Failed to print monitor.\n");
    }
    end:
    cJSON_Delete(info);
    return string;
}

static void print_stats(void *args) {
    while (1) {
        uint8_t apmac[MAC_ADDR_LEN];
        uint8_t stamac[MAC_ADDR_LEN];
        esp_wifi_get_mac(WIFI_IF_AP, apmac);
        esp_wifi_get_mac(WIFI_IF_STA, stamac);
        ESP_LOGI(TAG,
                 "[%s] LAYER: %1d | TYPE: %1d | MESH_SSID: ESPM_%2X%2X%2X | TOPO: %s | NODE#: %d | AP: " MACSTR " | STA: " MACSTR,
                 esp_mesh_is_root() ? "ROOT" : "NODE", esp_mesh_get_layer(), esp_mesh_get_type(), stamac[3], stamac[4],
                 stamac[5],
                 esp_mesh_get_topology() == MESH_TOPO_TREE ? "tree" : "chain", esp_mesh_get_total_node_num(),
                 MAC2STR(apmac), MAC2STR(stamac));
        char *json = build_json_message();
        mqtt_app_publish(TOPIC, json);
        free(json);
        vTaskDelay(2000 / portTICK_RATE_MS);
    }
}

void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
            .uri = MQTT_SERVER,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, s_client);
    esp_mqtt_client_start(s_client);

    xTaskCreate(print_stats, "print_stats", 3072, NULL, 5, NULL);
}
