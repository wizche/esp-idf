/*
 * SPDX-FileCopyrightText: 2020-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/param.h>
#include "esp_mbedtls_dynamic_impl.h"

int __real_mbedtls_ssl_write(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
int __real_mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
void __real_mbedtls_ssl_free(mbedtls_ssl_context *ssl);
int __real_mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl);
int __real_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf);
int __real_mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message);
int __real_mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl);

int __wrap_mbedtls_ssl_write(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
int __wrap_mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
void __wrap_mbedtls_ssl_free(mbedtls_ssl_context *ssl);
int __wrap_mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl);
int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf);
int __wrap_mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message);
int __wrap_mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl);

static const char *TAG = "SSL TLS";

static int tx_done(mbedtls_ssl_context *ssl)
{
    if (!ssl->MBEDTLS_PRIVATE(out_left))
        return 1;

    return 0;
}

static int rx_done(mbedtls_ssl_context *ssl)
{
    if (!ssl->MBEDTLS_PRIVATE(in_msglen)) {
        return 1;
    }

    ESP_LOGD(TAG, "RX left %d bytes", ssl->MBEDTLS_PRIVATE(in_msglen));

    return 0;
}

int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf)
{
    CHECK_OK(__real_mbedtls_ssl_setup(ssl, conf));

    mbedtls_free(ssl->MBEDTLS_PRIVATE(out_buf));
    ssl->MBEDTLS_PRIVATE(out_buf) = NULL;
    CHECK_OK(esp_mbedtls_setup_tx_buffer(ssl));

    mbedtls_free(ssl->MBEDTLS_PRIVATE(in_buf));
    ssl->MBEDTLS_PRIVATE(in_buf) = NULL;
    esp_mbedtls_setup_rx_buffer(ssl);

    return 0;
}

int __wrap_mbedtls_ssl_write(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
    int ret;

    CHECK_OK(esp_mbedtls_add_tx_buffer(ssl, 0));

    ret = __real_mbedtls_ssl_write(ssl, buf, len);

    if (tx_done(ssl)) {
        CHECK_OK(esp_mbedtls_free_tx_buffer(ssl));
    }

    return ret;
}

int __wrap_mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
    int ret;

    ESP_LOGD(TAG, "add mbedtls RX buffer");
    ret = esp_mbedtls_add_rx_buffer(ssl);
    if (ret == MBEDTLS_ERR_SSL_CONN_EOF) {
        ESP_LOGD(TAG, "fail, the connection indicated an EOF");
        return 0;
    } else if (ret < 0) {
        ESP_LOGD(TAG, "fail, error=-0x%x", -ret);
        return ret;
    }
    ESP_LOGD(TAG, "end");

    ret = __real_mbedtls_ssl_read(ssl, buf, len);

    if (rx_done(ssl)) {
        CHECK_OK(esp_mbedtls_free_rx_buffer(ssl));
    }

    return ret;
}

void __wrap_mbedtls_ssl_free(mbedtls_ssl_context *ssl)
{
    if (ssl->MBEDTLS_PRIVATE(out_buf)) {
        esp_mbedtls_free_buf(ssl->MBEDTLS_PRIVATE(out_buf));
        ssl->MBEDTLS_PRIVATE(out_buf) = NULL;
    }

    if (ssl->MBEDTLS_PRIVATE(in_buf)) {
        esp_mbedtls_free_buf(ssl->MBEDTLS_PRIVATE(in_buf));
        ssl->MBEDTLS_PRIVATE(in_buf) = NULL;
    }

    __real_mbedtls_ssl_free(ssl);
}

int __wrap_mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl)
{
    CHECK_OK(esp_mbedtls_reset_add_tx_buffer(ssl));

    CHECK_OK(esp_mbedtls_reset_add_rx_buffer(ssl));

    CHECK_OK(__real_mbedtls_ssl_session_reset(ssl));

    CHECK_OK(esp_mbedtls_reset_free_tx_buffer(ssl));

    esp_mbedtls_reset_free_rx_buffer(ssl);

    return 0;
}

int __wrap_mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message)
{
    int ret;

    CHECK_OK(esp_mbedtls_add_tx_buffer(ssl, 0));

    ret = __real_mbedtls_ssl_send_alert_message(ssl, level, message);

    if (tx_done(ssl)) {
        CHECK_OK(esp_mbedtls_free_tx_buffer(ssl));
    }

    return ret;
}

int __wrap_mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl)
{
    int ret;

    CHECK_OK(esp_mbedtls_add_tx_buffer(ssl, 0));

    ret = __real_mbedtls_ssl_close_notify(ssl);

    if (tx_done(ssl)) {
        CHECK_OK(esp_mbedtls_free_tx_buffer(ssl));
    }

    return ret;
}
