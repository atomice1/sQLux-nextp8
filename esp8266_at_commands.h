/*
 * ESP8266 AT Command Parser and Handlers
 *
 * Handles parsing of AT commands from UART input and dispatching
 * to appropriate command handlers. Generates responses.
 *
 * Copyright (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

#ifndef ESP8266_AT_COMMANDS_H
#define ESP8266_AT_COMMANDS_H

#include "esp8266_model.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== AT Command Handler Function Type ========== */

/**
 * AT command handler function signature
 *
 * @param esp         ESP8266 model instance
 * @param params      Array of parameter strings (already parsed and unquoted)
 * @param param_count Number of parameters
 * @param response    Output buffer for response message
 * @param max_len     Size of response buffer
 *
 * @return 0 on success, -1 on error
 *         Response should NOT include "OK" or "ERROR" - that's added by dispatcher
 */
typedef int (*ATCommandHandler)(ESP8266_t *esp,
                                 const char *command_name,
                                 const char **params,
                                 int param_count,
                                 char *response,
                                 size_t max_len);

/* ========== Parser API ========== */

/**
 * Process an incoming character for AT command line assembly
 * Handles line buffering, echo, backspace, line ending detection
 *
 * @param esp   ESP8266 model instance
 * @param byte  Character received from UART
 *
 * When a complete command line is detected (CR+LF), the command
 * is parsed and dispatched, with response queued to TX buffer.
 */
extern void ESP8266_ATProcessByte(ESP8266_t *esp, uint8_t byte);

/**
 * Dispatch and execute an AT command string
 * Called internally by ATProcessByte after line is complete
 *
 * @param esp     ESP8266 model instance
 * @param cmd_str Complete AT command string (e.g., "AT+CWMODE=1")
 */
extern void ESP8266_ATDispatch(ESP8266_t *esp, const char *cmd_str);

/**
 * Send a response string to the TX buffer
 * Automatically appends CRLF
 *
 * @param esp ESP8266 model instance
 * @param fmt Printf-style format string
 * @param ... Variable arguments
 */
extern void ESP8266_ATResponse(ESP8266_t *esp, const char *fmt, ...);

/**
 * Send an unsolicited message (e.g., WiFi CONNECTED)
 * Used for async state changes
 *
 * @param esp ESP8266 model instance
 * @param fmt Printf-style format string
 */
extern void ESP8266_ATUnsolicited(ESP8266_t *esp, const char *fmt, ...);

/**
 * Send an ERROR response
 */
extern void ESP8266_ATError(ESP8266_t *esp);

/**
 * Send an OK response
 */
extern void ESP8266_ATOKK(ESP8266_t *esp);

/* ========== Command Handlers (Phase 1) ========== */

/**
 * AT - Test AT startup
 * Response: OK
 */
extern int AT_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                      char *response, size_t max_len);

/**
 * AT+RST - Restart module
 * Response: OK (then reset happens internally)
 */
extern int AT_RST_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                          char *response, size_t max_len);

/**
 * AT+GMR - View version info
 * Response:
 *   AT version info
 *   SDK version info
 *   Build time
 *   OK
 */
extern int AT_GMR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                          char *response, size_t max_len);

/**
 * ATE0 - Disable echo
 * Response: OK
 */
extern int ATE0_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                       char *response, size_t max_len);

/**
 * ATE1 - Enable echo
 * Response: OK
 */
extern int ATE1_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                       char *response, size_t max_len);

/**
 * AT+UART_CUR - Current UART configuration (non-persistent)
 * Query:   AT+UART_CUR?
 * Response: +UART_CUR:<baud>,<databits>,<stopbits>,<parity>,<flow>
 *
 * Set:     AT+UART_CUR=<baud>,<databits>,<stopbits>,<parity>,<flow>
 * Response: OK
 *
 * Test:    AT+UART_CUR=?
 * Response: +UART_CUR:(baud ranges),(databits),(stopbits),(parity),(flow control)
 */
extern int AT_UART_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                               char *response, size_t max_len);

/* ========== Command Handlers (Phase 2 - WiFi) ========== */

/**
 * AT+CWMODE_CUR - Current WiFi mode (non-persistent)
 * Query:   AT+CWMODE_CUR?
 * Response: +CWMODE_CUR:<mode>
 * Set:     AT+CWMODE_CUR=<mode>  (1=Station, 2=SoftAP, 3=Dual)
 */
extern int AT_CWMODE_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                 char *response, size_t max_len);

/**
 * AT+CWJAP_CUR - Connect to AP (non-persistent)
 * Query:   AT+CWJAP_CUR?
 * Response: +CWJAP_CUR:<ssid>,<bssid>,<channel>,<rssi>
 * Set:     AT+CWJAP_CUR=<ssid>,<password>[,<bssid>]
 */
extern int AT_CWJAP_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                char *response, size_t max_len);

/**
 * AT+CWQAP - Disconnect from AP
 * Execute: AT+CWQAP
 */
extern int AT_CWQAP_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                            char *response, size_t max_len);

/**
 * AT+CWLAP - List available APs
 * Execute: AT+CWLAP
 * Response: +CWLAP:<ecn>,<ssid>,<rssi>,<mac>,<channel>,<freq_offset>,<freq_cal>
 * Set:     AT+CWLAP=<ssid>[,<mac>,<channel>]  (filter results)
 */
extern int AT_CWLAP_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                            char *response, size_t max_len);

/**
 * AT+CIFSR - Get local IP address
 * Execute: AT+CIFSR
 * Response: +CIFSR:STAIP,<station_ip>
 *           +CIFSR:STAMAC,<station_mac>
 */
extern int AT_CIFSR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                            char *response, size_t max_len);

/**
 * AT+CWSAP_CUR - Configure SoftAP (non-persistent)
 * Query:   AT+CWSAP_CUR?
 * Set:     AT+CWSAP_CUR=<ssid>,<pwd>,<chl>,<ecn>[,<max_conn>][,<ssid_hidden>]
 */
extern int AT_CWSAP_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                char *response, size_t max_len);

/**
 * AT+CWDHCP_CUR - DHCP configuration (non-persistent)
 * Query:   AT+CWDHCP_CUR?
 * Set:     AT+CWDHCP_CUR=<mode>,<en>  (mode: 0=SoftAP, 1=Station, 2=Both)
 */
extern int AT_CWDHCP_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                 char *response, size_t max_len);

/**
 * AT+CIPSTA_CUR - Station IP configuration (non-persistent)
 * Query:   AT+CIPSTA_CUR?
 * Set:     AT+CIPSTA_CUR=<ip>[,<gateway>,<netmask>]
 */
extern int AT_CIPSTA_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                 char *response, size_t max_len);

/**
 * AT+CIPSTAMAC_CUR - Station MAC address (non-persistent)
 * Query:   AT+CIPSTAMAC_CUR?
 * Set:     AT+CIPSTAMAC_CUR=<mac>
 */
extern int AT_CIPSTAMAC_CUR_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                    char *response, size_t max_len);

/* ========== Command Handlers (Phase 3 - TCP/IP) ========== */

/**
 * AT+CIPMUX - Enable/disable multiple connections
 * Query:   AT+CIPMUX?
 * Response: +CIPMUX:<mode>  (0=single connection, 1=multiple connections)
 * Set:     AT+CIPMUX=<mode>
 */
extern int AT_CIPMUX_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                             char *response, size_t max_len);

/**
 * AT+CIPSTART - Establish TCP/UDP/SSL connection
 * Single: AT+CIPSTART=<type>,<remote_ip>,<remote_port>[,<local_port>]
 * Multi:  AT+CIPSTART=<link_id>,<type>,<remote_ip>,<remote_port>[,<local_port>]
 * type: "TCP", "UDP", or "SSL"
 * Response: CONNECT or ERROR
 */
extern int AT_CIPSTART_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                               char *response, size_t max_len);

/**
 * AT+CIPDOMAIN - DNS lookup
 * Set: AT+CIPDOMAIN=<domain name>
 * Response: +CIPDOMAIN:<IP address>
 */
extern int AT_CIPDOMAIN_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                char *response, size_t max_len);

/**
 * AT+CIPSEND - Send data on connection
 * Single: AT+CIPSEND=<length>
 * Multi:  AT+CIPSEND=<link_id>,<length>
 * Response: > (then wait for data)
 */
extern int AT_CIPSEND_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                              char *response, size_t max_len);

/**
 * AT+CIPCLOSE - Close TCP/UDP/SSL connection
 * Single: AT+CIPCLOSE
 * Multi:  AT+CIPCLOSE=<link_id>
 * Response: CLOSED
 */
extern int AT_CIPCLOSE_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                               char *response, size_t max_len);

/**
 * AT+CIPSTATUS - Get connection status
 * Execute: AT+CIPSTATUS
 * Response: STATUS:<stat>
 *           +CIPSTATUS:<link_id>,<type>,<remote_ip>,<remote_port>,<local_port>,<tetype>
 */
extern int AT_CIPSTATUS_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                char *response, size_t max_len);

/**
 * AT+CIPSERVER - Configure TCP server
 * Set: AT+CIPSERVER=<mode>[,<port>]
 * mode: 1=create, 0=delete
 * Response: OK or ERROR
 */
extern int AT_CIPSERVER_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                char *response, size_t max_len);

/**
 * AT+CIPSSLSIZE - Set SSL buffer size
 * Set: AT+CIPSSLSIZE=<size>
 * Query: AT+CIPSSLSIZE?
 * Response: +CIPSSLSIZE:<size> (query), OK or ERROR
 * size: 2048 ~ 4096
 */
extern int AT_CIPSSLSIZE_Handler(ESP8266_t *esp, const char *command_name, const char **params, int param_count,
                                  char *response, size_t max_len);

/* ========== Helper Functions ========== */

/**
 * Parse a single parameter as unsigned integer
 * Returns -1 on parse error
 */
extern long ESP8266_ParseInt(const char *str);

/**
 * Parse a MAC address string (xx:xx:xx:xx:xx:xx format)
 * Returns 0 on success, -1 on invalid format
 */
extern int ESP8266_ParseMAC(const char *str);

/**
 * Parse an IP address string (dotted quad)
 * Returns 0 on success, -1 on invalid format
 */
extern int ESP8266_ParseIP(const char *str);

/**
 * Quote-aware string compare (compares unquoted values)
 * Handles escape sequences like \" and \\
 * Returns 0 if equal, else non-zero
 */
extern int ESP8266_StrCmp(const char *s1, const char *s2);

/**
 * Initialize AT command dispatch table
 * Called during ESP8266 creation
 */
extern void ESP8266_ATCommandsInit(void);

#ifdef __cplusplus
}
#endif

#endif  /* ESP8266_AT_COMMANDS_H */
