#define I2C_SDA 1 // I2C pins for this board
#define I2C_SCL 2

#define HAS_NEOPIXEL                         // Enable the use of neopixels
#define NEOPIXEL_COUNT 1                     // How many neopixels are connected
#define NEOPIXEL_DATA 21                     // gpio pin used to send data to the neopixels
#define NEOPIXEL_TYPE (NEO_GRB + NEO_KHZ800) // type of neopixels in use

#define BUTTON_PIN 0 // If defined, this will be used for user button presses
#define BUTTON_NEED_PULLUP

#define USE_SX1262

#define LORA_MISO 8
#define LORA_SCK 7
#define LORA_MOSI 9
#define LORA_CS 41

#define LORA_RESET 42
#define LORA_DIO1 39

#define LORA_DIO2 38

#ifdef USE_SX1262
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY 40
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_RXEN 38
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
#endif

#define HAS_WIFI 1
#define RADIOLIB_EXCLUDE_CC1101 1
#define RADIOLIB_EXCLUDE_NRF24 1
#define RADIOLIB_EXCLUDE_RF69 1
#define RADIOLIB_EXCLUDE_SX1231 1
#define RADIOLIB_EXCLUDE_SX1233 1
#define RADIOLIB_EXCLUDE_SI443X 1
#define RADIOLIB_EXCLUDE_RFM2X 1
#define RADIOLIB_EXCLUDE_AFSK 1
#define RADIOLIB_EXCLUDE_BELL 1
#define RADIOLIB_EXCLUDE_HELLSCHREIBER 1
#define RADIOLIB_EXCLUDE_MORSE 1
#define RADIOLIB_EXCLUDE_RTTY 1
#define RADIOLIB_EXCLUDE_SSTV 1
#define RADIOLIB_EXCLUDE_AX25 1
#define RADIOLIB_EXCLUDE_DIRECT_RECEIVE 1
#define RADIOLIB_EXCLUDE_BELL 1
#define RADIOLIB_EXCLUDE_PAGER 1
#define RADIOLIB_EXCLUDE_FSK4 1
#define RADIOLIB_EXCLUDE_APRS 1
