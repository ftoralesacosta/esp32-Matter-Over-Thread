# Matter PWM Fan Controller with Rotary Dial (ESP32-C6 / ESP32-H2)

A Matter-over-Thread PWM fan controller built with ESP-IDF and ESP-Matter. Drives a standard
4-pin Noctua-style PWM fan, exposed to Apple Home (or any Matter controller) as a Fan device
with speed control, plus local physical control via a rotary encoder ("dial") and its
integrated push-button.

This branch (`feature/fan_with_dial`) adds the rotary encoder on top of the base fan
controller on `main`. It builds for two different boards (see below); which one you get is
purely a build-target choice, not a branch choice.

## Why a rotary encoder, not a potentiometer

A potentiometer reports an absolute physical position. If HomeKit sets the fan to 80% but the
pot is still sitting wherever it was last turned (say, 20%), the two are now out of sync -
nudge the pot even slightly and the firmware reads "20%" and silently overwrites whatever
HomeKit had just set, with no way to reconcile physical position against displayed state.

A rotary encoder instead reports *relative* motion - "+1 step" / "-1 step" per detent - with no
fixed physical position at all. Turning it always adjusts whatever the fan's *current* speed
is, whether that speed was last set by HomeKit or a previous turn of the dial. There's no
positional conflict to resolve, and HomeKit stays the single source of truth for displayed
speed. This is why the dial is wired to adjust the Matter `PercentSetting` attribute through
the exact same code path a HomeKit write uses - it never drives the fan's PWM output directly.

For deep architecture notes, hardware pin mapping, and a running troubleshooting log, see
[`.agents/FINDINGS.md`](.agents/FINDINGS.md) - read it before changing `sdkconfig.defaults*`
or the Thread/mDNS configuration, since several non-obvious footguns are documented there.

## Hardware

Two supported boards, selected via `idf.py set-target`:

### Seeed Studio XIAO ESP32-C6
* **Fan PWM output:** physical pin `D3` / GPIO21 (LEDC, low-speed mode, 10-bit duty, 25 kHz)
* **Toggle button:** physical pin `D2` / GPIO2 (active high) - reuse the encoder module's
  integrated push-button here rather than a separate standalone tactile button
* **Encoder A/B:** physical pins `D4` / GPIO22 and `D5` / GPIO23
* **Onboard RF switch:** GPIO3 (enable, drive LOW) + GPIO14 (antenna select, LOW = ceramic) -
  required on this board or the radio gets no usable antenna path

### Waveshare ESP32-H2-Zero
* **Fan PWM output:** GPIO12
* **Toggle button:** GPIO10 (external - deliberately not the onboard BOOT button on GPIO9,
  which enters the bootloader if held low at power-on) - same button-reuse note as above
* **Encoder A/B:** GPIO22 and GPIO4. GPIO22 is already validated as a plain, unshared pin on
  this board (from `feature/tachometer-and-ota`); GPIO4 is picked by the same reasoning
  (avoiding GPIO9 BOOT, GPIO13/14 32.768kHz crystal, GPIO23/24 UART0) but **this specific pair
  has not been verified against real hardware yet** - double check against your board's
  silkscreen before wiring.
* Pin-compatible with Espressif's official ESP32-H2-DevKitM-1; GPIO13/14 (32.768kHz crystal)
  and GPIO23/24 (UART0 TX/RX, the serial console) are avoided for that reason
* Has a ceramic antenna with no RF-switch chip, unlike the XIAO C6 - no antenna GPIO setup
  needed on this board (`init_rf_switch()` is skipped for this target)

### Rotary encoder module (both boards)
A standard EC11-style encoder module (CLK/DT quadrature outputs + integrated SW push-button)
wired to the A/B pins above, using the [`espressif/knob`](https://components.espressif.com/components/espressif/knob)
component for software quadrature decoding - portable across targets without needing per-chip
hardware PCNT setup. The module's own push-button reuses the existing toggle-button GPIO/logic
(on/off), so no separate standalone button is needed in the BOM.

See `.agents/FINDINGS.md` for the full pin table and wiring notes.

## 1. Environment Setup

Requires ESP-IDF and ESP-Matter set up per the
[ESP-Matter getting started guide](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html),
with `IDF_PATH` and `ESP_MATTER_PATH` sourced/exported in your shell.

## 2. Build & Flash

`clean_build_and_flash.sh` hardcodes `esp32c6` as the target. On that board you can just run
it directly:

```bash
./clean_build_and_flash.sh
```

For the H2, set the target manually before running it, or drive `idf.py` directly:

```bash
idf.py set-target esp32h2
idf.py build
idf.py erase-flash
idf.py flash monitor
```

Both boards have 4MB of flash; the custom partition table (`partitions.csv`) is sized for it.

## 3. Commissioning

* **Manual setup code:** `20202021`
* **Discriminator:** `3840`

The device advertises over BLE for commissioning, then over Thread (via SRP registration to
your Thread Border Router) once joined. If commissioning hangs or the accessory shows "No
Response" after previously pairing, see the troubleshooting workflows in
`.agents/FINDINGS.md` before re-flashing - most historical failures traced back to
configuration drift (a stale committed `sdkconfig`) or Thread mDNS/SRP misconfiguration, not
hardware.

**H2 board note:** the first-ever real H2 commissioning attempt (on the LED-controller branch,
which shares the same underlying Matter/Thread stack) hard-hung during
`AddTrustedRootCertificate` due to a hardware ECDSA-peripheral driver bug in this ESP-IDF
version, not application code. Fixed by `CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY=n` in
`sdkconfig.defaults.esp32h2` (falls back to mbedTLS software verification; commissioning-only
cost, so no runtime performance impact). With that fix, H2 commissioning is confirmed working
end-to-end on `main` itself, on real hardware. See `.agents/FINDINGS.md` Section -2 for the full
diagnosis.

The BLE commissioning window closes on its own after some minutes of inactivity
(`Commissioning window closed` in the log). If you miss it, no need to erase-flash or rebuild -
just reset the device (power cycle, or reattach `idf.py monitor`, which resets the board by
default) to reopen it.

## 4. Post-Commissioning Behavior

* Fan speed is controlled via the Fan Control cluster's `PercentSetting` attribute (0-100%),
  debounced 300ms before being applied to the LEDC output and synced back to
  `PercentCurrent`/`FanMode`.
* The physical button toggles the fan between off and its default speed.
* Rotating the encoder adjusts `PercentSetting` by ±5% per detent (`ENCODER_STEP_PERCENT` in
  `main/app_driver.cpp`), reading the *current* value and writing the new one back through the
  same attribute a HomeKit write uses - it does not touch the LEDC output directly. Rapid turns
  coalesce through the existing 300ms debounce the same way rapid HomeKit slider drags do.
* `FanMode`'s `MultiSpeed` feature bit is set so Apple Home shows a speed slider rather than
  just an on/off switch.
