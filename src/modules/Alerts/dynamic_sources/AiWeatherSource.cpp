#if defined(HAS_ALERTING) && HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_AIWEATHER

#include "AiWeatherSource.h"
#include "concurrency/LockGuard.h"
#include <time.h>
#include <RTC.h>
#include <ctype.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#ifdef ARCH_ESP32
#include <esp_task_wdt.h>
#endif

namespace {
constexpr char OPEN_METEO_FORECAST_URL[] =
    "https://api.open-meteo.com/v1/forecast?latitude=52.4069&longitude=16.9299&hourly=temperature_2m,relative_humidity_2m,precipitation_probability,precipitation,surface_pressure,cloud_cover,visibility,wind_speed_10m,wind_direction_10m,soil_temperature_0cm&timezone=Europe%2FBerlin&forecast_days=1&forecast_hours=24&temporal_resolution=hourly_6&format=json&timeformat=unixtime";
constexpr size_t WIKIDATA_READ_CHUNK_BYTES = 512;
#ifdef ARCH_ESP32
// 12KB task stack (xTaskCreate uses stack words on ESP-IDF/FreeRTOS, so divide by word size).
constexpr uint16_t ASYNC_FETCH_TASK_STACK_BYTES = 12 * 1024;
constexpr uint16_t ASYNC_FETCH_TASK_STACK_WORDS = ASYNC_FETCH_TASK_STACK_BYTES / sizeof(StackType_t);
#endif
}

// Helper to reset watchdog timer (architecture-aware)
static inline void feedWatchdog()
{
#ifdef ARCH_ESP32
    esp_task_wdt_reset();
#endif
}

static inline void yieldMillis(unsigned long ms)
{
#ifdef ARCH_ESP32
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    delay(ms);
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
#ifdef ARCH_ESP32
    stopAsyncFetch();
#endif
}

unsigned long AiWeatherSource::getFetchIntervalMs() const
{
    return lastFetchFailed ? RETRY_FETCH_INTERVAL_MS : DEFAULT_FETCH_INTERVAL_MS;
}

String AiWeatherSource::fetchAndFormat(
    std::function<String(const char*, int&)> httpGetCallback)
{
    static unsigned long lastAsyncInProgressLogMs = 0;

    // Check if AI service is available
    if (aiService == nullptr) {
        LOG_ERROR("[AiWeatherSource] AIService not available");
        return "";
    }

    if (!aiService->hasConfiguredProviders()) {
        LOG_ERROR("[AiWeatherSource] No AI providers configured");
        return "";
    }

    // Check if we have enough memory for AI processing + optional Wikidata query
    // AI calls require ~16KB for JSON buffer + HTTP client overhead; Wikidata adds parsing + prompt construction
    const size_t MIN_HEAP_FOR_AI_WITH_WIKIDATA = 55000; // 55KB minimum
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_AI_WITH_WIKIDATA) {
        LOG_WARN("[AiWeatherSource] Insufficient heap for processing (%d bytes free, need at least %d). Skipping...",
                 freeHeap, MIN_HEAP_FOR_AI_WITH_WIKIDATA);
        return "";
    }

    String birthDate;
    String tomorrowDate;
    String birthPersonHints;
    String weatherJson;
    bool useAsyncResult = false;
    int queryMonth = 0;
    int queryDay = 0;

#ifdef ARCH_ESP32
    bool startAsync = false;
    bool asyncInProgress = false;
    String asyncFailure;
    String asyncBirthDateCached;
    String asyncTomorrowDateCached;

    {
        concurrency::LockGuard guard(&asyncStateLock);
        switch (asyncState) {
            case AsyncFetchState::RUNNING:
                asyncInProgress = true;
                break;
            case AsyncFetchState::FAILED:
                asyncFailure = asyncError;
                asyncState = AsyncFetchState::IDLE;
                asyncError = "";
                break;
            case AsyncFetchState::READY: {
                asyncBirthDateCached = asyncBirthDate;
                asyncTomorrowDateCached = asyncTomorrowDate;
                weatherJson = asyncWeatherJson;
                birthPersonHints = asyncBirthPersonHints;
                asyncWeatherJson = "";
                asyncBirthPersonHints = "";
                asyncState = AsyncFetchState::IDLE;
                asyncError = "";
                useAsyncResult = true;
                break;
            }
            case AsyncFetchState::IDLE:
            default:
                startAsync = true;
                break;
        }
    }

    if (asyncInProgress) {
        const unsigned long now = millis();
        if (now - lastAsyncInProgressLogMs > 15000UL) {
            LOG_DEBUG("[AiWeatherSource] Async weather/Wikidata fetch in progress");
            lastAsyncInProgressLogMs = now;
        }
        return "";
    }

    if (asyncFailure.length() > 0) {
        LOG_WARN("[AiWeatherSource] Last async prefetch failed: %s. Will retry in %lu min",
                 asyncFailure.c_str(), RETRY_FETCH_INTERVAL_MS / 60000);
        lastFetchFailed = true;
        return "";
    }

    if (useAsyncResult) {
        birthDate = asyncBirthDateCached;
        tomorrowDate = asyncTomorrowDateCached;
        LOG_DEBUG("[AiWeatherSource] Using async-prefetched date for prompt: %s", birthDate.c_str());
    } else {
        // Check if we're within the allowed fetch window
        if (!isWithinFetchWindow()) {
            LOG_INFO("[AiWeatherSource] Too early in day (before %02d:00), skipping fetch", getMinHourOfDay());
            return "";
        }

        // Calculate tomorrow's date for SPARQL lookup and prompt (synchronous/fallback path)
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
            LOG_WARN("[AiWeatherSource] Failed to get local time for date calculation");
            return ""; // Can't proceed without time
        }
        timeinfo.tm_mday += 1;
        mktime(&timeinfo); // Normalize the date

        const char* months[] = {
            "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
            "lipca", "sierpnia", "września", "października", "listopada", "grudnia"
        };

        birthDate = String(timeinfo.tm_mday) + " " + String(months[timeinfo.tm_mon]);
        tomorrowDate = birthDate + " " + String(timeinfo.tm_year + 1900);
        LOG_DEBUG("[AiWeatherSource] Using fallback date for prompt: %s", birthDate.c_str());
        queryMonth = timeinfo.tm_mon + 1;
        queryDay = timeinfo.tm_mday;

        if (startAsync) {
            if (startAsyncFetchForDate(timeinfo.tm_mon + 1, timeinfo.tm_mday, birthDate, tomorrowDate)) {
                LOG_INFO("[AiWeatherSource] Started background data fetch for date %s (async)", birthDate.c_str());
                return "";
            }
            {
                concurrency::LockGuard guard(&asyncStateLock);
                asyncState = AsyncFetchState::IDLE;
                asyncError = "";
                asyncWeatherJson = "";
                asyncBirthPersonHints = "";
            }
            LOG_WARN("[AiWeatherSource] Async fetch task not started; falling back to synchronous mode");
        }
    }
#endif

#ifndef ARCH_ESP32
    // Calculate tomorrow's date for SPARQL lookup and prompt
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        LOG_WARN("[AiWeatherSource] Failed to get local time for date calculation");
        return ""; // Can't proceed without time
    }

    // Check if we're within the allowed fetch window
    if (!isWithinFetchWindow()) {
        LOG_INFO("[AiWeatherSource] Too early in day (before %02d:00), skipping fetch", getMinHourOfDay());
        return "";
    }

    // Calculate tomorrow's date
    timeinfo.tm_mday += 1;
    mktime(&timeinfo); // Normalize the date

    // Polish month names for prompt formatting
    const char* months[] = {
        "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
        "lipca", "sierpnia", "września", "października", "listopada", "grudnia"
    };

    // Format date as "DD miesiąc YYYY" (Polish format for display)
    birthDate = String(timeinfo.tm_mday) + " " +
                 String(months[timeinfo.tm_mon]);
    tomorrowDate = birthDate + " " +
                   String(timeinfo.tm_year + 1900);
    LOG_DEBUG("[AiWeatherSource] Using tomorrow's date for prompt: %s", birthDate.c_str());
    queryMonth = timeinfo.tm_mon + 1;
    queryDay = timeinfo.tm_mday;

#endif

    if (!useAsyncResult) {
        // Fetch people list from Wikidata SPARQL (query runs directly from firmware, not from model)
        if (!fetchBirthPersonHints(queryMonth, queryDay, birthDate, birthPersonHints)) {
            LOG_WARN("[AiWeatherSource] Failed to fetch or parse Wikidata birthday candidates");
            birthPersonHints = "";
        } else {
            LOG_DEBUG("[AiWeatherSource] Retrieved Wikidata hints for %s (%d chars)",
                      birthDate.c_str(), birthPersonHints.length());
        }
    } else {
        LOG_DEBUG("[AiWeatherSource] Consuming async results: weather=%d bytes, hints=%d chars",
                  weatherJson.length(), birthPersonHints.length());
    }

    // Fetch real weather data from Open-Meteo API
    const char* weatherApiUrl = OPEN_METEO_FORECAST_URL;

    if (!useAsyncResult) {
        int httpCode = 0;
        feedWatchdog();
        weatherJson = httpGetCallback(weatherApiUrl, httpCode);
        feedWatchdog();

        if (httpCode != 200 || weatherJson.length() == 0) {
            LOG_ERROR("[AiWeatherSource] Failed to fetch weather data (HTTP %d)", httpCode);
            return "";
        }
        LOG_DEBUG("[AiWeatherSource] Weather fetch succeeded synchronously: %d bytes", weatherJson.length());
    }

    // Build the AI prompt with real weather data and formatted date
    String prompt = buildWeatherPrompt(weatherJson, tomorrowDate, birthDate, birthPersonHints);
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

        if (message.length() > meshtastic_Constants_DATA_PAYLOAD_LEN) {
            LOG_WARN("[AiWeatherSource] Weather forecast from [%s] too long (%d > %d bytes), trying next provider...", provider.name.c_str(), message.length(),
                     meshtastic_Constants_DATA_PAYLOAD_LEN);
            continue;
        }

        LOG_INFO("[AiWeatherSource] Successfully generated weather forecast with [%s]: %s", provider.name.c_str(), message.c_str());
        aiService->setCurrentProviderIndex(providerIdx); // Remember successful provider
        lastFetchFailed = false;
        return message;
    }

    LOG_ERROR("[AiWeatherSource] All AI providers failed (either HTTP error or parsing error)");
    return "";
}

#ifdef ARCH_ESP32
bool AiWeatherSource::isAsyncFetchInProgress() const
{
    concurrency::LockGuard guard(&asyncStateLock);
    return asyncState == AsyncFetchState::RUNNING;
}

bool AiWeatherSource::isAsyncFetchReady() const
{
    concurrency::LockGuard guard(&asyncStateLock);
    return asyncState == AsyncFetchState::READY;
}

bool AiWeatherSource::startAsyncFetchForDate(int month, int day, const String& birthDate, const String& tomorrowDate)
{
    {
        concurrency::LockGuard guard(&asyncStateLock);
        if (asyncState != AsyncFetchState::IDLE) {
            LOG_WARN("[AiWeatherSource] Async fetch not started; state=%d", (int)asyncState);
            return false;
        }

        asyncMonth = month;
        asyncDay = day;
        asyncBirthDate = birthDate;
        asyncTomorrowDate = tomorrowDate;
        asyncWeatherJson = "";
        asyncBirthPersonHints = "";
        asyncError = "";
        asyncStopRequested = false;
        asyncState = AsyncFetchState::RUNNING;
    }

    LOG_DEBUG("[AiWeatherSource] Scheduling async fetch for date=%s (%02d-%02d), tomorrow=%s",
              birthDate.c_str(), month, day, tomorrowDate.c_str());

    BaseType_t created = xTaskCreate(
        asyncFetchTask, "AiWeatherFetch", ASYNC_FETCH_TASK_STACK_WORDS, this, 1, &asyncTaskHandle);
    if (created != pdPASS) {
        setAsyncResultError("Failed to create async fetch task");
        return false;
    }

    LOG_INFO("[AiWeatherSource] Async task created for date=%s, stack=%d bytes",
             birthDate.c_str(), ASYNC_FETCH_TASK_STACK_BYTES);

    return true;
}

void AiWeatherSource::stopAsyncFetch()
{
    TaskHandle_t taskHandle = nullptr;
    {
        concurrency::LockGuard guard(&asyncStateLock);
        asyncStopRequested = true;
        if (asyncTaskHandle == nullptr) {
            asyncState = AsyncFetchState::IDLE;
            asyncWeatherJson = "";
            asyncBirthPersonHints = "";
            asyncError = "";
            asyncStopRequested = false;
            LOG_DEBUG("[AiWeatherSource] Async stop requested but no active task");
            return;
        }
        taskHandle = asyncTaskHandle;
    }

    if (taskHandle == xTaskGetCurrentTaskHandle()) {
        return;
    }

    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        {
            concurrency::LockGuard guard(&asyncStateLock);
            if (asyncTaskHandle == nullptr) {
                return;
            }
        }
    }

    concurrency::LockGuard guard(&asyncStateLock);
    if (asyncTaskHandle != nullptr) {
        vTaskDelete(asyncTaskHandle);
        asyncTaskHandle = nullptr;
    }
    asyncStopRequested = false;
    asyncState = AsyncFetchState::IDLE;
    asyncError = "";
    LOG_WARN("[AiWeatherSource] Async task %p did not stop in time, force-deleting", (void*)taskHandle);
}

void AiWeatherSource::asyncFetchTask(void *pvParameters)
{
    if (pvParameters == nullptr) {
        vTaskDelete(NULL);
        return;
    }

    auto *self = static_cast<AiWeatherSource *>(pvParameters);
    LOG_INFO("[AiWeatherSource] Async fetch task started");
    self->runAsyncFetch();
    LOG_DEBUG("[AiWeatherSource] Async fetch task finished");
    vTaskDelete(NULL);
}

void AiWeatherSource::runAsyncFetch()
{
    int month = 0;
    int day = 0;
    String birthDate;
    String weatherApiUrl = OPEN_METEO_FORECAST_URL;

    {
        concurrency::LockGuard guard(&asyncStateLock);
        if (asyncStopRequested) {
            asyncState = AsyncFetchState::IDLE;
            asyncTaskHandle = nullptr;
            LOG_WARN("[AiWeatherSource] Async fetch canceled before start");
            return;
        }
        month = asyncMonth;
        day = asyncDay;
        birthDate = asyncBirthDate;
    }

    String birthPersonHints;
    LOG_INFO("[AiWeatherSource] Running async fetch for date=%02d-%02d (%s)", month, day, birthDate.c_str());
    if (!fetchBirthPersonHints(month, day, birthDate, birthPersonHints)) {
        setAsyncResultError("Wikidata birthday candidates unavailable");
        return;
    }
    LOG_DEBUG("[AiWeatherSource] Async Wikidata stage done: %d hint chars", birthPersonHints.length());

    int weatherHttpCode = 0;
    String weatherJson = httpGetLongTimeout(weatherApiUrl, weatherHttpCode, WIKIDATA_QUERY_TIMEOUT_MS);
    if (asyncStopRequested) {
        setAsyncResultError("Async fetch interrupted");
        return;
    }

    if (weatherHttpCode != HTTP_CODE_OK || weatherJson.length() == 0) {
        setAsyncResultError("Weather API call failed");
        return;
    }
    LOG_DEBUG("[AiWeatherSource] Async weather stage done: HTTP=%d, bytes=%d", weatherHttpCode, weatherJson.length());

    setAsyncResultReady(weatherJson, birthPersonHints);
}

void AiWeatherSource::setAsyncResultReady(const String& weatherJson, const String& birthPersonHints)
{
    concurrency::LockGuard guard(&asyncStateLock);
    if (asyncStopRequested) {
        asyncState = AsyncFetchState::IDLE;
        asyncWeatherJson = "";
        asyncBirthPersonHints = "";
        asyncError = "Async fetch interrupted";
        LOG_WARN("[AiWeatherSource] Async result ready but stop was requested; dropping payload");
    } else {
        asyncWeatherJson = weatherJson;
        asyncBirthPersonHints = birthPersonHints;
        asyncState = AsyncFetchState::READY;
        asyncError = "";
        LOG_INFO("[AiWeatherSource] Async result ready (%d weather bytes, %d hint chars)",
                 weatherJson.length(), birthPersonHints.length());
    }
    asyncTaskHandle = nullptr;
    asyncStopRequested = false;
}

void AiWeatherSource::setAsyncResultError(const String& error)
{
    concurrency::LockGuard guard(&asyncStateLock);
    const bool wasStopRequested = asyncStopRequested;
    asyncWeatherJson = "";
    asyncBirthPersonHints = "";
    asyncError = wasStopRequested ? "" : error;
    asyncTaskHandle = nullptr;
    asyncStopRequested = false;
    asyncState = wasStopRequested ? AsyncFetchState::IDLE : AsyncFetchState::FAILED;
    if (wasStopRequested) {
        LOG_DEBUG("[AiWeatherSource] Async fetch stopped before completing");
    } else {
        LOG_WARN("[AiWeatherSource] Async fetch failed: %s", error.c_str());
    }
}
#else
bool AiWeatherSource::isAsyncFetchInProgress() const
{
    return false;
}

bool AiWeatherSource::isAsyncFetchReady() const
{
    return false;
}
#endif

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

String AiWeatherSource::buildWeatherPrompt(const String& weatherJson, const String& tomorrowDate, const String& birthDate, const String& birthPersonHints) const
{
    String prompt = String("Jesteś kreatywnym asystentem AI specjalizującym się w tworzeniu polskich prognoz pogody w stylu historycznych postaci.\n\n") +
                    "Wykonaj dokładnie te kroki:\n\n" +
                    "1. Wybierz losowo jedną sławną postać historyczną urodzoną " + birthDate + ". Osoba może pochodzić z dowolnego kraju, ale musi być rozpoznawalna i znana Polakom. Wybierz kogoś o charakterystycznym stylu wyrażania się - może to być styl pisania, mówienia, tworzenia muzyki, malowania, czy inne formy artystycznego wyrazu.\n"
                    "BEZWZGLĘDNIE WAŻNE: chodzi o datę URODZIN (nie śmierci, nie imienin). Postać musi mieć datę urodzenia " + birthDate + ".\n\n";

    if (birthPersonHints.length() > 0) {
        prompt += "2. Wybierz jedną osobę z poniższej listy osób urodzonych " + birthDate + ":\n\n";
        prompt += birthPersonHints + "\n\n";
        prompt += "Jeśli żadna z tych osób nie pasuje, możesz wybrać inną znaną postać o podobnym, wyrazistym stylu. "
                  "MUSISZ jednak wybrać postać urodzoną DOKŁADNIE " + birthDate + " (data urodzenia, nie śmierci).\n\n";
    } else {
        prompt += "2. Wybierz dowolną, rozpoznawalną postać historyczną o charakterystycznym stylu, "
                  "ale MUSISZ wybrać osobę urodzoną DOKŁADNIE " + birthDate + " (data urodzenia, nie śmierci).\n\n";
    }

    prompt += "3. Przeanalizuj poniższe rzeczywiste dane pogodowe dla Poznania na jutro (" + tomorrowDate + ") z Open-Meteo API:\n\n" +
              weatherJson + "\n\n" +
              "Dane zawierają:\n" +
              "- hourly.temperature_2m: temperatura na różnych godzinach (°C)\n" +
              "- hourly.precipitation_probability: prawdopodobieństwo opadów (%)\n" +
              "- hourly.precipitation: ilość opadów (mm)\n" +
              "- hourly.wind_speed_10m: prędkość wiatru (km/h)\n" +
              "- hourly.wind_direction_10m: kierunek wiatru (°)\n" +
              "- hourly.cloud_cover: zachmurzenie (%)\n\n" +
              "4. Stwórz podsumowanie pogody na jutro, uwzględniając:\n" +
              "- Średnią temperaturę w dzień (godziny 6:00-18:00) i w nocy (godziny 18:00-6:00)\n" +
              "- Maksymalne prawdopodobieństwo opadów i ich ilość\n" +
              "- Średnią prędkość wiatru i dominujący kierunek\n" +
              "- Średnie zachmurzenie\n\n" +
              "5. Przepisz tę prognozę w charakterystycznym stylu wybranej postaci - użyj jej znanych powiedzeń, stylu językowego, gwary, idiomów, czy innych form artystycznego wyrazu. Spraw, aby prognoza brzmiała tak, jakby została napisana lub wypowiedziana przez tę osobę.\n\n" +
              "6. Odpowiedz WYŁĄCZNIE w tym formacie (nic więcej, bez markdown):\n" +
              "{Imię i nazwisko postaci}: {prognoza w jej stylu}\n\n" +
              "FORMAT PRZYKŁADU (tylko dla pokazania formatu - NIE KOPIUJ treści):\n" +
              "Maria Skłodowska-Curie: Dzisiaj będzie słonecznie z temperaturą 15-18°C w dzień i 8-10°C w nocy.\n\n" +
              "UWAGA: NIE KOPIUJ przykładu dosłownie! Użyj prawdziwych danych pogodowych z JSON powyżej. Przeanalizuj liczby i stwórz oryginalną prognozę.\n\n" +
              "WAŻNE: Całkowita długość odpowiedzi musi być bliska, ale nie może przekroczyć " + String(MAX_MESSAGE_BYTES - 25) + " znaków.\n\n" +
              "WAŻNE: Odpowiedź musi zawierać analizę prawdziwych danych pogodowych, nie kopię przykładu.\n\n" +
              "BEZWZGLĘDNIE WAŻNE: Po wygenerowaniu odpowiedzi sprawdź ją jeszcze raz. Jeśli jej całkowita długość przekracza " + String(MAX_MESSAGE_BYTES - 25) + " znaków, edytuj ją i skróć do MAKSIMUM " + String(MAX_MESSAGE_BYTES - 25) + " znaków.\n\n" +
              "BEZWZGLĘDNIE ZAKAZANE: NIE DODAWAJ żadnych meta-informacji do odpowiedzi! NIE podawaj liczby znaków, słów, bajtów. NIE dodawaj komentarzy w nawiasach typu '(X znaków)', '(X słów)'. NIE wyjaśniaj formatu. Zwróć TYLKO prognozę pogody w wymaganym formacie, nic więcej.";

    return prompt;
}

String AiWeatherSource::buildWikidataBirthdayQuery(int month, int day) const
{
    String query;
    query.reserve(900);

    query += "SELECT DISTINCT ?personLabel ?personDescription\n";
    query += "WHERE {\n";
    query += "  ?person wdt:P31 wd:Q5 .\n";
    query += "  ?person p:P569/psv:P569 [wikibase:timePrecision 11; wikibase:timeValue ?dob] .\n";
    query += "  FILTER(MONTH(?dob) = " + String(month) + " && DAY(?dob) = " + String(day) + ") .\n";
    query += "  ?person wdt:P19/wdt:P17 wd:Q36 .\n";
    query += "  VALUES ?culturalOccupation {\n";
    query += "    wd:Q483501 wd:Q36180 wd:Q49757 wd:Q33999 wd:Q639669\n";
    query += "    wd:Q1028181 wd:Q177220 wd:Q214917 wd:Q10737 wd:Q1281618\n";
    query += "  }\n";
    query += "  ?person wdt:P106 ?occ .\n";
    query += "  ?occ wdt:P279* ?culturalOccupation .\n";
    query += "  ?person rdfs:label ?personLabel .\n";
    query += "  FILTER(LANG(?personLabel) = \"pl\")\n";
    query += "  SERVICE wikibase:label { bd:serviceParam wikibase:language \"pl,en\". }\n";
    query += "}\n";
    query += "ORDER BY DESC(?dob)\n";
    query += "LIMIT " + String(WIKIDATA_QUERY_LIMIT) + "\n";

    return query;
}

String AiWeatherSource::urlEncode(const String& value) const
{
    String encoded;
    encoded.reserve(value.length() * 3);

    const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < value.length(); i++) {
        unsigned char c = static_cast<unsigned char>(value.charAt(i));
        bool shouldKeep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';

        if (shouldKeep) {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[(c >> 4) & 0xF];
            encoded += hex[c & 0xF];
        }
    }

    return encoded;
}

String AiWeatherSource::httpGetLongTimeout(const String& url, int& httpCode, unsigned long timeoutMs) const
{
    String payload;
    httpCode = -1;
    const int maxBytes = WIKIDATA_MAX_RESPONSE_BYTES;

    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();
    http.begin(client, url.c_str());
    http.setTimeout(timeoutMs);
    http.addHeader("User-Agent", "Mozilla/5.0 (ESP32; AiWeatherSource)");
    http.addHeader("Accept", "application/sparql-results+json");
    http.addHeader("Accept-Encoding", "identity");

    int requestCode = http.GET();
    feedWatchdog();
    httpCode = requestCode;
    LOG_DEBUG("[AiWeatherSource] HTTP GET start (%s), requestCode=%d", url.c_str(), requestCode);

    if (httpCode != HTTP_CODE_OK) {
        if (httpCode > 0) {
            LOG_WARN("[AiWeatherSource] Wikidata query request returned HTTP %d", httpCode);
        } else {
            LOG_ERROR("[AiWeatherSource] Wikidata query request failed with code %d", httpCode);
        }
        http.end();
        return payload;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        // Fallback for non-streaming client paths
        payload = http.getString();
        http.end();
        return payload;
    }

    payload.reserve(4096);
    unsigned long startTime = millis();
    unsigned long lastWatchdogFeed = millis();
    char buffer[WIKIDATA_READ_CHUNK_BYTES];

    while ((stream->connected() || stream->available()) && payload.length() < maxBytes) {
        while (stream->available()) {
            size_t availableBytes = (size_t)stream->available();
            if (availableBytes > sizeof(buffer)) {
                availableBytes = sizeof(buffer);
            }

            size_t remainingBytes = (size_t)maxBytes - payload.length();
            if (availableBytes > remainingBytes) {
                availableBytes = remainingBytes;
            }

            if (availableBytes == 0) {
                break;
            }

            int bytesRead = stream->readBytes(buffer, (int)availableBytes);
            if (bytesRead <= 0) {
                break;
            }

            if (payload.length() + (size_t)bytesRead <= (size_t)maxBytes) {
                payload.concat(buffer, bytesRead);
            } else {
                size_t canAdd = (size_t)maxBytes - payload.length();
                if (canAdd > 0) {
                    payload.concat(buffer, (int)canAdd);
                }
                break;
            }
        }

        if (millis() - lastWatchdogFeed > 500) {
            feedWatchdog();
            yield();
            lastWatchdogFeed = millis();
        }

        if (millis() - startTime > timeoutMs) {
            LOG_WARN("[AiWeatherSource] Wikidata response read timed out after %lu ms", timeoutMs);
            break;
        }

        if (!stream->available()) {
            yieldMillis(1);
        }
    }

    if (payload.length() >= (size_t)maxBytes) {
        LOG_WARN("[AiWeatherSource] Wikidata response truncated to %d bytes", maxBytes);
    }

    if (payload.length() == 0) {
        LOG_WARN("[AiWeatherSource] Wikidata response is empty");
    }

    LOG_INFO("[AiWeatherSource] HTTP GET done (%s), code=%d, received=%d bytes, elapsed=%lu ms",
             httpCode == HTTP_CODE_OK ? "success" : "failed", httpCode, payload.length(),
             millis() - startTime);

    http.end();
    return payload;
}

bool AiWeatherSource::parseBirthPersonHints(const String& payload, const String& birthDate, String& outHints) const
{
    // Fallback for environments where streaming parser input is not available
    size_t parseCapacity = payload.length() * 2 + 2048;
    if (parseCapacity < 8192) {
        parseCapacity = 8192;
    }
    if (parseCapacity > 32768) {
        parseCapacity = 32768;
    }
    DynamicJsonDocument doc(parseCapacity);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        LOG_ERROR("[AiWeatherSource] Failed to parse Wikidata JSON response: %s", error.c_str());
        return false;
    }

    JsonArray bindings = doc["results"]["bindings"].as<JsonArray>();
    if (bindings.isNull() || bindings.size() == 0) {
        LOG_WARN("[AiWeatherSource] Wikidata response had no bindings for %s", birthDate.c_str());
        return false;
    }

    String hints = "Lista kandydatów urodzonych " + birthDate + ":\n";
    int added = 0;

    for (JsonObject binding : bindings) {
        if (added >= WIKIDATA_MAX_CANDIDATES) {
            break;
        }

        const char* label = binding["personLabel"]["value"].as<const char*>();
        const char* description = binding["personDescription"]["value"].as<const char*>();

        if (label == nullptr || label[0] == '\0') {
            continue;
        }

        String line = "- " + String(added + 1) + ". " + String(label);
        if (description != nullptr && description[0] != '\0') {
            line += " — " + String(description);
        }

        if (hints.length() + line.length() > (size_t)WIKIDATA_MAX_PROMPT_CHARS) {
            break;
        }

        hints += line;
        hints += "\n";
        added++;
    }

    if (added == 0) {
        LOG_WARN("[AiWeatherSource] No usable person entries in Wikidata response for %s", birthDate.c_str());
        return false;
    }

    outHints = hints;
    return true;
}

bool AiWeatherSource::parseBirthPersonHints(WiFiClient* stream, const String& birthDate, String& outHints) const
{
    if (stream == nullptr) {
        LOG_WARN("[AiWeatherSource] Wikidata stream is null");
        return false;
    }

    LOG_DEBUG("[AiWeatherSource] Starting streaming SPARQL parsing");

    StaticJsonDocument<256> filter;
    filter["personLabel"]["value"] = true;
    filter["personDescription"]["value"] = true;

    auto waitForData = [&](unsigned long timeoutMs) -> bool {
        unsigned long startTime = millis();
        while (!stream->available()) {
            if (millis() - startTime > timeoutMs) {
                return false;
            }
            if (!stream->connected()) {
                return false;
            }
            feedWatchdog();
            yieldMillis(1);
        }
        return true;
    };

    if (!waitForData(2000) || !stream->find("\"results\"")) {
        LOG_WARN("[AiWeatherSource] Could not locate results object in Wikidata response");
        return false;
    }

    if (!stream->find("\"bindings\"")) {
        LOG_WARN("[AiWeatherSource] Could not locate bindings array in Wikidata response");
        return false;
    }

    while (stream->available() && isspace(stream->peek())) {
        stream->read();
    }

    if (stream->peek() < 0) {
        LOG_WARN("[AiWeatherSource] Stream closed before bindings data for %s", birthDate.c_str());
        return false;
    }

    if (!waitForData(2000)) {
        LOG_WARN("[AiWeatherSource] Stream closed while looking for bindings array for %s", birthDate.c_str());
        return false;
    }

    if (stream->peek() == ':') {
        stream->read();
        while (stream->available() && isspace(stream->peek())) {
            stream->read();
        }
    }

    if (stream->peek() != '[') {
        LOG_WARN("[AiWeatherSource] Binding value is not an array for %s (peek '%c')", birthDate.c_str(), stream->peek());
        return false;
    }
    stream->read();

    while (stream->available() && isspace(stream->peek())) {
        stream->read();
    }
    if (!stream->available()) {
        LOG_WARN("[AiWeatherSource] Stream closed before first bindings entry for %s", birthDate.c_str());
        return false;
    }
    if (stream->peek() == ']') {
        LOG_WARN("[AiWeatherSource] Wikidata response has no bindings for %s", birthDate.c_str());
        return false;
    }

    String hints = "Lista kandydatów urodzonych " + birthDate + ":\n";
    int parsed = 0;
    int added = 0;
    unsigned long startTime = millis();
    DynamicJsonDocument doc(4096);

    do {
        doc.clear();
        DeserializationError error = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));

        if (error) {
            if (error == DeserializationError::EmptyInput) {
                break;
            }
            LOG_DEBUG("[AiWeatherSource] Failed to parse one bindings object: %s", error.c_str());
        } else {
            parsed++;

            const char* label = doc["personLabel"]["value"].as<const char*>();
            const char* description = doc["personDescription"]["value"].as<const char*>();

            if (label != nullptr && label[0] != '\0') {
                String line = "- " + String(added + 1) + ". " + String(label);
                if (description != nullptr && description[0] != '\0') {
                    line += " — " + String(description);
                }

                if (hints.length() + line.length() > (size_t)WIKIDATA_MAX_PROMPT_CHARS) {
                    LOG_DEBUG("[AiWeatherSource] Hints list reached prompt limit (%d chars)", hints.length());
                    break;
                }

                hints += line;
                hints += "\n";
                added++;
            }

            if (added >= WIKIDATA_MAX_CANDIDATES) {
                break;
            }
        }

        if (millis() - startTime > WIKIDATA_QUERY_TIMEOUT_MS) {
            LOG_WARN("[AiWeatherSource] Streaming Wikidata parse timed out after %lu ms",
                     WIKIDATA_QUERY_TIMEOUT_MS);
            break;
        }

        feedWatchdog();
        yieldMillis(1);
    } while (stream->findUntil(",", "]"));

    if (parsed == 0) {
        LOG_WARN("[AiWeatherSource] No binding objects were parsed from Wikidata response");
        return false;
    }

    if (added == 0) {
        LOG_WARN("[AiWeatherSource] No usable person entries in streaming Wikidata response for %s", birthDate.c_str());
        return false;
    }

    outHints = hints;
    LOG_INFO("[AiWeatherSource] Parsed %d bindings, kept %d candidate hints", parsed, added);
    return true;
}

bool AiWeatherSource::fetchBirthPersonHints(int month, int day, const String& birthDate, String& outHints) const
{
    outHints = "";
    const String query = buildWikidataBirthdayQuery(month, day);
    const String encodedQuery = urlEncode(query);
    const String url = "https://query.wikidata.org/sparql?query=" + encodedQuery + "&format=json";

    for (int attempt = 1; attempt <= WIKIDATA_QUERY_MAX_RETRIES; attempt++) {
        int httpCode = 0;
        WiFiClientSecure client;
        HTTPClient http;
        client.setInsecure();
        http.begin(client, url.c_str());
        http.setTimeout(WIKIDATA_QUERY_TIMEOUT_MS);
        http.addHeader("User-Agent", "Mozilla/5.0 (ESP32; AiWeatherSource)");
        http.addHeader("Accept", "application/sparql-results+json");
        http.addHeader("Accept-Encoding", "identity");

        int requestCode = http.GET();
        feedWatchdog();
        httpCode = requestCode;

        if (httpCode != HTTP_CODE_OK) {
            if (httpCode > 0) {
                LOG_WARN("[AiWeatherSource] Wikidata query failed (attempt %d/%d, HTTP %d)",
                         attempt, WIKIDATA_QUERY_MAX_RETRIES, httpCode);
            } else {
                LOG_ERROR("[AiWeatherSource] Wikidata query failed to execute (attempt %d/%d, code %d)",
                          attempt, WIKIDATA_QUERY_MAX_RETRIES, httpCode);
            }
            http.end();
            if (attempt < WIKIDATA_QUERY_MAX_RETRIES) {
                yieldMillis(15000);
            }
            continue;
        }

        WiFiClient* stream = http.getStreamPtr();
        String hints;
        bool parsed = false;

        if (stream != nullptr) {
            parsed = parseBirthPersonHints(stream, birthDate, hints);
            if (!parsed) {
                String response = http.getString();
                if (response.length() > 0) {
                    LOG_DEBUG("[AiWeatherSource] Falling back to buffered Wikidata parse after streaming parser failed");
                    LOG_DEBUG("[AiWeatherSource] Buffered fallback payload size: %d bytes", response.length());
                    LOG_DEBUG("[AiWeatherSource] Buffered response preview: %s",
                              response.substring(0, (response.length() > 180) ? 180 : response.length()).c_str());
                    parsed = parseBirthPersonHints(response, birthDate, hints);
                } else {
                    LOG_WARN("[AiWeatherSource] Streaming parser failed and no buffered payload was available");
                }
            } else if (hints.length() == 0) {
                parsed = false;
            }
        } else {
            LOG_WARN("[AiWeatherSource] Wikidata response stream unavailable, falling back to buffered parse");
            String response = http.getString();
            parsed = parseBirthPersonHints(response, birthDate, hints);
        }

        http.end();

        if (parsed && hints.length() > 0) {
            outHints = hints;
            LOG_DEBUG("[AiWeatherSource] Wikidata query succeeded on attempt %d", attempt);
            return true;
        }

        LOG_WARN("[AiWeatherSource] Wikidata response parse failed (attempt %d/%d)",
                 attempt, WIKIDATA_QUERY_MAX_RETRIES);

        if (attempt < WIKIDATA_QUERY_MAX_RETRIES) {
            yieldMillis(15000);
        }
    }

    return false;
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


#endif // HAS_ALERTING && !MESHTASTIC_EXCLUDE_ALERT_AIWEATHER
