<div align="center" markdown="1">

<img src=".github/meshtastic_logo.png" alt="Meshtastic Logo" width="80"/>
<h1>Meshtastic Firmware — BroadcastBeacon branch</h1>

</div>

This branch adds the **BroadcastBeacon** module to the upstream Meshtastic firmware. The base firmware is unchanged; everything is behind the `HAS_BROADCAST_BEACON` compile guard and is only active when that flag is set at build time.

## BroadcastBeacon — English

The BroadcastBeacon module lets a single Meshtastic node rebroadcast the same text messages across several different LoRa presets in rotation. It solves a very real problem: in many regions, different groups of users are on different radio settings (e.g. LONG_FAST, SHORT_SLOW, MEDIUM_FAST), and a node running on one preset is completely invisible to the other networks. Instead of forcing everyone onto a single preset, BroadcastBeacon cycles its radio through the configured presets and sends the same content on each one.

The switch happens via a short device reboot into the new preset — this is the simplest and most reliable path, avoiding the hard-to-reproduce state-machine behaviour of in-flight radio reconfiguration. The user defines a **home preset**, which is always included in the rotation as slot zero and guarantees that in every full cycle the node appears on its primary network at least once — so it stays reachable for admin commands and normal mesh traffic. The total cycle interval and the list of additional presets are configurable; per-preset window time is computed automatically (interval divided by the number of presets).

The module supports multiple broadcast messages at once. Each message has its own body, a list of channels to send on (up to eight in parallel), optional start and end dates, and an individual hop limit. Once the end date expires the message stops going out automatically, and once no messages are active anymore the node restores the home preset and ends the cycle. This gives a clean path for publishing announcements with limited validity — event notices, network-migration calls, local information — just set an expiry and the module turns itself off.

Everything is managed via private text messages starting with `/bb`, sent directly to the node. Only administrators whose PKI keys are stored in the device config can use it — no other node can change the configuration or add messages. An interactive wizard walks the user through creating and editing messages and changing config, accepting `.` for "keep / default", `!` for "abort", and `-` to clear a field.

In addition to the text body, the module also broadcasts a NodeInfo packet on each preset switch, bypassing the standard NodeInfo throttle — neighbours on the new network immediately learn who the node is. Position broadcasting is also optional: with that enabled the node emits its GPS position as soon as a fix is acquired on each preset, using the device's default hop count and ignoring PositionModule's normal interval. That turns BroadcastBeacon into a **multi-preset tracker** — one device becomes visible as a position source on every supported network simultaneously.

Typical use cases include publishing emergency notices, field-event information, weather warnings, or migration messages during the window when a community is moving from one preset to another. For tracking use, it fits vehicle, drone, or personal tracking across regions where local operators use different radio settings — no need for multiple devices. Configuration is persisted to device flash and survives both the scheduled cycle reboots and firmware upgrades, so the module can be left running as a long-term autonomous communication tool.

## BroadcastBeacon — Polski

Moduł BroadcastBeacon to rozszerzenie firmware'u Meshtastic, które pozwala jednemu węzłowi rozsyłać te same wiadomości tekstowe na wielu różnych presetach LoRa w określonej rotacji. Rozwiązuje to realny problem: w wielu regionach różne grupy użytkowników korzystają z różnych ustawień radia (np. LONG_FAST, SHORT_SLOW, MEDIUM_FAST), a węzeł pracujący na jednym presecie jest całkowicie niewidoczny dla pozostałych sieci. Zamiast zmuszać wszystkich do wspólnego presetu, BroadcastBeacon cyklicznie przełącza radio swojego węzła pomiędzy zdefiniowanymi presetami i na każdym z nich wysyła tę samą treść.

Przełączanie odbywa się poprzez krótki restart urządzenia na nowym presecie — to najprostsza i najbardziej niezawodna metoda, bo unika trudnych do odtworzenia zmian stanu sprzętu radia w locie. Użytkownik definiuje tzw. preset domowy (ang. home preset), który jest automatycznie włączany w rotację jako slot zerowy i gwarantuje, że w każdym pełnym cyklu węzeł co najmniej raz pojawia się w swojej macierzystej sieci — zachowuje więc osiągalność dla administratorów i normalnego ruchu mesh. Całkowity interwał cyklu oraz lista dodatkowych presetów są konfigurowalne, a czas na pojedynczym presecie obliczany jest automatycznie (interwał podzielony przez liczbę presetów).

Moduł obsługuje wiele wiadomości broadcastowych jednocześnie. Każda wiadomość ma własną treść, listę kanałów, na które ma zostać wysłana (do ośmiu równolegle), opcjonalną datę rozpoczęcia i zakończenia ważności oraz indywidualny limit przeskoków (hops). Po wygaśnięciu daty końcowej wiadomość przestaje być nadawana automatycznie, a gdy żadna z wiadomości nie jest już aktywna, węzeł wraca do presetu domowego i kończy cykl. Daje to prostą ścieżkę np. do publikacji ogłoszeń o ograniczonym czasie ważności, takich jak informacje o wydarzeniach, zmianach w lokalnej sieci czy migracji na nowy preset — wystarczy ustawić datę wygaśnięcia i moduł sam się wyłączy.

Moduł jest zarządzany wyłącznie przez prywatne wiadomości tekstowe rozpoczynające się od `/bb`, wysyłane bezpośrednio do węzła. Dostęp mają tylko administratorzy, których klucze PKI są zapisane w konfiguracji urządzenia — żaden inny węzeł nie może zmienić konfiguracji ani dodać wiadomości. Interaktywny kreator prowadzi użytkownika krok po kroku przez tworzenie i edycję wiadomości oraz zmiany konfiguracji, przyjmując potwierdzenia znakiem `.`, przerwanie `!`, a wyczyszczenie pola `-`.

Oprócz samej treści tekstowej, na każdym przełączeniu presetu moduł wysyła również pakiet NodeInfo z pominięciem standardowego ograniczenia częstotliwości wysyłania — sąsiedzi w nowej sieci od razu dowiadują się, kim jest ten węzeł. Opcjonalnie można włączyć nadawanie pozycji GPS: w takim przypadku węzeł wysyła swoją lokalizację natychmiast po uzyskaniu fixa GPS na każdym presecie, korzystając z domyślnej liczby przeskoków i ignorując interwał PositionModule. Ta funkcja zamienia BroadcastBeacon w wielopresetowy tracker — jedno urządzenie staje się widoczne jako nadajnik pozycji we wszystkich wspieranych sieciach jednocześnie.

Typowe zastosowania obejmują publikację ogłoszeń ratunkowych, informacji o wydarzeniach terenowych, ostrzeżeń pogodowych albo komunikatów migracyjnych w okresach, gdy społeczność przechodzi z jednego presetu na inny. W kontekście trackingowym moduł sprawdza się przy śledzeniu pojazdu, drona lub osoby poruszającej się przez regiony, gdzie lokalni operatorzy używają odmiennych ustawień radia — bez konieczności posiadania wielu urządzeń. Konfiguracja jest w pełni przechowywana we flashu urządzenia i przetrwa zarówno planowane restarty cyklu, jak i aktualizacje firmware'u, co pozwala traktować ten moduł jako długoterminowe, autonomiczne narzędzie komunikacyjne.

## Prebuilt firmware / Gotowe buildy

Every push to the `broadcast_beacon` branch triggers a GitHub Actions workflow (`.github/workflows/build_bb_branch.yml`) that builds BroadcastBeacon-enabled firmware for all supported targets. Artifacts are available without a GitHub account via **nightly.link**:

**Browse all builds:** [https://nightly.link/duraseb/meshtastic_firmware/workflows/build_bb_branch/broadcast_beacon](https://nightly.link/duraseb/meshtastic_firmware/workflows/build_bb_branch/broadcast_beacon)

Each target has two artifacts: `firmware-<platform>-<pio_env>.zip` (flashable binaries — `.uf2`, `.hex`, `.bin`) and `manifest-<platform>-<pio_env>.zip` (the `.mt.json` descriptor). Example direct link for the nRF52 Pro Micro DIY TCXO target:

```
https://nightly.link/duraseb/meshtastic_firmware/workflows/build_bb_branch/broadcast_beacon/firmware-nrf52840-nrf52_promicro_diy_tcxo.zip
```

Każdy push na gałąź `broadcast_beacon` uruchamia workflow GitHub Actions, który buduje firmware z włączonym BroadcastBeaconem dla wszystkich wspieranych urządzeń. Gotowe pliki można pobrać bez konta GitHub przez powyższy link do nightly.link. Każdy cel zawiera paczkę `firmware-<platform>-<pio_env>.zip` z obrazami do flashowania (`.uf2`, `.hex`, `.bin`) oraz `manifest-<platform>-<pio_env>.zip` z deskryptorem `.mt.json`.

## Building from source

```bash
PLATFORMIO_BUILD_FLAGS="-DHAS_BROADCAST_BEACON=1" \
  ~/.platformio/penv/bin/pio run -e nrf52_promicro_diy_tcxo
```

Without `-DHAS_BROADCAST_BEACON=1` the firmware builds identically to upstream Meshtastic — the module is completely excluded.

## Overview (upstream)

This repository contains the official device firmware for Meshtastic, an open-source LoRa mesh networking project designed for long-range, low-power communication without relying on internet or cellular infrastructure. The firmware supports various hardware platforms, including ESP32, nRF52, RP2040/RP2350, and Linux-based devices.

Meshtastic enables text messaging, location sharing, and telemetry over a decentralized mesh network, making it ideal for outdoor adventures, emergency preparedness, and remote operations.

### Get Started

- 🔧 **[Building Instructions](https://meshtastic.org/docs/development/firmware/build)** – Learn how to compile the firmware from source.
- ⚡ **[Flashing Instructions](https://meshtastic.org/docs/getting-started/flashing-firmware/)** – Install or update the firmware on your device.

Join our community and help improve Meshtastic! 🚀
