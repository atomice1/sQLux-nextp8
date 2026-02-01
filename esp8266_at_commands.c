/*
 * ESP8266 AT Command Parser and Handlers
 *
 * Implements AT command line parsing, tokenization, and command dispatch
 *
 * Copyright (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

#include "esp8266_model.h"
#include "esp8266_model_internal.h"
#include "esp8266_at_commands.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ========== Forward Declarations from esp8266_model.c ========== */

extern int ESP8266_WiFiConnect(ESP8266_t *esp, const char *ssid, const char *password);
extern void ESP8266_WiFiDisconnect(ESP8266_t *esp);
extern int ESP8266_WiFiScan(ESP8266_t *esp, ScanResult *results, int max_results);
extern int ESP8266_GetConnectedAPInfo(ESP8266_t *esp, char *bssid, uint8_t *channel, int8_t *rssi);
extern int ESP8266_SocketConnect(ESP8266_t *esp, const char *remote_ip, uint16_t remote_port, Connection_Type type);
extern int ESP8266_SocketClose(ESP8266_t *esp, uint8_t link_id);
extern int ESP8266_SocketSend(ESP8266_t *esp, uint8_t link_id, const uint8_t *data, uint16_t len);

/* Virtual AP database access - we need this to query connected AP info */
#define MAX_VIRTUAL_APS 10

/* ========== Command Dispatch Table ========== */

typedef struct {
    const char *command;
    ATCommandHandler handler;
} CommandEntry;

static CommandEntry command_table[] = {
    { "AT", AT_Handler },
    { "RST", AT_RST_Handler },
    { "GMR", AT_GMR_Handler },
    { "E0", ATE0_Handler },
    { "E1", ATE1_Handler },
    { "UART_CUR", AT_UART_CUR_Handler },
    { "UART", AT_UART_CUR_Handler },
    // Phase 2: WiFi commands
    { "CWMODE_CUR", AT_CWMODE_CUR_Handler },
    { "CWMODE_DEF", AT_CWMODE_CUR_Handler },
    { "CWMODE", AT_CWMODE_CUR_Handler },
    { "CWJAP_CUR", AT_CWJAP_CUR_Handler },
    { "CWJAP_DEF", AT_CWJAP_CUR_Handler },
    { "CWJAP", AT_CWJAP_CUR_Handler },
    { "CWQAP", AT_CWQAP_Handler },
    { "CWLAP", AT_CWLAP_Handler },
    { "CIFSR", AT_CIFSR_Handler },
    { "CWSAP_CUR", AT_CWSAP_CUR_Handler },
    { "CWSAP_DEF", AT_CWSAP_CUR_Handler },
    { "CWSAP", AT_CWSAP_CUR_Handler },
    { "CWDHCP_CUR", AT_CWDHCP_CUR_Handler },
    { "CWDHCP_DEF", AT_CWDHCP_CUR_Handler },
    { "CWDHCP", AT_CWDHCP_CUR_Handler },
    { "CIPSTA_CUR", AT_CIPSTA_CUR_Handler },
    { "CIPSTA_DEF", AT_CIPSTA_CUR_Handler },
    { "CIPSTA", AT_CIPSTA_CUR_Handler },
    { "CIPSTAMAC_CUR", AT_CIPSTAMAC_CUR_Handler },
    { "CIPSTAMAC_DEF", AT_CIPSTAMAC_CUR_Handler },
    { "CIPSTAMAC", AT_CIPSTAMAC_CUR_Handler },
    // Phase 3: TCP/IP commands
    { "CIPMUX", AT_CIPMUX_Handler },
    { "CIPSTART", AT_CIPSTART_Handler },
    { "CIPSEND", AT_CIPSEND_Handler },
    { "CIPCLOSE", AT_CIPCLOSE_Handler },
    { "CIPSTATUS", AT_CIPSTATUS_Handler },
    { "CIPSERVER", AT_CIPSERVER_Handler },
    { "CIPDOMAIN", AT_CIPDOMAIN_Handler },
    { "CIPSSLSIZE", AT_CIPSSLSIZE_Handler },
    { NULL, NULL }
};

/* ========== Parser State ========== */

typedef struct {
    char command_name[64];      // e.g., "CWMODE_CUR"
    char command_type;          // '?'=query, '='=set, '\0'=execute, 'T'=test
    char *params[16];           // Parsed parameter strings
    int param_count;
} ParsedCommand;

/* ========== Helper Functions ========== */

static int ESP8266_ParseCommand(const char *line, ParsedCommand *parsed) {
    if (!line || !parsed) return -1;

    memset(parsed, 0, sizeof(*parsed));
    parsed->command_type = '\0';

    // Skip "AT" prefix
    if (line[0] != 'A' || line[1] != 'T') {
        return -1;
    }

    const char *p = &line[2];

    // Handle bare "AT" command
    if (*p == '\0' || *p == '\r' || *p == '\n') {
        strcpy(parsed->command_name, "");
        return 0;
    }

    // Check for extended command (AT+...) vs basic command (ATE0, etc)
    bool is_extended = (*p == '+');
    if (is_extended) {
        p++;
    }

    // Extract command name (letters and numbers)
    char *cmd = parsed->command_name;
    int cmd_len = 0;
    while (*p && (size_t)cmd_len < sizeof(parsed->command_name) - 1) {
        if (isalnum(*p) || *p == '_') {
            *cmd++ = *p++;
            cmd_len++;
        } else {
            break;
        }
    }
    *cmd = '\0';

    if (cmd_len == 0) {
        return -1;  // No command name
    }

    // Check command type and parse parameters
    if (*p == '?') {
        parsed->command_type = '?';  // Query
        p++;
    } else if (*p == '=') {
        p++;
        if (*p == '?') {
            parsed->command_type = 'T';  // Test
            p++;
        } else {
            parsed->command_type = '=';  // Set
            // Parse parameters
            while (*p && parsed->param_count < 16) {
                // Skip whitespace
                while (*p && isspace(*p) && *p != '\r' && *p != '\n') p++;

                if (*p == '\r' || *p == '\n' || *p == '\0') break;

                // Handle quoted strings
                if (*p == '"') {
                    p++;
                    char param_buf[256];
                    int plen = 0;

                    while (*p && *p != '"' && (size_t)plen < sizeof(param_buf) - 1) {
                        if (*p == '\\' && *(p + 1)) {
                            // Handle escape sequences
                            p++;
                            param_buf[plen++] = *p++;
                        } else {
                            param_buf[plen++] = *p++;
                        }
                    }

                    if (*p == '"') p++;

                    param_buf[plen] = '\0';
                    parsed->params[parsed->param_count] = malloc(plen + 1);
                    if (parsed->params[parsed->param_count]) {
                        strcpy(parsed->params[parsed->param_count], param_buf);
                        parsed->param_count++;
                    }
                } else {
                    // Unquoted parameter (number, etc)
                    char param_buf[256];
                    int plen = 0;

                    while (*p && *p != ',' && !isspace(*p) && (size_t)plen < sizeof(param_buf) - 1) {
                        param_buf[plen++] = *p++;
                    }

                    if (plen > 0) {
                        param_buf[plen] = '\0';
                        parsed->params[parsed->param_count] = malloc(plen + 1);
                        if (parsed->params[parsed->param_count]) {
                            strcpy(parsed->params[parsed->param_count], param_buf);
                            parsed->param_count++;
                        }
                    }
                }

                // Skip to next parameter
                while (*p && *p != ',' && !isspace(*p) && *p != '\r' && *p != '\n') p++;
                if (*p == ',') p++;
            }
        }
    } else {
        parsed->command_type = '\0';  // Execute
    }

    // Skip any trailing whitespace/CRLF
    while (*p && (isspace(*p) || *p == '\r' || *p == '\n')) p++;

    // Command must end cleanly
    if (*p != '\0') {
        return -1;
    }

    return 0;
}

static void ESP8266_FreeParseResult(ParsedCommand *parsed) {
    for (int i = 0; i < parsed->param_count; i++) {
        if (parsed->params[i]) {
            free(parsed->params[i]);
            parsed->params[i] = NULL;
        }
    }
    parsed->param_count = 0;
}

static ATCommandHandler ESP8266_FindHandler(const char *cmd_name) {
    for (int i = 0; command_table[i].command != NULL; i++) {
        if (strcmp(command_table[i].command, cmd_name) == 0) {
            return command_table[i].handler;
        }
    }
    return NULL;
}

/* ========== Core Parser Functions ========== */

void ESP8266_ATProcessByte(ESP8266_t *esp, uint8_t byte) {
    if (!esp) return;

    ESP8266_Internal *state = &esp->state;

    // Check if we're in CIPSEND data collection mode
    if (state->send_mode) {
        // Collect data bytes
        if (state->send_bytes_collected < state->send_bytes_expected) {
            state->send_buffer[state->send_bytes_collected++] = byte;

            // Check if we've collected all bytes
            if (state->send_bytes_collected >= state->send_bytes_expected) {
                // Send the data
                int result = ESP8266_SocketSend(esp, state->send_link_id,
                                               state->send_buffer,
                                               state->send_bytes_expected);

                if (result >= 0) {
                    // Success
                    if (state->mux_enabled) {
                        ESP8266_ATResponse(esp, "SEND OK");
                    } else {
                        ESP8266_ATResponse(esp, "SEND OK");
                    }
                } else {
                    // Error
                    ESP8266_ATResponse(esp, "SEND FAIL");
                }

                // Exit send mode
                state->send_mode = 0;
                state->send_bytes_collected = 0;
                state->send_bytes_expected = 0;
            }
        }
        return;
    }

    // Normal AT command processing
    // Echo if enabled
    if (state->echo_enabled && byte >= 32 && byte < 127) {
        // Queue echo character to TX
        uint16_t next_head = (state->tx_head + 1) % ESP8266_TX_BUFFER_SIZE;
        if (next_head != state->tx_tail) {
            state->tx_buffer[state->tx_head] = byte;
            state->tx_head = next_head;
        }
    }

    // Handle backspace
    if (byte == 0x08 || byte == 0x7F) {
        if (state->cmd_line_pos > 0) {
            state->cmd_line_pos--;
            state->cmd_line_buffer[state->cmd_line_pos] = '\0';
        }
        return;
    }

    // Handle CR+LF line ending
    if (byte == '\r') {
        return;  // Wait for LF
    }

    if (byte == '\n' || byte == '\r') {
        // Process complete line
        if (state->cmd_line_pos > 0) {
            // Trim trailing whitespace
            while (state->cmd_line_pos > 0 && isspace(state->cmd_line_buffer[state->cmd_line_pos - 1])) {
                state->cmd_line_pos--;
            }
            state->cmd_line_buffer[state->cmd_line_pos] = '\0';

            // Dispatch command
            ESP8266_ATDispatch(esp, state->cmd_line_buffer);
        }

        // Reset buffer
        state->cmd_line_pos = 0;
        state->cmd_line_buffer[0] = '\0';
        return;
    }

    // Accumulate character
    if (state->cmd_line_pos < sizeof(state->cmd_line_buffer) - 1) {
        state->cmd_line_buffer[state->cmd_line_pos++] = (char)byte;
        state->cmd_line_buffer[state->cmd_line_pos] = '\0';
    }
}

void ESP8266_ATDispatch(ESP8266_t *esp, const char *cmd_str) {
    if (!esp || !cmd_str) return;

    ParsedCommand parsed;
    char response[ESP8266_RESPONSE_BUFFER_SIZE];
    int result = -1;

    // Parse the command
    printf("[ESP] Received command: %s\n", cmd_str);
    if (ESP8266_ParseCommand(cmd_str, &parsed) < 0) {
        ESP8266_ATError(esp);
        return;
    }

    // Handle bare "AT" command
    if (parsed.command_name[0] == '\0') {
        ESP8266_ATOKK(esp);
        return;
    }

    printf("[ESP] Parsed command: %s, type: %c, params: %d\n",
           parsed.command_name, parsed.command_type, parsed.param_count);

    // Find and invoke handler
    ATCommandHandler handler = ESP8266_FindHandler(parsed.command_name);

    if (!handler) {
        ESP8266_ATError(esp);
        ESP8266_FreeParseResult(&parsed);
        return;
    }

    // Invoke handler
    response[0] = '\0';
    result = handler(esp, parsed.command_name, (const char **)parsed.params, parsed.param_count,
                     response, sizeof(response));

    // Send response
    if (result == 0) {
        // Handler successful
        if (response[0]) {
            // Custom response (e.g., "+CWMODE:1")
            ESP8266_ATResponse(esp, "%s", response);
        }
        ESP8266_ATOKK(esp);
    } else {
        // Handler failed
        ESP8266_ATError(esp);
    }

    ESP8266_FreeParseResult(&parsed);
}

/* ========== Handler Implementations (Phase 1) ========== */

int AT_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
               char *response, size_t max_len) {
    (void)esp;
    (void)command_name;
    (void)params;
    (void)param_count;
    (void)response;
    (void)max_len;
    // AT just returns OK (handled by dispatcher)
    return 0;
}

int AT_RST_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                   char *response, size_t max_len) {
    (void)params;
    (void)command_name;
    (void)param_count;
    (void)response;
    (void)max_len;

    if (!esp) return -1;

    // Reset will happen after OK is sent
    ESP8266_Reset(esp);
    return 0;
}

int AT_GMR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                   char *response, size_t max_len) {
    (void)params;
    (void)command_name;
    (void)param_count;

    if (!esp || !response) return -1;

    ESP8266_Internal *state = &esp->state;

    snprintf(response, max_len, "%s\r\n%s\r\n%s",
             state->at_version,
             state->sdk_version,
             state->build_date);

    return 0;
}

int ATE0_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                 char *response, size_t max_len) {
    (void)params;
    (void)command_name;
    (void)param_count;
    (void)response;
    (void)max_len;

    if (!esp) return -1;

    ESP8266_SetEcho(esp, 0);
    return 0;
}

int ATE1_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                 char *response, size_t max_len) {
    (void)params;
    (void)command_name;
    (void)param_count;
    (void)response;
    (void)max_len;

    if (!esp) return -1;

    ESP8266_SetEcho(esp, 1);
    return 0;
}

int AT_UART_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                        char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query
        snprintf(response, max_len, "+UART_CUR:%u,%d,%d,%d,%d",
                 state->uart_baud,
                 state->uart_databits,
                 state->uart_stopbits,
                 state->uart_parity,
                 state->uart_flow_control);
        return 0;
    } else if (param_count == 5) {
        // Set
        long baud = ESP8266_ParseInt(params[0]);
        long databits = ESP8266_ParseInt(params[1]);
        long stopbits = ESP8266_ParseInt(params[2]);
        long parity = ESP8266_ParseInt(params[3]);
        long flow = ESP8266_ParseInt(params[4]);

        // Validate ranges
        if (baud < 110 || baud > 115200 * 40) return -1;
        if (databits < 5 || databits > 8) return -1;
        if (stopbits < 1 || stopbits > 3) return -1;
        if (parity < 0 || parity > 2) return -1;
        if (flow < 0 || flow > 3) return -1;

        // Update state
        state->uart_baud = (uint32_t)baud;
        state->uart_databits = (uint8_t)databits;
        state->uart_stopbits = (uint8_t)stopbits;
        state->uart_parity = (uint8_t)parity;
        state->uart_flow_control = (uint8_t)flow;

        response[0] = '\0';
        return 0;
    }

    return -1;
}

/* ========== WiFi Command Handlers (Phase 2) ========== */

int AT_CWMODE_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                          char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query
        snprintf(response, max_len, "+CWMODE_CUR:%d", state->wifi_mode);
        return 0;
    } else if (param_count == 1) {
        // Set
        long mode = ESP8266_ParseInt(params[0]);
        if (mode < 1 || mode > 3) return -1;

        state->wifi_mode = (WiFi_Mode)mode;
        response[0] = '\0';
        return 0;
    }

    return -1;
}

int AT_CWJAP_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                         char *response, size_t max_len) {
    if (!esp || !response) return -1;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query - return current connection info
        if (state->wifi_state != WIFI_STATE_CONNECTED && state->wifi_state != WIFI_STATE_GOT_IP) {
            snprintf(response, max_len, "No AP");
            return 0;
        }

        // Get connected AP info
        char bssid[ESP8266_MAX_MAC_STR_LEN + 1];
        uint8_t channel;
        int8_t rssi;

        if (ESP8266_GetConnectedAPInfo(esp, bssid, &channel, &rssi) < 0) {
            snprintf(response, max_len, "No AP");
            return 0;
        }

        snprintf(response, max_len, "+%s:\"%s\",\"%s\",%d,%d",
                 command_name, state->station_ssid, bssid, channel, rssi);
        return 0;
    } else if (param_count >= 2) {
        // Set - connect to AP
        const char *ssid = params[0];
        const char *password = params[1];

        // Attempt connection
        int result = ESP8266_WiFiConnect(esp, ssid, password);

        if (result == 0) {
            // Connection initiated successfully
            response[0] = '\0';
            return 0;
        } else {
            // Connection failed - return error code
            snprintf(response, max_len, "+CWJAP:3");  // Error code 3: cannot find target AP
            return -1;
        }
    }

    return -1;
}

int AT_CWQAP_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                     char *response, size_t max_len) {
    (void)params;
    (void)command_name;
    (void)param_count;
    (void)response;
    (void)max_len;

    if (!esp) return -1;

    // Disconnect from AP
    ESP8266_WiFiDisconnect(esp);

    return 0;
}

int AT_CWLAP_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                     char *response, size_t max_len) {
    if (!esp || !response) return -1;

    (void)params;
    (void)command_name;
    (void)param_count;

    // Get scan results
    ScanResult results[ESP8266_MAX_AP_RESULTS];
    int count = ESP8266_WiFiScan(esp, results, ESP8266_MAX_AP_RESULTS);

    // Format response with all APs
    size_t pos = 0;
    for (int i = 0; i < count && pos < max_len - 100; i++) {
        int written = snprintf(response + pos, max_len - pos,
                               "+CWLAP:%d,\"%s\",%d,\"%s\",%d,%d,%d\r\n",
                               results[i].encryption,
                               results[i].ssid,
                               results[i].rssi,
                               results[i].bssid,
                               results[i].channel,
                               results[i].freq_offset,
                               results[i].freq_cal);
        if (written > 0) {
            pos += written;
        }
    }

    // Remove trailing CRLF (dispatcher will add it)
    if (pos >= 2 && response[pos-2] == '\r' && response[pos-1] == '\n') {
        response[pos-2] = '\0';
    }

    return 0;
}

int AT_CIFSR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                     char *response, size_t max_len) {
    (void)params;
    (void)command_name;
    (void)param_count;

    if (!esp || !response) return -1;

    ESP8266_Internal *state = &esp->state;

    // Return station IP and MAC
    snprintf(response, max_len, "+CIFSR:STAIP,\"%s\"\r\n+CIFSR:STAMAC,\"%s\"",
             state->station_ip, state->station_mac);

    return 0;
}

int AT_CWSAP_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                         char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query
        snprintf(response, max_len, "+CWSAP_CUR:\"%s\",\"%s\",%d,%d,4,0",
                 state->ap_ssid[0] ? state->ap_ssid : "ESP8266",
                 state->ap_password,
                 state->ap_channel,
                 state->ap_encryption);
        return 0;
    } else if (param_count >= 4) {
        // Set - configure SoftAP
        const char *ssid = params[0];
        const char *password = params[1];
        long channel = ESP8266_ParseInt(params[2]);
        long encryption = ESP8266_ParseInt(params[3]);

        // Validate
        if (channel < 1 || channel > 13) return -1;
        if (encryption < 0 || encryption > 4 || encryption == 1) return -1;  // WEP not supported

        // Apply config
        strncpy(state->ap_ssid, ssid, ESP8266_MAX_SSID_LEN);
        state->ap_ssid[ESP8266_MAX_SSID_LEN] = '\0';
        strncpy(state->ap_password, password, ESP8266_MAX_PASSWORD_LEN);
        state->ap_password[ESP8266_MAX_PASSWORD_LEN] = '\0';
        state->ap_channel = (uint8_t)channel;
        state->ap_encryption = (Encryption_Type)encryption;

        response[0] = '\0';
        return 0;
    }

    return -1;
}

int AT_CWDHCP_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                          char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query - return DHCP state as bitfield
        int dhcp_state = (state->dhcp_enabled[0] ? 1 : 0) |
                         (state->dhcp_enabled[1] ? 2 : 0);
        snprintf(response, max_len, "+CWDHCP_CUR:%d", dhcp_state);
        return 0;
    } else if (param_count == 2) {
        // Set
        long mode = ESP8266_ParseInt(params[0]);
        long enable = ESP8266_ParseInt(params[1]);

        if (mode < 0 || mode > 2) return -1;
        if (enable < 0 || enable > 1) return -1;

        // Apply settings
        if (mode == 0 || mode == 2) {
            state->dhcp_enabled[0] = (uint8_t)enable;  // SoftAP
        }
        if (mode == 1 || mode == 2) {
            state->dhcp_enabled[1] = (uint8_t)enable;  // Station
        }

        response[0] = '\0';
        return 0;
    }

    return -1;
}

int AT_CIPSTA_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                          char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query
        snprintf(response, max_len, "+CIPSTA_CUR:ip:\"%s\"\r\n+CIPSTA_CUR:gateway:\"%s\"\r\n+CIPSTA_CUR:netmask:\"%s\"",
                 state->station_ip, state->station_gateway, state->station_netmask);
        return 0;
    } else if (param_count >= 1) {
        // Set
        const char *ip = params[0];

        // Validate IP
        if (ESP8266_ParseIP(ip) < 0) return -1;

        // Apply IP
        strncpy(state->station_ip, ip, ESP8266_MAX_IP_STR_LEN);
        state->station_ip[ESP8266_MAX_IP_STR_LEN] = '\0';

        // Optional gateway and netmask
        if (param_count >= 2) {
            const char *gateway = params[1];
            if (ESP8266_ParseIP(gateway) < 0) return -1;
            strncpy(state->station_gateway, gateway, ESP8266_MAX_IP_STR_LEN);
            state->station_gateway[ESP8266_MAX_IP_STR_LEN] = '\0';
        }

        if (param_count >= 3) {
            const char *netmask = params[2];
            if (ESP8266_ParseIP(netmask) < 0) return -1;
            strncpy(state->station_netmask, netmask, ESP8266_MAX_IP_STR_LEN);
            state->station_netmask[ESP8266_MAX_IP_STR_LEN] = '\0';
        }

        // Disable DHCP if static IP is set
        state->dhcp_enabled[1] = 0;

        response[0] = '\0';
        return 0;
    }

    return -1;
}

int AT_CIPSTAMAC_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                              char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query
        snprintf(response, max_len, "+CIPSTAMAC_CUR:\"%s\"", state->station_mac);
        return 0;
    } else if (param_count == 1) {
        // Set
        const char *mac = params[0];

        // Validate MAC
        if (ESP8266_ParseMAC(mac) < 0) return -1;

        // Apply MAC
        strncpy(state->station_mac, mac, ESP8266_MAX_MAC_STR_LEN);
        state->station_mac[ESP8266_MAX_MAC_STR_LEN] = '\0';

        response[0] = '\0';
        return 0;
    }

    return -1;
}

/* ========== Phase 3: TCP/IP Commands ========== */

/**
 * AT+CIPMUX - Enable/disable multiple connections (0=single, 1=multiple)
 */
int AT_CIPMUX_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                      char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query current mux mode
        snprintf(response, max_len, "+CIPMUX:%d", state->mux_enabled);
        return 0;
    } else if (param_count == 1) {
        // Set mux mode
        long mode = ESP8266_ParseInt(params[0]);
        printf("[ESP] Setting CIPMUX to %ld\n", mode);
        if (mode < 0 || mode > 1) return -1;

        // Cannot change mode if any connections are active
        for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
            if (state->connections[i].active) {
                return -1;  // ERROR: connections active
            }
        }

        state->mux_enabled = (uint8_t)mode;
        response[0] = '\0';
        return 0;
    }

    return -1;
}

/**
 * AT+CIPSTART - Start TCP/UDP connection
 * Format: AT+CIPSTART=<type>,<remote_ip>,<remote_port>[,<local_port>]
 * Or with mux: AT+CIPSTART=<link_id>,<type>,<remote_ip>,<remote_port>[,<local_port>]
 */
int AT_CIPSTART_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                        char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;
    int link_id = 0;
    int param_offset = 0;

    // If mux enabled, first parameter is link_id
    if (state->mux_enabled) {
        if (param_count < 4) return -1;
        link_id = ESP8266_ParseInt(params[0]);
        if (link_id < 0 || link_id >= ESP8266_MAX_CONNECTIONS) return -1;
        param_offset = 1;
    } else {
        if (param_count < 3) return -1;
        link_id = 0;  // Single connection mode always uses link 0
    }

    // Parse connection type
    const char *type_str = params[param_offset];
    Connection_Type type;
    if (ESP8266_StrCmp(type_str, "\"TCP\"") == 0 || ESP8266_StrCmp(type_str, "TCP") == 0) {
        type = CONNECTION_TYPE_TCP;
    } else if (ESP8266_StrCmp(type_str, "\"UDP\"") == 0 || ESP8266_StrCmp(type_str, "UDP") == 0) {
        type = CONNECTION_TYPE_UDP;
    } else if (ESP8266_StrCmp(type_str, "\"SSL\"") == 0 || ESP8266_StrCmp(type_str, "SSL") == 0) {
        type = CONNECTION_TYPE_SSL;
    } else {
        return -1;
    }

    // Parse remote IP/domain (strip quotes if present)
    const char *remote_ip = params[param_offset + 1];
    char ip_buf[ESP8266_MAX_DOMAIN_LEN];
    if (remote_ip[0] == '"') {
        int len = strlen(remote_ip);
        if (len < 2 || remote_ip[len-1] != '"') return -1;
        strncpy(ip_buf, remote_ip + 1, sizeof(ip_buf) - 1);
        ip_buf[len - 2] = '\0';
        remote_ip = ip_buf;
    }

    // Parse remote port
    long remote_port = ESP8266_ParseInt(params[param_offset + 2]);
    if (remote_port < 0 || remote_port > 65535) return -1;

    // Connect
    int result = ESP8266_SocketConnect(esp, remote_ip, (uint16_t)remote_port, type);
    if (result < 0) {
        snprintf(response, max_len, "ERROR");
        return -1;
    }

    // Success - for UDP, connection is immediate
    // For TCP/SSL, connection is asynchronous and CONNECT message will be sent by poll loop
    if (type == CONNECTION_TYPE_UDP) {
        if (state->mux_enabled) {
            snprintf(response, max_len, "%d,CONNECT", link_id);
        } else {
            snprintf(response, max_len, "CONNECT");
        }
    } else {
        // TCP/SSL: empty response, framework will add OK, CONNECT message will come later
        response[0] = '\0';
    }

    return 0;
}

/**
 * AT+CIPDOMAIN - DNS lookup
 * Format: AT+CIPDOMAIN="<domain>"
 * Returns: +CIPDOMAIN:<ip_address>
 */
int AT_CIPDOMAIN_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                         char *response, size_t max_len) {
    if (!esp || !response || param_count < 1) return -1;
    (void)command_name;

    // Parse domain name (strip quotes if present)
    const char *domain = params[0];
    char domain_buf[ESP8266_MAX_DOMAIN_LEN];
    if (domain[0] == '"') {
        int len = strlen(domain);
        if (len < 2 || domain[len-1] != '"') return -1;
        strncpy(domain_buf, domain + 1, sizeof(domain_buf) - 1);
        domain_buf[len - 2] = '\0';
        domain = domain_buf;
    }

    // Perform DNS lookup using gethostbyname
    struct hostent *host = gethostbyname(domain);
    if (!host || !host->h_addr_list[0]) {
        return -1;
    }

    // Convert IP address to string
    struct in_addr addr;
    memcpy(&addr, host->h_addr_list[0], sizeof(struct in_addr));
    const char *ip_str = inet_ntoa(addr);
    if (!ip_str) {
        return -1;
    }

    // Format response: +CIPDOMAIN:<ip>
    snprintf(response, max_len, "+CIPDOMAIN:%s", ip_str);
    return 0;
}

/**
 * AT+CIPSEND - Send data on a connection
 * Format: AT+CIPSEND=<length> (single connection)
 * Or: AT+CIPSEND=<link_id>,<length> (multiple connections)
 * This initiates send mode - we return ">" prompt and wait for data
 */
int AT_CIPSEND_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                       char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;
    int link_id = 0;
    int length = 0;

    if (state->mux_enabled) {
        if (param_count < 2) return -1;
        link_id = ESP8266_ParseInt(params[0]);
        if (link_id < 0 || link_id >= ESP8266_MAX_CONNECTIONS) return -1;
        length = ESP8266_ParseInt(params[1]);
    } else {
        if (param_count < 1) return -1;
        link_id = 0;
        length = ESP8266_ParseInt(params[0]);
    }

    if (length <= 0 || length > 2048) return -1;

    // Check if connection is active
    if (!state->connections[link_id].active) {
        snprintf(response, max_len, "link is not valid");
        return -1;
    }

    // Enter data collection mode
    state->send_mode = 1;
    state->send_link_id = link_id;
    state->send_bytes_expected = length;
    state->send_bytes_collected = 0;

    // Send ">" prompt to indicate ready for data
    snprintf(response, max_len, ">");

    return 0;
}

/**
 * AT+CIPCLOSE - Close a connection
 * Format: AT+CIPCLOSE (single connection)
 * Or: AT+CIPCLOSE=<link_id> (multiple connections)
 */
int AT_CIPCLOSE_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                        char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;
    int link_id = 0;

    if (param_count == 0) {
        // Execute form - close connection 0
        link_id = 0;
    } else if (param_count == 1) {
        // Close specific connection
        if (state->mux_enabled) {
            link_id = ESP8266_ParseInt(params[0]);
            if (link_id < 0 || link_id >= ESP8266_MAX_CONNECTIONS) return -1;
        } else {
            link_id = 0;
        }
    } else {
        return -1;
    }

    if (ESP8266_SocketClose(esp, link_id) < 0) {
        return -1;
    }

    if (state->mux_enabled) {
        snprintf(response, max_len, "%d,CLOSED", link_id);
    } else {
        snprintf(response, max_len, "CLOSED");
    }

    return 0;
}

/**
 * AT+CIPSTATUS - Get connection status
 */
int AT_CIPSTATUS_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                         char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)params;
    (void)command_name;
    (void)param_count;

    ESP8266_Internal *state = &esp->state;

    // Build status response
    char *p = response;
    size_t remaining = max_len;
    int written;

    // Status line
    const char *status_str;
    if (state->wifi_state == WIFI_STATE_GOT_IP) {
        status_str = "STATUS:3";  // 3 = connected
    } else {
        status_str = "STATUS:5";  // 5 = WiFi disconnected
    }

    written = snprintf(p, remaining, "%s\n", status_str);
    p += written;
    remaining -= written;

    // List active connections
    for (int i = 0; i < ESP8266_MAX_CONNECTIONS; i++) {
        Connection *conn = &state->connections[i];
        if (conn->active && remaining > 0) {
            const char *type_str;
            if (conn->type == CONNECTION_TYPE_TCP) type_str = "TCP";
            else if (conn->type == CONNECTION_TYPE_UDP) type_str = "UDP";
            else type_str = "SSL";

            written = snprintf(p, remaining, "+CIPSTATUS:%d,\"%s\",\"%s\",%u,%u,%d\n",
                             i, type_str, conn->remote_ip, conn->remote_port,
                             conn->local_port, 0);  // 0 = client mode
            if (written > 0 && (size_t)written < remaining) {
                p += written;
                remaining -= written;
            }
        }
    }

    return 0;
}

/**
 * AT+CIPSERVER - Start/stop TCP server
 * Format: AT+CIPSERVER=<mode>[,<port>]
 * mode: 1=create server, 0=delete server
 */
int AT_CIPSERVER_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                         char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;
    (void)max_len;

    ESP8266_Internal *state = &esp->state;

    if (param_count < 1) return -1;

    long mode = ESP8266_ParseInt(params[0]);
    if (mode < 0 || mode > 1) return -1;

    if (mode == 1) {
        // Create server
        uint16_t port = 333;  // Default port
        if (param_count >= 2) {
            long p = ESP8266_ParseInt(params[1]);
            if (p < 0 || p > 65535) return -1;
            port = (uint16_t)p;
        }

        // Server mode requires mux enabled
        if (!state->mux_enabled) {
            return -1;
        }

        // TODO: Create listening socket
        // For now, just mark server as active
        state->server_active = 1;
        state->server_port = port;
        state->server_socket = -1;  // Placeholder

        response[0] = '\0';
        return 0;
    } else {
        // Delete server
        if (state->server_socket >= 0) {
            // TODO: Close listening socket
        }
        state->server_active = 0;
        state->server_socket = -1;

        response[0] = '\0';
        return 0;
    }
}

/**
 * AT+CIPSSLSIZE - Set SSL buffer size
 * Format: AT+CIPSSLSIZE=<size>
 * size: 2048 ~ 4096
 */
int AT_CIPSSLSIZE_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                          char *response, size_t max_len) {
    if (!esp || !response) return -1;
    (void)command_name;

    ESP8266_Internal *state = &esp->state;

    if (param_count == 0) {
        // Query current SSL buffer size
        snprintf(response, max_len, "+CIPSSLSIZE:%d", state->ssl_buffer_size);
        return 0;
    } else if (param_count == 1) {
        // Set SSL buffer size
        long size = ESP8266_ParseInt(params[0]);
        if (size < 2048 || size > 4096) return -1;

        state->ssl_buffer_size = (uint16_t)size;
        response[0] = '\0';
        return 0;
    }

    return -1;
}

/* ========== Helper Functions ========== */

long ESP8266_ParseInt(const char *str) {
    if (!str) return -1;
    char *endptr;
    long value = strtol(str, &endptr, 10);
    if (*endptr != '\0') return -1;
    return value;
}

int ESP8266_ParseMAC(const char *str) {
    if (!str) return -1;
    // Format: xx:xx:xx:xx:xx:xx
    int a, b, c, d, e, f;
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f) != 6) {
        return -1;
    }
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 ||
        d < 0 || d > 255 || e < 0 || e > 255 || f < 0 || f > 255) {
        return -1;
    }
    return 0;
}

int ESP8266_ParseIP(const char *str) {
    if (!str) return -1;
    // Format: 192.168.1.1
    int a, b, c, d;
    if (sscanf(str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        return -1;
    }
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        return -1;
    }
    return 0;
}

int ESP8266_StrCmp(const char *s1, const char *s2) {
    // Simple string compare for now; can enhance with unquoting later
    if (!s1 || !s2) return 1;
    return strcmp(s1, s2);
}

void ESP8266_ATCommandsInit(void) {
    // Initialize command table if needed (could register handlers dynamically)
}
