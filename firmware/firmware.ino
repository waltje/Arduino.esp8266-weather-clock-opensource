/*
 * TJ-56-654 Weather Clock - Custom NTP Firmware with OTA v1.9.3
 *
 * Hardware:
 * - ESP-01S (ESP8266)
 * - GM009605v4.3 OLED 128x64 display (SSD1306 I2C)
 *
 * Connections:
 * - GPIO0 -> OLED SDA (I2C Data) - SWAPPED!
 * - GPIO2 -> OLED SCL (I2C Clock) - SWAPPED!
 *
 * Features:
 * - Async NTP time sync
 * - Async weather from Open-Meteo API
 * - OTA updates (web + ArduinoOTA)
 * - WiFi resilience with exponential backoff
 * - "Thanos snap" dissolve transition effect
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFiManager.h>

#include "config.h"
#include "globals.h"

// ============ Global variable definitions ============

// Configuration
Config config;

// OLED Display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// NTP Client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// Web server
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// State machines
WeatherState weatherState = WEATHER_IDLE;
NTPState ntpState = NTP_IDLE;
WiFiConnectionState wifiConnState = WIFI_CONN_IDLE;

// Retry configurations
RetryConfig ntpRetry;
RetryConfig weatherRetry;
WiFiRetryConfig wifiRetry;

// NTP packet buffer and timing
byte ntpPacketBuffer[48];
unsigned long ntpRequestTime = 0;

// Independent epoch tracking
unsigned long syncedEpoch = 0;
unsigned long syncedMillis = 0;
bool timeIsSynced = false;

// WiFi connection timing
unsigned long wifiConnectStart = 0;

// Display state
bool colonBlink = false;
unsigned long lastBlinkTime = 0;
unsigned long lastNTPUpdate = 0;
unsigned long ipDisplayUntil = 0;

// Debug variables
String lastError = "";
int ntpAttempts = 0;
int ntpSuccesses = 0;
bool internetConnected = false;

// Weather and sun data
WeatherData weather;
SunTimes sunTimes;

// Display rotation state
uint8_t displayMode = 0;
unsigned long lastModeSwitch = 0;
unsigned long lastWeatherUpdate = 0;

// Dissolve transition state
bool inTransition = false;
unsigned long transitionStart = 0;
unsigned long lastDissolveFrame = 0;
uint8_t nextDisplayMode = 0;

// ============ Helper functions ============

void ICACHE_FLASH_ATTR safeStringCopy(const String& src, char* dest, size_t maxLen) {
  if (src.length() >= maxLen) {
    Serial.printf("WARNING: String truncated from %d to %d chars\n", src.length(), maxLen - 1);
  }
  src.toCharArray(dest, maxLen);
  dest[maxLen - 1] = '\0';
}

// ============ EEPROM functions ============

void ICACHE_FLASH_ATTR loadConfig() {
  EEPROM.begin(512);

  Config tempConfig;
  EEPROM.get(0, tempConfig);

  if (tempConfig.magic == CONFIG_MAGIC) {
    config = tempConfig;
    Serial.println("Valid configuration loaded from EEPROM");
  } else {
    Serial.println("Invalid EEPROM data, using defaults");
    config.magic = CONFIG_MAGIC;
    saveConfig();
  }

  EEPROM.end();

  Serial.println("Configuration:");
  Serial.printf("  Magic: 0x%08X %s\n", config.magic, config.magic == CONFIG_MAGIC ? "OK" : "INVALID");
  Serial.printf("  SSID: %s\n", config.ssid);
  Serial.printf("  Timezone: %ld\n", config.timezone_offset);
  Serial.printf("  Hostname: %s\n", config.hostname);
}

void ICACHE_FLASH_ATTR saveConfig() {
  EEPROM.begin(512);
  EEPROM.put(0, config);
  EEPROM.commit();
  EEPROM.end();

  Serial.println("Configuration saved!");
}

// ============ OTA setup ============

void ICACHE_FLASH_ATTR setupOTA() {
  ArduinoOTA.setHostname(config.hostname);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start OTA updating " + type);
    clearDisplay();
    showNumber(0, false);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update complete!");
    showNumber(100, false);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percent = (progress / (total / 100));
    Serial.printf("Progress: %u%%\r", percent);
    showNumber(percent, false);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

// ============ Setup ============

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\nTJ-56-654 NTP Clock with OTA v" FIRMWARE_VERSION);
  Serial.println("==========================================");
  Serial.println("Display: GM009605v4.3 OLED 128x64 (SSD1306 I2C)");

  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize OLED display
  Serial.print("Initializing OLED at 0x");
  Serial.println(OLED_ADDRESS, HEX);

  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED initialization FAILED!");
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("OLED not found at 0x3C or 0x3D!");
    } else {
      Serial.println("OLED found at 0x3D");
    }
  } else {
    Serial.println("OLED initialized successfully!");
  }

  // Set display rotation
  display.setRotation(config.display_orientation);
  Serial.printf("Display rotation: %d (180 deg)\n", config.display_orientation);

  // Show startup animation
  Serial.println("Showing startup animation...");
  showStartupAnimation();

  // Load configuration
  loadConfig();

  // Setup WiFi
  setupWiFi();

  // Setup OTA
  setupOTA();

  // Setup web server
  setupWebServer();

  // Setup NTP
  timeClient = NTPClient(ntpUDP, config.ntp_server, 0, config.ntp_interval * 1000);
  timeClient.begin();

  // Test internet connectivity
  testInternetConnectivity();

  // Initialize timing to prevent immediate flicker/transition
  lastBlinkTime = millis();
  lastModeSwitch = millis();
  colonBlink = true;

  Serial.println("Setup complete!");
}

// ============ Loop ============

void loop() {
  // Handle OTA updates
  ArduinoOTA.handle();

  // Handle web server
  server.handleClient();
  MDNS.update();

  // WiFi reconnection logic
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 1000) {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED && wifiConnState == WIFI_CONN_CONNECTED) {
      Serial.println("WiFi disconnected!");
      wifiConnState = WIFI_CONN_FAILED;
      internetConnected = false;
      wifiRetry.reset();
      wifiRetry.scheduleRetry();
    }

    if (wifiConnState == WIFI_CONN_FAILED && wifiRetry.isRetryTime()) {
      Serial.printf("WiFi retry attempt (backoff level %d)...\n", wifiRetry.currentRetry);

      if (wifiRetry.currentRetry >= 5 && WiFi.getMode() != WIFI_AP_STA) {
        Serial.println("Enabling fallback AP (dual mode)");
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("TJ56654-Setup", "12345678");
        Serial.print("Fallback AP IP: ");
        Serial.println(WiFi.softAPIP());
      }

      if (strlen(config.password) > 0) {
        WiFi.begin(config.ssid, config.password);
      } else {
        WiFi.begin();
      }
      wifiConnState = WIFI_CONN_CONNECTING;
      wifiConnectStart = millis();
    }
  }

  // Process async WiFi reconnection
  processWiFiConnection();

  // Process async NTP response
  processNTPResponse();

  // Check for NTP retry
  if (ntpRetry.isRetryTime() && ntpState == NTP_IDLE) {
    Serial.println("NTP retry time reached, attempting retry...");
    sendNTPRequestAsync();
  }

  // Trigger async NTP update periodically
  unsigned long ntpInterval = config.ntp_interval * 1000UL;
  if (millis() - lastNTPUpdate > ntpInterval || lastNTPUpdate == 0) {
    if (ntpState == NTP_IDLE && !ntpRetry.isRetryTime()) {
      sendNTPRequestAsync();
      lastNTPUpdate = millis();
    }

    if (timeIsSynced || timeClient.isTimeSet()) {
      calculateSunTimes();
    }
  }

  // Check for weather retry
  if (weatherRetry.isRetryTime() && weatherState == WEATHER_IDLE) {
    Serial.println("Weather retry time reached, attempting retry...");
    fetchWeatherAsync();
  }

  // Update weather periodically
  if (config.weather_enabled && millis() > 10000) {
    unsigned long weatherInterval = config.weather_interval * 1000UL;
    if (millis() - lastWeatherUpdate > weatherInterval || lastWeatherUpdate == 0) {
      if (timeClient.isTimeSet() && weatherState == WEATHER_IDLE && !weatherRetry.isRetryTime()) {
        fetchWeatherAsync();
        lastWeatherUpdate = millis();
      }
    }
  }

  // Check if IP display should be cleared
  if (ipDisplayUntil > 0 && millis() >= ipDisplayUntil) {
    clearDisplay();
    ipDisplayUntil = 0;
  }

  // Update display with rotation
  updateDisplayRotation();

  // Blink colon every second
  if (millis() - lastBlinkTime > 500) {
    colonBlink = !colonBlink;
    lastBlinkTime = millis();
  }
}
