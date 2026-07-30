# Seeed Studio XIAO ESP32-C6 Matter Fan Controller - Project Findings & Architecture

This document contains architectural findings, hardware mappings, and troubleshooting
workflows for the Matter PWM Fan Controller.

Future AI agents starting a new session MUST read this document before touching config,
because several earlier conclusions in this file were WRONG and have been corrected. Read
the "Corrected Root Cause" section first.

---

## -3. `feature/fan_with_dial` branch: rotary encoder for local speed control

Adds a rotary encoder ("dial") on top of `main`'s fan controller, for local physical speed
adjustment alongside HomeKit control. Not yet flashed/tested on real hardware as of this
writing - build-verified only.

### Design decision: rotary encoder, not a potentiometer
The user's requirement was that HomeKit must keep showing the fan's speed accurately no matter
which input (HomeKit or the dial) last changed it. A potentiometer is unsuitable for this: it
reports an absolute physical position, so if HomeKit sets speed to 80% while the pot is
physically sitting at 20%, the two immediately disagree, and the next tiny nudge of the pot
silently overwrites HomeKit's value with whatever the pot happens to be pointing at - there is
no way to reconcile "physical knob position" against "last commanded value" without either
fighting the user's expectations or adding a fragile deadband/hysteresis hack.

A rotary encoder reports *relative* motion (`KNOB_LEFT`/`KNOB_RIGHT` per detent) with no fixed
physical position at all. This sidesteps the problem entirely: every turn adjusts whatever the
*current* PercentSetting is, regardless of whether that value was last set by HomeKit or a
previous turn of the dial. There is exactly one source of truth (the Matter attribute), and the
dial only ever nudges it - it never drives the LEDC PWM output directly.

### Implementation
* Uses [`espressif/knob`](https://components.espressif.com/components/espressif/knob)
  (`iot_knob.h`), the same component family as the `espressif/button` dependency this project
  already uses for the toggle button. It does software quadrature decoding from a background
  timer/task (not a raw external GPIO ISR), which is why it's safe to call
  `attribute::update()` directly from its callback - same pattern as the existing
  `app_driver_button_toggle_cb`.
* `app_driver_knob_adjust()` reads the *live* `PercentSetting` value via `attribute::get_val()`
  each time (not a locally cached "last known speed" variable) before computing the new value.
  This matters: a cached variable would only be correct once a HomeKit-originated write had
  happened at least once since boot, and would otherwise let the very first encoder turn jump
  from a stale/zero baseline instead of the fan's actual current speed.
* Because encoder writes go through `app_driver_attribute_update()` exactly like a HomeKit
  write does, rapid encoder turns coalesce through the *existing* 300ms debounce timer the same
  way rapid HomeKit slider drags already do - no new debounce logic was needed.
* The encoder module's integrated push-button (most EC11-style modules have one) reuses the
  existing toggle-button GPIO and `app_driver_button_toggle_cb` logic for on/off, rather than
  adding a second physical button to the BOM.

### Pin caveats
* C6 (XIAO): encoder A/B on GPIO22/GPIO23 (`D4`/`D5`) - unused pins per the Section 1 pin
  table, not yet verified against real hardware on this branch.
* H2 (Waveshare H2-Zero): encoder A/B on GPIO22/GPIO4. GPIO22 was already validated as a plain,
  unshared pin on this board (`feature/tachometer-and-ota`'s tach signal uses it). GPIO4 is a
  new pick by the same reasoning used for every other H2 pin choice in this project (avoid
  GPIO9 BOOT strap, GPIO13/14 crystal, GPIO23/24 UART0) but has not been confirmed against a
  real board yet - verify against the silkscreen before wiring.

---

## -2. H2 board support (added on `main`): hardware ECDSA verify hang blocks first-ever real H2 commissioning

> [!IMPORTANT]
> This looks superficially like Section -1's "pairing fails" bug (same project, same class of
> complaint) but is a genuinely different bug: a hard hang in a crypto peripheral driver, not a
> silent mDNS/SRP gap. Different symptom (task watchdog firing repeatedly vs. commissioner
> idling silently), different root cause, different fix location (per-target
> `sdkconfig.defaults.esp32h2`, not shared app code). Don't assume a "pairing fails on a new
> board" report is this same C6 mDNS bug without checking the log first.

This bug was originally found and fixed on the `feature/led-mosfet-control` branch (a
different device-type branch that shares the same underlying Matter/Thread stack), then the
same fix was applied here when H2 pin support was added to `main`'s fan controller.

### Symptom
First-ever real commissioning attempt on a physical Waveshare ESP32-H2-Zero hard-hung partway
through PASE: FreeRTOS task watchdog fired repeatedly (IDLE task starved, no recovery) while
the CHIP task was stuck inside `ecdsa_hal_verify_signature` - specifically spinning in
`ecdsa_ll_get_state`/`ecdsa_ll_set_stage` (`hal/esp32h2/include/hal/ecdsa_ll.h`) and never
returning - while processing the commissioner's `AddTrustedRootCertificate` command.

### Root cause
`CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY` defaults to `y` for any chip with
`SOC_ECDSA_SUPPORTED` (per this ESP-IDF version's `components/mbedtls/Kconfig`), which includes
the H2. That routes ECDSA signature verification through the H2's hardware ECDSA peripheral,
which appears to have a driver bug in this IDF version - it never returns from the
state-polling loop. Not application code; the C6 build never exercises this path at all
(`SOC_ECDSA_SUPPORTED` is H2-specific in this project's target set).

### Fix
Added to `sdkconfig.defaults.esp32h2`:
```ini
CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY=n
```
Falls back to mbedTLS's software ECDSA verification, bypassing the hardware peripheral
entirely. ECDSA verification only happens during commissioning, so the software-path
performance cost is irrelevant to runtime operation.

### Status: confirmed fixed on both branches, including real hardware on `main`
On the LED branch, with this fix applied, the H2 device commissioned successfully end-to-end
twice in one session (two CASE sessions/fabrics), and the identical firmware was later
re-flashed to a second, different physical H2 chip (the first was damaged during unrelated
soldering work) and confirmed to boot and open its commissioning window cleanly - the fix is in
`sdkconfig.defaults.esp32h2`, not tied to any single chip's state. This same fix, plus H2 pin
conditionals for the fan/button/RF-switch-skip (see Section 1's H2 pin table), was then ported
onto `main` so the fan controller itself builds and commissions on H2 hardware.

**Validated on real H2 hardware on `main` itself, same day.** Built for `esp32h2`, erase-flashed,
and commissioned successfully - `AddTrustedRootCertificate successful`, `AddNOC`/`successfully
created fabric`, `CASE Session established`, `GeneralCommissioning: Received
CommissioningComplete`, no watchdog hangs. As with the LED branch, this happened twice in the
same session (two fabrics - phone + Home Hub). The fan driver initialized correctly
(`app_driver: Fan speed updated to 50%`) on first boot. Consider H2 support on `main` fully
closed, not just build-verified.

**Operational note (not a bug):** the BLE commissioning window closes on its own after some
minutes of inactivity (`app_main: Commissioning window closed` in the log) if you don't
complete pairing in time. If you miss the window, you don't need to erase-flash or rebuild -
just reset the device (power cycle, or reattach `idf.py monitor`, which toggles the RTS line
and resets the board by default). A fresh boot with no fabric yet commissioned always reopens
BLE advertising and the commissioning window automatically.

---

## -1. 2026-07-18 session: commissioning never completes (mDNS/SRP root cause)

> [!IMPORTANT]
> Two more real bugs found and fixed this session, on top of everything below. Also: several
> "fixed" items elsewhere in this doc (sdkconfig untracking, MRP removal, minimal mDNS revert)
> turned out NOT to actually be applied in the repo despite being documented as done. Verify
> against the actual current file contents, not just this doc's prose.

### Bug 1: duplicate `main/app driver.cpp` (space in filename) broke every build
Commit `67a8a35` added the debounce re-entrancy fix to a *new* file `main/app driver.cpp`
(note the space) instead of editing the tracked `main/app_driver.cpp`. `idf_component_register
(SRC_DIRS ".")` globbed both, producing ninja error `multiple rules generate ...
app_driver.cpp.obj`. Fixed by merging the fix into the correctly-named file and deleting the
duplicate. While fixing this, also discovered `sdkconfig`/`sdkconfig.old` were still tracked in
git despite Section 5 below claiming `git rm --cached` had been run - it hadn't. Untracking them
exposed two more config gaps that only ever lived in that stale committed `sdkconfig` and were
never captured in `sdkconfig.defaults.esp32c6`: `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` (board has 4MB
flash; IDF's 2MB default doesn't fit `partitions.csv`) and `CONFIG_MBEDTLS_HKDF_C` (CHIP's crypto
layer calls `mbedtls_hkdf` directly; link fails without it). Both are now pinned in the defaults
file with comments.

### Bug 2: minimal mDNS has no SRP integration - commissioning hangs after Thread attach
**This is very likely the real explanation for the long-standing "connects, then drops and never
self-heals (No Response)" complaint, not just today's failed pairing.**

Symptom: every commissioning attempt completed PASE, attestation, CSR, and AddNOC successfully,
joined Thread cleanly, logged `mDNS service published: _matter._tcp`... and then nothing. The
commissioner just idled (BLE keepalive, occasional `ArmFailSafe` re-arms) until the device's own
120s fail-safe timer expired. `dns-sd -B _matter._tcp` from a Mac on the same LAN never showed
the current attempt's operational record, even though several *stale* records from earlier failed
attempts sat there indefinitely.

Root cause: `CONFIG_USE_MINIMAL_MDNS` defaults to `y` in CHIP's own Kconfig
(`connectedhomeip/config/esp32/components/chip/Kconfig`). This repo's history has a confusing
flip-flop on it - commit `d17ea4d` set it explicitly to `y` ("for proper Thread SRP discovery" -
backwards; minimal mDNS has *no* SRP code at all, verified by grep), then `f980c23` "reverted"
that by deleting the line. But because the Kconfig default is also `y`, deleting the override
did nothing - minimal mDNS silently stayed active in every build since, including the one running
in production. `chip_mdns=minimal` compiles `Advertiser_ImplMinimalMdns.cpp`, which only
broadcasts on the local Thread mesh via raw multicast. Apple's Thread border routers don't bridge
that to WiFi; they rely on the device doing real SRP registration, which they reflect via their
own Advertising Proxy. Minimal mDNS never does that, so the device is structurally invisible to
HomeKit on WiFi every time - **including after a normal Thread re-attach following any
disconnect**, not just at initial commissioning.

Fix: explicitly set `CONFIG_USE_MINIMAL_MDNS=n` in `sdkconfig.defaults.esp32c6`, which switches
the build to `chip_mdns=platform` (ESP32's `DnssdImpl.cpp` + `OpenThreadDnssdImpl.cpp`), which
calls the real OpenThread SRP client (`AddSrpService`) - already enabled via
`CONFIG_OPENTHREAD_SRP_CLIENT=y`. Confirmed via BUILD.gn that this is fully wired up for a
Thread-only (no WiFi/Ethernet) ESP32 target.

Also fixed as part of chasing this: esp_matter's own `device_callback_internal()`
(`esp_matter_core.cpp`) only restarts DNS-SD advertising on `kInterfaceIpAddressChanged`, gated
behind `#if CHIP_DEVICE_CONFIG_ENABLE_WIFI || CHIP_DEVICE_CONFIG_ENABLE_ETHERNET` - dead code on
a Thread-only build. Thread instead posts `kThreadStateChange`
(`GenericThreadStackManagerImpl_OpenThread::OnOpenThreadStateChange`), which nothing was
listening for. Added a handler in `app_main.cpp`'s `app_event_cb` that calls
`chip::app::DnssdServer::Instance().StartServer()` on `kThreadStateChange` - this is what fixed
the very first advertise failure (`CHIP_ERROR_INVALID_ADDRESS` racing the interface's IPv6
address assignment right after attach) and is also what actually re-triggers the new SRP-backed
publish path above whenever Thread's role or address changes (e.g. after a reconnect).

### Bug 3 (cosmetic, found after pairing worked): misleading FanMode ESP_FAIL on every debounce firing
Once commissioning succeeded, adjusting the fan speed logged `WriteAttribute failed with status:
Status<240>` / `Failed to update FanMode in debounce: ESP_FAIL` on every debounce firing, even
though the fan itself worked correctly (`PercentCurrent`/LEDC duty always applied fine). Status
240 (`0xF0`) is `Status::WriteIgnored`, a CHIP-internal, non-spec status code (see
`StatusCodeList.h`, marked "use only internally"). CHIP's own `FanControlCluster.cpp`
`PreAttributeChangedCallback` treats a raw write of `FanModeEnum::kOn` (4) as a convenience value:
it auto-substitutes the concrete `FanModeEnum::kHigh` (3) and reports `WriteIgnored` for the
original write to signal the substitution. esp-matter's generic `set_val_via_write_attribute()`
treats any non-Success status as a hard failure and logs `ESP_LOGE`, so this benign,
spec-compliant substitution was being reported as an error every single time.

`app_driver.cpp`'s debounce callback was writing `FanMode = 4` (kOn) whenever `target_speed > 0`.
Changed it to write `FanMode = 3` (kHigh) directly - the concrete value the cluster already
substitutes to - which produces the identical stored state without going through the
`WriteIgnored` path. Purely a log-noise fix; no functional change. **Committed but not yet
flashed** (didn't want to interrupt a working paired device to test a cosmetic fix) - apply on the
next natural rebuild.

### Status as of this session
**All three bugs confirmed fixed. The device paired successfully** after Bug 1 + Bug 2 fixes were
flashed - `monitor_log.txt` shows `GeneralCommissioning: Received CommissioningComplete` /
`Commissioning completed successfully`. Bug 3's fix is a follow-on cosmetic change, committed but
not yet flashed as of this writing.

**Self-heal CONFIRMED in the field (2026-07-18, later same day/session).** The original
long-standing complaint was that the device connects fine via HomeKit but later drops and shows
"No Response" without self-healing. Bug 2's fix (the `kThreadStateChange` handler in
`app_main.cpp`) was theorized to also fix this, since it re-announces the device via SRP after
*any* Thread re-attach, not just at initial commissioning - not just tested at that point though.
It has now been verified with a real test: physically unplugged the device's power (not a serial
reset), Apple Home showed "No Response" as expected while it was off, then on plugging power back
in the device rejoined Thread and became reachable in Apple Home again **on its own, with no
manual intervention** (no re-pairing, no reboot-via-serial). This is the actual real-world
scenario the whole day's investigation was aimed at fixing, and it worked. Consider this bug
closed pending longer-term observation - if "No Response" ever recurs, check for `Thread
role/address changed, restarting DNS-SD advertising` in the log around the reconnect and confirm
the device resolves via `dns-sd -B _matter._tcp` from a Mac on the same LAN before assuming this
is a new/different bug rather than a regression of this one.

`sdkconfig`/`sdkconfig.old` untracking is done (verified via `git ls-files` as part of the repo
cleanup later this session) - no longer an open item.

---

## 0. Corrected Root Cause (supersedes earlier EMI / data-race conclusions)

> [!IMPORTANT]
> The Thread disconnects are a **software/configuration** problem, not motor EMI. Every
> disconnect test was performed with the fan physically disconnected. The device drops
> from Thread regardless of the fan. Do not reintroduce the "conducted EMI is the sole
> cause" claim.

### Primary cause: committed `sdkconfig` overriding the defaults
`sdkconfig.defaults.*` only seeds a build when there is no existing `sdkconfig`. A stale
`sdkconfig` was committed to the repo and silently overrode the defaults. As a result the
firmware that actually shipped did NOT match `sdkconfig.defaults.esp32c6`:

* The committed `sdkconfig` had `# CONFIG_OPENTHREAD_ENABLED is not set` and
  `# CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE is not set`.
* `sdkconfig.old` shows a prior FTD build (`CONFIG_OPENTHREAD_FTD=y`, MTD not set).
* Meanwhile `sdkconfig.defaults.esp32c6` claimed MTD + RX_ON_WHEN_IDLE.

Three different device-role stories across three files. Editing the defaults did nothing
because the committed `sdkconfig` won. This is why "switching to MTD" never took effect and
the device kept behaving like an FTD that partitions itself as leader.

**Fix applied:**
* `sdkconfig` and `sdkconfig.old` are now git-ignored and must be removed from tracking
  (`git rm --cached sdkconfig sdkconfig.old`). Never commit a generated `sdkconfig` again.
* Device role is now set deliberately in `sdkconfig.defaults.esp32c6` (see Section 2).
* Always build via `clean_build_and_flash.sh`, which does `rm -f sdkconfig` before
  `idf.py set-target esp32c6` so the defaults are actually applied.

### Secondary cause: over-aggressive MRP retry intervals
The C6 defaults forced every MRP retry interval to 2000ms with MAX_RETRANS=3. Long retry
intervals widen the recovery window after a dropped packet. The runtime log
(`extra_monitor_log.txt`) shows the device healthy through ~4.8s uptime, then a ~9-minute
silent gap, then `OPENTHREAD:[N] Mle: Attach attempt 0, BetterParent` — i.e. it lost its
parent and re-attached, with no crash and no reboot (uptime kept climbing). That is a
link/timing signature, not RF noise (which would show `NoAck` bursts and 6LoWPAN fragment
failures) and not a reboot loop. The MRP overrides have been removed so the faster SDK
defaults apply.

### What was genuinely fixed earlier and should stay fixed
* **Deleting `print_ip_addresses_task`** (Section G, old doc): correct and necessary. That
  task called `otIp6GetUnicastAddresses()` without the OpenThread stack lock, a real data
  race. Keep it deleted. It was NOT, however, "the ultimate fix" — drops continued after it,
  so it was one bug among several.
* **RF switch init** (`init_rf_switch()` in app_main.cpp): correctly drives GPIO3=LOW
  (enable switch) and GPIO14=LOW (select ceramic antenna). Keep it.
* **FeatureMap = 1 (MultiSpeed)**: correctly set so the Home app shows a speed slider.

### Debunked claims removed from this document
* "Conducted EMI from the fan motor is the sole cause" — FALSE (fan was disconnected).
* "Debounce timer verified working" — the cited log line is not reproducible in the repo
  logs, and the timer had a re-entrancy bug (see Section 3). Now fixed.
* The tangled MED vs SED vs MTD narrative — MTD is a build-time device type; RX_ON_WHEN_IDLE
  is a radio behavior. They are independent. See Section 2 for the correct combination.

---

## 1. Hardware Overview & Pin Mapping

Built on the **Seeed Studio XIAO ESP32-C6**. (Earlier work also used the
**ESP32-H2-DevKitM-1-N4S**; both showed the same drops, which is consistent with the
shared software/config root cause above, not a per-board silicon defect.)

> [!WARNING]
> **DO NOT confuse physical pin labels with the chip's internal GPIO numbers.** The XIAO
> form factor uses a multiplexed pinout where silkscreen numbers do NOT map 1-to-1 to GPIOs.

### Seeed Studio XIAO ESP32-C6 Pin Map

| Silkscreen Label | Function | ESP32-C6 GPIO | Role in this Project |
| :---: | :---: | :---: | :--- |
| **D0** | Analog/Digital | **GPIO0** | Unused |
| **D1** | Analog/Digital | **GPIO1** | Unused |
| **D2** | Analog/Digital | **GPIO2** | **IoT Button Input** (Active High) |
| **D3** | Digital | **GPIO21** | **Noctua Fan PWM Output (LEDC)** |
| **D4** | I2C SDA | **GPIO22** | Unused |
| **D5** | I2C SCL | **GPIO23** | Unused |
| **D6** | UART TX | **GPIO16** | Unused |
| **D7** | UART RX | **GPIO17** | Unused |
| **D8** | SPI SCK | **GPIO19** | Unused |
| **D9** | SPI MISO | **GPIO20** | Unused |
| **D10** | SPI MOSI | **GPIO18** | Unused |
| **User LED** | Onboard LED | **GPIO15** | Status Indicator |
| **RF Switch Power** | Antenna Power | **GPIO3** | RF switch enable (drive LOW) |
| **RF Switch Port** | Antenna Select | **GPIO14** | Antenna select (LOW = ceramic) |

> [!NOTE]
> The onboard status LED turning off after commissioning is NORMAL. Do not treat "LED went
> dark" as evidence of a power/USB shutdown — in past testing the logs kept showing activity
> with the LED off. LED state is not a reliable liveness signal.

### PWM Fan Wiring (Noctua Standard)
* **Control Pin**: Fan PWM wire (blue) to physical pin **D3** = **GPIO21** in software.
* **LEDC**: low-speed mode, 10-bit duty (0-1023), **25 kHz** (Noctua spec).

### Waveshare ESP32-H2-Zero Pin Map (added later, see Section -2)
Selected via `idf.py set-target esp32h2`; same `main/app_driver.cpp` and `main/app_main.cpp`,
gated with `#if CONFIG_IDF_TARGET_ESP32H2`.

| Function | ESP32-H2 GPIO | Notes |
| :--- | :---: | :--- |
| **Noctua Fan PWM Output (LEDC)** | **GPIO12** | Plain, unshared GPIO on this board |
| **IoT Button Input** (Active High) | **GPIO10** | External button - NOT the onboard BOOT button (GPIO9), which enters the bootloader if held low at power-on |
| RF switch init | *(skipped)* | This board has a ceramic antenna with no RF-switch chip; `init_rf_switch()` is gated `#if CONFIG_OPENTHREAD_ENABLED && !CONFIG_IDF_TARGET_ESP32H2` |

Avoid GPIO13/14 (32.768kHz crystal per Espressif's official DevKitM-1 guide, which this board
is pin-compatible with) and GPIO23/24 (UART0 TX/RX - the serial console this project's whole
flashing workflow depends on).

---

## 2. Core Matter & Thread Architecture

### Thread Device Role (current: FTD, radio always on)
Configured in `sdkconfig.defaults.esp32c6`:
```ini
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_FTD=y
# CONFIG_OPENTHREAD_MTD is not set
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=y
```
* **FTD vs MTD:** FTD is the current choice (project preference) and is stable as long as a
  healthy border router (Apple TV/HomePod) is present. The old "detaches -> leader ->
  isolated partition" symptom was caused by the config drift in Section 0, not by FTD per se.
* **Fallback:** If leader-partition drops ever recur, switch to a Minimal End Device:
  `CONFIG_OPENTHREAD_FTD=n` / `CONFIG_OPENTHREAD_MTD=y`, keeping `RX_ON_WHEN_IDLE=y`. A
  non-sleepy MED keeps the radio on (low latency) and cannot become leader.
* **Do NOT** set `RX_ON_WHEN_IDLE=n` (sleepy) for this mains-powered controller; polling
  latency hurts CASE resumption and large wildcard reads.

### MRP timing
The four `CONFIG_MRP_*` overrides (all 2000ms) have been removed. Use SDK defaults unless
there is a measured reason to change them; if you do, re-add the lines deliberately and
document why here.

---

## 3. Application Driver Notes

### Debounce timer (fixed)
`app_driver.cpp` debounces rapid `PercentSetting` writes (300ms) before applying LEDC duty
and syncing `PercentCurrent`/`FanMode`.

* **Bug that was fixed:** the debounce callback calls `attribute::update()`, which re-enters
  the attribute-update callback path. That re-entry previously hit the same
  `esp_timer_stop`/`esp_timer_start_once` logic and re-armed the timer from inside its own
  callback, so it effectively never settled. A `driver_initiated_update` guard now short-
  circuits driver-issued writes so only genuine user writes (re)arm the timer.
* If you ever simplify: for a fan slider it is acceptable to drop the debounce entirely and
  apply LEDC duty immediately. The "packet storm" it guards against is not a real problem on
  a correctly configured non-sleepy device.

---

## 4. Critical Developer Workflows

### Target selection & clean builds
Always build through `clean_build_and_flash.sh`. It removes the stale `sdkconfig`, sets the
target to `esp32c6` (so `sdkconfig.defaults.esp32c6` is merged), builds, erases flash, and
flashes. Setting the target explicitly matters: without it ESP-IDF defaults to `esp32` and
skips the C6 defaults.

### Verifying the flashed config actually matches intent
After flashing, confirm in the boot log that OpenThread is enabled and the MLE role settles
to `child`/`router` (FTD) rather than `disabled` or `leader`. Do not trust the defaults file
alone — verify against the running device.

### Stabilizing HomeKit connection failures
If the chip shows "Not Responding", or logs `SRP update error: ... duplicated` or
`unknown session`, the border router likely holds stale fabrics/sessions from a previous
flash. Clear NVS and re-pair:
```bash
idf.py erase-flash
idf.py flash monitor
```
Remove the accessory from Apple Home and re-add with Manual Setup Code `20202021`,
Discriminator `3840`.

### Diagnosing a drop
Capture the serial log ACROSS the disconnect; the informative part is the gap. Distinguish:
* Role goes to `leader` -> FTD partition problem -> switch to MTD fallback.
* `detached -> child` re-attach after a silent gap -> link/timing -> MRP/defaults change
  should help; check border-router health and RSSI.
External checks from a Mac on the same network remain useful: `ping6` the device's OMR
address, `dns-sd -B _matter._tcp` to confirm the border router's advertising proxy, and
`dns-sd -L`/`-G v6` to confirm hostname resolution.

---

## 5. Git & Workspace Rules
* `.agents/` is tracked in Git and synced across the user's flashing/dev Macs.
* **NEVER commit `sdkconfig` or `sdkconfig.old`** — they are generated and override the
  defaults. They are now in `.gitignore`. Run `git rm --cached sdkconfig sdkconfig.old` once
  to stop tracking them.
* Auto-push policy: code/config/doc changes are staged, committed, and pushed immediately so
  they are available on the flashing machine.
