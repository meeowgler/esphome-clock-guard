# clock_guard for ESPHome

An ESPHome external component that works around the ESP32-C6 SYSTIMER fault described in
[espressif/esp-idf#19036](https://github.com/espressif/esp-idf/issues/19036): on some chips,
the counter behind `esp_timer` (SYSTIMER counter 0) suddenly loses minutes to hours.

**Status: experimental.** Use it only if your C6 boards show the symptoms below.

## Symptoms

`esp_timer`, and with it ESPHome's `millis()`, uptime and scheduler, steps backward. ESPHome's
next scheduled run is then due at a time the clock has to count back up to, so:

- every `update_interval` sensor (uptime, debug, Wi-Fi signal) goes quiet for exactly the time
  that was lost, often 15 minutes to several hours;
- uptime then resumes from where it stopped, so it ends up behind the wall clock;
- event-driven work keeps running: a Bluetooth proxy keeps forwarding adverts through it.

Some affected chips also crash: an interrupt or task watchdog with the backtrace in
`esp_timer_impl_get_time` / `systimer_hal_get_counter_value`.

## How it works

The C6 has a second SYSTIMER counter (counter 1, which drives the FreeRTOS tick), running from
the same clock. Their difference is constant, so a task at the highest priority wakes every
1 ms and compares the two. If `esp_timer` has fallen behind by more than the threshold, it
steps `esp_timer` forward by exactly the lost time (`esp_timer_private_advance()`) and re-arms
the `esp_timer` alarm, so timers that fell due in the meantime run at once.

- Both counters are read with interrupts off and with a bounded wait, so the guard can never
  hang on the counter itself.
- A slow read can only make `esp_timer` look ahead, never behind, so delays cannot cause a
  false correction. A clock that is ahead is never touched.
- A counter that stops, rather than jumps back, is stepped forward until it runs again;
  corrections less than a second apart count as one event.
- If counter 1 itself goes backward, the guard re-learns the offset and counts it as a
  "tick counter jump" instead of acting.

Each check takes a few microseconds; on a busy Bluetooth proxy the main loop time went from
19 to 20 ms.

## Limits

- **ESP32-C6, ESP-IDF framework only.**
- **No automatic light sleep.** Power management (`CONFIG_PM_ENABLE`) and tickless idle step
  both counters during sleep, so the build stops with an error if either is on. Wi-Fi modem
  sleep (`power_save_mode: LIGHT`, the ESP32 default) is fine.
- It works around the symptom; it does not fix the cause, and it cannot prevent the watchdog
  crash where the counter's "value valid" flag never sets.
- `esp_timer_private_advance()` is an ESP-IDF private API. Check that the component still
  builds and behaves after ESP-IDF updates.
- Tested on ESPHome 2026.10.0 (ESP-IDF 5.5) with Seeed XIAO ESP32-C6 boards, chip revisions
  0.1 and 0.2.

## Usage

```yaml
external_components:
  - source: github://meeowgler/esphome-clock-guard
    components: [clock_guard]

clock_guard:
  corrections:
    name: "Clock Corrections"
  time_recovered:
    name: "Clock Time Recovered"
  tick_counter_jumps:
    name: "Clock Tick Counter Jumps"
```

### Options

- **threshold** (*Optional*, time, default `2ms`, minimum `500us`): how far `esp_timer` may
  fall behind before it is corrected.
- **update_interval** (*Optional*, time, default `60s`): how often the sensors publish. They
  also publish at once after a correction.
- **corrections** (*Optional*, sensor): number of correction events since boot.
- **time_recovered** (*Optional*, sensor, seconds): total time put back since boot.
- **tick_counter_jumps** (*Optional*, sensor): how often counter 1 went backward. Non-zero
  would mean the fault is not limited to counter 0.

Each correction is also logged as a warning:

```
[W][clock_guard]: esp_timer lost 600.000 s and was stepped forward (1 corrections, 600.0 s in total)
```

### Testing it

`simulate_jump(seconds)` steps `esp_timer` back the way the fault does (never further than
the uptime allows). Keep a test button disabled by default:

```yaml
clock_guard:
  id: clock_guard_1

button:
  - platform: template
    name: "Clock Guard Test"
    entity_category: diagnostic
    disabled_by_default: true
    on_press:
      - lambda: id(clock_guard_1).simulate_jump(600);
```

## License

MIT
