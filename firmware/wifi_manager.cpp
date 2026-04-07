/*
 * wifi_manager.cpp - WiFi connection management
 * TJ-56-654 Weather Clock v1.9.3
 */

#include "globals.h"
#include <WiFiManager.h>

// Async WiFi - Process connection (call in loop)
void ICACHE_FLASH_ATTR processWiFiConnection() {
  if (wifiConnState != WIFI_CONN_CONNECTING) return;

  // Check connection status
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnState = WIFI_CONN_CONNECTED;
    wifiRetry.reset();
    internetConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    // Disable fallback AP if it was enabled
    if (WiFi.getMode() == WIFI_AP_STA) {
      Serial.println("Disabling fallback AP (back to STA mode)");
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
    }

    // Sync connected SSID to config
    if (strlen(config.ssid) == 0) {
      safeStringCopy(WiFi.SSID(), config.ssid, sizeof(config.ssid));
      saveConfig();
    }

    showIP();
    return;
  }

  // Check timeout for this attempt
  if (millis() - wifiConnectStart > WIFI_TIMEOUT_MS) {
    wifiRetry.scheduleRetry();
    unsigned long nextRetryMs = wifiRetry.getBackoffDelay();

    Serial.printf("\nWiFi connection failed. Retry in %lu seconds\n", nextRetryMs / 1000);

    wifiConnState = WIFI_CONN_FAILED;
    internetConnected = false;

    showNoWiFi(nextRetryMs / 1000);
    return;
  }

  // Still connecting
  static unsigned long lastDot = 0;
  if (millis() - lastDot > 500) {
    Serial.print(".");
    lastDot = millis();
  }
}

// WiFi setup (SYNCHRONOUS in setup(), async reconnect in loop())
void ICACHE_FLASH_ATTR setupWiFi() {
  Serial.println("WiFi Setup - Synchronous for initial connection");

  WiFi.hostname(config.hostname);
  WiFi.mode(WIFI_STA);

  // Try 1: Use WiFi.begin() without params - uses SDK stored credentials
  Serial.println("Trying SDK-stored credentials...");
  WiFi.begin();

  // SYNCHRONOUS wait for connection (max 10 seconds)
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    showWiFiConnecting(attempts);
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());

    safeStringCopy(WiFi.SSID(), config.ssid, sizeof(config.ssid));
    saveConfig();

    showIP();
    wifiConnState = WIFI_CONN_CONNECTED;
    return;
  }

  // Try 2: If we have EEPROM credentials, try those
  if (strlen(config.ssid) > 0 && strlen(config.password) > 0) {
    Serial.println("\nTrying EEPROM credentials...");
    WiFi.begin(config.ssid, config.password);

    attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      showWiFiConnecting(attempts);
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected via EEPROM credentials!");
      showIP();
      wifiConnState = WIFI_CONN_CONNECTED;
      return;
    }
  }

  // No stored credentials - use WiFiManager
  if (strlen(config.ssid) == 0) {
    Serial.println("\nNo saved credentials, using WiFiManager...");
    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(180);

    // Show AP mode indicator
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 15);
    display.print("Setup Mode");
    display.setTextSize(1);
    display.setCursor(10, 35);
    display.print("Connect to WiFi:");
    display.setCursor(10, 48);
    display.print("TJ56654-Setup");
    display.display();

    Serial.println("Attempting WiFiManager auto-connect...");
    if (!wifiManager.autoConnect("TJ56654-Setup", "12345678")) {
      Serial.println("WiFi connection failed. Starting fallback AP...");
      WiFi.mode(WIFI_AP);
      WiFi.softAP("TJ56654-Clock", "12345678");
      Serial.print("Fallback AP IP: ");
      Serial.println(WiFi.softAPIP());
      wifiConnState = WIFI_CONN_CONNECTED;
      return;
    }

    Serial.println("WiFi connected via WiFiManager!");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    safeStringCopy(WiFi.SSID(), config.ssid, sizeof(config.ssid));
    saveConfig();

    showIP();
    wifiConnState = WIFI_CONN_CONNECTED;
    return;
  }

  // We have credentials but WiFi is not available
  Serial.println("\nWiFi not available. Will retry in background.");
  wifiConnState = WIFI_CONN_FAILED;
  wifiRetry.scheduleRetry();
  showNoWiFi(wifiRetry.getBackoffDelay() / 1000);
}
