# Console OLED over CAN (proposal)

> **Status:** design only. No firmware yet. It replaces the plan to hang the RP2040 encoder controller off a spare center UART (`nano_uart.md`). The interactive prototype is the "El Camino Cluster" artifact, which simulates this node next to the three round displays.

## Hardware

- **Display:** SSD1322 256×64, amber, 4-bit grey (16 levels), 4-wire SPI. The full frame buffer is 8 KB, so any of the MCUs below can keep one in RAM and use grey levels (U8g2's 1-bit mode is not enough).
- **MCU:** an RP2040 with an MCP2515 + TJA1051 (or SN65HVD230), or an ESP32-S3 with its built-in TWAI controller and a transceiver.
- **Inputs (same as the encoder board today):** Select encoder with push (`PIN_MENU_A/B/BTN`), Volume encoder with push (`PIN_AUX_A/B/BTN`), Back key (`PIN_MENU_LEFT`), View key (`PIN_MENU_SELECT`). The IR receiver can stay.
- **Center side:** the ESP32-P4 has two TWAI controllers. It needs a TX/RX pin pair and a 3.3 V transceiver. The center is the only board the OLED talks to.

## Roles

- **Touch is for looking, knobs are for changing.** The round displays keep the full LVGL menu for deep pages (Wi-Fi join, Service). The OLED handles quick settings without covering a gauge.
- **The center owns the state.** An OLED edit is a request (0x201). The center applies it, saves it to NVS, pushes it to the other two displays over cluster I2C as it does today, and broadcasts the new state (0x110). A change made by touch reaches the OLED the same way, and the OLED shows a toast.
- **The knob can drive the center menu.** While the touch menu is open (opened by touch, or by an OLED item such as Wi-Fi or Service), Select turn/push map to the existing `center_menu_rotate()` / `center_menu_press()`, and the OLED mirrors the selected item.
- **Type follows the theme:** Classic → Barlow, Modern → Saira, Fallout → VT323.

## Screens

| Screen | Contents |
| --- | --- |
| Now Playing | 48×48 grey cover thumbnail, title (scrolls if long), artist, progress, elapsed / remaining, play state |
| Trip | Trip A miles, average MPG (instant below), range and gallons in tank |
| Engine | Oil psi, coolant °F, fuel %, charge, each with a bar; a blinking badge on the one in warning |
| Clock | Time, date, street, time source |
| Menu | Center view, Theme, Brightness, Dim with lights, Right layout, Trip A reset, OLED contrast, Wi-Fi ↗, Service ↗ (↗ opens the page on the center) |
| Alert | Low oil, engine hot, not charging, low fuel, no sensor data, odometer not stored. Fills the screen until acknowledged, then stays as a badge; re-arms when the condition clears |
| Reverse | "R" and a pointer to the rear camera on the center |
| No CAN | The bowtie, as the encoder firmware shows when its UART goes quiet |

## Controls

| Input | Drive pages | OLED menu | Center menu open |
| --- | --- | --- | --- |
| Select turn | Next / previous page | Move, or change the value being edited (applied live) | `center_menu_rotate()` |
| Select push | Open OLED menu | Edit / confirm / open on center | `center_menu_press()` |
| Back | Now Playing | Undo the edit, then close | Back to the wheel, then close |
| Volume turn | Volume, with a bar over any screen | | |
| Volume push | Play / pause; hold = next track (AVRCP via the audio bridge) | | |
| View | Next center view (same as the 3 s hold) | | |

## Draft frames (500 kbit/s, 11-bit IDs)

| ID | From → to | Rate | Payload |
| --- | --- | --- | --- |
| 0x100 | center → all | 20 Hz | mph ×10 u16, rpm u16, coolant °F u8 (+40 offset), oil psi u8, fuel % u8, flags u8 (fresh, charging, key) |
| 0x101 | center → all | 5 Hz | amp s8, gear u8, faults u16, odometer tenths u32 |
| 0x102 | center → all | 1 Hz | trip tenths u24, mpg avg ×10 u16, mpg inst ×10 u8, range mi u16 |
| 0x110 | center → all | on change + 1 Hz | theme, view, menu page, wheel index, brightness %, dim-with-lights, right layout, reverse |
| 0x120 | center → all | 1 Hz | h, m, s, time source, month, day, weekday, GPS fix |
| 0x130 | center → OLED | 1 Hz | playing, elapsed s u16, duration s u16, volume u8, art id u16 |
| 0x131–0x133 | center → OLED | on change | title, artist, street: UTF-8 in 7-byte segments (sequence byte first) |
| 0x134 | center → OLED | on new art | 48×48 4-bit cover thumbnail, 1152 bytes in 7-byte segments |
| 0x200 | OLED → center | on input | knob id, detents s8, push / hold / release, key id |
| 0x201 | OLED → center | on change | setting id + new value, or an "open page" request |
| 0x210 | OLED → center | 1 Hz | heartbeat: firmware version, uptime, contrast, input counters |

If 0x100 is missing for 1 s the OLED shows the bowtie. The center treats a missing 0x210 as "no OLED" and changes nothing else.

## Open questions

- Volume: the audio bridge has no volume control today; either scale in the bridge's I2S output or send AVRCP absolute volume to the phone.
- Where Trip A lives: the center (NVS) is simplest; the sensor pack's FRAM would survive a center swap.
- Whether other boards (sensor pack, a future body controller) should join the bus later; the IDs above leave room below 0x100 for them.
