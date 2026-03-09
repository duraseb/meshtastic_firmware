#if defined(HAS_ALERTING) && HAS_ALERTING

#include "AiWeatherSource.h"
#include <time.h>
#include <RTC.h>
#include <ctype.h>
#ifdef ARCH_ESP32
#include <esp_task_wdt.h>
#endif

// Helper to reset watchdog timer (architecture-aware)
static inline void feedWatchdog()
{
#ifdef ARCH_ESP32
    esp_task_wdt_reset();
#endif
}

// Initialize AIService if not already done
extern AIService* aiService;

AiWeatherSource::AiWeatherSource()
{
    // Ensure AIService is initialized
    if (aiService == nullptr) {
        aiService = new AIService();
    }
}

AiWeatherSource::~AiWeatherSource()
{
}

unsigned long AiWeatherSource::getFetchIntervalMs() const
{
    return DEFAULT_FETCH_INTERVAL_MS; // 24 hours
}

String AiWeatherSource::fetchAndFormat(
    std::function<String(const char*, int&)> httpGetCallback)
{
    LOG_INFO("[AiWeatherSource] Starting AI weather forecast generation");

    // Check if we're within the allowed fetch window
    if (!isWithinFetchWindow()) {
        LOG_INFO("[AiWeatherSource] Too early in day (before %02d:00), skipping fetch", getMinHourOfDay());
        return "";
    }

    // Check if we have enough memory for AI processing
    // AI calls require ~16KB for JSON buffer + HTTP client overhead
    const size_t MIN_HEAP_FOR_AI = 40000; // 40KB minimum
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_AI) {
        LOG_WARN("[AiWeatherSource] Insufficient heap for AI processing (%d bytes free, need %d). Skipping...",
                 freeHeap, MIN_HEAP_FOR_AI);
        return "";
    }

    // Check if AI service is available
    if (aiService == nullptr) {
        LOG_ERROR("[AiWeatherSource] AIService not available");
        return "";
    }

    if (!aiService->hasConfiguredProviders()) {
        LOG_ERROR("[AiWeatherSource] No AI providers configured");
        return "";
    }

    // Fetch real weather data from Open-Meteo API
    String weatherApiUrl = "https://api.open-meteo.com/v1/forecast?latitude=52.4069&longitude=16.9299&hourly=temperature_2m,relative_humidity_2m,precipitation_probability,precipitation,surface_pressure,cloud_cover,visibility,wind_speed_10m,wind_direction_10m,soil_temperature_0cm&timezone=Europe%2FBerlin&forecast_days=1&forecast_hours=24&temporal_resolution=hourly_6&format=json&timeformat=unixtime";

    int httpCode = 0;
    feedWatchdog();
    String weatherJson = httpGetCallback(weatherApiUrl.c_str(), httpCode);
    feedWatchdog();

    if (httpCode != 200 || weatherJson.length() == 0) {
        LOG_ERROR("[AiWeatherSource] Failed to fetch weather data (HTTP %d)", httpCode);
        return "";
    }

    // Calculate tomorrow's date for Wikipedia lookup and prompt
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        LOG_WARN("[AiWeatherSource] Failed to get local time for date calculation");
        return ""; // Can't proceed without time
    }

    // Calculate tomorrow's date
    timeinfo.tm_mday += 1;
    mktime(&timeinfo); // Normalize the date

    // Polish month names for Wikipedia URL and date formatting
    const char* months[] = {
        "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
        "lipca", "sierpnia", "września", "października", "listopada", "grudnia"
    };

    // Format date as "DD miesiąc YYYY" (Polish format for display)
    String birthDate = String(timeinfo.tm_mday) + " " +
                         String(months[timeinfo.tm_mon]);
    String tomorrowDate = birthDate + " " +
                         String(timeinfo.tm_year + 1900);

    // Format date for API URL as "DD_miesiąca" (without year)
    String apiDate = String(timeinfo.tm_mday) + "_" + String(months[timeinfo.tm_mon]);

    LOG_DEBUG("[AiWeatherSource] Using tomorrow's date: %s (API format: %s)", birthDate.c_str(), apiDate.c_str());

    // Don't fetch Wikipedia content - let AI handle it
    String wikiContent = ""; // Not used anymore

    // Build the AI prompt with real weather data and formatted date
    String prompt = buildWeatherPrompt(weatherJson, tomorrowDate, birthDate, apiDate);
    if (prompt.length() == 0) {
        LOG_ERROR("[AiWeatherSource] Failed to build AI prompt");
        return "";
    }

    if (prompt.length() > 50000) {
        LOG_WARN("[AiWeatherSource] Prompt size is very large (%d bytes) - may cause API issues", prompt.length());
    }

    // Clear large strings to free memory after prompt is built
    weatherJson = String();

    // Try each AI provider until one succeeds (both HTTP call AND parsing)
    for (int providerIdx = 0; providerIdx < aiService->getMaxProviders(); providerIdx++) {
        feedWatchdog();
        AIService::AIProvider& provider = aiService->getProviders()[providerIdx];

        // Skip if provider not configured
        if (provider.endpoint.length() == 0 || provider.apiKey.length() == 0) {
            continue;
        }

        LOG_INFO("[AiWeatherSource] Attempting AI call with [%s]...", provider.name.c_str());
        feedWatchdog();

        String aiResponse;
        bool httpSuccess = aiService->callProvider(providerIdx, prompt, aiResponse);
        feedWatchdog();

        if (!httpSuccess) {
            LOG_WARN("[AiWeatherSource] HTTP call failed for [%s], trying next provider...", provider.name.c_str());
            continue;
        }

        // Extract raw text from AI response
        String rawText;
        if (!aiService->extractTextFromAIResponse(aiResponse, rawText)) {
            LOG_WARN("[AiWeatherSource] Failed to extract text from AI response from [%s], trying next provider...", provider.name.c_str());
            feedWatchdog();
            continue;
        }
        feedWatchdog();

        LOG_DEBUG("[AiWeatherSource] Extracted text from [%s] (length: %d bytes): %s",
                 provider.name.c_str(), rawText.length(), rawText.c_str());

        // Parse the raw text to extract the weather forecast in expected format
        String message = extractWeatherForecast(rawText);

        // Validate the extracted result
        if (message.length() == 0) {
            LOG_WARN("[AiWeatherSource] No weather forecast extracted from [%s] response, trying next provider...", provider.name.c_str());
            continue;
        }

        if (message.length() > MAX_MESSAGE_BYTES) {
            LOG_WARN("[AiWeatherSource] Weather forecast from [%s] too long (%d > %d bytes), trying next provider...", provider.name.c_str(), message.length(), MAX_MESSAGE_BYTES);
            continue;
        }

        LOG_INFO("[AiWeatherSource] Successfully generated weather forecast with [%s]: %s", provider.name.c_str(), message.c_str());
        aiService->setCurrentProviderIndex(providerIdx); // Remember successful provider
        return message;
    }

    LOG_ERROR("[AiWeatherSource] All AI providers failed (either HTTP error or parsing error)");
    return "";
}

bool AiWeatherSource::isWithinFetchWindow() const
{
    // Get current time
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        LOG_WARN("[AiWeatherSource] Failed to get local time, preventing fetch until time is synced");
        return false; // Prevent fetch if time is not available
    }

    int currentHour = timeinfo.tm_hour;
    return currentHour >= getMinHourOfDay();
}

String AiWeatherSource::buildWeatherPrompt(const String& weatherJson, const String& tomorrowDate, const String& birthDate, const String& apiDate) const
{

    return String("Jesteś kreatywnym asystentem AI specjalizującym się w tworzeniu polskich prognoz pogody w stylu historycznych postaci.\n\n") +
           "Wykonaj dokładnie te kroki:\n\n" +
           "1. Wybierz losowo jedną sławną postać historyczną urodzoną " + birthDate + ". Osoba może pochodzić z dowolnego kraju, ale musi być rozpoznawalna i znana Polakom. Wybierz kogoś o charakterystycznym stylu wyrażania się - może to być styl pisania, mówienia, tworzenia muzyki, malowania, czy inne formy artystycznego wyrazu.\n\n" +
           "Sprawdź listę osób urodzonych tego dnia używając API: https://api.wikimedia.org/core/v1/wikipedia/pl/page/" + apiDate + "\n\n" +
           "API zwraca JSON z polem 'content' zawierającym wikitext. Przeszukaj sekcję '== Urodzili się ==' i wybierz jedną osobę, która jest znana i ma charakterystyczny styl. Jeśli strona nie istnieje lub nie zawiera odpowiednich osób, możesz wybrać inną znaną postać historyczną. MUSISZ wybrać osobę urodzoną DOKŁADNIE " + tomorrowDate + ".\n\n" +
           "2. Przeanalizuj poniższe rzeczywiste dane pogodowe dla Poznania na jutro (" + tomorrowDate + ") z Open-Meteo API:\n\n" +
           weatherJson + "\n\n" +
           "Dane zawierają:\n" +
           "- hourly.temperature_2m: temperatura na różnych godzinach (°C)\n" +
           "- hourly.precipitation_probability: prawdopodobieństwo opadów (%)\n" +
           "- hourly.precipitation: ilość opadów (mm)\n" +
           "- hourly.wind_speed_10m: prędkość wiatru (km/h)\n" +
           "- hourly.wind_direction_10m: kierunek wiatru (°)\n" +
           "- hourly.cloud_cover: zachmurzenie (%)\n\n" +
           "Na podstawie tych danych stwórz podsumowanie pogody na jutro, uwzględniając:\n" +
           "- Średnią temperaturę w dzień (godziny 6:00-18:00) i w nocy (godziny 18:00-6:00)\n" +
           "- Maksymalne prawdopodobieństwo opadów i ich ilość\n" +
           "- Średnią prędkość wiatru i dominujący kierunek\n" +
           "- Średnie zachmurzenie\n\n" +
           "3. Przepisz tę prognozę w charakterystycznym stylu wybranej postaci - użyj jej znanych powiedzeń, stylu językowego, gwary, idiomów, czy innych form artystycznego wyrazu. Spraw, aby prognoza brzmiała tak, jakby została napisana lub wypowiedziana przez tę osobę.\n\n" +
           "4. Odpowiedz WYŁĄCZNIE w tym formacie (nic więcej, bez markdown):\n" +
           "{Imię i nazwisko postaci}: {prognoza w jej stylu}\n\n" +
           "FORMAT PRZYKŁADU (tylko dla pokazania formatu - NIE KOPIUJ treści):\n" +
           "Maria Skłodowska-Curie: Dzisiaj będzie słonecznie z temperaturą 15-18°C w dzień i 8-10°C w nocy.\n\n" +
           "UWAGA: NIE KOPIUJ przykładu dosłownie! Użyj prawdziwych danych pogodowych z JSON powyżej. Przeanalizuj liczby i stwórz oryginalną prognozę.\n\n" +
           "WAŻNE: Całkowita długość odpowiedzi musi być bliska, ale nie może przekroczyć " + String(MAX_MESSAGE_BYTES - 5) + " znaków. Odpowiedź musi zawierać analizę prawdziwych danych pogodowych, nie kopię przykładu.\n\n" +
           "BEZWZGLĘDNIE ZAKAZANE: NIE DODAWAJ żadnych meta-informacji do odpowiedzi! NIE podawaj liczby znaków, słów, bajtów. NIE dodawaj komentarzy w nawiasach typu '(X znaków)', '(X słów)'. NIE wyjaśniaj formatu. Zwróć TYLKO prognozę pogody w wymaganym formacie, nic więcej.";
}

String AiWeatherSource::cleanFootnotes(const String& input) const
{
    String result = input;

    // Remove footnote markers like [1], [2], etc.
    // These are often added by AI models when referencing sources
    int bracketStart;
    while ((bracketStart = result.indexOf('[')) >= 0) {
        int bracketEnd = result.indexOf(']', bracketStart);
        if (bracketEnd > bracketStart) {
            // Check if content between brackets is numeric (footnote marker)
            bool isFootnote = true;
            for (int i = bracketStart + 1; i < bracketEnd; i++) {
                if (!isdigit(result.charAt(i))) {
                    isFootnote = false;
                    break;
                }
            }

            if (isFootnote) {
                // Remove the footnote marker including brackets
                result = result.substring(0, bracketStart) + result.substring(bracketEnd + 1);
            } else {
                // Not a footnote, skip this bracket pair
                break;
            }
        } else {
            // No closing bracket, stop processing
            break;
        }
    }

    // Trim any trailing whitespace that might be left
    result.trim();
    return result;
}

String AiWeatherSource::extractWeatherForecast(const String& fullResponse) const
{
    // Look for the specific format expected for weather forecasts: "{Name}: {weather forecast}"
    // Also handle markdown formatting that some AI models add
    LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Parsing response (length: %d bytes)", fullResponse.length());
    
    // Log first and last 200 chars for debugging
    if (fullResponse.length() > 0) {
        String preview = fullResponse.substring(0, (fullResponse.length() > 200) ? 200 : fullResponse.length());
        LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Response preview (first %d chars): %s", 
                 preview.length(), preview.c_str());
        
        if (fullResponse.length() > 400) {
            String end = fullResponse.substring(fullResponse.length() - 200);
            LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Response end (last 200 chars): %s", end.c_str());
        }
    }

    // First, try to find a line that exactly matches the expected format
    int start = 0;
    int lineCount = 0;
    while (start < fullResponse.length()) {
        int end = fullResponse.indexOf('\n', start);
        if (end == -1) end = fullResponse.length();

        String line = fullResponse.substring(start, end);
        line.trim();
        lineCount++;

        // Log all non-empty lines for debugging
        if (line.length() > 0) {
            LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Line %d (%d chars): %s", 
                     lineCount, line.length(), line.c_str());
        }

        // Check if this line matches the expected weather forecast format
        int colonPos = line.indexOf(": ");
        if (colonPos > 0 && colonPos < line.length() - 5) {  // Has colon with content after
            String namePart = line.substring(0, colonPos);
            String forecastPart = line.substring(colonPos + 2);

            // Strip markdown formatting from name
            // Remove ** from start and end
            while (namePart.startsWith("**")) {
                namePart = namePart.substring(2);
            }
            while (namePart.endsWith("**")) {
                namePart = namePart.substring(0, namePart.length() - 2);
            }
            
            // Strip single * markers from name
            while (namePart.startsWith("*")) {
                namePart = namePart.substring(1);
            }
            while (namePart.endsWith("*")) {
                namePart = namePart.substring(0, namePart.length() - 1);
            }
            namePart.trim();

            LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Found colon at pos %d: name=%d chars (cleaned), forecast=%d chars", 
                     colonPos, namePart.length(), forecastPart.length());

            // Validate the format:
            // 1. Name part should be reasonable length (person's name)
            // 2. Forecast part should be reasonable length for weather content
            // Note: Allow longer forecasts and truncate if needed
            if (namePart.length() >= 3 && namePart.length() <= 50 &&
                forecastPart.length() >= 10) {

                // If forecast is too long, truncate it while respecting UTF-8 boundaries
                String finalForecast = forecastPart;
                if (forecastPart.length() > MAX_MESSAGE_BYTES) {
                    LOG_WARN("[AiWeatherSource::extractWeatherForecast] Forecast too long (%d bytes), truncating to %d",
                            forecastPart.length(), MAX_MESSAGE_BYTES);
                    finalForecast = forecastPart.substring(0, MAX_MESSAGE_BYTES);
                    // Trim to avoid cutting in middle of word
                    int lastSpace = finalForecast.lastIndexOf(' ');
                    if (lastSpace > MAX_MESSAGE_BYTES - 50) {  // If space is reasonably close
                        finalForecast = finalForecast.substring(0, lastSpace);
                    }
                    finalForecast.trim();
                }

                // Reconstruct the clean line
                String cleanedLine = namePart + ": " + finalForecast;
                cleanedLine = cleanFootnotes(cleanedLine);
                LOG_INFO("[AiWeatherSource::extractWeatherForecast] Found weather forecast: %s", cleanedLine.c_str());
                return cleanedLine;
            } else {
                LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Line has colon but failed validation: name=%d (need 3-50), forecast=%d (need 10+)",
                         namePart.length(), forecastPart.length());
            }
        }

        start = end + 1;
    }

    LOG_WARN("[AiWeatherSource::extractWeatherForecast] No matching lines found in first pass (parsed %d lines)", lineCount);

    // If no perfect match found, try to extract the last reasonable line with weather content
    // This handles cases where the AI puts the result at the end
    start = fullResponse.length() - 300;  // Check last 300 chars
    if (start < 0) start = 0;

    LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Trying fallback (searching last 300 chars from pos %d)", start);

    while (start < fullResponse.length()) {
        int end = fullResponse.indexOf('\n', start);
        if (end == -1) end = fullResponse.length();

        String line = fullResponse.substring(start, end);
        line.trim();

        if (line.indexOf(": ") > 0 && line.length() > 15) {
            // Clean up markdown and footnote/reference markers
            String cleanedLine = cleanFootnotes(line);
            
            // Strip markdown
            while (cleanedLine.startsWith("**")) {
                cleanedLine = cleanedLine.substring(2);
            }
            while (cleanedLine.startsWith("*")) {
                cleanedLine = cleanedLine.substring(1);
            }
            cleanedLine.trim();

            // Truncate if needed
            if (cleanedLine.length() > MAX_MESSAGE_BYTES) {
                cleanedLine = cleanedLine.substring(0, MAX_MESSAGE_BYTES);
                int lastSpace = cleanedLine.lastIndexOf(' ');
                if (lastSpace > MAX_MESSAGE_BYTES - 50) {
                    cleanedLine = cleanedLine.substring(0, lastSpace);
                }
                cleanedLine.trim();
            }

            // Accept any reasonable line with colon as potential weather forecast
            LOG_INFO("[AiWeatherSource::extractWeatherForecast] Using fallback line: %s", cleanedLine.c_str());
            return cleanedLine;
        } else if (line.length() > 15) {
            LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Fallback: Line doesn't match pattern: %d chars, has colon: %s",
                     line.length(), (line.indexOf(": ") > 0) ? "yes" : "no");
        }

        start = end + 1;
    }

    LOG_ERROR("[AiWeatherSource::extractWeatherForecast] No weather forecast found in response of %d bytes", fullResponse.length());
    return "";
}

int AiWeatherSource::getMinHourOfDay()
{
    // ALERT_MIN_HOUR_OF_DAY is defined as a string from environment variable
    String hourStr = ALERT_MIN_HOUR_OF_DAY;
    if (hourStr.length() > 0) {
        int hour = hourStr.toInt();
        if (hour >= 0 && hour <= 23) {
            return hour;
        }
    }
    // Use default if not set or invalid
    return DEFAULT_MIN_HOUR_OF_DAY;
}


#endif // HAS_ALERTING
