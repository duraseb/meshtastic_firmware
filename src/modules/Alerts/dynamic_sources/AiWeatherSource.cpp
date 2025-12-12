#include "AiWeatherSource.h"
#include <time.h>
#include <RTC.h>

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
    String weatherJson = httpGetCallback(weatherApiUrl.c_str(), httpCode);

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
    String tomorrowDate = String(timeinfo.tm_mday) + " " +
                         String(months[timeinfo.tm_mon]) + " " +
                         String(timeinfo.tm_year + 1900);

    // Format date for API URL as "DD_miesiąca" (without year)
    String apiDate = String(timeinfo.tm_mday) + "_" + String(months[timeinfo.tm_mon]);

    LOG_DEBUG("[AiWeatherSource] Using tomorrow's date: %s (API format: %s)", tomorrowDate.c_str(), apiDate.c_str());

    // Don't fetch Wikipedia content - let AI handle it
    String wikiContent = ""; // Not used anymore

    // Build the AI prompt with real weather data and formatted date
    String prompt = buildWeatherPrompt(weatherJson, tomorrowDate, apiDate);
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
        AIService::AIProvider& provider = aiService->getProviders()[providerIdx];

        // Skip if provider not configured
        if (provider.endpoint.length() == 0 || provider.apiKey.length() == 0) {
            continue;
        }

        LOG_INFO("[AiWeatherSource] Attempting AI call with [%s]...", provider.name.c_str());

        String aiResponse;
        bool httpSuccess = aiService->callProvider(providerIdx, prompt, aiResponse);

        if (!httpSuccess) {
            LOG_WARN("[AiWeatherSource] HTTP call failed for [%s], trying next provider...", provider.name.c_str());
            continue;
        }

        // Extract raw text from AI response
        String rawText;
        if (!aiService->extractTextFromAIResponse(aiResponse, rawText)) {
            LOG_WARN("[AiWeatherSource] Failed to extract text from AI response from [%s], trying next provider...", provider.name.c_str());
            continue;
        }

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

String AiWeatherSource::buildWeatherPrompt(const String& weatherJson, const String& tomorrowDate, const String& apiDate) const
{

    return String("Jesteś kreatywnym asystentem AI specjalizującym się w tworzeniu polskich prognoz pogody w stylu historycznych postaci.\n\n") +
           "Wykonaj dokładnie te kroki:\n\n" +
           "1. Wybierz losowo jedną sławną postać historyczną urodzoną " + tomorrowDate + ". Osoba może pochodzić z dowolnego kraju, ale musi być rozpoznawalna i znana Polakom. Wybierz kogoś o charakterystycznym stylu wyrażania się - może to być styl pisania, mówienia, tworzenia muzyki, malowania, czy inne formy artystycznego wyrazu.\n\n" +
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
           "4. Odpowiedz WYŁĄCZNIE w tym formacie (nic więcej):\n" +
           "{Imię i nazwisko postaci}: {prognoza w jej stylu}\n\n" +
           "Przykład: Adam Mickiewicz: Jutro w Poznaniu będzie pochmurno z temperaturą około 15 stopni w dzień i 8 stopni w nocy. Lekki wiatr z zachodu przyniesie kilka kropel deszczu.\n\n" +
           "WAŻNE: Całkowita długość odpowiedzi nie może przekroczyć " + String(MAX_MESSAGE_BYTES - 5) + " znaków. Postaraj się, aby wypowiedź była jak najbardziej naturalna i prawdopodobna i jak najdłuższa w granicy " + String(MAX_MESSAGE_BYTES - 5) + " znaków.";
}

String AiWeatherSource::extractWeatherForecast(const String& fullResponse) const
{
    // Look for the specific format expected for weather forecasts: "{Name}: {weather forecast}"

    // First, try to find a line that exactly matches the expected format
    int start = 0;
    while (start < fullResponse.length()) {
        int end = fullResponse.indexOf('\n', start);
        if (end == -1) end = fullResponse.length();

        String line = fullResponse.substring(start, end);
        line.trim();

        // Check if this line matches the expected weather forecast format
        int colonPos = line.indexOf(": ");
        if (colonPos > 0 && colonPos < line.length() - 5) {  // Has colon with content after
            String namePart = line.substring(0, colonPos);
            String forecastPart = line.substring(colonPos + 2);

            // Validate the format:
            // 1. Name part should be reasonable length (person's name)
            // 2. Forecast part should be reasonable length for weather content
            if (namePart.length() >= 3 && namePart.length() <= 50 &&
                forecastPart.length() >= 10 && forecastPart.length() <= MAX_MESSAGE_BYTES) {

                // Trust the AI to provide weather-related content when prompted
                // The format validation and length checks are sufficient
                LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Found weather forecast: %s", line.c_str());
                return line;
            }
        }

        start = end + 1;
    }

    // If no perfect match found, try to extract the last reasonable line with weather content
    // This handles cases where the AI puts the result at the end
    start = fullResponse.length() - 300;  // Check last 300 chars
    if (start < 0) start = 0;

    while (start < fullResponse.length()) {
        int end = fullResponse.indexOf('\n', start);
        if (end == -1) end = fullResponse.length();

        String line = fullResponse.substring(start, end);
        line.trim();

        if (line.indexOf(": ") > 0 && line.length() > 15 && line.length() < MAX_MESSAGE_BYTES) {
            // Accept any reasonable line with colon as potential weather forecast
            // Trust the AI prompt to produce weather-related content
            LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] Using fallback line: %s", line.c_str());
            return line;
        }

        start = end + 1;
    }

    LOG_DEBUG("[AiWeatherSource::extractWeatherForecast] No weather forecast found in response");
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

