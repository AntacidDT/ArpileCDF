# LOFS-001 — Wi-Fi scan finds no networks (Magif invisible; `wifi scan` refused while disconnected)

- **Status:** SOLVED
- **Severity:** High
- **Subsystem:** Wi-Fi (`components/wifi_test`, ESP-Hosted P4↔C6 link, `components/arpile_ui` terminal + WiFi app)
- **Date:** 2026-08-22

## Symptom

Arpile's Wi-Fi scanner did not reliably show nearby networks on the
ESP32-P4 Nano + ESP32-C6 (esp-hosted) system:

- The known nearby AP **Magif** (2.4 GHz, **channel 5**) never appeared in the
  scan list.
- The terminal command `wifi scan` returned:

  ```text
  Not connected. Try: wifi connect <ssid> <password>
  ```

  i.e. the scanner refused to scan simply because no connection existed — a
  scanner must obviously work while disconnected.
- Serial monitor logs showed `WIFI_EVENT_SCAN_DONE` apparently never arriving,
  and `esp_wifi_scan_get_ap_num()` returning `ESP_OK, n=1` while
  `esp_wifi_scan_get_ap_records()` returned 0 records.
- The WiFi GUI app auto-rescanned every ~7 s but kept showing few or zero APs.

## Initial Hypotheses

At various points we suspected:

1. The ESP32-C6 coprocessor was dead / RF broken.
2. ESP-Hosted version mismatch: C6 slave firmware v2.7.0 vs host stack built
   against v2.12.12.
3. Regulatory / country configuration blocking channels 12/13 (and possibly
   others).
4. Scan RPC/event forwarding over the esp-hosted SDIO link being broken — in
   particular `WIFI_EVENT_SCAN_DONE` never reaching the P4 host.
5. AP record retrieval failing (`esp_wifi_scan_get_ap_records()` returning 0).
6. UI result propagation: backend found APs but the Arpile UI failed to render
   them.
7. An asynchronous timing/race condition between scan start, scan completion,
   and UI rendering.

## Investigation

Key tests, observations, and discoveries:

- **C6 slave rebuilt to ESP-Hosted v2.12.12** to match the host's esp-hosted
  version. This removed the version mismatch but did *not* by itself fix
  scanning.
- **Channel check:** Magif sits on channel 5, so any channels-12/13 regulatory
  explanation could not account for its absence.
- **Hardware sanity proof:** the older *Espelt* firmware consistently detected
  Magif on this exact hardware, and manual connects (Ctrl+I → SSID/password)
  worked. Radio and C6 were alive.
- **TX power bump** to +20 dBm (`esp_wifi_set_max_tx_power(80)`) improved
  association reliability but did not make scans return APs.
- **Diagnostic instrumentation** (`wifi rawscan` command + `ESP_LOGI` dumps in
  the `SCAN_DONE` handler) revealed:
  - `WIFI_EVENT_SCAN_DONE` was frequently never seen by the host event handler
    (the remote link drops it), so the event-path harvest often never ran.
  - When a timeout-based "live harvest" ran instead,
    `get_ap_records()` still yielded 0 records despite `get_ap_num()` reporting
    some.
- **Monitor logs proved unreliable** in this workflow (port contention,
  re-enumeration drops, log-level questions), which repeatedly obscured what
  the code was actually doing.
- **Final decisive step:** stop trusting comments/diagnostics and read the
  actual pipeline end-to-end:
  `arpile_wifi_start_scan()` → `esp_wifi_scan_start()` → `WIFI_EVENT_SCAN_DONE`
  → `esp_wifi_scan_get_ap_num()` → `esp_wifi_scan_get_ap_records()` →
  `merge_records()` → shared model → UI/terminal.

That read exposed the true failure chain (see Root Cause).

## False Leads

Things investigated that were **not** the root cause:

- **Dead/broken C6 hardware** — disproven by Espelt scans and working manual
  connects on the same radio.
- **ESP-Hosted version mismatch as the direct cause of empty scans** — real
  hygiene issue, fixed by reflashing the C6 slave to v2.12.12, but scans stayed
  broken afterwards.
- **Country/regulatory config and channels 12/13** — Magif is on channel 5;
  irrelevant here.
- **Weak RF / TX power defaults** — the +20 dBm bump helped connections but
  scans remained empty.
- **"SCAN_DONE loss is THE bug"** — dropped `SCAN_DONE` events are a genuine
  secondary quirk of the remote link, but even when the fallback harvest ran,
  results had already been destroyed host-side (see below). Treating event
  delivery as the primary bug sent us down the wrong path for a long time.

## Root Cause

Four stacked host-side (P4) defects, no C6/radio fault:

1. **`wifi scan` was never wired up.**
   `term_wifi()` in `components/arpile_ui/arpile_app_terminal.c` only
   recognized the `connect` and `rawscan` subcommands. Plain `wifi scan` fell
   through to the status branch and printed "Not connected". A previously
   claimed fix for this had never actually been applied.

2. **The result buffer was being consumed speculatively.**
   `arpile_wifi_poll()` called `esp_wifi_scan_get_ap_records()` roughly once
   per second "to probe progress". In ESP-IDF that call **consumes** the
   driver's internal scan-result buffer. The t=1 s probe ate the records but
   then discarded them because a `<3000 ms` guard refused the partial sweep;
   the later "real" harvest got nothing. This is exactly the observed
   `ap_num=OK(n=…) → records=0` signature.

3. **Nobody harvested when the WiFi GUI was closed.**
   `arpile_wifi_poll()` was only invoked from the WiFi app's update function,
   so terminal-initiated scans were never collected at all when `SCAN_DONE`
   was dropped by the remote link.

4. **No on-device way to see results.**
   The terminal only said "results appear in monitor logs", making the whole
   path dependent on unreliable serial logging.

## Fix

P4-side only; no further C6 changes, no country changes, no added delays:

- `components/wifi_test/wifi_test.c`
  - Introduced a single `harvest_results()` used **exactly once per scan**, either
    from the `WIFI_EVENT_SCAN_DONE` handler or from a 6 s timeout fallback in
    `arpile_wifi_poll()` (covers the dropped-event case).
  - Rewrote `arpile_wifi_poll()` to **hands off the result buffer** until the
    sweep completes (event flag or timeout). No more speculative
    `get_ap_records()` probing.
  - Removed the verbose diagnostic log block from the event handler.
  - Added `channel` to the shared `arpile_wifi_ap_t` model
    (`include/wifi_test.h`) populated from `r->primary`.
- `components/arpile_ui/arpile_ui.c`
  - Central UI loop now calls `arpile_wifi_poll()` every tick, so scans
    progress regardless of which app (if any) is open.
- `components/arpile_ui/arpile_app_terminal.c`
  - `wifi scan` (and alias `rawscan`) now calls `arpile_wifi_start_scan()`
    unconditionally — works while disconnected.
  - New `wifi list` subcommand prints the shared AP model
    (SSID/RSSI/channel/auth) directly on the device screen.
  - New `term_update()` announces scan completion in the terminal itself
    ("Scan done: N network(s)") via edge detection on
    `arpile_wifi_scan_in_progress()`. No serial-monitor dependency.
  - Updated `help`.

Terminal and GUI now both render the same underlying merged model
(`s_merged[]` via `arpile_wifi_get_ap_count()/get_ap()`).

## Verification

- Project built clean (full rebuild after an IDF-master toolchain/env refresh)
  and flashed to `/dev/ttyACM0`; esptool hash verified; board boots without
  panic/assert.
- On-device acceptance run:
  1. Device disconnected from Wi-Fi.
  2. Terminal: `wifi scan` → starts scanning (no more "Not connected").
  3. After a few seconds the terminal itself printed
     **"Scan done: 7 network(s)"**.
  4. `wifi list` displayed all 7 networks including **Magif**.
- Follow-up session fully closed the loop: WiFi GUI app lists the same
  networks, PgUp/PgDn move selection one row at a time (scrolling past the
  visible window), Enter chooses an AP, password connect succeeded
  (**"connected!"**), and successful connections are now remembered in NVS for
  auto-reconnect at boot (`wifi forget` clears).

## Lessons Learned

- **`esp_wifi_scan_get_ap_records()` consumes the internal scan buffer.** It
  must be called exactly once per completed scan. Never use it as a
  "progress probe".
- Detect async-scan completion with non-consuming signals only (the
  `WIFI_EVENT_SCAN_DONE` flag plus a bounded timeout), then harvest once.
- Don't park background polling inside one app's lifecycle. Centralize it in
  the main loop so subsystem work continues no matter which window is open.
- Verify claimed fixes against the actual code. This incident dragged partly
  because a fix ("`wifi scan` calls `start_scan`") existed in conversation but
  not in the repository.
- Prefer feedback surfaces that survive without a PC: on-device terminal
  output beat serial-monitor diagnostics throughout this saga.
- One strong counterexample ("Magif is on channel 5") is enough to kill a
  whole class of hypotheses (regulatory/12–13); test cheap falsifiers early.
