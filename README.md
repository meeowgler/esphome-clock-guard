# systimer_guard for ESPHome

An ESPHome external component that works around the ESP32-C6 SYSTIMER fault described in
[espressif/esp-idf#19036](https://github.com/espressif/esp-idf/issues/19036): on some chips,
the counter behind `esp_timer` (SYSTIMER counter 0) suddenly loses minutes to hours, and its
snapshot handshake can wedge.

**Status: experimental.** It has corrected one real fault so far (see [Results](#results)).
Use it only if your C6 boards show the symptoms below, and please report how it behaves.

## Related work

[danjurgens/esp32c6-systimer-backstep](https://github.com/danjurgens/esp32c6-systimer-backstep)
(GPL-3.0) is where this fault was pinned down: a bare ESP-IDF reproducer, the register-level
analysis showing the bit-clear signature, a ledger of verified events, and its own ESPHome
`clock_guard` and `loop_watchdog` components, which have absorbed real events in production
since September 2026. **If you want the established option, use that one.**

This component was written independently, and was called `clock_guard` until 25 October 2026;
it was renamed to avoid confusion with danjurgens' component of that name. The main
differences: it compares the two SYSTIMER hardware counters directly, every 1 ms, with
interrupts off, rather than `esp_timer` against the FreeRTOS tick count; its counters reset at
boot instead of persisting in NVS; and it has no correction cap, since a backward step can
never be larger than the counter's own value.

## Results

- **25 October 2026, first real event:** on the board that had stalled most (20 times in 17
  days), `esp_timer` lost 2667.577 s (44.5 min). That is exactly bits 28, 29, 30, 31, 32 and 35
  of the 16 MHz counter cleared at once. The guard put the time back within its 1 ms check, and
  the board's 60-second uptime reports carried on without a gap; before the guard, the same
  fault silenced the board for the time lost. Counter 1 did not move backward.
- **26 October 2026:** the same board lost 1149.239 s (19.2 min), bits 27, 30 and 34.
  Corrected with no gap.
- **27 October 2026:** a board that had never shown a stall lost 4.079 s, bits 18 and 21 to 25.
  Events this small were invisible before: a few seconds' silence in a 60-second report never
  looked like a stall.
- **27 October 2026, Wi-Fi power saving set to off (not yet confirmed applied):** to see whether
  modem sleep sets the fault off, the worst board was set to `power_save_mode: NONE`. Seven and a
  half hours later it lost 1166.016 s (19.4 min), bits 27, 28, 30 and 34, corrected with no gap.
  It is not yet confirmed that the Wi-Fi driver applied NONE while Bluetooth is running (ESPHome
  logs a refused setting only at verbose level, and the board's ping times look like modem sleep),
  so this does not yet show whether modem sleep plays a part.
- **To 28 October 2026:** 4 real events on 2 of 12 boards since the guard went on all of them
  on 25 October, all corrected; no stalls, no snapshot recoveries, and counter 1 never moved
  backward.
- Simulated 600 s jumps (see [Testing it](#testing-it)) are corrected to the millisecond.
- A simulated wedged handshake (`simulate_wedge()`) ends in an abort about a second later, and
  the board restarts with an `Abort` crash record and reset reason "exception/panic".

## Symptoms

`esp_timer`, and with it ESPHome's `millis()`, uptime and scheduler, steps backward. ESPHome's
next scheduled run is then due at a time the clock has to count back up to, so:

- every `update_interval` sensor (uptime, debug, Wi-Fi signal) goes quiet for exactly the time
  that was lost, often 15 minutes to several hours;
- uptime then resumes from where it stopped, so it ends up behind the wall clock;
- event-driven work keeps running: a Bluetooth proxy keeps forwarding adverts through it.

Some affected chips also crash: an interrupt or task watchdog with the backtrace in
`esp_timer_impl_get_time` / `systimer_hal_get_counter_value`, where the counter's snapshot
"value valid" flag never sets.

## How it works

The C6 has a second SYSTIMER counter (counter 1, which drives the FreeRTOS tick), running from
the same clock. Their difference is constant, so a task at the highest priority wakes every
1 ms and compares the two. If `esp_timer` has fallen behind by more than the threshold, it
steps `esp_timer` forward by exactly the lost time (`esp_timer_private_advance()`) and re-arms
the `esp_timer` alarm, so timers that fell due in the meantime run at once.

- Both counters are read with interrupts off. A slow read can only make `esp_timer` look
  ahead, never behind, so delays cannot cause a false correction. A clock that is ahead is
  never touched.
- A counter that stops, rather than jumps back, is stepped forward until it runs again;
  corrections less than a second apart count as one event.
- If counter 1 itself goes backward, the guard re-learns the offset and counts it as a
  "tick counter jump" instead of acting.

### A wedged snapshot handshake

ESP-IDF reads a counter by requesting a snapshot and spinning, without a limit, until its
"value valid" flag sets. If the flag never sets, every `esp_timer` reader hangs until a
watchdog resets the board. The guard's own reads are bounded and re-request the snapshot up
to four times; a re-request can also release the other readers, which wait on the same flag,
and each one is counted as a "snapshot recovery". If the reads still fail for about a second,
the guard calls `esp_system_abort()` (without logging, since the logger reads the clock), so
the board restarts promptly with a crash record naming the cause instead of waiting for a
watchdog.

Each check takes a few microseconds; on a busy Bluetooth proxy the main loop time went from
19 to 20 ms.

## Limits

- **ESP32-C6, ESP-IDF framework only.**
- **No automatic light sleep.** Power management (`CONFIG_PM_ENABLE`) and tickless idle step
  both counters during sleep, so the build stops with an error if either is on.
- **Wi-Fi modem sleep** (`power_save_mode: LIGHT`, the ESP32 default) works with the guard,
  but danjurgens reports that with modem sleep on, the fault can also hang the Wi-Fi PHY path
  faster than any poll can act (an interrupt watchdog reset). `power_save_mode: NONE` avoids
  that form, if the driver applies it alongside Bluetooth (being checked; see [Results](#results)).
- It works around the symptom; it does not fix the cause. A wedged handshake still costs a
  restart if re-requesting the snapshot does not clear it.
- `esp_timer_private_advance()` is an ESP-IDF private API. Check that the component still
  builds and behaves after ESP-IDF updates.
- Tested on ESPHome 2026.10.0 (ESP-IDF 5.5) with Seeed XIAO ESP32-C6 boards, chip revisions
  0.1 and 0.2.

## Usage

```yaml
external_components:
  - source: github://meeowgler/esphome-systimer-guard
    components: [systimer_guard]

systimer_guard:
  corrections:
    name: "Clock Corrections"
  time_recovered:
    name: "Clock Time Recovered"
  tick_counter_jumps:
    name: "Clock Tick Counter Jumps"
  snapshot_recoveries:
    name: "Clock Snapshot Recoveries"
```

### Options

- **threshold** (*Optional*, time, default `2ms`, minimum `500us`): how far `esp_timer` may
  fall behind before it is corrected.
- **update_interval** (*Optional*, time, default `60s`): how often the sensors publish. They
  also publish at once after any event.
- **corrections** (*Optional*, sensor): number of correction events since boot.
- **time_recovered** (*Optional*, sensor, seconds): total time put back since boot.
- **tick_counter_jumps** (*Optional*, sensor): how often counter 1 went backward. Non-zero
  would mean the fault is not limited to counter 0.
- **snapshot_recoveries** (*Optional*, sensor): reads that only succeeded after the snapshot
  was re-requested.

Each event is also logged as a warning:

```
[W][systimer_guard]: esp_timer lost 600.000 s and was stepped forward (1 corrections, 600.0 s in total)
```

### Testing it

`simulate_jump(seconds)` steps `esp_timer` back the way the fault does (never further than
the uptime allows). `simulate_wedge()` makes the guard's own reads fail, so it should abort
and the board restart about a second later. Keep test buttons disabled by default:

```yaml
systimer_guard:
  id: systimer_guard_1

button:
  - platform: template
    name: "Clock Guard Test"
    entity_category: diagnostic
    disabled_by_default: true
    on_press:
      - lambda: id(systimer_guard_1).simulate_jump(600);
  - platform: template
    name: "Clock Guard Wedge Test"
    entity_category: diagnostic
    disabled_by_default: true
    on_press:
      - lambda: id(systimer_guard_1).simulate_wedge();
```

## License

MIT
