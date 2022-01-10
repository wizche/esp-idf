//
// Created by wizche on 1/7/22.
//

#ifndef NONMESH_INTERNAL_COMMUNICATION_CONFIG_H
#define NONMESH_INTERNAL_COMMUNICATION_CONFIG_H

#include <esp_netif.h>

#define TOPIC "/topic/mesh/info"
#define MQTT_SERVER "wss://mqtt.eclipseprojects.io:443/mqtt"
#define MAC_ADDR_LEN (6u)

static const uint8_t MESH_ID[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};


#endif //NONMESH_INTERNAL_COMMUNICATION_CONFIG_H
