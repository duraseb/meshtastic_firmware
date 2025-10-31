// #define RADIOLIB_CUSTOM_ARDUINO 1
// #define RADIOLIB_TONE_UNSUPPORTED 1
// #define RADIOLIB_SOFTWARE_SERIAL_UNSUPPORTED 1

#define ARDUINO_ARCH_AVR

// #define USE_SH1106 1

// default I2C pins:
// SDA = 4
// SCL = 5

// Recommended pins for SerialModule:
// txd = 8
// rxd = 9

#define EXT_NOTIFY_OUT 25 // Blue LED
#define BUTTON_PIN -1 // Pin 17 used for antenna switching via DIO4

#define LED_PIN PIN_LED

// #define BATTERY_PIN 26
//  ratio of voltage divider = 3.0 (R17=200k, R18=100k)
// #define ADC_MULTIPLIER 3.1 // 3.0 + a bit for being optimistic

#define HAS_CPU_SHUTDOWN 1
#define USE_SX1262

#undef LORA_SCK
#undef LORA_MISO
#undef LORA_MOSI
#undef LORA_CS

// https://wiki.kamamilabs.com/index.php/KAPico_(PL)
#define LORA_SCK 2 // Pin 2
#define LORA_MISO 0 // Pin 0
#define LORA_MOSI 3 // Pin 3
#define LORA_CS 1 // Pin 1

#define LORA_DIO0 RADIOLIB_NC // No GPIO connection
#define LORA_RESET 27 // Pin 27
#define LORA_BUSY 28 // Pin 28
#define LORA_DIO1 29 // Pin 29
#define LORA_DIO2 26 // Pin 26
#define LORA_DIO3 RADIOLIB_NC // No GPIO connection
#define LORA_DIO4 RADIOLIB_NC // No GPIO connection

// On rp2040-lora board the antenna switch is wired and works with complementary-pin control logic.
// See PE4259 datasheet page 4

#ifdef USE_SX1262
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_BUSY
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH // Antenna switch CTRL
#define SX126X_RXEN LORA_DIO4    // Antenna switch !CTRL via GPIO17
// #define SX126X_DIO3_TCXO_VOLTAGE 1.8
#endif
