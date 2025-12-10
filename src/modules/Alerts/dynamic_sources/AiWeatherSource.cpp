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
           "   Wybierz jedną najbardziej ikoniczną postać dla Polaków.\n" +
           "2. Pobierz rzeczywistą prognozę pogody dla Poznania (Polska) dokładnie na dzień " + tomorrowDate + ", korzystając wyłącznie z wiarygodnych bezpłatnych źródeł (wybierz najlepsze dostępne w danej chwili):  \n" +
           "   - weather.com -> wyszukaj \"Poznań\" - \"10 Day\" lub \"Hourly\"\n" +
           "   - accuweather.com → /pl/pl/poznan/274480/daily-weather-forecast/274480?day=2\n" +
           "   - meteoblue.com/pl/pogoda/tydzień/poznań_polska_3088171\n" +
           "   - imgw.pl (Instytut Meteorologii i Gospodarki Wodnej)\n" +
           "   Podaj prawdziwe wartości: temperatura dzienna/nocna, opady, wiatr, zachmurzenie.\n" +
           "3. Przetłumacz tę prognozę na charakterystyczny język/manierę mówienia wybranej postaci (używaj jej typowych zwrotów, akcentu gwarowego, stylu piosenek lub cytatów).\n" +
           "4. Odpowiedz WYŁĄCZNIE jednym krótkim zdaniem w formacie:\n" +
           "   \"{Imię i nazwisko postaci} prognozuje: {prognoza w jej stylu}\"\n" +
           "   Całość (wraz z imieniem i \"prognozuje: \") maksymalnie 180 znaków ze spacjami włącznie.\n\n" +
           "Nie dodawaj żadnych wyjaśnień, wstępów ani podpisów - tylko tę jedną linijkę.";
}


bool AiWeatherSource::validateMessage(const String& message) const
{
    // Check if message is empty
    if (message.length() == 0) {
        LOG_ERROR("[AiWeatherSource] Message is empty");
        return false;
    }

    // Check maximum length (180 characters as specified in prompt)
    if (message.length() > 180) {
        LOG_ERROR("[AiWeatherSource] Message too long: %d > 180 chars", message.length());
        return false;
    }

    // Check for required format: "{Name} prognozuje: {forecast}"
    if (message.indexOf(" prognozuje: ") < 0) {
        LOG_ERROR("[AiWeatherSource] Message doesn't contain required 'prognozuje:' format");
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