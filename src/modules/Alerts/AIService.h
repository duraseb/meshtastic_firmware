#pragma once

#include <Arduino.h>
#include <functional>

/**
 * Shared AI service for interacting with various AI providers.
 * Provides a unified interface for different AI platforms with automatic fallback.
 */
class AIService {
public:
    // AI provider configuration
    struct AIProvider {
        String name;
        String endpoint;
        String apiKey;
        String requestFormat; // "gemini", "groq", or "mistral"
        String model;         // Model name for providers that need it (Groq, Mistral)
    };

    AIService();
    ~AIService();

    /**
     * Initialize AI providers from compile-time defines
     */
    void initializeProviders();

    /**
     * Call AI with a prompt and return the response
     * @param prompt The prompt to send to AI
     * @return AI response string, or empty string on failure
     */
    String callAI(const String& prompt);

    /**
     * Check if any AI providers are configured
     */
    bool hasConfiguredProviders() const;

    /**
     * Get count of configured providers
     */
    int getConfiguredProviderCount() const;

    /**
     * Get access to AI providers array (for fallback logic)
     */
    AIProvider* getProviders() { return aiProviders; }
    int getMaxProviders() const { return MAX_AI_PROVIDERS; }

    /**
     * Call a specific AI provider directly (for fallback logic)
     * @param providerIdx Index of the provider to call
     * @param prompt The prompt to send
     * @param outResponse Response output
     * @return true if successful
     */
    bool callProvider(int providerIdx, const String& prompt, String& outResponse);

    /**
     * Parse AI response and extract weather forecast information
     * @param response Raw AI response string
     * @param outMessage Extracted message text
     * @return true if parsing successful
     */
    bool parseAIResponse(const String& response, String& outMessage);

    /**
     * Set the current AI provider index (for remembering successful providers)
     */
    void setCurrentProviderIndex(int idx) { currentAIProviderIndex = idx; }

private:
    static constexpr int MAX_AI_PROVIDERS = 4;
    static constexpr unsigned long AI_TIMEOUT_MS = 15000;

    AIProvider aiProviders[MAX_AI_PROVIDERS];
    int currentAIProviderIndex;

    // Provider-specific implementations
    bool callGeminiAPI(const AIProvider& provider, const String& prompt, String& outResponse);
    bool callMistralAPI(const AIProvider& provider, const String& prompt, String& outResponse);
    bool callGroqAPI(const AIProvider& provider, const String& prompt, String& outResponse);

    // HTTP helper
    String httpPost(const char* url, const String& payload, const String& authHeader = "");
};

// Global instance
extern AIService* aiService;