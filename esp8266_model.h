/*
 * ESP8266 ESP-01 Module Simulator for sQLux
 *
 * This module provides a software model of the ESP8266 ESP-01 WiFi module
 * with AT command interface integrated into sQLux via UART.
 *
 * Architecture:
 * - Virtual WiFi simulation with hardcoded AP database
 * - Real TCP/UDP/SSL via Linux socket APIs
 * - AT command parsing and dispatch
 * - UART integration for serial communication
 *
 * Copyright (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

#ifndef ESP8266_MODEL_H
#define ESP8266_MODEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Forward Declarations ========== */

/* Forward declare the main ESP8266 instance type */
typedef struct ESP8266_t ESP8266_t;

/* ========== Type Definitions ========== */

/* ========== Enumerations ========== */

typedef enum {
    WIFI_MODE_OFF = 0,
    WIFI_MODE_STATION = 1,
    WIFI_MODE_SOFTAP = 2,
    WIFI_MODE_DUAL = 3
} WiFi_Mode;

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING = 1,
    WIFI_STATE_CONNECTED = 2,
    WIFI_STATE_GOT_IP = 3,
    WIFI_STATE_FAILED = 4
} WiFi_State;

typedef enum {
    ENCRYPTION_OPEN = 0,
    ENCRYPTION_WEP = 1,
    ENCRYPTION_WPA_PSK = 2,
    ENCRYPTION_WPA2_PSK = 3,
    ENCRYPTION_WPA_WPA2_PSK = 4
} Encryption_Type;

typedef enum {
    CONNECTION_TYPE_TCP = 0,
    CONNECTION_TYPE_UDP = 1,
    CONNECTION_TYPE_SSL = 2
} Connection_Type;

/* ========== Constants ========== */

#define ESP8266_MAX_SSID_LEN 32
#define ESP8266_MAX_PASSWORD_LEN 64
#define ESP8266_MAX_IP_STR_LEN 16
#define ESP8266_MAX_MAC_STR_LEN 18
#define ESP8266_MAX_DOMAIN_LEN 256
#define ESP8266_MAX_CONNECTIONS 5
#define ESP8266_MAX_AP_RESULTS 10
#define ESP8266_RX_BUFFER_SIZE 2048
#define ESP8266_RESPONSE_BUFFER_SIZE (ESP8266_RX_BUFFER_SIZE + 256)
#define ESP8266_TX_BUFFER_SIZE 65536

/* Default configuration */
#define ESP8266_DEFAULT_BAUD_RATE 115200
#define ESP8266_DEFAULT_STATION_IP "192.168.1.100"
#define ESP8266_DEFAULT_STATION_GATEWAY "192.168.1.1"
#define ESP8266_DEFAULT_STATION_NETMASK "255.255.255.0"
#define ESP8266_DEFAULT_AP_IP "192.168.5.1"
#define ESP8266_DEFAULT_AP_GATEWAY "192.168.5.1"
#define ESP8266_DEFAULT_AP_NETMASK "255.255.255.0"
#define ESP8266_DEFAULT_AP_CHANNEL 6

/* Version strings */
#define ESP8266_AT_VERSION "AT/1.5.4"
#define ESP8266_SDK_VERSION "2.0.0(esp8266_model)"
#define ESP8266_BUILD_DATE "20250201"

/* ========== Structures ========== */

/**
 * ESP8266 model instance (opaque type)
 * Internal structure defined in esp8266_model_internal.h
 */
typedef struct ESP8266_t ESP8266_t;

/**
 * Virtual AP entry for WiFi scanning
 */
typedef struct {
    char ssid[ESP8266_MAX_SSID_LEN + 1];
    char password[ESP8266_MAX_PASSWORD_LEN + 1];
    char bssid[ESP8266_MAX_MAC_STR_LEN + 1];
    uint8_t channel;
    int8_t rssi;  // Signal strength (-30 to -90)
    Encryption_Type encryption;
} VirtualAP;

/**
 * Scan result entry
 */
typedef struct {
    Encryption_Type encryption;
    char ssid[ESP8266_MAX_SSID_LEN + 1];
    int8_t rssi;
    char bssid[ESP8266_MAX_MAC_STR_LEN + 1];
    uint8_t channel;
    int16_t freq_offset;
    int8_t freq_cal;
} ScanResult;

/**
 * TCP/UDP connection info
 */
typedef struct {
    uint8_t active;
    Connection_Type type;
    uint8_t is_server;  // 0=client, 1=server
    uint8_t connected;  // 0=connecting, 1=connected (for TCP/SSL)
    int socket_fd;
    void *ssl;          // SSL* pointer (for SSL connections)
    char remote_ip[ESP8266_MAX_IP_STR_LEN + 1];
    uint16_t remote_port;
    uint16_t local_port;
    uint8_t *rx_buffer;
    uint16_t rx_buffer_len;
    uint16_t rx_buffer_pos;
} Connection;

/* ========== Public API ========== */

/**
 * Create and initialize an ESP8266 model instance
 * Returns NULL on allocation failure
 */
extern ESP8266_t* ESP8266_Create(void);

/**
 * Destroy and cleanup an ESP8266 model instance
 */
extern void ESP8266_Destroy(ESP8266_t *esp);

/**
 * Reset the ESP8266 to default state
 */
extern void ESP8266_Reset(ESP8266_t *esp);

/**
 * Process a single byte received from UART
 * This buffers the byte and processes complete AT command lines
 */
extern void ESP8266_ProcessUARTByte(ESP8266_t *esp, uint8_t byte);

/**
 * Get the next byte to transmit to UART
 * Returns -1 if no data available
 */
extern int ESP8266_GetUARTByte(ESP8266_t *esp);

/**
 * Poll for periodic updates (socket timeouts, connection state changes, etc)
 * Should be called regularly from the main loop
 */
extern void ESP8266_Poll(ESP8266_t *esp);

/**
 * Check if there are bytes waiting in the TX buffer
 * Returns number of bytes available
 */
extern size_t ESP8266_TXDataAvailable(ESP8266_t *esp);

/**
 * Set the UART baud rate (stored for querying)
 * Note: Actual UART speed is handled by uart.h, this just stores the value
 */
extern void ESP8266_SetBaudRate(ESP8266_t *esp, uint32_t baud);

/**
 * Get the current UART baud rate
 */
extern uint32_t ESP8266_GetBaudRate(ESP8266_t *esp);

/**
 * Enable/disable AT command echo
 */
extern void ESP8266_SetEcho(ESP8266_t *esp, uint8_t enabled);

/**
 * Get AT command echo state
 */
extern uint8_t ESP8266_GetEcho(ESP8266_t *esp);

/**
 * Get WiFi mode
 */
extern WiFi_Mode ESP8266_GetWiFiMode(ESP8266_t *esp);

/**
 * Set WiFi mode
 */
extern void ESP8266_SetWiFiMode(ESP8266_t *esp, WiFi_Mode mode);

/**
 * Get WiFi connection state
 */
extern WiFi_State ESP8266_GetWiFiState(ESP8266_t *esp);

/**
 * Connect to a WiFi access point
 */
extern int ESP8266_WiFiConnect(ESP8266_t *esp, const char *ssid, const char *password);

/**
 * Get station IP address
 */
extern const char* ESP8266_GetStationIP(ESP8266_t *esp);

/**
 * Get SoftAP IP address
 */
extern const char* ESP8266_GetAPIP(ESP8266_t *esp);

/**
 * Get connection info
 * Returns pointer to connection struct if active, NULL if not
 */
extern Connection* ESP8266_GetConnection(ESP8266_t *esp, uint8_t link_id);

/* ========== Debugging/Info ========== */

/**
 * Print ESP8266 state to stdout (for debugging)
 */
extern void ESP8266_DebugPrint(ESP8266_t *esp);

/**
 * Get human-readable WiFi state name
 */
extern const char* ESP8266_WiFiStateName(WiFi_State state);

/**
 * Get human-readable WiFi mode name
 */
extern const char* ESP8266_WiFiModeName(WiFi_Mode mode);

#ifdef __cplusplus
}
#endif

#endif  /* ESP8266_MODEL_H */
