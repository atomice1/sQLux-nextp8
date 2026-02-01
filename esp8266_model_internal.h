/*
 * ESP8266 Model - Internal Structure Definition
 *
 * This header defines the internal state structure for the ESP8266 model.
 * It's included by esp8266_model.c and esp8266_at_commands.c but NOT
 * exposed in the public API (esp8266_model.h).
 *
 * Copyright (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

#ifndef ESP8266_MODEL_INTERNAL_H
#define ESP8266_MODEL_INTERNAL_H

#include "esp8266_model.h"
#include <stdint.h>

/* ========== Internal State Structure ========== */

typedef struct {
    // Ring buffers for UART I/O
    uint8_t rx_buffer[ESP8266_RX_BUFFER_SIZE];
    uint8_t tx_buffer[ESP8266_TX_BUFFER_SIZE];
    uint16_t rx_head, rx_tail;
    uint16_t tx_head, tx_tail;

    // AT command line assembly
    char cmd_line_buffer[256];
    uint16_t cmd_line_pos;

    // UART configuration
    uint32_t uart_baud;
    uint8_t uart_databits;
    uint8_t uart_stopbits;
    uint8_t uart_parity;
    uint8_t uart_flow_control;

    // AT command control
    uint8_t echo_enabled;

    // WiFi state
    WiFi_Mode wifi_mode;
    WiFi_State wifi_state;
    uint64_t wifi_state_change_time;  // For simulating connection delays

    // Station mode
    char station_ssid[ESP8266_MAX_SSID_LEN + 1];
    char station_password[ESP8266_MAX_PASSWORD_LEN + 1];
    char station_ip[ESP8266_MAX_IP_STR_LEN + 1];
    char station_gateway[ESP8266_MAX_IP_STR_LEN + 1];
    char station_netmask[ESP8266_MAX_IP_STR_LEN + 1];
    char station_mac[ESP8266_MAX_MAC_STR_LEN + 1];
    uint8_t station_connected;
    uint8_t station_has_ip;

    // SoftAP mode
    char ap_ssid[ESP8266_MAX_SSID_LEN + 1];
    char ap_password[ESP8266_MAX_PASSWORD_LEN + 1];
    char ap_ip[ESP8266_MAX_IP_STR_LEN + 1];
    char ap_mac[ESP8266_MAX_MAC_STR_LEN + 1];
    uint8_t ap_channel;
    Encryption_Type ap_encryption;

    // DHCP
    uint8_t dhcp_enabled[2];  // [0]=SoftAP, [1]=Station

    // TCP/IP connections
    Connection connections[ESP8266_MAX_CONNECTIONS];
    uint8_t mux_enabled;
    uint8_t transparent_mode;

    // CIPSEND state machine
    uint8_t send_mode;          // 0=normal, 1=collecting data
    uint8_t send_link_id;       // Which connection to send on
    uint16_t send_bytes_expected; // How many bytes to collect
    uint16_t send_bytes_collected; // How many bytes collected so far
    uint8_t send_buffer[2048];  // Buffer for data collection

    // Server mode
    int server_socket;
    uint16_t server_port;
    uint8_t server_active;

    // SSL configuration
    uint16_t ssl_buffer_size;  // 2048-4096, default 2048

    // Version strings
    char at_version[64];
    char sdk_version[64];
    char build_date[32];
} ESP8266_Internal;

/* ========== Public Structure Definition ========== */

struct ESP8266_t {
    ESP8266_Internal state;
};

/* ========== Helper declarations for AT commands ========== */

void ESP8266_ATResponse(ESP8266_t *esp, const char *fmt, ...);
void ESP8266_ATUnsolicited(ESP8266_t *esp, const char *fmt, ...);
void ESP8266_ATError(ESP8266_t *esp);
void ESP8266_ATOKK(ESP8266_t *esp);

#endif  /* ESP8266_MODEL_INTERNAL_H */
