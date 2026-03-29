/*
 * MIT License
 * 
 * Copyright (c) 2026 KokoSoft
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _VARIANT_BIRBMESH_ONE_DIY_
#define _VARIANT_BIRBMESH_ONE_DIY_

#include "../nrf52_promicro_diy_tcxo/variant.h"

#define _PINNUM(port, pin)		((port)*32 + (pin))

// GPIOs connected to reset pin
#define PIN_RESET				_PINNUM(0, 14)
#define PIN_RESET2				_PINNUM(0, 16)

// Hardware reset pin
#define PIN_HW_RESET			_PINNUM(0, 18)

// Charger control
#define PIN_CHARGER				_PINNUM(1, 6)
#define CHARGER_STATE_ENABLED	LOW

#if defined(USE_E22) || defined(USE_E22P)
#undef USE_LLCC68
#undef USE_SX1262
#undef USE_RF95
#undef USE_LR1121
#undef LR11X0_DIO_AS_RF_SWITCH
#endif

// BirbMesh board have TXEN connected to DIO2
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_TXEN				RADIOLIB_NC

#ifdef USE_E22P_EN_PIN
/* Please note : For E22P-868M30S and E22P-915M30S, PA_EN and RF_switch T/R CTRL are connected together,
 * and LNA_EN and RF_switch EN are connected together. Therefore, users only need to control the T/R CTRL
 * when transmitting and use it when receiving. However, for E22P-433M30S, RF_switch is controlled by TX_EN and
 * RX_EN.
 */
#define SX126X_POWER_EN			_PINNUM(0, 17)
#undef SX126X_RXEN
#define SX126X_RXEN RADIOLIB_NC
#endif

// #define SX126X_MAX_POWER 8 set this if using a high-power board!

#ifdef USE_E22P
/* Internally, a DIO3 is used to power a 32MHz TCXO crystal oscillator(the DIO3 is configured to output 1.8V) . */
#undef TCXO_OPTIONAL
#endif


#endif
