/* Non-Mesh IP Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>
#include <esp_console.h>
#include <lwip/netif.h>
#include <lwip/inet.h>
#include "esp_mesh.h"
#include "lwip/lwip_napt.h"
#include "esp_wifi_netif.h"
#include "mesh_netif.h"

/*******************************************************
 *                Macros
 *******************************************************/
#define RX_SIZE      (1560)

#if CONFIG_MESH_USE_GLOBAL_DNS_IP
#define DNS_IP_ADDR CONFIG_MESH_GLOBAL_DNS_IP
#endif

/*******************************************************
 *                Type Definitions
 *******************************************************/
typedef struct mesh_netif_driver* mesh_netif_driver_t;

typedef struct mesh_netif_driver {
    esp_netif_driver_base_t base;
    uint8_t sta_mac_addr[MAC_ADDR_LEN];
}* mesh_netif_driver_t;

NETIF_DECLARE_EXT_CALLBACK(netif_callback)

/*******************************************************
 *                Constants
 *******************************************************/
static const char* TAG = "mesh_netif";
const esp_netif_ip_info_t g_mesh_netif_subnet_ip = {        // mesh subnet IP info
        .ip = { .addr = ESP_IP4TOADDR( 172, 16, 0, 1) },
        .gw = { .addr = ESP_IP4TOADDR( 172, 16, 0, 1) },
        .netmask = { .addr = ESP_IP4TOADDR( 255, 255, 255, 0) },
};
const esp_netif_ip_info_t g_nonmesh_netif_subnet_ip = {        // mesh subnet IP info
        .ip = { .addr = ESP_IP4TOADDR( 172, 16, 1, 1) },
        .gw = { .addr = ESP_IP4TOADDR( 172, 16, 1, 1) },
        .netmask = { .addr = ESP_IP4TOADDR( 255, 255, 255, 0) },
};

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static esp_netif_t *netif_sta = NULL;
static esp_netif_t *netif_ap = NULL;
static bool receive_task_is_running = false;
static mesh_addr_t s_route_table[CONFIG_MESH_ROUTE_TABLE_SIZE] = { 0 };
static mesh_raw_recv_cb_t *s_mesh_raw_recv_cb = NULL;
static bool leaf_mode = false;

void hex_dump(const uint8_t* pData, uint32_t length);

/*******************************************************
 *                Function Definitions
 *******************************************************/
void adjust_mtu(int newMTU, const char netif_name[2]){
    struct netif *netif;
    NETIF_FOREACH(netif) {
        if(netif->name[0] == netif_name[0] && netif->name[1] == netif_name[1]){
            ESP_LOGI(TAG, "Adjusting MTU for interface %s from %d to %d bytes", netif->name, netif->mtu, newMTU);
            netif->mtu = newMTU;
        }
        ESP_LOGI(TAG, "netif_find: %c%c %d\n", netif->name[0], netif->name[1], netif->num);
    }
}

//  setup DHCP server's DNS OFFER
//
static esp_err_t set_dhcps_dns(esp_netif_t *netif, uint32_t addr)
{
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = addr;
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    return ESP_OK;
}

// Receive task
//
static void receive_task(void *arg)
{
    esp_err_t err;
    mesh_addr_t from;
    int flag = 0;
    mesh_data_t data;
    static uint8_t rx_buf[RX_SIZE] = { 0, };

    ESP_LOGD(TAG, "Receiving task started");
    while (receive_task_is_running) {
        data.data = rx_buf;
        data.size = RX_SIZE;
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Received with err code %d %s", err, esp_err_to_name(err));
            continue;
        }
        if (data.proto == MESH_PROTO_BIN && s_mesh_raw_recv_cb) {
            s_mesh_raw_recv_cb(&from, &data);
        }
        if (esp_mesh_is_root()) {
            if (data.proto == MESH_PROTO_AP) {
                ESP_LOGI(TAG, "Root received: from: " MACSTR " to " MACSTR " size: %d",
                         MAC2STR((uint8_t*)data.data) ,MAC2STR((uint8_t*)(data.data+6)), data.size);
                if (netif_ap) {
                    // actual receive to TCP/IP stack
                    esp_netif_receive(netif_ap, data.data, data.size, NULL);
                }
            } else if (data.proto == MESH_PROTO_STA) {
                ESP_LOGE(TAG, "Root station Should never receive data from mesh!");
            }
        } else {
            if (data.proto == MESH_PROTO_AP) {
                ESP_LOGE(TAG, "Node AP should never receive data from mesh");
            } else if (data.proto == MESH_PROTO_STA) {
                ESP_LOGI(TAG, "Node received: from: " MACSTR " to " MACSTR " size: %d",
                         MAC2STR((uint8_t*)data.data) ,MAC2STR((uint8_t*)(data.data+6)), data.size);

                if(leaf_mode && netif_ap){
                    ESP_LOGI(TAG, "Forwarding response to TCP/IP stack of AP interface!");
                    esp_netif_receive(netif_sta, data.data, data.size, NULL);
                } else if (netif_sta) {
                    // actual receive to TCP/IP stack
                    ESP_LOGI(TAG, "Forwarding response to TCP/IP stack of STA interface!");
                    esp_netif_receive(netif_sta, data.data, data.size, NULL);
                } else {
                    ESP_LOGE(TAG, "Dont know what to do with the packet!");
                }
            }
        }
    }
    vTaskDelete(NULL);

}

// Free RX buffer (not used as the buffer is static)
//
static void mesh_free(void *h, void* buffer)
{
    free(buffer);
}

// Transmit function variants
//
static esp_err_t mesh_netif_transmit_from_root_ap(void *h, void *buffer, size_t len)
{
    // Use only to transmit data from root AP to node's AP
    static const uint8_t eth_broadcast[MAC_ADDR_LEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    int route_table_size = 0;
    mesh_netif_driver_t mesh_driver = h;
    mesh_addr_t dest_addr;
    mesh_data_t data;
    ESP_LOGI(TAG, "Sending to node: " MACSTR ", size: %d" ,MAC2STR((uint8_t*)buffer), len);
    memcpy(dest_addr.addr, buffer, MAC_ADDR_LEN);
    data.data = buffer;
    data.size = len;
    data.proto = MESH_PROTO_STA; // sending from root AP -> Node's STA
    data.tos = MESH_TOS_P2P;

    if (MAC_ADDR_EQUAL(dest_addr.addr, eth_broadcast)) {
        ESP_LOGI(TAG, "Broadcasting!");
        esp_mesh_get_routing_table((mesh_addr_t *) &s_route_table,
                                   CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
        for (int i = 0; i < route_table_size; i++) {
            if (MAC_ADDR_EQUAL(s_route_table[i].addr, mesh_driver->sta_mac_addr)) {
                ESP_LOGD(TAG, "That was me, skipping!");
                continue;
            }
            ESP_LOGI(TAG, "Broadcast: Sending to [%d] " MACSTR, i, MAC2STR(s_route_table[i].addr));
            esp_err_t err = esp_mesh_send(&s_route_table[i], &data, MESH_DATA_P2P, NULL, 0);
            if (ESP_OK != err) {
                ESP_LOGE(TAG, "Send with err code %d %s", err, esp_err_to_name(err));
            }
        }
    } else {
        // Standard P2P
        ESP_LOGI(TAG, "Sending to " MACSTR " %d bytes", MAC2STR(dest_addr.addr), data.size);
        esp_err_t err = esp_mesh_send(&dest_addr, &data, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Send with err code %d %s", err, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
static esp_err_t mesh_netif_transmit_from_root_ap_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return mesh_netif_transmit_from_root_ap(h, buffer, len);
}

static esp_err_t mesh_netif_transmit_from_node_sta(void *h, void *buffer, size_t len)
{
    mesh_data_t data;
    ESP_LOGI(TAG, "Sending to root, dest addr: " MACSTR ", size: %d" ,MAC2STR((uint8_t*)buffer), len);
    data.data = buffer;
    if(len >= MESH_MPS){
        ESP_LOGW(TAG, "We cant send a packet bigger then %d, packet is %d", MESH_MPS, len);
        return ESP_OK;
    }
    data.size = len;
    data.proto = MESH_PROTO_AP; // Node's station transmits data to root's AP
    data.tos = MESH_TOS_P2P;
    esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send with err code %d %s, dumping packet", err, esp_err_to_name(err));
        hex_dump(buffer, len);
    }
    return err;
}

static esp_err_t mesh_netif_transmit_from_node_sta_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return mesh_netif_transmit_from_node_sta(h, buffer, len);
}

// Construct and Destruct functions
//
static esp_err_t mesh_driver_start_root_ap(esp_netif_t * esp_netif, void * args)
{
    mesh_netif_driver_t driver = args;
    driver->base.netif = esp_netif;
    esp_netif_driver_ifconfig_t driver_ifconfig = {
            .handle =  driver,
            .transmit = mesh_netif_transmit_from_root_ap,
            .transmit_wrap = mesh_netif_transmit_from_root_ap_wrap,
            .driver_free_rx_buffer = mesh_free
    };

    return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
}

static esp_err_t mesh_driver_start_node_sta(esp_netif_t * esp_netif, void * args)
{
    mesh_netif_driver_t driver = args;
    driver->base.netif = esp_netif;
    esp_netif_driver_ifconfig_t driver_ifconfig = {
            .handle =  driver,
            .transmit = mesh_netif_transmit_from_node_sta,
            .transmit_wrap = mesh_netif_transmit_from_node_sta_wrap,
            .driver_free_rx_buffer = mesh_free
    };

    return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
}


void mesh_delete_if_driver(mesh_netif_driver_t driver)
{
    // Stop the task once both drivers are removed
    //    receive_task_is_running = true;
    free(driver);
}

mesh_netif_driver_t mesh_create_if_driver(bool is_ap, bool is_root)
{
    mesh_netif_driver_t driver = calloc(1, sizeof(struct mesh_netif_driver));
    if (driver == NULL) {
        ESP_LOGE(TAG, "No memory to create a wifi interface handle");
        return NULL;
    }
    if (is_ap && is_root) {
        driver->base.post_attach = mesh_driver_start_root_ap;
    } else if (!is_ap && !is_root) {
        driver->base.post_attach = mesh_driver_start_node_sta;
    } else {
        return NULL;
    }

    if (!receive_task_is_running) {
        receive_task_is_running = true;
        xTaskCreate(receive_task, "netif rx task", 3072, NULL, 5, NULL);
    }

    // save station mac address to exclude it from routing-table on broadcast
    esp_wifi_get_mac(WIFI_IF_STA, driver->sta_mac_addr);

    return driver;
}

esp_err_t mesh_netifs_destroy(void)
{
    receive_task_is_running = false;
    return ESP_OK;
}

static void mesh_netif_init_station(void)
{
    // By default create a station that would connect to AP (expecting root to connect to external network)
    esp_netif_config_t cfg_sta = ESP_NETIF_DEFAULT_WIFI_STA();
    netif_sta = esp_netif_new(&cfg_sta);
    assert(netif_sta);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif_sta));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
}

// Init by default for both potential root and node
//
esp_err_t mesh_netifs_init()
{
    mesh_netif_init_station();
    return ESP_OK;

}

/**
 * @brief Starts AP esp-netif link over mesh (root's AP on mesh)
 */
static esp_err_t start_mesh_link_ap(void)
{
    uint8_t mac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    esp_netif_set_mac(netif_ap, mac);
    esp_netif_action_start(netif_ap, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Starts station link over wifi (root node to the router)
 */
static esp_err_t start_wifi_link_sta(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_err_t ret;
    void *driver = esp_netif_get_io_driver(netif_sta);
    if ((ret = esp_wifi_register_if_rxcb(driver,  esp_netif_receive, netif_sta)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_register_if_rxcb for if=%p failed with %d", driver, ret);
        return ESP_FAIL;
    }
    esp_netif_set_mac(netif_sta, mac);
    esp_netif_action_start(netif_sta, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Starts station link over mesh (node to root over mesh)
 */
static esp_err_t start_mesh_link_sta(void)
{
    uint8_t mac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_netif_set_mac(netif_sta, mac);
    esp_netif_action_start(netif_sta, NULL, 0, NULL);
    esp_netif_action_connected(netif_sta, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Creates esp-netif for AP interface over mesh
 *
 * @return Pointer to esp-netif instance
 */
static esp_netif_t* create_mesh_link_ap(void)
{
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    base_cfg.if_desc = "mesh_link_ap";
    base_cfg.ip_info = &g_mesh_netif_subnet_ip;

    esp_netif_config_t cfg = {
            .base = &base_cfg,
            .driver = NULL,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP };

    esp_netif_t * netif = esp_netif_new(&cfg);
    assert(netif);
    return netif;
}

/**
 * @brief Creates esp-netif for station interface over mesh
 *
 * @note Interface needs to be started (later) using the above APIs
 * based on the actual configuration root/node,
 * since root connects normally over wifi
 *
 * @return Pointer to esp-netif instance
 */
static esp_netif_t* create_mesh_link_sta(void)
{
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base_cfg.if_desc = "mesh_link_sta";

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA };
    esp_netif_t * netif = esp_netif_new(&cfg);
    assert(netif);
    return netif;
}

esp_err_t mesh_netif_start_root_ap(bool is_root, uint32_t addr)
{
    if (is_root) {
        if (netif_ap) {
            esp_netif_action_disconnected(netif_ap, NULL, 0, NULL);
            mesh_delete_if_driver(esp_netif_get_io_driver(netif_ap));
            esp_netif_destroy(netif_ap);
            netif_ap = NULL;
        }
        netif_ap = create_mesh_link_ap();
        mesh_netif_driver_t driver = mesh_create_if_driver(true, true);
        if (driver == NULL) {
            ESP_LOGE(TAG, "Failed to create wifi interface handle");
            return ESP_FAIL;
        }
        esp_netif_attach(netif_ap, driver);
        set_dhcps_dns(netif_ap, addr);
        start_mesh_link_ap();
        ip_napt_enable(g_mesh_netif_subnet_ip.ip.addr, 1);
        // TODO: we limit the MTU size of the STA network (the one talking to internet)
        // to the max size of a mesh payload this unfortunately still seem to
        // generate ESP_ERR_MESH_ARGUMENT for certain packets, why?
        //const char name[] = {'s', 't'};
        //adjust_mtu(MESH_MPS, name);
        //const char name1[] = {'a', 'p'};
        //adjust_mtu(MESH_MPS, name1);
    }
    return ESP_OK;
}

esp_err_t mesh_netifs_start(bool is_root)
{
    if (is_root) {
        // ROOT: need both sta should use standard wifi, AP mesh link netif

        // Root: Station
        if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "sta") == 0) {
            ESP_LOGI(TAG, "Already wifi station, no need to do anything");
        } else if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "mesh_link_sta") == 0) {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            mesh_delete_if_driver(esp_netif_get_io_driver(netif_sta));
            esp_netif_destroy(netif_sta);
            mesh_netif_init_station();
        } else if (netif_sta == NULL) {
            mesh_netif_init_station();
        }

        // Root: AP is initialized only if GLOBAL DNS configured
        // (otherwise have to wait until the actual DNS record received from the router)
#if CONFIG_MESH_USE_GLOBAL_DNS_IP
         mesh_netif_start_root_ap(true, htonl(DNS_IP_ADDR));
#endif

    } else {
        // NODE: create only STA in form of mesh link
        if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "mesh_link_sta") == 0) {
            ESP_LOGI(TAG, "Already mesh link station, no need to do anything");
            return ESP_OK;
        }
        if (netif_sta) {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            // should remove the actual driver
            if (strcmp(esp_netif_get_desc(netif_sta), "sta") == 0) {
                ESP_LOGI(TAG, "It was a wifi station removing stuff");
                esp_wifi_clear_default_wifi_driver_and_handlers(netif_sta);
            }
            esp_netif_destroy(netif_sta);

        }
        netif_sta = create_mesh_link_sta();
        ESP_ERROR_CHECK(esp_netif_dhcps_start(netif_sta));

        // now we create a mesh driver and attach it to the existing netif
        mesh_netif_driver_t driver = mesh_create_if_driver(false, false);
        if (driver == NULL) {
            ESP_LOGE(TAG, "Failed to create wifi interface handle");
            return ESP_FAIL;
        }
        esp_netif_attach(netif_sta, driver);
        start_mesh_link_sta();
        // If we have an AP on NODE -> stop and remove it!
        if (netif_ap) {
            esp_netif_action_disconnected(netif_ap, NULL, 0, NULL);
            mesh_delete_if_driver(esp_netif_get_io_driver(netif_ap));
            esp_netif_destroy(netif_ap);
            netif_ap = NULL;
            ESP_LOGW(TAG, "Destroyed netif_ap for node!");
        }
    }
    return ESP_OK;
}

esp_err_t mesh_netifs_stop(void)
{
    if (netif_sta && strcmp(esp_netif_get_desc(netif_sta), "sta") == 0 && netif_ap == NULL) {
        return ESP_OK;
    }

    if (netif_sta) {
        if (strcmp(esp_netif_get_desc(netif_sta), "sta") == 0) {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            esp_netif_action_stop(netif_sta, NULL, 0, NULL);
            esp_wifi_clear_default_wifi_driver_and_handlers(netif_sta);
        } else {
            esp_netif_action_disconnected(netif_sta, NULL, 0, NULL);
            mesh_delete_if_driver(esp_netif_get_io_driver(netif_sta));
        }
        esp_netif_destroy(netif_sta);
        netif_sta = NULL;
    }

    if (netif_ap) {
        esp_netif_action_disconnected(netif_ap, NULL, 0, NULL);
        mesh_delete_if_driver(esp_netif_get_io_driver(netif_ap));
        esp_netif_destroy(netif_ap);
        netif_ap = NULL;
    }
    // reserve the default (STA gets ready to become root)
    mesh_netif_init_station();
    start_wifi_link_sta();
    return ESP_OK;
}


void enable_pnat_callback(void* arg){
    if(netif_ap){
        ESP_LOGI(TAG, "Enabling NAT on AP interface");
        ip_napt_enable(g_nonmesh_netif_subnet_ip.ip.addr, 1);
        // TODO: same as the other adjust_mtu todo, it still generate ESP_ERR_MESH_ARGUMENT in some cases
        //const char name[] = {'a', 'p'};
        //adjust_mtu(1400, name);
        //const char name2[] = {'s', 't'};
        //adjust_mtu(1400, name2);
    }
}

int do_convert_to_entrypoint_node(int argc, char* argv[]) {
    ESP_LOGI(TAG, "Configuring node AP interface to support DHCP Server");
    if (netif_ap) {
        ESP_LOGW(TAG, "Lets first destroy everything we can!");
        esp_netif_action_disconnected(netif_ap, NULL, 0, NULL);
        mesh_delete_if_driver(esp_netif_get_io_driver(netif_ap));
        esp_netif_destroy(netif_ap);
        netif_ap = NULL;
    }
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    base_cfg.if_desc = "nonmesh_ap";
    base_cfg.ip_info = &g_nonmesh_netif_subnet_ip;
    esp_netif_config_t cfg = {
            .base = &base_cfg,
            .driver = NULL,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP };
    netif_ap = esp_netif_new(&cfg);
    // Forward the DNS given by the root
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif_sta, ESP_NETIF_DNS_MAIN, &dns));
    dns.ip.u_addr.ip4.addr = dns.ip.u_addr.ip4.addr;
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_LOGI(TAG, "Setting DNS address for non-mesh client to %s", inet_ntoa(dns.ip.u_addr.ip4.addr));
    ESP_ERROR_CHECK(esp_netif_attach_wifi_ap(netif_ap));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());
    wifi_config_t wifi_config = {
            .ap = {
                    .ssid = CONFIG_NONMESH_AP_SSID,
                    .channel = CONFIG_NONMESH_CHANNEL,
                    .max_connection = 5,
                    .password = CONFIG_NONMESH_AP_PASSWD,
                    .authmode = CONFIG_AP_AUTHMODE
            }
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    // we need to increase this value since the mesh AP will trigger an expiration (the node is concretly not part of the
    ESP_LOGI(TAG, "Increasing mesh association timeout value");
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(INT_MAX));
    // This 1sec delayed task is just to allow the netif to be created (otherwise enabling NAPT will have no effect)
    // Unfortunately the LWIP_NETIF_EXT_STATUS_CALLBACK cannot be enabled via the usual menuconfig
    esp_timer_handle_t oneshot_timer;
    const esp_timer_create_args_t oneshot_timer_args = {
            .callback = &enable_pnat_callback,
            .name = "one-shot"
    };
    ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, 1000000));
    return 0;
}


void configure_ap(){
    ESP_LOGI(TAG, "Configuring node AP interface to support DHCP Server");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif_ap, &g_nonmesh_netif_subnet_ip));
    // Forward the DNS given by the root
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif_sta, ESP_NETIF_DNS_MAIN, &dns));
    dns.ip.u_addr.ip4.addr = dns.ip.u_addr.ip4.addr;
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif_sta, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));
    //ESP_ERROR_CHECK(esp_netif_set_dns_info(netif_sta, ESP_NETIF_DNS_MAIN, &dns));
    ESP_LOGI(TAG, "Setting DNS address for non-mesh client to %s", inet_ntoa(dns.ip.u_addr.ip4.addr));

    esp_timer_handle_t oneshot_timer;
    const esp_timer_create_args_t oneshot_timer_args = {
            .callback = &enable_pnat_callback,
            .name = "one-shot"
    };
    ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, 1000000));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif_sta));

}

void register_ap_command() {
    esp_console_cmd_t ap_command = {
            .command = "ap",
            .help = "Convert to entrypoint node (create softAP)",
            .func = &do_convert_to_entrypoint_node
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ap_command));
}