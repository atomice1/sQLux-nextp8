/*
 * ESP8266 AT Command Test Tool
 *
 * Standalone tool to test ESP8266 model with UART interface
 * Reads from stdin, sends to ESP8266, outputs ESP8266 responses to stdout
 *
 * Copyright (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

#include "esp8266_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>

static volatile int running = 1;

void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Set up signal handler for clean exit
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create ESP8266 instance
    ESP8266_t *esp = ESP8266_Create();
    if (!esp) {
        fprintf(stderr, "Failed to create ESP8266 instance\n");
        return 1;
    }

    // Set stdin to non-blocking mode
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // Save terminal settings
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

    fprintf(stderr, "ESP8266 Test Tool\n");
    fprintf(stderr, "Type AT commands and press Enter. Ctrl+C to exit.\n");
    fprintf(stderr, "---\n");

    while (running) {
        struct pollfd fds[1];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;

        // Poll with 10ms timeout
        int ret = poll(fds, 1, 10);
        
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            // Read available data from stdin
            char buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                // Send each byte to ESP8266
                for (ssize_t i = 0; i < n; i++) {
                    ESP8266_ProcessUARTByte(esp, (uint8_t)buf[i]);
                }
            } else if (n == 0) {
                // EOF
                running = 0;
            }
        }

        // Poll ESP8266 for network events
        ESP8266_Poll(esp);

        // Check for output from ESP8266
        int byte;
        while ((byte = ESP8266_GetUARTByte(esp)) >= 0) {
            putchar((char)byte);
            fflush(stdout);
        }
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);

    // Cleanup
    ESP8266_Destroy(esp);

    fprintf(stderr, "\nExiting...\n");
    return 0;
}
