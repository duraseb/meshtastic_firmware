#if defined(HAS_ALERTING) && HAS_ALERTING

#include "AIService.h"
#include "main.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <ctype.h>
#include <ArduinoJson.h>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#include <esp_heap_caps.h>
#endif

// Helper to reset watchdog timer (architecture-aware)
static inline void feedWatchdog()
{
#ifdef ARCH_ESP32
    esp_task_wdt_reset();
#endif
}

AIService* aiService = nullptr;

AIService::AIService() : requestDoc(JSON_DOC_SIZE)
{
    currentAIProviderIndex = 0;
    initializeProviders();
}

AIService::~AIService()
{
}

void AIService::initializeProviders()
{
    aiProviders[0].name = "Gemini-1.5";
    aiProviders[0].endpoint = "https://generativelanguage.googleapis.com/v1/models/gemini-1.5-flash:generateContent";
    #ifdef GEMINI_API_KEY
    aiProviders[0].apiKey = GEMINI_API_KEY;
    #else
    aiProviders[0].apiKey = "";
    #endif
    aiProviders[0].requestFormat = "gemini";

    aiProviders[1].name = "Gemini-2.5-Fallback";
    aiProviders[1].endpoint = "https://generativelanguage.googleapis.com/v1/models/gemini-2.5-flash-lite:generateContent";
    #ifdef GEMINI_API_KEY
    aiProviders[1].apiKey = GEMINI_API_KEY;
    #else
    aiProviders[1].apiKey = "";
    #endif
    aiProviders[1].requestFormat = "gemini";

    aiProviders[2].name = "Mistral-7B";
    aiProviders[2].endpoint = "https://api.mistral.ai/v1/chat/completions";
    aiProviders[2].model = "open-mistral-7b";
    #ifdef MISTRAL_API_KEY
    aiProviders[2].apiKey = MISTRAL_API_KEY;
    #else
    aiProviders[2].apiKey = "";
    #endif
    aiProviders[2].requestFormat = "mistral";

    aiProviders[3].name = "Groq";
    aiProviders[3].endpoint = "https://api.groq.com/openai/v1/chat/completions";
    aiProviders[3].model = "llama-3.3-70b-versatile";
    #ifdef GROQ_API_KEY
    aiProviders[3].apiKey = GROQ_API_KEY;
    #else
    aiProviders[3].apiKey = "";
    #endif
    aiProviders[3].requestFormat = "groq";

    aiProviders[4].name = "GPT-4o mini";
    aiProviders[4].endpoint = "https://api.openai.com/v1/chat/completions";
    aiProviders[4].model = "gpt-4o-mini";
    #ifdef OPENAI_API_KEY
    aiProviders[4].apiKey = OPENAI_API_KEY;
    #else
    aiProviders[4].apiKey = "";
    #endif
    aiProviders[4].requestFormat = "openai";

    LOG_INFO("[AIService] Initialized with %d AI providers", MAX_AI_PROVIDERS);
}

String AIService::callAI(const String& prompt)
{
    LOG_DEBUG("[AIService] Calling AI with prompt length: %d", prompt.length());

    // Try each configured AI provider until one succeeds
    for (int providerIdx = 0; providerIdx < MAX_AI_PROVIDERS; providerIdx++) {
        AIProvider& provider = aiProviders[providerIdx];

        // Skip if provider not configured
        if (provider.endpoint.length() == 0 || provider.apiKey.length() == 0) {
            LOG_DEBUG("[AIService] Skipping provider %s (not configured)", provider.name.c_str());
            continue;
        }

        LOG_INFO("[AIService] Attempting AI call with [%s]...", provider.name.c_str());

        String response;
        bool success = false;

        if (provider.requestFormat == "gemini") {
            success = callGeminiAPI(provider, prompt, response);
        } else if (provider.requestFormat == "groq") {
            success = callGroqAPI(provider, prompt, response);
        } else if (provider.requestFormat == "openai") {
            success = callOpenAIAPI(provider, prompt, response);
        } else if (provider.requestFormat == "mistral") {
            success = callMistralAPI(provider, prompt, response);
        }

        if (success) {
            LOG_INFO("[AIService] AI call successful with [%s] (response length: %d)", provider.name.c_str(), response.length());

        // Debug: Log first 200 chars of response for troubleshooting
        if (response.length() > 200) {
            LOG_DEBUG("[AIService] Response preview: %s...", response.substring(0, 200).c_str());
        } else {
            LOG_DEBUG("[AIService] Full response: %s", response.c_str());
        }

        currentAIProviderIndex = providerIdx; // Remember successful provider for next time
        return response;
        } else {
            LOG_WARN("[AIService] Provider [%s] failed, trying next fallback...", provider.name.c_str());
        }
    }

    LOG_ERROR("[AIService] All AI providers failed");
    return "";
}

bool AIService::hasConfiguredProviders() const
{
    for (int i = 0; i < MAX_AI_PROVIDERS; i++) {
        if (aiProviders[i].apiKey.length() > 0) {
            return true;
        }
    }
    return false;
}

int AIService::getConfiguredProviderCount() const
{
    int configuredCount = 0;
    for (int i = 0; i < MAX_AI_PROVIDERS; i++) {
        if (aiProviders[i].apiKey.length() > 0) {
            configuredCount++;
        }
    }
    return configuredCount;
}

bool AIService::processHttpResponse(HTTPClient& http, int httpCode, const String& providerName, String& outResponse)
{
    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        outResponse = http.getString();
        LOG_DEBUG("[AIService] %s response received (%d bytes)", providerName.c_str(), outResponse.length());
        success = true;
    } else {
        String errorResponse = http.getString();
        if (errorResponse.length() > 0) {
            LOG_ERROR("[AIService] %s request failed with HTTP code %d. Response: %s",
                     providerName.c_str(), httpCode, errorResponse.c_str());
        } else {
            LOG_ERROR("[AIService] %s request failed with HTTP code %d (no response body)",
                     providerName.c_str(), httpCode);
        }
    }
    http.end();

    return success;
}

bool AIService::callGeminiAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    // Call AI endpoint with POST
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    // Add API key to URL as query parameter for Gemini API
    String url = provider.endpoint;
    if (provider.apiKey.length() > 0) {
        url += "?key=" + provider.apiKey;
    }
    http.begin(client, url.c_str());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Gemini API request using reusable JSON document
    requestDoc.clear();

    JsonArray contents = requestDoc.createNestedArray("contents");
    JsonObject contentObj = contents.createNestedObject();
    JsonArray parts = contentObj.createNestedArray("parts");
    JsonObject partObj = parts.createNestedObject();

    // ArduinoJson handles escaping automatically
    partObj["text"] = prompt;

    String body;
    serializeJson(requestDoc, body);

    LOG_DEBUG("[AIService] Sending Gemini request (prompt length: %d, body length: %d)", prompt.length(), body.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    return processHttpResponse(http, httpCode, "Gemini", outResponse);
}

bool AIService::callMistralAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Mistral API request using reusable JSON document
    requestDoc.clear();

    requestDoc["model"] = provider.model;

    JsonArray messages = requestDoc.createNestedArray("messages");
    JsonObject messageObj = messages.createNestedObject();
    messageObj["role"] = "user";
    messageObj["content"] = prompt; // ArduinoJson handles escaping automatically

    requestDoc["temperature"] = 0.1;
    requestDoc["max_tokens"] = 500;

    String body;
    serializeJson(requestDoc, body);

    LOG_DEBUG("[AIService] Sending Mistral request to %s (prompt length: %d, body length: %d)", provider.name.c_str(), prompt.length(), body.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    return processHttpResponse(http, httpCode, provider.name, outResponse);
}

bool AIService::callGroqAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Groq API request using reusable JSON document
    requestDoc.clear();

    requestDoc["model"] = provider.model;

    JsonArray messages = requestDoc.createNestedArray("messages");
    JsonObject messageObj = messages.createNestedObject();
    messageObj["role"] = "user";
    messageObj["content"] = prompt; // ArduinoJson handles escaping automatically

    requestDoc["temperature"] = 0.1;
    requestDoc["max_tokens"] = 500;

    String body;
    serializeJson(requestDoc, body);

    LOG_DEBUG("[AIService] Sending Groq request to %s (prompt length: %d, body length: %d)", provider.name.c_str(), prompt.length(), body.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    return processHttpResponse(http, httpCode, provider.name, outResponse);
}

bool AIService::callOpenAIAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build OpenAI API request using reusable JSON document
    requestDoc.clear();

    requestDoc["model"] = provider.model;

    JsonArray messages = requestDoc.createNestedArray("messages");
    JsonObject messageObj = messages.createNestedObject();
    messageObj["role"] = "user";
    messageObj["content"] = prompt; // ArduinoJson handles escaping automatically

    requestDoc["temperature"] = 0.1;
    requestDoc["max_tokens"] = 500;

    String body;
    serializeJson(requestDoc, body);

    LOG_DEBUG("[AIService] Sending OpenAI request to %s (prompt length: %d, body length: %d)", provider.name.c_str(), prompt.length(), body.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    return processHttpResponse(http, httpCode, provider.name, outResponse);
}

String AIService::httpPost(const char* url, const String& payload, const String& authHeader)
{
    if (!WiFi.isConnected()) {
        LOG_DEBUG("[AIService] WiFi not available for HTTP request");
        return "";
    }

    WiFiClientSecure client;
    HTTPClient http;
    client.setInsecure();

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    if (authHeader.length() > 0) {
        http.addHeader("Authorization", authHeader);
    }
    http.setTimeout(AI_TIMEOUT_MS);

    int httpCode = http.POST(payload);
    String response = "";

    if (httpCode == HTTP_CODE_OK) {
        response = http.getString();
    }

    http.end();
    return response;
}

bool AIService::callProvider(int providerIdx, const String& prompt, String& outResponse)
{
    if (providerIdx < 0 || providerIdx >= MAX_AI_PROVIDERS) {
        return false;
    }

    AIProvider& provider = aiProviders[providerIdx];

    // Skip if provider not configured
    if (provider.endpoint.length() == 0 || provider.apiKey.length() == 0) {
        return false;
    }

    if (provider.requestFormat == "gemini") {
        return callGeminiAPI(provider, prompt, outResponse);
    } else if (provider.requestFormat == "groq") {
        return callGroqAPI(provider, prompt, outResponse);
    } else if (provider.requestFormat == "openai") {
        return callOpenAIAPI(provider, prompt, outResponse);
    } else if (provider.requestFormat == "mistral") {
        return callMistralAPI(provider, prompt, outResponse);
    }

    return false;
}

bool AIService::extractTextFromAIResponse(const String& response, String& outText)
{
    // Use ArduinoJson to parse the response (use heap allocation to avoid stack overflow)
    // Typical AI responses are 2-6KB, allocate 8KB on heap for safety
    DynamicJsonDocument doc(8192);

    // Parse the JSON response
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        LOG_ERROR("[AIService::extractTextFromAIResponse] JSON parsing failed: %s", error.c_str());
        return false;
    }

    // JSON parsing successful

    // Try to extract text content from different AI provider formats
    const char* text = nullptr;

    // Check for OpenAI-compatible format (Mistral, Groq)
    if (doc.containsKey("choices") && doc["choices"].is<JsonArray>() && doc["choices"].size() > 0) {
        JsonVariant choice = doc["choices"][0];
        if (choice.containsKey("message")) {
            JsonVariant message = choice["message"];
            if (message.containsKey("content")) {
                text = message["content"];
            }
        }
    }
    // Check for Gemini format
    else if (doc.containsKey("candidates") && doc["candidates"].is<JsonArray>() && doc["candidates"].size() > 0) {
        LOG_DEBUG("[AIService::extractTextFromAIResponse] Detected Gemini format");

        JsonVariant candidate = doc["candidates"][0];

        // Try different Gemini response structures
        if (candidate.containsKey("content")) {
            JsonVariant content = candidate["content"];

            // Format 1: candidates[0].content.parts[0].text
            if (content.containsKey("parts") && content["parts"].is<JsonArray>() && content["parts"].size() > 0) {
                JsonVariant part = content["parts"][0];
                if (part.containsKey("text")) {
                    text = part["text"];
                    LOG_DEBUG("[AIService::extractTextFromAIResponse] Extracted text from candidates[0].content.parts[0].text");
                }
            }
            // Format 2: candidates[0].content.text
            else if (content.containsKey("text")) {
                text = content["text"];
                LOG_DEBUG("[AIService::extractTextFromAIResponse] Extracted text from candidates[0].content.text");
            }
        }
        // Format 3: candidates[0].text (fallback)
        else if (candidate.containsKey("text")) {
            text = candidate["text"];
            LOG_DEBUG("[AIService::extractTextFromAIResponse] Extracted text from candidates[0].text");
        }
    }

    // Check if we successfully extracted text
    if (text == nullptr) {
        LOG_ERROR("[AIService::extractTextFromAIResponse] Could not find text content in AI response");
        return false;
    }

    // Convert to Arduino String - return raw text as-is
    // Individual sources will specify how to parse their expected formats
    String extractedText = String(text);

    if (extractedText.length() == 0) {
        LOG_ERROR("[AIService::extractTextFromAIResponse] Extracted text is empty");
        return false;
    }

    outText = extractedText;
    LOG_DEBUG("[AIService::extractTextFromAIResponse] Successfully extracted text (length: %d)", outText.length());
    return true;
}

#endif // HAS_ALERTING
