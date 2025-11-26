#include "IMGWSynopSource.h"
#include "configuration.h"

IMGWSynopSource::IMGWSynopSource()
{
}

String IMGWSynopSource::getFetchUrl() const
{
    return "https://danepubliczne.imgw.pl/api/data/synop/station/poznan/format/csv";
}

unsigned long IMGWSynopSource::getFetchIntervalMs() const
{
    return 60 * 60 * 1000; // 1 hour
}

String IMGWSynopSource::fetchAndFormat(
    std::function<String(const char*, int&)> httpGetCallback)
{
    int httpCode = 0;
    String payload = httpGetCallback(getFetchUrl().c_str(), httpCode);

    if (httpCode != 200 || payload.length() == 0) {
        LOG_WARN("IMGWSynopSource: Failed to fetch data (HTTP code: %d)", httpCode);
        return "";
    }

    // CSV format has header on first line, data on second line
    // Find the second line (skip header)
    int newlinePos = payload.indexOf('\n');
    if (newlinePos < 0) {
        LOG_WARN("IMGWSynopSource: Invalid CSV format (no newline found)");
        return "";
    }

    String dataLine = payload.substring(newlinePos + 1);
    dataLine.trim();

    if (dataLine.length() == 0) {
        LOG_WARN("IMGWSynopSource: Empty data line");
        return "";
    }

    // Parse required fields
    String station = parseCSVField(dataLine, FIELD_STATION);
    String date = parseCSVField(dataLine, FIELD_DATE);
    String hour = parseCSVField(dataLine, FIELD_HOUR);
    String temp = parseCSVField(dataLine, FIELD_TEMP);
    String windSpeed = parseCSVField(dataLine, FIELD_WIND_SPEED);
    String windDir = parseCSVField(dataLine, FIELD_WIND_DIR);
    String precip = parseCSVField(dataLine, FIELD_PRECIP);
    String pressure = parseCSVField(dataLine, FIELD_PRESSURE);

    // Validate we got the essential fields
    if (station.length() == 0 || date.length() == 0 || temp.length() == 0) {
        LOG_WARN("IMGWSynopSource: Missing essential fields (station/date/temp)");
        return "";
    }

    // Convert wind direction from degrees to compass letters
    int windDegrees = windDir.toInt();
    const char* windCompass = azimuthToCompass(windDegrees);

    // Format hour with leading zero if needed
    if (hour.length() == 1) {
        hour = "0" + hour;
    }

    // Build message
    // "Pogoda: 📍Poznań, 2025-11-25 23:00, 🌡️temp.: 0.5°C, suma opadów: 0.1mm, ciśnienie: 1009.2hPa, 💨wiatr: N 4m/s"
    String message = "Pogoda: \xF0\x9F\x93\x8D"; // UTF-8 for 📍 (PIN emoji)
    message += station;
    message += ", ";

    // Temperature with thermometer icon
    message += "\xF0\x9F\x8C\xA1"; // UTF-8 for 🌡️ (thermometer)
    message += "temp.: ";
    message += temp;
    message += "\xC2\xB0" "C"; // UTF-8 for ° (degree symbol)

    // Precipitation
    message += ", suma opad\xC3\xB3w: "; // opadów with ó
    message += precip;
    message += "mm";

    // Pressure
    message += ", ci\xC5\x9Bnienie: "; // ciśnienie with ś
    message += pressure;
    message += " hPa";

    // Wind with wind icon
    message += ", \xF0\x9F\x92\xA8wiatr: "; // UTF-8 for 💨 (wind)
    message += windCompass;
    message += " ";
    message += windSpeed;
    message += "m/s";

    // Add km/h conversion in parentheses (1 m/s = 3.6 km/h)
    float windSpeedMs = windSpeed.toFloat();
    int windSpeedKmh = (int)(windSpeedMs * 3.6 + 0.5); // Round to nearest integer
    message += " (";
    message += String(windSpeedKmh);
    message += "km/h)";

    message += " (";
    message += date;
    message += " ";
    message += hour;
    message += ":00): ";

    LOG_INFO("IMGWSynopSource: Formatted weather: %s", message.c_str());

    return message;
}

const char* IMGWSynopSource::azimuthToCompass(int degrees)
{
    // Normalize to 0-359
    degrees = degrees % 360;
    if (degrees < 0) {
        degrees += 360;
    }

    // 16-point compass rose
    // Each sector is 22.5 degrees wide, centered on the cardinal/intercardinal direction
    // N is centered at 0°, so N covers 348.75° to 11.25°
    // Adding 11.25 and dividing by 22.5 gives us the sector index

    static const char* directions[] = {
        "N", "NNE", "NE", "ENE",
        "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW",
        "W", "WNW", "NW", "NNW"
    };

    int index = ((degrees * 10 + 1125) / 2250) % 16;
    return directions[index];
}

String IMGWSynopSource::parseCSVField(const String& line, int fieldIndex)
{
    int currentField = 0;
    int startPos = 0;

    for (int i = 0; i <= line.length(); i++) {
        if (i == line.length() || line.charAt(i) == ',') {
            if (currentField == fieldIndex) {
                return line.substring(startPos, i);
            }
            currentField++;
            startPos = i + 1;
        }
    }

    return "";
}

