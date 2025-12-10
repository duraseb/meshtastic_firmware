#include "AIService.h"
#include "main.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <ctype.h>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

// Helper to reset watchdog timer (architecture-aware)
static inline void feedWatchdog()
{
#ifdef ARCH_ESP32
    esp_task_wdt_reset();
#endif
}

AIService* aiService = nullptr;

AIService::AIService()
{
    currentAIProviderIndex = 0;
    initializeProviders();
}

AIService::~AIService()
{
}

void AIService::initializeProviders()
{
    // AI provider fallback chain (Gemini → Perplexity → Mistral → Groq)
    aiProviders[0].name = "Gemini-2.5";
    aiProviders[0].endpoint = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent";
    #ifdef GEMINI_API_KEY
    aiProviders[0].apiKey = GEMINI_API_KEY;
    #else
    aiProviders[0].apiKey = "";
    #endif
    aiProviders[0].requestFormat = "gemini";

    aiProviders[1].name = "Perplexity-Sonar";
    aiProviders[1].endpoint = "https://api.perplexity.ai/chat/completions";
    aiProviders[1].model = "sonar";
    #ifdef PERPLEXITY_API_KEY
    aiProviders[1].apiKey = PERPLEXITY_API_KEY;
    #else
    aiProviders[1].apiKey = "";
    #endif
    aiProviders[1].requestFormat = "perplexity";

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
        } else if (provider.requestFormat == "perplexity") {
            success = callMistralAPI(provider, prompt, response); // Reuse Mistral (OpenAI-compatible)
        } else if (provider.requestFormat == "mistral") {
            success = callMistralAPI(provider, prompt, response);
        } else if (provider.requestFormat == "groq") {
            success = callGroqAPI(provider, prompt, response);
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

bool AIService::callGeminiAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    // Call AI endpoint with POST
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    // Add API key to URL as query parameter for Gemini API
    String url = provider.endpoint;
    if (provider.apiKey.length() > 0) {
        url += "?key=" + provider.apiKey;
    }
    http.begin(client, url.c_str());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Gemini API request format
    String body = "{\"contents\":[{\"parts\":[{\"text\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}]}]}";

    LOG_DEBUG("[AIService] Sending Gemini request (prompt length: %d)", prompt.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        outResponse = http.getString();
        LOG_DEBUG("[AIService] Gemini response received (%d bytes)", outResponse.length());
        success = true;
    } else {
        String errorResponse = http.getString();
        LOG_ERROR("[AIService] Gemini request failed with HTTP code %d. Response: %s",
                 httpCode, errorResponse.c_str());
    }
    http.end();

    return success;
}

bool AIService::callMistralAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Mistral API request format (OpenAI-compatible)
    String body = "{\"model\":\"" + provider.model + "\",\"messages\":[{\"role\":\"user\",\"content\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}],\"temperature\":0.1,\"max_tokens\":500}";

    LOG_DEBUG("[AIService] Sending Mistral request to %s (prompt length: %d)", provider.name.c_str(), prompt.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        outResponse = http.getString();
        LOG_DEBUG("[AIService] Mistral response received (%d bytes)", outResponse.length());
        success = true;
    } else {
        String errorResponse = http.getString();
        LOG_ERROR("[AIService] Mistral request to %s failed with HTTP code %d. Response: %s",
                 provider.name.c_str(), httpCode, errorResponse.c_str());
    }
    http.end();

    return success;
}

bool AIService::callGroqAPI(const AIProvider& provider, const String& prompt, String& outResponse)
{
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    http.begin(client, provider.endpoint.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + provider.apiKey);
    http.setTimeout(AI_TIMEOUT_MS);

    // Build Groq API request format (OpenAI-compatible)
    String body = "{\"model\":\"" + provider.model + "\",\"messages\":[{\"role\":\"user\",\"content\":\"";

    // Escape prompt for JSON
    for (int i = 0; i < prompt.length(); i++) {
        char c = prompt.charAt(i);
        if (c == '"') {
            body += "\\\"";
        } else if (c == '\\') {
            body += "\\\\";
        } else if (c == '\n') {
            body += "\\n";
        } else if (c == '\r') {
            body += "\\r";
        } else {
            body += c;
        }
    }
    body += "\"}],\"temperature\":0.1,\"max_tokens\":500}";

    LOG_DEBUG("[AIService] Sending Groq request to %s (prompt length: %d)", provider.name.c_str(), prompt.length());
    int httpCode = http.POST(body);

    // Reset watchdog after potentially long AI API call
    feedWatchdog();

    bool success = false;

    if (httpCode == HTTP_CODE_OK) {
        outResponse = http.getString();
        LOG_DEBUG("[AIService] Groq response received (%d bytes)", outResponse.length());
        success = true;
    } else {
        String errorResponse = http.getString();
        LOG_ERROR("[AIService] Groq request to %s failed with HTTP code %d. Response: %s",
                 provider.name.c_str(), httpCode, errorResponse.c_str());
    }
    http.end();

    return success;
}

String AIService::httpPost(const char* url, const String& payload, const String& authHeader)
{
    if (!WiFi.isConnected()) {
        LOG_DEBUG("[AIService] WiFi not available for HTTP request");
        return "";
    }

    HTTPClient http;
    WiFiClientSecure client;
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
    } else if (provider.requestFormat == "perplexity" || provider.requestFormat == "mistral") {
        return callMistralAPI(provider, prompt, outResponse);
    } else if (provider.requestFormat == "groq") {
        return callGroqAPI(provider, prompt, outResponse);
    }

    return false;
}

bool AIService::parseAIResponse(const String& response, String& outMessage)
{
    LOG_DEBUG("[AIService::parseAIResponse] Starting parsing (response length: %d)", response.length());

    // Check if response looks like valid JSON
    if (!response.startsWith("{")) {
        LOG_WARN("[AIService::parseAIResponse] Response doesn't start with '{' - might be HTML error or malformed (first 100 chars): %s",
                 response.length() > 100 ? response.substring(0, 100).c_str() : response.c_str());
        return false;
    }

    // Log response type detection
    bool hasCandidates = response.indexOf("\"candidates\"") >= 0;
    bool hasChoices = response.indexOf("\"choices\"") >= 0;

    LOG_DEBUG("[AIService::parseAIResponse] Response contains: candidates=%s, choices=%s",
              hasCandidates ? "YES" : "NO", hasChoices ? "YES" : "NO");

    String extractedText;

    // Prioritize OpenAI format first (Perplexity, Mistral, Groq), then Gemini
    if (hasChoices) {
        LOG_DEBUG("[AIService::parseAIResponse] Detected OpenAI format response");

        // OpenAI-compatible format (Perplexity, Mistral, Groq)
        // Format: {"choices":[{"message":{"content":"weather forecast text"}}]}

        int contentPos = response.indexOf("\"content\"");
        if (contentPos < 0) {
            // Sometimes it's just "content" without quotes, or different structure
            contentPos = response.indexOf("content");
            if (contentPos >= 0) {
                LOG_DEBUG("[AIService::parseAIResponse] Found unquoted 'content' field");
            }
        }

        if (contentPos < 0) {
            LOG_WARN("[AIService::parseAIResponse] 'content' field not found in OpenAI AI response (response length: %d)", response.length());
            // Log first 300 chars for debugging
            if (response.length() > 300) {
                LOG_DEBUG("[AIService::parseAIResponse] Response start: %s...", response.substring(0, 300).c_str());
            } else {
                LOG_DEBUG("[AIService::parseAIResponse] Full response: %s", response.c_str());
            }
            return false;
        }

        // Find the colon after "content"
        int colonPos = response.indexOf(':', contentPos);
        if (colonPos < 0) {
            LOG_WARN("[AIService::parseAIResponse] Colon not found after 'content' field");
            return false;
        }

        // Find the opening quote of the content value (skip whitespace)
        int textStart = colonPos + 1;
        while (textStart < response.length() && (response.charAt(textStart) == ' ' || response.charAt(textStart) == '\t' || response.charAt(textStart) == '\n' || response.charAt(textStart) == '\r')) {
            textStart++;
        }

        if (textStart >= response.length() || response.charAt(textStart) != '"') {
            LOG_WARN("[AIService::parseAIResponse] Opening quote not found for 'content' value");
            return false;
        }

        textStart++; // Skip the opening quote

        // Find the closing quote (handle escaped quotes)
        int textEnd = textStart;
        bool escaped = false;
        while (textEnd < response.length()) {
            char c = response.charAt(textEnd);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            }
            textEnd++;
        }

        if (textEnd >= response.length()) {
            LOG_WARN("[AIService::parseAIResponse] Closing quote not found for 'content' value");
            return false;
        }

        extractedText = response.substring(textStart, textEnd);

        // Handle JSON escapes
        extractedText.replace("\\n", "\n");
        extractedText.replace("\\\"", "\"");
        extractedText.replace("\\\\", "\\");

        LOG_DEBUG("[AIService::parseAIResponse] Successfully extracted text from OpenAI response");

    } else if (hasCandidates) {
        LOG_DEBUG("[AIService::parseAIResponse] Detected Gemini format response");

        // Try multiple ways to extract text from Gemini response
        // Format 1: {"candidates":[{"content":{"parts":[{"text":"..."}]}}]}
        // Format 2: {"candidates":[{"content":{"text":"..."}}]}
        // Format 3: {"candidates":[{"text":"..."}]}

        int textKeyPos = -1;
        bool foundText = false;

        // Try Format 1 first (nested parts)
        if (response.indexOf("\"parts\"") >= 0) {
            textKeyPos = response.indexOf("\"text\"", response.indexOf("\"parts\""));
            if (textKeyPos >= 0) {
                foundText = true;
                LOG_DEBUG("[AIService::parseAIResponse] Using Gemini format 1 (parts)");
            }
        }

        // Try Format 2 (direct content.text)
        if (!foundText && response.indexOf("\"content\"") >= 0) {
            textKeyPos = response.indexOf("\"text\"", response.indexOf("\"content\""));
            if (textKeyPos >= 0) {
                foundText = true;
                LOG_DEBUG("[AIService::parseAIResponse] Using Gemini format 2 (content.text)");
            }
        }

        // Try Format 3 (direct candidates.text)
        if (!foundText) {
            textKeyPos = response.indexOf("\"text\"", response.indexOf("\"candidates\""));
            if (textKeyPos >= 0) {
                foundText = true;
                LOG_DEBUG("[AIService::parseAIResponse] Using Gemini format 3 (candidates.text)");
            }
        }

        if (!foundText) {
            LOG_WARN("[AIService::parseAIResponse] No supported 'text' field found in Gemini AI response (response length: %d)", response.length());
            // Log first 300 chars for debugging
            if (response.length() > 300) {
                LOG_DEBUG("[AIService::parseAIResponse] Response start: %s...", response.substring(0, 300).c_str());
            } else {
                LOG_DEBUG("[AIService::parseAIResponse] Full response: %s", response.c_str());
            }
            return false;
        }

        // Parse the text value
        int colonPos = response.indexOf(':', textKeyPos);
        if (colonPos < 0) {
            LOG_WARN("[AIService::parseAIResponse] Colon not found after 'text' field");
            return false;
        }

        // Find the opening quote of the text value (skip whitespace)
        int textStart = colonPos + 1;
        while (textStart < response.length() && (response.charAt(textStart) == ' ' || response.charAt(textStart) == '\t' || response.charAt(textStart) == '\n' || response.charAt(textStart) == '\r')) {
            textStart++;
        }

        if (textStart >= response.length() || response.charAt(textStart) != '"') {
            LOG_WARN("[AIService::parseAIResponse] Opening quote not found for 'text' value");
            return false;
        }

        textStart++; // Skip the opening quote

        // Find the closing quote (handle escaped quotes)
        int textEnd = textStart;
        bool escaped = false;
        while (textEnd < response.length()) {
            char c = response.charAt(textEnd);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            }
            textEnd++;
        }

        if (textEnd >= response.length()) {
            LOG_WARN("[AIService::parseAIResponse] Closing quote not found for 'text' value");
            return false;
        }

        extractedText = response.substring(textStart, textEnd);

        // Handle JSON escapes
        extractedText.replace("\\n", "\n");
        extractedText.replace("\\\"", "\"");
        extractedText.replace("\\\\", "\\");

        LOG_DEBUG("[AIService::parseAIResponse] Successfully extracted text from Gemini response");

    } else {
        LOG_WARN("[AIService::parseAIResponse] Unknown AI response format - neither Gemini nor OpenAI format detected (response length: %d)", response.length());
        // Log first 300 chars for debugging unknown formats
        if (response.length() > 300) {
            LOG_DEBUG("[AIService::parseAIResponse] Unknown response start: %s...", response.substring(0, 300).c_str());
        } else {
            LOG_DEBUG("[AIService::parseAIResponse] Unknown full response: %s", response.c_str());
        }
        return false;
    }

    outMessage = extractedText;
    LOG_DEBUG("[AIService::parseAIResponse] Successfully parsed AI response (message length: %d)", outMessage.length());
    return true;
}