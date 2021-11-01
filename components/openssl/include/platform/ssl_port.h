/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef _SSL_PORT_H_
#define _SSL_PORT_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include "esp_types.h"
#include "esp_log.h"
#include "esp_system.h"
#include "string.h"
#include "malloc.h"

void *ssl_mem_zalloc(size_t size);

#define ssl_mem_malloc malloc
#define ssl_mem_free   free

#define ssl_memcpy     memcpy
#define ssl_strlen     strlen

#define ssl_speed_up_enter()
#define ssl_speed_up_exit()

#define SSL_DEBUG_FL
#define SSL_DEBUG_LOG(fmt, ...) ESP_LOGI("openssl", fmt, ##__VA_ARGS__)

#ifdef __cplusplus
 }
#endif

#endif
