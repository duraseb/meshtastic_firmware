#pragma once

#if defined(HAS_ALERTING) && HAS_ALERTING

#include <Arduino.h>
#include <time.h>

// Forward-declare to avoid pulling the full RTC header (and its transitive deps)
// into every translation unit that uses this utility.
extern int32_t getTZOffset();

namespace AlertsDateUtils {

/**
 * Convert a "YYYY-MM-DD HH:MM:SS" (or "YYYY-MM-DD HH:MM") UTC timestamp to a
 * local-time string in the same format, honouring the device's configured
 * timezone (config.device.tzdef, via getTZOffset()).
 *
 * If parsing fails or time is not yet synced (tz offset unknown), the input is
 * returned unchanged so callers never produce an empty/invalid timestamp.
 */
static inline String utcStringToLocal(const String &utcDt)
{
    if (utcDt.length() < 10) {
        return utcDt;
    }

    int y, mo, d, h = 0, mi = 0, s = 0;
    int parsed = sscanf(utcDt.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    if (parsed < 3) {
        return utcDt;
    }

    struct tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = s;
    tmv.tm_isdst = 0;

    // mktime() treats the struct as local time; our fields are UTC, so we need
    // to add the TZ offset to get the real UTC epoch.
    time_t localInterpEpoch = mktime(&tmv);
    if (localInterpEpoch == (time_t)-1) {
        return utcDt;
    }
    time_t utcEpoch = localInterpEpoch + getTZOffset();

    // Re-decompose using gmtime on (utcEpoch + offset) to get the local wall-clock
    // representation without relying on localtime's TZ handling.
    time_t displayEpoch = utcEpoch + getTZOffset();
    struct tm displayTm;
    if (gmtime_r(&displayEpoch, &displayTm) == nullptr) {
        return utcDt;
    }

    char buf[24];
    const char *fmt = (parsed >= 6) ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M";
    strftime(buf, sizeof(buf), fmt, &displayTm);
    return String(buf);
}

} // namespace AlertsDateUtils

#endif // HAS_ALERTING
