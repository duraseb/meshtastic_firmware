#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include "../DynamicSource.h"

/**
 * Dynamic source for IMGW SYNOP weather data.
 *
 * Fetches current weather observations from IMGW public API (CSV format)
 * and formats them into a human-readable message for mesh broadcast.
 *
 * API: https://danepubliczne.imgw.pl/api/data/synop/station/{station}/format/csv
 */
class IMGWSynopSource : public DynamicSource {
public:
    IMGWSynopSource();

    String getSourceId() const override { return "SYNOP"; }
    String getFetchUrl() const override;
    String getChannelName() const override { return "PoznanEvent"; }
    unsigned long getFetchIntervalMs() const override;

    String fetchAndFormat(
        std::function<String(const char*, int&)> httpGetCallback) override;

private:
    // CSV field indices (0-based, after header line)
    static constexpr int FIELD_STATION = 1;       // stacja (station name)
    static constexpr int FIELD_DATE = 2;          // data_pomiaru (YYYY-MM-DD)
    static constexpr int FIELD_HOUR = 3;          // godzina_pomiaru (0-23)
    static constexpr int FIELD_TEMP = 4;          // temperatura (°C)
    static constexpr int FIELD_WIND_SPEED = 5;    // predkosc_wiatru (m/s)
    static constexpr int FIELD_WIND_DIR = 6;      // kierunek_wiatru (degrees)
    static constexpr int FIELD_PRECIP = 8;        // suma_opadu (mm)
    static constexpr int FIELD_PRESSURE = 9;      // cisnienie (hPa)
    static constexpr int NUM_FIELDS = 10;

    // Wind direction conversion (azimuth to 16-point compass)
    static const char* azimuthToCompass(int degrees);

    // Parse a single CSV field by index from a comma-separated line
    static String parseCSVField(const String& line, int fieldIndex);
};


#endif // HAS_ALERTING
