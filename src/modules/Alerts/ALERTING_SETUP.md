# AlertsModule Configuration

This document explains how to configure the AlertsModule for building the Meshtastic firmware.

## API Key Setup

The AlertsModule uses AI services for alert processing with automatic fallback support. **You need to configure at least one API key** before building.

### Supported AI Providers

The module supports multiple AI providers with automatic fallback:

1. **Gemini 2.0 Flash** - Fast, free tier: 15 req/min, 1,500 req/day (Primary)
2. **Perplexity (Llama 3.1 Sonar)** - Pro tier: $5/month credit (Secondary)
3. **Mistral 7B** - Free tier, good for Polish (Tertiary)
4. **Groq (Llama 3.3 70B)** - Very fast, generous free tier: 30 req/min, 14,400 req/day (Last resort)

**You can configure:**
- ✅ Only Gemini (single provider, free)
- ✅ Only Groq (single provider, very fast and free)
- ✅ Any combination (full fallback chain for maximum reliability)
- ✅ All providers (4 levels of fallback, highest reliability)

The module will automatically use whichever providers you've configured. If one provider fails (e.g., quota exceeded, network error), it automatically tries the next configured provider.

### Required: At Least One API Key

You must set at least one of:
- `GEMINI_API_KEY` (recommended - free, reliable)
- `PERPLEXITY_API_KEY` (paid - $5/month credit, very good quality)
- `MISTRAL_API_KEY` (free - good for Polish language)
- `GROQ_API_KEY` (free - fast and generous limits, good fallback)
- Multiple keys (maximum resilience and quota)

#### Method 1: Using `.env` file (Recommended)

PlatformIO automatically loads environment variables from a `.env` file in the project root.

1. Copy the example file:
   ```bash
   cp .env.example .env
   ```

2. Edit `.env` and add at least one API key:

   **Option A: Gemini only (1 provider, free)**
   ```bash
   GEMINI_API_KEY=your_gemini_key_here
   ```

   **Option B: Groq only (1 provider, very fast, free)**
   ```bash
   GROQ_API_KEY=your_groq_key_here
   ```

   **Option C: Gemini + Groq (2 providers, both free, reliable)**
   ```bash
   GEMINI_API_KEY=your_gemini_key_here
   GROQ_API_KEY=your_groq_key_here
   ```

   **Option D: All providers (4 providers, maximum resilience)**
   ```bash
   GEMINI_API_KEY=your_gemini_key_here
   PERPLEXITY_API_KEY=your_perplexity_key_here
   MISTRAL_API_KEY=your_mistral_key_here
   GROQ_API_KEY=your_groq_key_here
   ```

3. Build using the helper script (recommended - ensures .env is loaded):
   ```bash
   # Build only
   ./tmp/build-with-env.sh run -e seeed-xiao-s3
   
   # Build and upload
   ./tmp/build-with-env.sh run -e seeed-xiao-s3 -t upload
   ```

**Why use the build script?**
- ✅ Ensures `.env` variables are properly exported
- ✅ PlatformIO's `${sysenv.VAR}` sometimes doesn't load `.env` correctly
- ✅ Explicitly exports each variable before building
- ✅ More reliable than relying on PlatformIO's env loading

**Advantages of .env file:**
- ✅ Already in `.gitignore` (won't be committed)
- ✅ Standard practice for sensitive data
- ✅ Easy to manage multiple keys
- ✅ Works across all environments

#### Method 2: Using system environment variables

Export the variables in your shell:

```bash
export GEMINI_API_KEY="your_gemini_key_here"
export PERPLEXITY_API_KEY="your_perplexity_key_here"  # Optional
export MISTRAL_API_KEY="your_mistral_key_here"        # Optional
export GROQ_API_KEY="your_groq_key_here"              # Optional
pio run -e seeed-xiao-s3
```

Or add them to your `~/.bashrc` or `~/.zshrc` for persistence.

#### Method 3: Inline with build command

```bash
GEMINI_API_KEY="your_key" GROQ_API_KEY="your_key" pio run -e seeed-xiao-s3
```

## How It Works

### API Key Injection

The API keys are injected at compile time via build flags in `variants/esp32s3/seeed_xiao_s3/platformio.ini`:

```ini
build_flags =
  -DGEMINI_API_KEY=\"${sysenv.GEMINI_API_KEY}\"
  -DPERPLEXITY_API_KEY=\"${sysenv.PERPLEXITY_API_KEY}\"
  -DMISTRAL_API_KEY=\"${sysenv.MISTRAL_API_KEY}\"
  -DGROQ_API_KEY=\"${sysenv.GROQ_API_KEY}\"
```

The `${sysenv.VARIABLE}` syntax tells PlatformIO to:
1. First check system environment variables
2. Then check the `.env` file in project root (sometimes)
3. Use the found value as a string literal in the code

**Note:** PlatformIO will pass empty strings for undefined variables. The module automatically skips providers with empty API keys.

### Automatic Fallback

When processing alerts, the module tries each configured provider in order:

**If you have all keys:**
1. Module tries **Gemini 2.0 Flash** first (fastest, free)
2. If it fails (quota, network error, etc.) → tries **Perplexity (Llama 3.1 Sonar)**
3. If still failing → tries **Mistral 7B** (good for Polish)
4. If still failing → tries **Groq (Llama 3.3 70B)** (last resort, very fast, generous quota)
5. If all providers fail → alert is retried on next fetch cycle

**If you only have Gemini:**
1. Uses Gemini 2.0 Flash
2. Retries on next cycle if it fails

**If you only have Groq:**
1. Uses Groq (Llama 3.3 70B Versatile model)
2. Retries on next cycle if it fails

**If you have Gemini + Groq (recommended free combo):**
1. Tries Gemini 2.0 Flash first
2. Falls back to Groq if Gemini fails
3. Both are free with generous limits

The module logs which provider succeeded for each alert, making it easy to track usage.

## Getting API Keys

### Gemini API Key (Recommended)

1. Visit: https://makersuite.google.com/app/apikey
2. Sign in with your Google account
3. Click "Create API Key"
4. Copy the key and add it to your `.env` file

**Free Tier:** 15 requests/minute, 1,500 requests/day  
**Why Gemini:** Fast, reliable, free tier is generous for this use case

### Groq API Key (Recommended for fallback)

1. Visit: https://console.groq.com/
2. Sign up or sign in (supports GitHub, Google)
3. Go to API Keys section
4. Click "Create API Key"
5. Copy the key and add it to your `.env` file

**Free Tier:** 30 requests/minute, 14,400 requests/day (with llama-3.3-70b-versatile)  
**Why Groq:** Extremely fast inference (often <1 second), very generous free tier, excellent quality with Llama 3.3 70B

### Perplexity API Key (Optional, Paid)

1. Visit: https://www.perplexity.ai/settings/api
2. Sign up or sign in
3. Purchase credits ($5 minimum)
4. Generate an API key
5. Copy the key and add it to your `.env` file

**Pricing:** Pay-as-you-go with $5 minimum credit  
**Why Perplexity:** High quality, good for Polish text understanding, reliable

### Mistral API Key (Optional, Free)

1. Visit: https://console.mistral.ai/
2. Sign up or sign in
3. Go to API Keys section
4. Create a new API key
5. Copy the key and add it to your `.env` file

**Free Tier:** Generous free tier for open-source models  
**Why Mistral:** Good for Polish language, free tier available

## Multi-Source Alert System

The AlertsModule supports multiple alert sources with a plugin architecture:

### Current Sources

1. **RCB (Rządowe Centrum Bezpieczeństwa)**
   - Polish government alerts
   - URL: https://www.gov.pl/web/rcb/komunikaty
   - Type: HTML parsing with two-phase fetching
   - Fetch interval: 5 minutes
   - Default severity: 3

2. **IMGW (Institute of Meteorology and Water Management)**
   - Weather alerts via MeteoAlarm API
   - Type: JSON API
   - Fetch interval: 15 minutes
   - Severity: Calculated from CAP alert level

### Source-Specific Features

- **Structured Dates**: IMGW provides dates directly, bypassing AI extraction
- **Custom AI Prompts**: Each source has optimized prompts for better extraction
- **Two-Phase Fetching**: RCB fetches article pages only for new alerts (after duplicate check)
- **Validation**: Each source validates and cleans up extracted data

## Security Notes

- ⚠️ **Never commit** your `.env` file to git
- ⚠️ The API keys are embedded in the compiled firmware binary
- ⚠️ Anyone with access to the binary can extract the keys
- ✅ The `.env` file is already in `.gitignore`
- ✅ Use `.env.example` for sharing configuration templates
- ✅ Use the build script (`tmp/build-with-env.sh`) for reliable builds

## Troubleshooting

### Build fails with "At least one AI API key must be defined"

This means you don't have any API keys configured. You need at least one:

**Quick fix:**
1. Create or edit `.env` file in project root
2. Add at least one key:
   ```bash
   GEMINI_API_KEY=your_key_here
   # OR
   GROQ_API_KEY=your_key_here
   # OR any other provider
   ```
3. Make sure there are no quotes around the key in `.env`
4. The file should have Unix line endings (LF, not CRLF)
5. Use the build script: `./tmp/build-with-env.sh run -e seeed-xiao-s3`

### Build succeeds but no AI providers available at runtime

Check the logs at boot:
```
INFO | AlertsModule: AI provider configured: Gemini-2.0
INFO | AlertsModule: AI provider configured: Perplexity
INFO | AlertsModule: AI provider configured: Mistral
INFO | AlertsModule: AI provider configured: Groq
INFO | AlertsModule: 4 AI provider(s) available
```

If you see `0 AI provider(s) available`, the API keys weren't embedded correctly.

**Debug steps:**
1. Verify keys are in `.env` with correct variable names (no quotes needed)
2. Check there are no typos in the keys
3. Use the build script instead of direct `pio` command
4. Try a clean rebuild:
   ```bash
   pio run -e seeed-xiao-s3 -t clean
   ./tmp/build-with-env.sh run -e seeed-xiao-s3
   ```

### LED not working / not blinking

This was a known issue caused by the `-DARDUINO_USB_MODE=0` flag. This has been removed from the build configuration.

**Expected behavior:**
- LED lights up for a few seconds during boot
- LED continues flashing as heartbeat during operation

If LED still doesn't work, check:
1. LED is properly connected (built-in LED on pin 48 for Seeed XIAO S3)
2. Firmware uploaded successfully
3. No hardware issues with the board

### Alerts not loading from disk after restart

The module loads alerts from disk on startup (unless `PURGE_ALERTS_ON_BOOT` is set to `true`).

**Check:**
1. Look for log: `AlertsModule: Loaded N binary alerts from disk`
2. Alerts are stored in `/alerts/` directory on the device
3. Binary format is 548 bytes per alert (v2 format)

**Note:** If you're upgrading from an older version, set `PURGE_ALERTS_ON_BOOT = true` temporarily to clear old format files.

### Alerts not being sent to mesh

Common causes:
1. **Time not synced**: Module waits for valid time before sending (check logs)
2. **No valid alerts**: Check if alerts loaded and are within validity period
3. **Channel not created**: Alert channel is created on first alert send

**Verify sending works:**
- Look for: `AlertsModule: Sent NEW alert [source, sev:X]: ...`
- Look for: `AlertsModule: Re-sent alert [source, sev:X]: ...`

### Verify API keys are embedded in firmware

For debugging only (exposes your keys!):
```bash
strings .pio/build/seeed-xiao-s3/firmware.bin | grep -E "AIza|pplx-|[a-z0-9]{32}"
```

You should see your API key(s) in the output:
- Gemini keys start with `AIza`
- Perplexity keys start with `pplx-`
- Mistral/Groq keys are 32-character hex strings

## Additional Configuration

### Module Settings

All major settings can be configured in `src/modules/Alerts/AlertsModule.h`:

```cpp
// Timing
static constexpr unsigned long CLEANUP_INTERVAL_MS = 60 * 60 * 1000;   // 1 hour
static constexpr unsigned long ALERT_RETENTION_DAYS = 30;              // 30 days

// Development
static constexpr bool PURGE_ALERTS_ON_BOOT = true;  // Set false for production
```

### Source Configuration

Each source has its own configuration in its respective class:

```cpp
// RCBAlertSource
static constexpr unsigned long FETCH_INTERVAL = 5 * 60 * 1000;    // 5 minutes
static constexpr uint8_t DEFAULT_SEVERITY = 3;

// IMGWAlertSource  
static constexpr unsigned long FETCH_INTERVAL = 15 * 60 * 1000;   // 15 minutes
```

## Advanced: Adding New Alert Sources

To add a new alert source:

1. Create new files: `src/modules/Alerts/sources/YourSource.{h,cpp}`
2. Inherit from `AlertSource` base class
3. Implement required virtual methods:
   - `getSourceId()` - e.g., "YOUR_SOURCE"
   - `getFetchURL()` - Source URL
   - `getFetchIntervalMs()` - How often to fetch
   - `getDefaultSeverity()` - Default severity for this source
   - `fetchAndParseAlerts()` - Parse source data into RawAlert stubs
   - `buildAIPrompt()` - Create AI prompt for this source type
   - `validateAndCleanup()` - Validate extracted data
4. Optionally implement `fetchFullAlertContent()` for two-phase fetching
5. Register in `AlertsModule` constructor

See `RCBAlertSource` and `IMGWAlertSource` for examples.

---

**For more information, see:**
- `tmp/ALERT_MODULE_SUMMARY.md` - Complete module documentation
- Source code comments in `src/modules/Alerts/`
