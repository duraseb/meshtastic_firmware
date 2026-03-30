#define HAS_SCREEN 0
#define HAS_GPS 0
#undef GPS_RX_PIN
#undef GPS_TX_PIN

#define BUTTON_PIN 2
#define BUTTON_NEED_PULLUP
#define EXT_NOTIFY_OUT 1 // Overridden default pin to use for Ext Notify Module (#975).

#define LORA_DIO0 RADIOLIB_NC  // a No connect on the SX1262/SX1268 module
#define LORA_RESET 5 // RST for SX1276, and for SX1262/SX1268
#define LORA_DIO1 3  // IRQ for SX1262/SX1268
#define LORA_DIO2 4  // BUSY for SX1262/SX1268
#define LORA_DIO3     // Not connected on PCB, but internally on the TTGO SX1262/SX1268, if DIO3 is high the TXCO is enabled

// E22P-868M30S RF switch control (section 4.2: EN controls LNA, T/R CTRL controls PA)
// EN (E22P pin 6, GPIO11): LNA enable — repurposed as POWER_EN by USE_EBYTE_E22P (always HIGH when active)
// T/R CTRL (E22P pin 7, GPIO12): HIGH=TX, LOW=RX — driven by RadioLib as TXEN
#define LORA_RXEN 11  // EN pin → becomes POWER_EN via USE_EBYTE_E22P logic
#define LORA_TXEN 12  // T/R CTRL pin

#define LORA_SCK 10
#define LORA_MISO 6
#define LORA_MOSI 7
#define LORA_CS 8

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_DIO2
#define SX126X_RESET LORA_RESET
#define SX126X_RXEN LORA_RXEN

// supported modules list
#define USE_SX1262

#define SX126X_DIO3_TCXO_VOLTAGE 1.8

#ifdef USE_EBYTE_E22P

// T/R CTRL driven as TXEN (HIGH=TX, LOW=RX). EN driven as POWER_EN (always HIGH) via LORA_RXEN remap.
// DIO2 (pin 8) is wired separately to GPIO13 and is NOT shorted to T/R CTRL, so DIO2_AS_RF_SWITCH must NOT be set.
#define SX126X_TXEN LORA_TXEN

#undef USE_LLCC68
#undef USE_RF95
#undef USE_LR1121
#undef LR11X0_DIO_AS_RF_SWITCH
#undef TCXO_OPTIONAL

#else

// E22 (non-P) 868/915MHz: pin 6 = TXEN (GPIO11), pin 7 = RXEN (GPIO12) — swapped vs E22P
#undef SX126X_RXEN
#define SX126X_RXEN LORA_TXEN  // GPIO12 = E22 RXEN
#define SX126X_TXEN LORA_RXEN  // GPIO11 = E22 TXEN

#endif
