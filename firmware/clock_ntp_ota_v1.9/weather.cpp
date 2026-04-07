/*
 * weather.cpp - Weather API functions
 * TJ-56-654 Weather Clock v1.9.3
 */

// Include AsyncHTTPRequest BEFORE globals.h to provide full type definition
#include <ESPAsyncTCP.h>
#define ASYNCHTTPREQUEST_GENERIC_VERSION_MIN_TARGET "AsyncHTTPRequest_Generic v1.13.0"
#define ASYNCHTTPREQUEST_GENERIC_VERSION_MIN 1013000
#include <AsyncHTTPRequest_Generic.h>

#include "globals.h"
#include <ArduinoJson.h>

// Async HTTP client for weather (local to this file)
static AsyncHTTPRequest weatherRequest;

// Weather response callback
void ICACHE_FLASH_ATTR onWeatherResponse(void* optParm, AsyncHTTPRequest* request, int readyState) {
  (void)optParm;  // Unused

  if (readyState == 4) {  // Request complete
    weatherState = WEATHER_IDLE;

    int httpCode = request->responseHTTPcode();
    if (httpCode == 200) {
      String payload = request->responseText();
      Serial.printf("Weather response: %d bytes\n", payload.length());

      // Parse JSON response
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        // Extract current weather
        JsonObject current = doc["current_weather"];
        weather.temperature = current["temperature"] | 0.0f;
        weather.weathercode = current["weathercode"] | -1;
        weather.windspeed = current["windspeed"] | 0.0f;
        weather.lastUpdate = millis();
        weather.valid = true;

        // Extract sunrise/sunset
        JsonArray daily_sunrise = doc["daily"]["sunrise"];
        JsonArray daily_sunset = doc["daily"]["sunset"];

        if (daily_sunrise.size() > 0 && daily_sunset.size() > 0) {
          const char* sunrise_str = daily_sunrise[0];
          const char* sunset_str = daily_sunset[0];

          // Parse ISO time (2026-01-02T07:52) -> HH:MM
          if (sunrise_str && strlen(sunrise_str) >= 16) {
            sunTimes.sunrise[0] = sunrise_str[11];
            sunTimes.sunrise[1] = sunrise_str[12];
            sunTimes.sunrise[2] = ':';
            sunTimes.sunrise[3] = sunrise_str[14];
            sunTimes.sunrise[4] = sunrise_str[15];
            sunTimes.sunrise[5] = '\0';

            sunTimes.sunriseMinutes = (sunrise_str[11] - '0') * 600 +
                                      (sunrise_str[12] - '0') * 60 +
                                      (sunrise_str[14] - '0') * 10 +
                                      (sunrise_str[15] - '0');
          }

          if (sunset_str && strlen(sunset_str) >= 16) {
            sunTimes.sunset[0] = sunset_str[11];
            sunTimes.sunset[1] = sunset_str[12];
            sunTimes.sunset[2] = ':';
            sunTimes.sunset[3] = sunset_str[14];
            sunTimes.sunset[4] = sunset_str[15];
            sunTimes.sunset[5] = '\0';

            sunTimes.sunsetMinutes = (sunset_str[11] - '0') * 600 +
                                     (sunset_str[12] - '0') * 60 +
                                     (sunset_str[14] - '0') * 10 +
                                     (sunset_str[15] - '0');
          }

          // Update lastDay
          time_t epochTime = timeClient.getEpochTime();
          struct tm *ptm = gmtime(&epochTime);
          sunTimes.lastDay = ptm->tm_yday;
        }

        weatherState = WEATHER_SUCCESS;
        weatherRetry.reset();
        Serial.printf("Weather: %.1f C, code %d, wind %.1f km/h\n",
                      weather.temperature, weather.weathercode, weather.windspeed);
      } else {
        weatherState = WEATHER_FAILED;
        weather.valid = false;
        lastError = String("JSON: ") + error.c_str();
        Serial.printf("JSON parse error (attempt %d/%d): %s\n",
                      weatherRetry.currentRetry + 1, weatherRetry.maxRetries, error.c_str());

        weatherRetry.scheduleRetry();
        if (weatherRetry.maxRetriesReached()) {
          Serial.println("Weather max retries reached");
        } else {
          unsigned long backoff = weatherRetry.getBackoffDelay() / 1000;
          Serial.printf("  Retry scheduled in %lu seconds\n", backoff);
        }
      }

      doc.clear();
    } else {
      weatherState = WEATHER_FAILED;
      weather.valid = false;
      lastError = "Weather API: " + String(httpCode);
      Serial.printf("HTTP error %d (attempt %d/%d)\n",
                    httpCode, weatherRetry.currentRetry + 1, weatherRetry.maxRetries);

      weatherRetry.scheduleRetry();
      if (weatherRetry.maxRetriesReached()) {
        Serial.println("Weather max retries reached");
      } else {
        unsigned long backoff = weatherRetry.getBackoffDelay() / 1000;
        Serial.printf("  Retry scheduled in %lu seconds\n", backoff);
      }
    }
  }
}

// Async weather fetch - non-blocking!
void ICACHE_FLASH_ATTR fetchWeatherAsync() {
  if (!config.weather_enabled) {
    Serial.println(F("Weather disabled"));
    return;
  }

  if (weatherState != WEATHER_IDLE) {
    Serial.println(F("Weather request already in progress"));
    return;
  }

  // Build URL
  String url = "http://api.open-meteo.com/v1/forecast?";
  url += "latitude=" + String(config.latitude, 2);
  url += "&longitude=" + String(config.longitude, 2);
  url += "&current_weather=true";
  url += "&daily=sunrise,sunset";
  url += "&timezone=auto";
  url += "&forecast_days=1";

  Serial.println(F("Fetching weather (async)..."));

  if (weatherRequest.open("GET", url.c_str())) {
    weatherRequest.onReadyStateChange(onWeatherResponse);
    weatherRequest.setTimeout(10);  // 10 seconds
    weatherRequest.send();
    weatherState = WEATHER_REQUESTING;
    Serial.println("Weather request sent (non-blocking)");
  } else {
    weatherState = WEATHER_FAILED;
    Serial.println("Failed to open weather request");
  }
}

// Calculate sun times (placeholder - data comes from API)
void ICACHE_FLASH_ATTR calculateSunTimes() {
  if (!config.show_sunrise_sunset) return;

  if (sunTimes.lastDay != -1) {
    Serial.println(F("Sun times already available from API"));
    return;
  }

  Serial.println(F("Sun times not available yet - will be fetched with weather"));
}
