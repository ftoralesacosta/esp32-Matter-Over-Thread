# Matter Dimmable LED Controller (ESP32-C6 / ESP32-H2)

A Matter-over-Thread dimmable LED controller built with ESP-IDF and ESP-Matter. Drives a 5V
LED through an opto-isolated MOSFET switch module, exposed to Apple Home (or any Matter
controller) as a Dimmable Light with on/off and a brightness slider, plus a physical toggle
button.

This branch is LED-only - it does not include the fan-controller code that lives on `main`.
It builds for two different boards (see below); which one you get is purely a build-target
choice, not a branch choice.

For deep architecture notes on the underlying Matter/Thread stack (mDNS/SRP self-healing,
config-drift footguns, etc.) that also apply here, see
[`.agents/FINDINGS.md`](.agents/FINDINGS.md) - read it before changing `sdkconfig.defaults*`
or the Thread/mDNS configuration.

## How the dimming works

PWM (Pulse Width Modulation), not voltage reduction. The LED is always driven at the full 5V
supply voltage whenever it's on - what varies is the fraction of time it's on within each
1kHz cycle (the "duty cycle"). At 50% brightness the LED is fully on for 0.5ms then fully off
for 0.5ms, 1000 times a second; far too fast for the eye to perceive as flicker, so it reads
as smooth, proportional brightness. This is the standard way to dim LEDs, since they're
current-driven devices that don't dim linearly (or safely) with reduced voltage the way
incandescent bulbs do.

## Hardware

Two supported boards, selected via `idf.py set-target`:

### Seeed Studio XIAO ESP32-C6
* **LED PWM signal:** physical pin `D3` / GPIO21 (LEDC, low-speed mode, 10-bit duty, 1 kHz)
* **Toggle button:** physical pin `D2` / GPIO2 (active high)

### Waveshare ESP32-H2-Zero
* **LED PWM signal:** GPIO11
* **Toggle button:** GPIO10 (external - deliberately not the onboard BOOT button on GPIO9,
  which enters the bootloader if held low at power-on)
* Pin-compatible with Espressif's official ESP32-H2-DevKitM-1; GPIO13/14 (32.768kHz crystal)
  and GPIO23/24 (UART0 TX/RX, the serial console) are avoided for that reason
* Has a ceramic antenna with no RF-switch chip, unlike the XIAO C6 - no antenna GPIO setup
  needed on this board

### MOSFET module wiring (both boards)
Signal input goes to the LED PWM GPIO above. V+/V- (load side) goes to your external 5V
supply and the LED. **Signal polarity (does GPIO-high turn the LED on or off?) has not been
verified against real hardware yet** - if the LED comes on inverted, flip the duty cycle
calculation in `app_driver_light_apply()` in `main/app_driver.cpp`.

## 1. Environment Setup

Requires ESP-IDF and ESP-Matter set up per the
[ESP-Matter getting started guide](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html),
with `IDF_PATH` and `ESP_MATTER_PATH` sourced/exported in your shell.

## 2. Build & Flash

`clean_build_and_flash.sh` hardcodes `esp32c6` as the target (it was written for the
fan-controller branch), so on this branch you need to set the target manually before running
it, or just drive `idf.py` directly:

```bash
idf.py set-target esp32c6   # or: esp32h2
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

**H2 board note:** the first-ever real H2 commissioning attempt hard-hung during
`AddTrustedRootCertificate` due to a hardware ECDSA-peripheral driver bug in this ESP-IDF
version, not our code. Fixed by `CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY=n` in
`sdkconfig.defaults.esp32h2` (falls back to mbedTLS software verification; commissioning-only
cost, so no runtime performance impact). See `.agents/FINDINGS.md` for the full diagnosis.
With that fix, H2 commissioning is confirmed working end-to-end (validated on two separate
physical H2 chips).

## 4. Post-Commissioning Behavior

* Brightness is controlled via the LevelControl cluster's `CurrentLevel` attribute (0-254),
  applied directly to the LEDC PWM output - no debounce timer, unlike the fan branch's
  `PercentSetting` handling (that exists to work around a `FanMode` auto-substitution quirk
  that has no `LevelControl` equivalent).
* On/off is controlled via the OnOff cluster; the physical button toggles it.
