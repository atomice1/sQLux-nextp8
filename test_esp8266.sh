#!/bin/bash
# ESP8266 Test Script
# Demonstrates AT command testing with the standalone tool

echo "=== ESP8266 AT Command Test ==="
echo

echo "Test 1: Basic AT commands"
cat << 'EOF' | timeout 2 ./build/esp8266_test 2>&1 | grep -v "^ESP8266" | grep -v "^Type" | grep -v "^---" | grep -v "Exiting"
AT
AT+GMR
AT+RST
EOF

echo
echo "Test 2: WiFi scan and connection"
cat << 'EOF' | timeout 3 ./build/esp8266_test 2>&1 | grep -v "^ESP8266" | grep -v "^Type" | grep -v "^---" | grep -v "Exiting"
AT+CWMODE_CUR=1
AT+CWLAP
AT+CWJAP_CUR="home-network","password123"
AT+CIFSR
EOF

echo
echo "Test 3: TCP connection (requires localhost server on port 8080)"
echo "Start server with: nc -l 8080"
cat << 'EOF' | timeout 5 ./build/esp8266_test 2>&1 | grep -v "^ESP8266" | grep -v "^Type" | grep -v "^---" | grep -v "Exiting"
AT+CIPMUX=1
AT+CIPSTART=0,"TCP","127.0.0.1",8080
AT+CIPSTATUS
AT+CIPSEND=0,13
Hello, World!
AT+CIPCLOSE=0
EOF

echo
echo "Test 4: SSL connection to real HTTPS server"
(echo "AT+CIPMUX=1"; \
 echo "AT+CIPSTART=0,\"SSL\",\"firefly.atomice.net\",443"; \
 sleep 2; \
 echo "AT+CIPSTATUS"; \
 echo "AT+CIPSEND=0,18"; \
 sleep 0.2; \
 echo -e "GET / HTTP/1.0\r\n\r\n"; \
 sleep 1; \
 echo "AT+CIPCLOSE=0") | \
 timeout 6 ./build/esp8266_test 2>&1 | \
 grep -v "^ESP8266" | grep -v "^Type" | grep -v "^---" | grep -v "Exiting" | head -20

echo
echo "=== Tests Complete ==="
