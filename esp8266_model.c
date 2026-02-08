/*
 * ESP8266 ESP-01 Module Simulator - Main Implementation
 *
 * Manages state, initialization, UART integration, and polling
 *
 * Copyright (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

#include "esp8266_model.h"
#include "esp8266_model_internal.h"
#include "esp8266_at_commands.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* Forward Declarations ========== */

static uint64_t ESP8266_GetTimestampMS(void);
static void ESP8266_TXQueueChar(ESP8266_t *esp, uint8_t byte);
static void ESP8266_TXQueueString(ESP8266_t *esp, const char *str);
static void ESP8266_GenerateMAC(char *mac_str);
static VirtualAP* ESP8266_FindVirtualAP(const char *ssid);
static int ESP8266_SetNonBlocking(int fd);
static void ESP8266_CheckSocketData(ESP8266_t *esp);
int ESP8266_SocketClose(ESP8266_t *esp, uint8_t link_id);
static SSL_CTX *ssl_ctx = NULL;

/* ========== SSL Initialization ========== */

static void ESP8266_InitSSL(void) {
    if (ssl_ctx) return;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        fprintf(stderr, "Failed to create SSL context\n");
        return;
    }

    // Disable certificate verification for simplicity
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
}

/* ========== Virtual AP Database ========== */

static const VirtualAP virtual_aps[] = {
    {
        .ssid = "home-network",
        .password = "password123",
        .bssid = "aa:bb:cc:dd:ee:01",
        .channel = 6,
        .rssi = -45,
        .encryption = ENCRYPTION_WPA2_PSK
    },
    {
        .ssid = "coffee-wifi",
        .password = "",
        .bssid = "aa:bb:cc:dd:ee:02",
        .channel = 11,
        .rssi = -65,
        .encryption = ENCRYPTION_OPEN
    },
    {
        .ssid = "office-secure",
        .password = "corp-password",
        .bssid = "aa:bb:cc:dd:ee:03",
        .channel = 1,
        .rssi = -50,
        .encryption = ENCRYPTION_WPA_WPA2_PSK
    }
};

static const size_t NUM_VIRTUAL_APS = sizeof(virtual_aps) / sizeof(virtual_aps[0]);

/* ========== WiFi Management Functions (Internal) ========== */

/**
 * Initiate connection to a virtual AP
 * Returns 0 on success, -1 if AP not found or password wrong
 */
int ESP8266_WiFiConnect(ESP8266_t *esp, const char *ssid, const char *password) {
    if (!esp || !ssid) return -1;

    ESP8266_Internal *state = &esp->state;

    // Find the AP in virtual database
    VirtualAP *ap = ESP8266_FindVirtualAP(ssid);
    if (!ap) {
        return -1;  // AP not found
    }

    // Validate password (if AP is not OPEN)
    if (ap->encryption != ENCRYPTION_OPEN) {
        if (!password || strcmp(password, ap->password) != 0) {
            return -1;  // Wrong password
        }
    }

    // Save connection info
    strncpy(state->station_ssid, ssid, ESP8266_MAX_SSID_LEN);
    state->station_ssid[ESP8266_MAX_SSID_LEN] = '\0';

    if (password) {
        strncpy(state->station_password, password, ESP8266_MAX_PASSWORD_LEN);
        state->station_password[ESP8266_MAX_PASSWORD_LEN] = '\0';
    }

    // Transition to CONNECTING state
    state->wifi_state = WIFI_STATE_CONNECTING;
    state->wifi_state_change_time = ESP8266_GetTimestampMS();
    state->station_connected = 0;
    state->station_has_ip = 0;

    return 0;
}

/**
 * Disconnect from WiFi AP
 */
void ESP8266_WiFiDisconnect(ESP8266_t *esp) {
    if (!esp) return;

    ESP8266_Internal *state = &esp->state;

    state->wifi_state = WIFI_STATE_DISCONNECTED;
    state->station_connected = 0;
    state->station_has_ip = 0;
    memset(state->station_ssid, 0, sizeof(state->station_ssid));
    memset(state->station_password, 0, sizeof(state->station_password));

    ESP8266_ATUnsolicited(esp, "WiFi DISCONNECT");
}

/**
 * Get scan results from virtual AP database
 */
int ESP8266_WiFiScan(ESP8266_t *esp, ScanResult *results, int max_results) {
    if (!esp || !results) return 0;

    int count = 0;
    for (size_t i = 0; i < NUM_VIRTUAL_APS && count < max_results; i++) {
        results[count].encryption = virtual_aps[i].encryption;

        /* Copy SSID with explicit null termination */
        size_t ssid_len = strlen(virtual_aps[i].ssid);
        if (ssid_len > ESP8266_MAX_SSID_LEN) ssid_len = ESP8266_MAX_SSID_LEN;
        memcpy(results[count].ssid, virtual_aps[i].ssid, ssid_len);
        results[count].ssid[ssid_len] = '\0';

        results[count].rssi = virtual_aps[i].rssi;

        /* Copy BSSID with explicit null termination */
        size_t bssid_len = strlen(virtual_aps[i].bssid);
        if (bssid_len > ESP8266_MAX_MAC_STR_LEN) bssid_len = ESP8266_MAX_MAC_STR_LEN;
        memcpy(results[count].bssid, virtual_aps[i].bssid, bssid_len);
        results[count].bssid[bssid_len] = '\0';

        results[count].channel = virtual_aps[i].channel;
        results[count].freq_offset = 0;
        results[count].freq_cal = 0;
        count++;
    }

    return count;
}

/**
 * Get info about the currently connected AP
 * Returns 0 on success, -1 if not found
 */
int ESP8266_GetConnectedAPInfo(ESP8266_t *esp, char *bssid, uint8_t *channel, int8_t *rssi) {
    if (!esp) return -1;

    ESP8266_Internal *state = &esp->state;

    if (state->wifi_state != WIFI_STATE_GOT_IP) {
        return -1;  // Not connected
    }

    // Find the AP in database
    VirtualAP *ap = ESP8266_FindVirtualAP(state->station_ssid);
    if (!ap) return -1;

    if (bssid) {
        strncpy(bssid, ap->bssid, ESP8266_MAX_MAC_STR_LEN);
        bssid[ESP8266_MAX_MAC_STR_LEN] = '\0';
    }
    if (channel) *channel = ap->channel;
    if (rssi) *rssi = ap->rssi;

    return 0;
}

/* ========== Utility Functions ========== */

static uint64_t ESP8266_GetTimestampMS(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ESP8266_GenerateMAC(char *mac_str) {
    // Generate a pseudo-random but consistent MAC
    // Format: aa:bb:cc:dd:ee:XX where XX is based on instance
    snprintf(mac_str, ESP8266_MAX_MAC_STR_LEN + 1,
             "aa:bb:cc:dd:ee:%02x", (unsigned)(rand() % 256));
}

static VirtualAP* ESP8266_FindVirtualAP(const char *ssid) {
    for (size_t i = 0; i < NUM_VIRTUAL_APS; i++) {
        if (strcmp(virtual_aps[i].ssid, ssid) == 0) {
            return (VirtualAP *)&virtual_aps[i];
        }
    }
    return NULL;
}

/* ========== TX Queue Functions ========== */

static void ESP8266_TXQueueChar(ESP8266_t *esp, uint8_t byte) {
    ESP8266_Internal *state = &esp->state;
    uint16_t next_head = (state->tx_head + 1) % ESP8266_TX_BUFFER_SIZE;

    // Drop if queue is full
    if (next_head == state->tx_tail) {
        return;
    }

    state->tx_buffer[state->tx_head] = byte;
    state->tx_head = next_head;
}

static void ESP8266_TXQueueString(ESP8266_t *esp, const char *str) {
    printf("[ESP] Queuing response: %s\n", str);
    for (const char *p = str; *p; p++) {
        ESP8266_TXQueueChar(esp, (uint8_t)*p);
    }
}

/* ========== Public API Implementation ========== */

ESP8266_t* ESP8266_Create(void) {
    ESP8266_t *esp = (ESP8266_t *)malloc(sizeof(ESP8266_t));
    if (!esp) return NULL;

    memset(esp, 0, sizeof(ESP8266_t));

    // Initialize UART
    esp->state.uart_baud = ESP8266_DEFAULT_BAUD_RATE;
    esp->state.uart_databits = 8;
    esp->state.uart_stopbits = 1;
    esp->state.uart_parity = 0;
    esp->state.uart_flow_control = 0;

    // Initialize version strings
    snprintf(esp->state.at_version, sizeof(esp->state.at_version),
             "%s", ESP8266_AT_VERSION);
    snprintf(esp->state.sdk_version, sizeof(esp->state.sdk_version),
             "%s", ESP8266_SDK_VERSION);
    snprintf(esp->state.build_date, sizeof(esp->state.build_date),
             "%s", ESP8266_BUILD_DATE);

    // Initialize WiFi
    esp->state.wifi_mode = WIFI_MODE_STATION;
    esp->state.wifi_state = WIFI_STATE_DISCONNECTED;
    esp->state.dhcp_enabled[0] = 1;  // SoftAP DHCP enabled by default
    esp->state.dhcp_enabled[1] = 1;  // Station DHCP enabled by default

    // Generate MACs
    ESP8266_GenerateMAC(esp->state.station_mac);
    ESP8266_GenerateMAC(esp->state.ap_mac);

    // Initialize IP addresses
    snprintf(esp->state.station_ip, sizeof(esp->state.station_ip),
             "%s", ESP8266_DEFAULT_STATION_IP);
    snprintf(esp->state.station_gateway, sizeof(esp->state.station_gateway),
             "%s", ESP8266_DEFAULT_STATION_GATEWAY);
    snprintf(esp->state.station_netmask, sizeof(esp->state.station_netmask),
             "%s", ESP8266_DEFAULT_STATION_NETMASK);

    snprintf(esp->state.ap_ip, sizeof(esp->state.ap_ip),
             "%s", ESP8266_DEFAULT_AP_IP);
    esp->state.ap_channel = ESP8266_DEFAULT_AP_CHANNEL;
    esp->state.ap_encryption = ENCRYPTION_WPA2_PSK;

    // Initialize connections
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        esp->state.connections[i].active = 0;
        esp->state.connections[i].socket_fd = -1;
        esp->state.connections[i].ssl = NULL;
        esp->state.connections[i].rx_buffer = NULL;
        esp->state.connections[i].rx_buffer_len = 0;
    }

    // Initialize CIPSEND state
    esp->state.send_mode = 0;
    esp->state.send_link_id = 0;
    esp->state.send_bytes_expected = 0;
    esp->state.send_bytes_collected = 0;

    // Initialize SSL configuration
    esp->state.ssl_buffer_size = 2048;  // Default SSL buffer size

    // Initialize AT command system
    ESP8266_ATCommandsInit();

    return esp;
}

void ESP8266_Destroy(ESP8266_t *esp) {
    if (!esp) return;

    // Close any open sockets
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        if (esp->state.connections[i].active && esp->state.connections[i].socket_fd >= 0) {
            close(esp->state.connections[i].socket_fd);
        }
        if (esp->state.connections[i].rx_buffer) {
            free(esp->state.connections[i].rx_buffer);
        }
    }

    // Close server socket if active
    if (esp->state.server_active && esp->state.server_socket >= 0) {
        close(esp->state.server_socket);
    }

    free(esp);
}

void ESP8266_Reset(ESP8266_t *esp) {
    if (!esp) return;

    // Clear UART buffers
    esp->state.rx_head = 0;
    esp->state.rx_tail = 0;
    esp->state.tx_head = 0;
    esp->state.tx_tail = 0;
    esp->state.cmd_line_pos = 0;

    // Reset WiFi state
    esp->state.wifi_state = WIFI_STATE_DISCONNECTED;
    esp->state.station_connected = 0;
    esp->state.station_has_ip = 0;
    memset(esp->state.station_ssid, 0, sizeof(esp->state.station_ssid));
    memset(esp->state.station_password, 0, sizeof(esp->state.station_password));

    // Close all connections
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        if (esp->state.connections[i].active && esp->state.connections[i].socket_fd >= 0) {
            close(esp->state.connections[i].socket_fd);
        }
        esp->state.connections[i].active = 0;
        esp->state.connections[i].socket_fd = -1;
    }

    // Notify of reset
    ESP8266_ATUnsolicited(esp, "ready");
}

void ESP8266_ProcessUARTByte(ESP8266_t *esp, uint8_t byte) {
    if (!esp) return;

    /* // Debug: print sent byte
    if (byte >= 32 && byte < 127) {
        fprintf(stdout, "[ESP TX] '%c' (0x%02x)\n", byte, byte);
    } else if (byte == '\r') {
        fprintf(stdout, "[ESP TX] '\\r' (0x%02x)\n", byte);
    } else if (byte == '\n') {
        fprintf(stdout, "[ESP TX] '\\n' (0x%02x)\n", byte);
    } else {
        fprintf(stdout, "[ESP TX] 0x%02x\n", byte);
    }
    fflush(stdout);*/

    ESP8266_ATProcessByte(esp, byte);
}

int ESP8266_GetUARTByte(ESP8266_t *esp) {
    if (!esp) return -1;

    ESP8266_Internal *state = &esp->state;
    if (state->tx_head == state->tx_tail) {
        return -1;  // No data
    }

    uint8_t byte = state->tx_buffer[state->tx_tail];
    state->tx_tail = (state->tx_tail + 1) % ESP8266_TX_BUFFER_SIZE;

    /* // Debug: print received byte
    if (byte >= 32 && byte < 127) {
        fprintf(stdout, "[ESP RX] '%c' (0x%02x)\n", byte, byte);
    } else if (byte == '\r') {
        fprintf(stdout, "[ESP RX] '\\r' (0x%02x)\n", byte);
    } else if (byte == '\n') {
        fprintf(stdout, "[ESP RX] '\\n' (0x%02x)\n", byte);
    } else {
        fprintf(stdout, "[ESP RX] 0x%02x\n", byte);
    }
    fflush(stdout);*/

    return (int)byte;
}

void ESP8266_Poll(ESP8266_t *esp) {
    if (!esp) return;

    ESP8266_Internal *state = &esp->state;
    uint64_t now = ESP8266_GetTimestampMS();

    // Handle WiFi connection state machine
    if (state->wifi_state == WIFI_STATE_CONNECTING) {
        // Simulate 2 second connection delay
        if (now - state->wifi_state_change_time > 2000) {
            state->wifi_state = WIFI_STATE_CONNECTED;
            ESP8266_ATUnsolicited(esp, "WiFi CONNECTED");
            state->wifi_state_change_time = now;
        }
    } else if (state->wifi_state == WIFI_STATE_CONNECTED) {
        // Simulate 1 second DHCP delay
        if (now - state->wifi_state_change_time > 1000) {
            state->wifi_state = WIFI_STATE_GOT_IP;
            state->station_has_ip = 1;
            ESP8266_ATUnsolicited(esp, "WiFi GOT IP");
        }
    }

    // Handle TCP and SSL connection establishment
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        Connection *conn = &state->connections[i];

        if (!conn->active || conn->connected) continue;
        if (conn->type != CONNECTION_TYPE_TCP && conn->type != CONNECTION_TYPE_SSL) continue;

        // Check if underlying TCP socket is connected
        struct pollfd pfd;
        pfd.fd = conn->socket_fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 0);
        if (ret <= 0 || !(pfd.revents & POLLOUT)) {
            continue;  // Not ready yet
        }

        // Check for TCP connection error
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(conn->socket_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            // TCP connection failed
            ESP8266_SocketClose(esp, i);
            char msg[64];
            snprintf(msg, sizeof(msg), "%d,CLOSED", i);
            ESP8266_ATUnsolicited(esp, msg);
            continue;
        }

        // TCP socket is connected successfully
        if (conn->type == CONNECTION_TYPE_TCP) {
            // Plain TCP - we're done
            conn->connected = 1;
            char msg[64];
            if (state->mux_enabled) {
                snprintf(msg, sizeof(msg), "%d,CONNECT", i);
            } else {
                snprintf(msg, sizeof(msg), "CONNECT");
            }
            ESP8266_ATUnsolicited(esp, msg);
        } else if (conn->type == CONNECTION_TYPE_SSL) {
            if (!conn->ssl) {
                // SSL object missing - shouldn't happen
                ESP8266_SocketClose(esp, i);
                char msg[64];
                snprintf(msg, sizeof(msg), "%d,CLOSED", i);
                ESP8266_ATUnsolicited(esp, msg);
                continue;
            }

            // SSL connection - try handshake
            SSL *ssl = (SSL*)conn->ssl;
            ret = SSL_connect(ssl);
            if (ret <= 0) {
                int ssl_err = SSL_get_error(ssl, ret);
                if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE) {
                    // SSL handshake failed
                    ESP8266_SocketClose(esp, i);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "%d,CLOSED", i);
                    ESP8266_ATUnsolicited(esp, msg);
                }
                // Otherwise, handshake in progress (will retry next poll)
            } else {
                // SSL handshake completed successfully
                conn->connected = 1;
                char msg[64];
                if (state->mux_enabled) {
                    snprintf(msg, sizeof(msg), "%d,CONNECT", i);
                } else {
                    snprintf(msg, sizeof(msg), "CONNECT");
                }
                ESP8266_ATUnsolicited(esp, msg);
            }
        }
    }

    // Check all sockets for incoming data
    ESP8266_CheckSocketData(esp);
}

size_t ESP8266_TXDataAvailable(ESP8266_t *esp) {
    if (!esp) return 0;

    ESP8266_Internal *state = &esp->state;
    if (state->tx_head >= state->tx_tail) {
        return state->tx_head - state->tx_tail;
    } else {
        return ESP8266_TX_BUFFER_SIZE - (state->tx_tail - state->tx_head);
    }
}

void ESP8266_SetBaudRate(ESP8266_t *esp, uint32_t baud) {
    if (!esp) return;
    esp->state.uart_baud = baud;
}

uint32_t ESP8266_GetBaudRate(ESP8266_t *esp) {
    if (!esp) return 0;
    return esp->state.uart_baud;
}

void ESP8266_SetEcho(ESP8266_t *esp, uint8_t enabled) {
    if (!esp) return;
    esp->state.echo_enabled = enabled ? 1 : 0;
}

uint8_t ESP8266_GetEcho(ESP8266_t *esp) {
    if (!esp) return 0;
    return esp->state.echo_enabled;
}

WiFi_Mode ESP8266_GetWiFiMode(ESP8266_t *esp) {
    if (!esp) return WIFI_MODE_OFF;
    return esp->state.wifi_mode;
}

void ESP8266_SetWiFiMode(ESP8266_t *esp, WiFi_Mode mode) {
    if (!esp) return;
    esp->state.wifi_mode = mode;
}

WiFi_State ESP8266_GetWiFiState(ESP8266_t *esp) {
    if (!esp) return WIFI_STATE_DISCONNECTED;
    return esp->state.wifi_state;
}

const char* ESP8266_GetStationIP(ESP8266_t *esp) {
    if (!esp) return "";
    return esp->state.station_ip;
}

const char* ESP8266_GetAPIP(ESP8266_t *esp) {
    if (!esp) return "";
    return esp->state.ap_ip;
}

Connection* ESP8266_GetConnection(ESP8266_t *esp, uint8_t link_id) {
    if (!esp || link_id >= ESP8266_MAX_CONNECTIONS) return NULL;
    if (!esp->state.connections[link_id].active) return NULL;
    return &esp->state.connections[link_id];
}

void ESP8266_DebugPrint(ESP8266_t *esp) {
    if (!esp) return;

    ESP8266_Internal *state = &esp->state;
    printf("\n=== ESP8266 State ===\n");
    printf("UART: %u baud, %d data bits, %d stop bits, parity=%d, flow=%d\n",
           state->uart_baud, state->uart_databits, state->uart_stopbits,
           state->uart_parity, state->uart_flow_control);
    printf("Echo: %s\n", state->echo_enabled ? "ON" : "OFF");
    printf("WiFi Mode: %s\n", ESP8266_WiFiModeName(state->wifi_mode));
    printf("WiFi State: %s\n", ESP8266_WiFiStateName(state->wifi_state));
    printf("Station SSID: %s\n", state->station_ssid[0] ? state->station_ssid : "(none)");
    printf("Station IP: %s (has IP: %s)\n", state->station_ip, state->station_has_ip ? "yes" : "no");
    printf("Station MAC: %s\n", state->station_mac);
    printf("AP IP: %s\n", state->ap_ip);
    printf("AP SSID: %s\n", state->ap_ssid[0] ? state->ap_ssid : "(not configured)");
    printf("AP MAC: %s\n", state->ap_mac);
    printf("Connections: %d active\n", ESP8266_MAX_CONNECTIONS);
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        if (state->connections[i].active) {
            printf("  [%d] %s %s:%u (FD=%d)\n", i,
                   state->connections[i].type == CONNECTION_TYPE_TCP ? "TCP" :
                   state->connections[i].type == CONNECTION_TYPE_UDP ? "UDP" : "SSL",
                   state->connections[i].remote_ip,
                   state->connections[i].remote_port,
                   state->connections[i].socket_fd);
        }
    }
    printf("==================\n\n");
}

const char* ESP8266_WiFiStateName(WiFi_State state) {
    switch (state) {
        case WIFI_STATE_DISCONNECTED: return "DISCONNECTED";
        case WIFI_STATE_CONNECTING:   return "CONNECTING";
        case WIFI_STATE_CONNECTED:    return "CONNECTED";
        case WIFI_STATE_GOT_IP:       return "GOT_IP";
        case WIFI_STATE_FAILED:       return "FAILED";
        default:                      return "UNKNOWN";
    }
}

const char* ESP8266_WiFiModeName(WiFi_Mode mode) {
    switch (mode) {
        case WIFI_MODE_OFF:      return "OFF";
        case WIFI_MODE_STATION:  return "STATION";
        case WIFI_MODE_SOFTAP:   return "SOFTAP";
        case WIFI_MODE_DUAL:     return "DUAL";
        default:                 return "UNKNOWN";
    }
}

/* ========== Socket Management Functions ========== */

/**
 * Set a file descriptor to non-blocking mode
 * Returns 0 on success, -1 on error
 */
static int ESP8266_SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * Create a TCP connection to remote host
 * Returns link_id on success, -1 on error
 */
int ESP8266_SocketConnect(ESP8266_t *esp, const char *remote_ip, uint16_t remote_port, Connection_Type type) {
    if (!esp || !remote_ip) return -1;

    ESP8266_Internal *state = &esp->state;

    // Find free connection slot
    int link_id = -1;
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        if (!state->connections[i].active) {
            link_id = i;
            break;
        }
    }

    if (link_id == -1) {
        return -1;  // No free slots
    }

    // Create socket
    int sockfd;
    if (type == CONNECTION_TYPE_TCP || type == CONNECTION_TYPE_SSL) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
    } else {  // UDP
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    }

    if (sockfd < 0) {
        return -1;
    }

    // Set non-blocking
    if (ESP8266_SetNonBlocking(sockfd) < 0) {
        close(sockfd);
        return -1;
    }

    // Connect to remote host
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(remote_port);

    // Try to parse IP address
    if (inet_pton(AF_INET, remote_ip, &server_addr.sin_addr) <= 0) {
        // Not an IP, try DNS lookup
        struct hostent *host = gethostbyname(remote_ip);
        if (!host) {
            close(sockfd);
            return -1;
        }
        memcpy(&server_addr.sin_addr, host->h_addr_list[0], host->h_length);
    }

    // For TCP/SSL, initiate connection (non-blocking, will complete later)
    if (type == CONNECTION_TYPE_TCP || type == CONNECTION_TYPE_SSL) {
        int ret = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (ret < 0 && errno != EINPROGRESS) {
            close(sockfd);
            return -1;
        }
    }

    // Setup connection structure
    Connection *conn = &state->connections[link_id];
    memset(conn, 0, sizeof(Connection));
    conn->active = 1;
    conn->type = type;
    conn->is_server = 0;
    conn->socket_fd = sockfd;
    conn->ssl = NULL;
    // UDP is immediately ready, TCP/SSL need connection establishment
    conn->connected = (type == CONNECTION_TYPE_UDP) ? 1 : 0;
    strncpy(conn->remote_ip, remote_ip, ESP8266_MAX_IP_STR_LEN);
    conn->remote_port = remote_port;
    conn->local_port = 0;  // OS assigns

    // For SSL connections, create SSL object
    if (type == CONNECTION_TYPE_SSL) {
        ESP8266_InitSSL();
        if (ssl_ctx) {
            SSL *ssl = SSL_new(ssl_ctx);
            if (ssl) {
                SSL_set_fd(ssl, sockfd);
                // Set SNI (Server Name Indication) hostname
                SSL_set_tlsext_host_name(ssl, remote_ip);
                SSL_set_connect_state(ssl);
                // Non-blocking SSL_connect will happen in poll loop
                conn->ssl = ssl;
            }
        }
    }

    // Allocate RX buffer
    conn->rx_buffer = malloc(ESP8266_RX_BUFFER_SIZE);
    if (!conn->rx_buffer) {
        if (conn->ssl) SSL_free((SSL*)conn->ssl);
        close(sockfd);
        memset(conn, 0, sizeof(Connection));
        return -1;
    }
    conn->rx_buffer_len = ESP8266_RX_BUFFER_SIZE;
    conn->rx_buffer_pos = 0;

    return link_id;
}

/**
 * Close a connection
 * Returns 0 on success, -1 on error
 */
int ESP8266_SocketClose(ESP8266_t *esp, uint8_t link_id) {
    if (!esp || link_id >= ESP8266_MAX_CONNECTIONS) return -1;

    ESP8266_Internal *state = &esp->state;
    Connection *conn = &state->connections[link_id];

    if (!conn->active) return -1;

    if (conn->ssl) {
        SSL_shutdown((SSL*)conn->ssl);
        SSL_free((SSL*)conn->ssl);
        conn->ssl = NULL;
    }

    if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
    }

    if (conn->rx_buffer) {
        free(conn->rx_buffer);
    }

    memset(conn, 0, sizeof(Connection));
    return 0;
}

/**
 * Send data on a connection
 * Returns number of bytes sent, -1 on error
 */
int ESP8266_SocketSend(ESP8266_t *esp, uint8_t link_id, const uint8_t *data, uint16_t len) {
    if (!esp || link_id >= ESP8266_MAX_CONNECTIONS || !data) return -1;

    ESP8266_Internal *state = &esp->state;
    Connection *conn = &state->connections[link_id];

    if (!conn->active) return -1;

    // Check if connection is established (for TCP/SSL)
    if ((conn->type == CONNECTION_TYPE_TCP || conn->type == CONNECTION_TYPE_SSL) && !conn->connected) {
        return -1;  // Not ready yet
    }

    // For UDP, we need to send to the remote address
    if (conn->type == CONNECTION_TYPE_UDP) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(conn->remote_port);
        inet_pton(AF_INET, conn->remote_ip, &addr.sin_addr);

        return sendto(conn->socket_fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    } else if (conn->type == CONNECTION_TYPE_SSL && conn->ssl) {
        // SSL - check if handshake is complete first
        SSL *ssl = (SSL*)conn->ssl;
        if (!SSL_is_init_finished(ssl)) {
            // Handshake not complete yet
            errno = EAGAIN;
            return -1;
        }

        int ret = SSL_write(ssl, data, len);
        if (ret <= 0) {
            int ssl_err = SSL_get_error(ssl, ret);
            if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                errno = EAGAIN;
            }
            return -1;
        }
        return ret;
    } else {
        // TCP
        return send(conn->socket_fd, data, len, 0);
    }
}

/**
 * Check all active sockets for incoming data
 * Called from ESP8266_Poll()
 */
static void ESP8266_CheckSocketData(ESP8266_t *esp) {
    if (!esp) return;

    ESP8266_Internal *state = &esp->state;

    // Build poll array for all active connections
    struct pollfd fds[ESP8266_MAX_CONNECTIONS];
    int fd_to_link[ESP8266_MAX_CONNECTIONS];
    int nfds = 0;

    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        if (state->connections[i].active && state->connections[i].socket_fd >= 0) {
            fds[nfds].fd = state->connections[i].socket_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            fd_to_link[nfds] = i;
            nfds++;
        }
    }

    if (nfds == 0) return;

    // Poll with 0 timeout (non-blocking check)
    int ret = poll(fds, nfds, 0);
    if (ret <= 0) return;

    // Check each socket for data
    for (int i = 0; i < nfds; i++) {
        if (fds[i].revents & POLLIN) {
            int link_id = fd_to_link[i];
            Connection *conn = &state->connections[link_id];

            // Try to read data into RX buffer
            // rx_buffer is used as a temporary staging area - always read from beginning
            if (conn->rx_buffer_len > 0) {
                ssize_t bytes_read;
                if (conn->type == CONNECTION_TYPE_UDP) {
                    bytes_read = recvfrom(conn->socket_fd,
                                        conn->rx_buffer,
                                        conn->rx_buffer_len, 0, NULL, NULL);
                } else if (conn->type == CONNECTION_TYPE_SSL && conn->ssl) {
                    // SSL read
                    bytes_read = SSL_read((SSL*)conn->ssl,
                                        conn->rx_buffer,
                                        conn->rx_buffer_len);
                    if (bytes_read <= 0) {
                        int ssl_err = SSL_get_error((SSL*)conn->ssl, bytes_read);
                        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                            bytes_read = -1;
                            errno = EAGAIN;
                        } else {
                            bytes_read = 0;  // Connection closed or error
                        }
                    }
                } else {
                    // TCP read
                    bytes_read = recv(conn->socket_fd,
                                    conn->rx_buffer,
                                    conn->rx_buffer_len, 0);
                }

                if (bytes_read > 0) {
                    // Got data! Send +IPD unsolicited message
                    // Format: \r\n+IPD,<len>:<data>\r\n (no CRLF between : and data!)
                    char ipd_header[64];
                    if (state->mux_enabled) {
                        snprintf(ipd_header, sizeof(ipd_header), "\r\n+IPD,%d,%d:", link_id, (int)bytes_read);
                    } else {
                        snprintf(ipd_header, sizeof(ipd_header), "\r\n+IPD,%d:", (int)bytes_read);
                    }
                    ESP8266_TXQueueString(esp, ipd_header);

                    // Queue the actual data
                    for (ssize_t j = 0; j < bytes_read; j++) {
                        ESP8266_TXQueueChar(esp, conn->rx_buffer[j]);
                    }

                    // Queue terminating CRLF
                    ESP8266_TXQueueString(esp, "\r\n");

                    // rx_buffer is reused on each read, no need to increment rx_buffer_pos
                } else if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // Connection closed or error
                    if (state->mux_enabled) {
                        ESP8266_ATUnsolicited(esp, "%d,CLOSED", link_id);
                    } else {
                        ESP8266_ATUnsolicited(esp, "CLOSED");
                    }
                    ESP8266_SocketClose(esp, link_id);
                }
            }
        }
    }
}

/* ========== Internal Response Functions ========== */

void ESP8266_ATResponse(ESP8266_t *esp, const char *fmt, ...) {
    if (!esp) return;

    char buffer[ESP8266_RESPONSE_BUFFER_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    ESP8266_TXQueueString(esp, buffer);
    ESP8266_TXQueueString(esp, "\r\n");
}

void ESP8266_ATUnsolicited(ESP8266_t *esp, const char *fmt, ...) {
    if (!esp) return;

    char buffer[ESP8266_RESPONSE_BUFFER_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    ESP8266_TXQueueString(esp, buffer);
    ESP8266_TXQueueString(esp, "\r\n");
}

void ESP8266_ATError(ESP8266_t *esp) {
    if (!esp) return;
    ESP8266_TXQueueString(esp, "ERROR\r\n");
}

void ESP8266_ATOKK(ESP8266_t *esp) {
    if (!esp) return;
    ESP8266_TXQueueString(esp, "OK\r\n");
}
