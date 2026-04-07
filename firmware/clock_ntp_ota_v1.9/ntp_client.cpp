/*
 * ntp_client.cpp - NTP client and time functions
 * TJ-56-654 Weather Clock v1.9.3
 */

#include "globals.h"

// DST calculation for European rules
// DST starts: last Sunday of March at 01:00 UTC
// DST ends: last Sunday of October at 01:00 UTC
bool ICACHE_FLASH_ATTR isDST(unsigned long epochTime) {
  if (!config.dst_enabled) return false;

  time_t t = epochTime;
  struct tm *timeinfo = gmtime(&t);

  int month = timeinfo->tm_mon + 1; // 1-12
  int day = timeinfo->tm_mday;      // 1-31
#if 0 //UNUSED
  int weekday = timeinfo->tm_wday;  // 0=Sunday
#endif
  int hour = timeinfo->tm_hour;

  // Not DST: November - February
  if (month < 3 || month > 10) return false;

  // Always DST: April - September
  if (month > 3 && month < 10) return true;

  // March: DST starts last Sunday at 01:00 UTC
  if (month == 3) {
    int lastSunday = 31 - ((5 + timeinfo->tm_year) % 7);
    if (day < lastSunday) return false;
    if (day > lastSunday) return true;
    if (hour < 1) return false;
    return true;
  }

  // October: DST ends last Sunday at 01:00 UTC
  if (month == 10) {
    int lastSunday = 31 - ((1 + timeinfo->tm_year) % 7);
    if (day < lastSunday) return true;
    if (day > lastSunday) return false;
    if (hour < 1) return true;
    return false;
  }

  return false;
}

// Get total timezone offset including DST
long ICACHE_FLASH_ATTR getTotalOffset(unsigned long epochTime) {
  long offset = config.timezone_offset;
  if (isDST(epochTime)) {
    offset += 3600; // Add 1 hour for DST
  }
  return offset;
}

// Get current epoch (async NTP independent tracking)
unsigned long ICACHE_FLASH_ATTR getAsyncEpoch() {
  if (!timeIsSynced) return timeClient.getEpochTime();
  unsigned long elapsed = (millis() - syncedMillis) / 1000;
  return syncedEpoch + elapsed;
}

// Test internet connectivity
void ICACHE_FLASH_ATTR testInternetConnectivity() {
  Serial.println("\n=== Testing Internet Connectivity ===");

  IPAddress ntpIP;
  if (WiFi.hostByName(config.ntp_server, ntpIP)) {
    Serial.print("DNS works: ");
    Serial.print(config.ntp_server);
    Serial.print(" -> ");
    Serial.println(ntpIP);
  } else {
    Serial.print("DNS failed: cannot resolve ");
    Serial.println(config.ntp_server);
    lastError = "DNS resolution failed";
    return;
  }

  if (WiFi.hostByName("google.com", ntpIP)) {
    Serial.println("Can resolve google.com");
    internetConnected = true;
  } else {
    Serial.println("Cannot resolve google.com - no internet?");
    lastError = "No internet connectivity";
    internetConnected = false;
  }
}

// Update NTP time (blocking)
void ICACHE_FLASH_ATTR updateNTPTime() {
  Serial.println("\n=== Updating NTP Time ===");
  ntpAttempts++;

  if (!internetConnected) {
    Serial.println("Skipping NTP update - no internet");
    lastError = "No internet connection";
    return;
  }

  bool success = timeClient.update();

  if (success) {
    ntpSuccesses++;
    Serial.print("NTP sync successful: ");
    Serial.println(timeClient.getFormattedTime());
    lastError = "";
  } else {
    Serial.println("NTP sync failed");
    lastError = "NTP sync failed (timeout or no response)";

    Serial.println("  Trying force update...");
    if (timeClient.forceUpdate()) {
      ntpSuccesses++;
      Serial.print("Force update successful: ");
      Serial.println(timeClient.getFormattedTime());
      lastError = "";
    } else {
      Serial.println("Force update also failed");
      testInternetConnectivity();
    }
  }

  Serial.print("NTP Stats: ");
  Serial.print(ntpSuccesses);
  Serial.print(" / ");
  Serial.print(ntpAttempts);
  Serial.println(" successful");
}

// Async NTP - Send request (non-blocking)
void ICACHE_FLASH_ATTR sendNTPRequestAsync() {
  if (ntpState != NTP_IDLE) return;

  if (!internetConnected) {
    Serial.println(F("Skip NTP - no internet"));
    return;
  }

  Serial.println(F("NTP request (async)..."));
  ntpAttempts++;

  memset(ntpPacketBuffer, 0, 48);
  ntpPacketBuffer[0] = 0b11100011;
  ntpPacketBuffer[1] = 0;
  ntpPacketBuffer[2] = 6;
  ntpPacketBuffer[3] = 0xEC;

  ntpUDP.beginPacket(config.ntp_server, 123);
  ntpUDP.write(ntpPacketBuffer, 48);
  ntpUDP.endPacket();

  ntpState = NTP_REQUEST_SENT;
  ntpRequestTime = millis();
  Serial.println("NTP sent (non-blocking)");
}

// Async NTP - Process response (call in loop)
void ICACHE_FLASH_ATTR processNTPResponse() {
  if (ntpState == NTP_IDLE) return;

  // Timeout check with exponential backoff retry
  if (millis() - ntpRequestTime > NTP_TIMEOUT_MS) {
    ntpState = NTP_IDLE;
    Serial.printf("NTP timeout (attempt %d/%d)\n", ntpRetry.currentRetry + 1, ntpRetry.maxRetries);

    ntpRetry.scheduleRetry();
    if (ntpRetry.maxRetriesReached()) {
      Serial.println("NTP max retries reached, will try again later");
      lastError = "NTP timeout - max retries";
    } else {
      unsigned long backoff = ntpRetry.getBackoffDelay() / 1000;
      Serial.printf("  Retry scheduled in %lu seconds\n", backoff);
    }
    return;
  }

  // Check for response packet
  if (ntpUDP.parsePacket() >= 48) {
    ntpUDP.read(ntpPacketBuffer, 48);

    unsigned long high = word(ntpPacketBuffer[40], ntpPacketBuffer[41]);
    unsigned long low = word(ntpPacketBuffer[42], ntpPacketBuffer[43]);
    unsigned long epoch = (high << 16 | low) - 2208988800UL;

    syncedEpoch = epoch;
    syncedMillis = millis();
    timeIsSynced = true;

    timeClient = NTPClient(ntpUDP, config.ntp_server, 0, config.ntp_interval * 1000);
    timeClient.begin();
    timeClient.update();

    ntpState = NTP_IDLE;
    ntpSuccesses++;
    ntpRetry.reset();
    lastError = "";

    unsigned long h = (epoch % 86400L) / 3600;
    unsigned long m = (epoch % 3600) / 60;
    Serial.printf("NTP synced (async): %02lu:%02lu UTC\n", h, m);
  }
}
