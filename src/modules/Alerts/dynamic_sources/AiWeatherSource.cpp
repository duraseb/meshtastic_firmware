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

    // Check if we're within the allowed fetch window (after 20:00)
    if (!isWithinFetchWindow()) {
        LOG_INFO("[AiWeatherSource] Too early in day (before %02d:00), skipping fetch", MIN_HOUR_OF_DAY);
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

    // Build the AI prompt
    String prompt = buildWeatherPrompt();
    if (prompt.length() == 0) {
        LOG_ERROR("[AiWeatherSource] Failed to build AI prompt");
        return "";
    }

    LOG_DEBUG("[AiWeatherSource] Built prompt (%d bytes)", prompt.length());

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

        LOG_DEBUG("[AiWeatherSource] AI response received from [%s] (%d bytes)", provider.name.c_str(), aiResponse.length());

        // Try to parse the response
        String message;
        bool parseSuccess = aiService->extractTextFromAIResponse(aiResponse, message);

        if (parseSuccess) {
            LOG_INFO("[AiWeatherSource] AI extraction successful with [%s]", provider.name.c_str());
            aiService->setCurrentProviderIndex(providerIdx); // Remember successful provider

            // Log the AI response (split into up to 5 lines of 180 chars each)
            logAIResponse(message);

            // Validate message format and length
            if (!validateMessage(message)) {
                LOG_ERROR("[AiWeatherSource] AI response failed validation");
                return "";
            }

            LOG_INFO("[AiWeatherSource] Successfully generated weather forecast: %s", message.c_str());
            return message;
        } else {
            LOG_WARN("[AiWeatherSource] Failed to parse AI response from [%s], trying next provider...", provider.name.c_str());
        }
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
    return currentHour >= MIN_HOUR_OF_DAY;
}

String AiWeatherSource::buildWeatherPrompt() const
{
    // Calculate tomorrow's date
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        LOG_ERROR("[AiWeatherSource] Cannot build prompt: time not available");
        return "";
    }

    // Add one day to get tomorrow
    timeinfo.tm_mday += 1;
    mktime(&timeinfo); // Normalize the date

    // Format date as "DD miesiąc YYYY" (Polish format)
    const char* months[] = {
        "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
        "lipca", "sierpnia", "września", "października", "listopada", "grudnia"
    };

    String tomorrowDate = String(timeinfo.tm_mday) + " " +
                         String(months[timeinfo.tm_mon]) + " " +
                         String(timeinfo.tm_year + 1900);

    LOG_DEBUG("[AiWeatherSource] Using tomorrow's date: %s", tomorrowDate.c_str());

    return String("Jesteś precyzyjnym asystentem z dostępem do internetu. Wykonaj dokładnie poniższe kroki i nie wymyślaj żadnych danych.\n\n") +
           "1. Znajdź sławną, bardzo rozpoznawalną w Polsce osobę (żyjącą lub nie), która urodziła się dokładnie dnia " + tomorrowDate + " (dowolnego roku). Najlepsze źródła:  \n" +
           "   - pl.wikipedia.org/wiki/Kategoria:Urodzeni_[dzień]_[miesiąca]  \n" +
           "   - en.wikipedia.org/wiki/[Miesiąc]_[dzień]  \n" +
           "   Wytypuj 1-10 najbardziej znanych postaci dla Polaków, a których sposób wypowiedzi lub sposób bycia jest lub był charakterystyczny i rozpoznawalny.Preferuj pisarzy, poetów, piosenkarzy, polityków, sportowców, naukowców, etc. Z tych wytypowanych osób wybierz jedną losowo.\n" +
           "2. Pobierz rzeczywistą prognozę pogody dla Poznania (Polska) dokładnie na dzień " + tomorrowDate + ", korzystając wyłącznie z poniższego URL (format JSON):  \n" +
           "https://api.open-meteo.com/v1/forecast?latitude=52.4069&longitude=16.9299&hourly=temperature_2m,relative_humidity_2m,precipitation_probability,precipitation,surface_pressure,cloud_cover,visibility,wind_speed_10m,wind_direction_10m,soil_temperature_0cm&timezone=Europe%2FBerlin&forecast_days=1&forecast_hours=24&temporal_resolution=hourly_6&format=json&timeformat=unixtime\n\n" +
           "   Podaj prawdziwe wartości: temperatura dzienna/nocna, opady, wiatr, zachmurzenie.\n" +
           "3. Przetłumacz tę prognozę na charakterystyczny język/manierę mówienia wybranej postaci (używaj jej typowych zwrotów, akcentu gwarowego, stylu piosenek lub cytatów). Przedstaw tę wypowiedź jako prognozę pogody na jutro. Jeśli imię lub imiona tej osoby są długie, użyj inicjałów, aby zachować więcej miejsca na prognozę.\n" +
           "4. Odpowiedz WYŁĄCZNIE jednym zdaniem w formacie:\n" +
           "   \"{Imię i nazwisko postaci}: {prognoza w jej stylu}\"\n" +
           "   Całość jak najdłuższa, ale maksymalnie " + String(MAX_MESSAGE_BYTES) + " bajtów.\n\n" +
           "Nie dodawaj żadnych wyjaśnień, wstępów ani podpisów - tylko tę jedną linijkę.";
}


bool AiWeatherSource::validateMessage(const String& message) const
{
    // Check if message is empty
    if (message.length() == 0) {
        LOG_ERROR("[AiWeatherSource] Message is empty");
        return false;
    }

    // Check maximum length (same as alert sources - reasonable limit to prevent issues)
    if (message.length() > MAX_MESSAGE_BYTES) {
        LOG_ERROR("[AiWeatherSource] Message too long: %d > %d bytes", message.length(), MAX_MESSAGE_BYTES);
        return false;
    }

    // Check for required format: "{Name}: {forecast}"
    if (message.indexOf(": ") < 0) {
        LOG_ERROR("[AiWeatherSource] Message doesn't contain required colon format");
        return false;
    }

    // Check for reasonable content (should have weather-related terms)
    bool hasWeatherTerms = message.indexOf("pogoda") >= 0 ||
                          message.indexOf("temperatura") >= 0 ||
                          message.indexOf("deszcz") >= 0 ||
                          message.indexOf("śnieg") >= 0 ||
                          message.indexOf("wiatr") >= 0 ||
                          message.indexOf("stopni") >= 0;

    if (!hasWeatherTerms) {
        LOG_WARN("[AiWeatherSource] Message may not contain weather information");
        // Don't fail validation for this, as the AI might use creative language
    }

    LOG_INFO("[AiWeatherSource] Message validation passed: %d chars", message.length());
    return true;
}

void AiWeatherSource::logAIResponse(const String& response) const
{
    const int MAX_LINE_LENGTH = 180;
    const int MAX_LINES = 5;

    LOG_INFO("[AiWeatherSource] AI Response (%d chars):", response.length());

    int remainingLength = response.length();
    int startPos = 0;

    for (int lineNum = 0; lineNum < MAX_LINES && startPos < response.length(); lineNum++) {
        int lineLength = min(MAX_LINE_LENGTH, remainingLength);
        String line = response.substring(startPos, startPos + lineLength);

        // If we're not at the end and this line ends mid-word, try to break at word boundary
        if (startPos + lineLength < response.length() && lineLength == MAX_LINE_LENGTH) {
            int lastSpace = line.lastIndexOf(' ');
            if (lastSpace > MAX_LINE_LENGTH / 2) { // Only break at space if it's not too early in line
                line = line.substring(0, lastSpace);
                lineLength = lastSpace + 1; // Include the space
            }
        }

        LOG_INFO("[AiWeatherSource]   [%d] %s", lineNum + 1, line.c_str());

        startPos += lineLength;
        remainingLength -= lineLength;

        if (remainingLength <= 0) {
            break;
        }
    }

    if (startPos < response.length()) {
        LOG_INFO("[AiWeatherSource]   ... (%d more chars truncated)", response.length() - startPos);
    }
}
