// DuraHydra - ESP32-S3 variant
// Pin assignments ported from hydra (ESP32). Review before use:
//   - BATTERY_PIN/ADC_CHANNEL: GPIO35 is not ADC-capable on ESP32-S3
//     (ADC1 = GPIO1-10, ADC2 = GPIO11-20). Reassign if battery reading is needed.
//   - Input-only GPIO34-39 restriction from ESP32 does not apply on ESP32-S3.

// For OLED LCD
#define I2C_SDA 21
#define I2C_SCL 22

// For GPS, 'undef's not needed
#define GPS_TX_PIN 15
#define GPS_RX_PIN 12
#define PIN_GPS_EN 4

#define BUTTON_PIN 39
// BATTERY_PIN / ADC_CHANNEL omitted: GPIO35 is not ADC-capable on ESP32-S3.
// Reassign BATTERY_PIN to GPIO1-10 (ADC1) and set ADC_CHANNEL to ADC1_GPIOx_CHANNEL.
#define ADC_MULTIPLIER 1.85 // (R1 = 470k, R2 = 680k)
#define EXT_PWR_DETECT 4    // Pin to detect connected external power source
#define EXT_NOTIFY_OUT 12   // Overridden default pin for Ext Notify Module
#define LED_POWER 2

// Radio
#define USE_SX1262 // E22-900M30S uses SX1262
// #define USE_SX1268 // E22-400M30S uses SX1268
#define SX126X_MAX_POWER 22 // ~30dBm output via E22 PA
#define SX126X_DIO3_TCXO_VOLTAGE 1.8 // E22 series TCXO reference voltage

#define SX126X_CS 18
#define SX126X_SCK 5
#define SX126X_MOSI 27
#define SX126X_MISO 19
#define SX126X_RESET 23
#define SX126X_BUSY 32
#define SX126X_DIO1 33

#define SX126X_TXEN 13
#define SX126X_RXEN 14

#define LORA_CS SX126X_CS
#define LORA_SCK SX126X_SCK
#define LORA_MOSI SX126X_MOSI
#define LORA_MISO SX126X_MISO
#define LORA_DIO1 SX126X_DIO1
#define LORA_TXEN SX126X_TXEN
#define LORA_RXEN SX126X_RXEN
#define LORA_RESET SX126X_RESET
#define LORA_DIO2 SX126X_BUSY

