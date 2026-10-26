#include "systimer_guard.h"

#include <cinttypes>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_private/esp_timer_private.h>
#include <esp_private/systimer.h>
#include <hal/systimer_ll.h>
#include <sdkconfig.h>
#include <soc/systimer_struct.h>
#include "esphome/core/log.h"

// Light sleep steps both SYSTIMER counters by its own sleep compensation, which this check
// would read as a fault. The guard is only for boards that never light-sleep.
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE || CONFIG_PM_ENABLE
#error "systimer_guard cannot be used with power management / automatic light sleep"
#endif

namespace esphome::systimer_guard {

static const char *const TAG = "systimer_guard";
static portMUX_TYPE s_read_lock = portMUX_INITIALIZER_UNLOCKED;

// Snapshot handshake limits: each request is polled this many times, and re-requested this
// many times, before the read counts as failed. A normal snapshot is valid within a few polls.
static const int SNAPSHOT_POLLS = 250;
static const int SNAPSHOT_REQUESTS = 4;
// Consecutive failed checks (one per millisecond) before the guard gives up and aborts.
static const uint32_t WEDGE_ABORT_CHECKS = 1000;

enum class ReadResult { OK, RECOVERED, FAILED };

// Reads one SYSTIMER counter, in ticks. Unlike the ESP-IDF HAL, which spins forever waiting
// for the snapshot's "value valid" flag, the wait is bounded and the snapshot is re-requested
// if it does not arrive. Affected chips can wedge on that flag (seen as watchdog resets with
// esp_timer stuck in systimer_hal_get_counter_value), and the guard must never hang there,
// least of all with interrupts off. A re-request may also release the other readers, which
// wait on the same flag.
static inline ReadResult read_counter_ticks(uint32_t counter, uint64_t *ticks) {
  bool valid = false;
  int request = 0;
  for (; request < SNAPSHOT_REQUESTS && !valid; request++) {
    systimer_ll_counter_snapshot(&SYSTIMER, counter);
    for (int poll = 0; poll < SNAPSHOT_POLLS; poll++) {
      if (systimer_ll_is_counter_value_valid(&SYSTIMER, counter)) {
        valid = true;
        break;
      }
    }
  }
  if (!valid)
    return ReadResult::FAILED;
  uint32_t lo, hi;
  uint32_t lo_start = systimer_ll_get_counter_value_low(&SYSTIMER, counter);
  do {
    lo = lo_start;
    hi = systimer_ll_get_counter_value_high(&SYSTIMER, counter);
    lo_start = systimer_ll_get_counter_value_low(&SYSTIMER, counter);
  } while (lo_start != lo);
  *ticks = (uint64_t(hi) << 32) | lo;
  return request > 1 ? ReadResult::RECOVERED : ReadResult::OK;
}

// Reads both counters back to back with interrupts off, tick counter first. A delay between
// the two reads can then only make esp_timer look AHEAD, never behind, so it cannot cause a
// false correction. esp_timer_get_time() is exactly systimer_ticks_to_us(counter 0).
// Gives esp_timer minus the tick counter, in microseconds.
static ReadResult read_offset(int64_t *offset_us, uint64_t *tick_us) {
  uint64_t tick_ticks = 0, timer_ticks = 0;
  portENTER_CRITICAL(&s_read_lock);
  ReadResult tick = read_counter_ticks(SYSTIMER_COUNTER_OS_TICK, &tick_ticks);
  ReadResult timer = tick == ReadResult::FAILED ? ReadResult::FAILED
                                                : read_counter_ticks(SYSTIMER_COUNTER_ESPTIMER, &timer_ticks);
  portEXIT_CRITICAL(&s_read_lock);
  if (tick == ReadResult::FAILED || timer == ReadResult::FAILED)
    return ReadResult::FAILED;
  *tick_us = systimer_ticks_to_us(tick_ticks);
  *offset_us = int64_t(systimer_ticks_to_us(timer_ticks)) - int64_t(*tick_us);
  return (tick == ReadResult::RECOVERED || timer == ReadResult::RECOVERED) ? ReadResult::RECOVERED
                                                                           : ReadResult::OK;
}

static void kick_cb(void *arg) {}

void SystimerGuard::setup() {
  // A one-shot esp_timer started right after a correction re-arms the esp_timer alarm, so
  // timers that fell due while the clock was behind run at once.
  esp_timer_create_args_t args{};
  args.callback = kick_cb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "systimer_guard_kick";
  if (esp_timer_create(&args, &this->kick_timer_) != ESP_OK) {
    this->mark_failed();
    return;
  }
  // Highest priority, so after a jump the guard runs at the next tick (within 1 ms), before
  // other tasks can schedule work against the wrong time. Each check takes a few microseconds.
  if (xTaskCreate(SystimerGuard::task_entry_, "systimer_guard", 2560, this, configMAX_PRIORITIES - 1, nullptr) !=
      pdPASS) {
    this->mark_failed();
    return;
  }
  this->running_ = true;
}

void SystimerGuard::task_entry_(void *arg) { static_cast<SystimerGuard *>(arg)->run_(); }

void SystimerGuard::run_() {
  // The two counters run from the same clock, so esp_timer minus the tick counter is a fixed
  // offset. Take the smallest offset seen as the true one (read delays only add to it).
  int64_t base = INT64_MAX;
  uint64_t prev_tick_us = 0;
  uint64_t last_correction_tick_us = 0;
  uint32_t failed_checks = 0;
  const int64_t threshold = this->threshold_us_;
  const TickType_t period = pdMS_TO_TICKS(1) > 0 ? pdMS_TO_TICKS(1) : 1;
  for (;;) {
    vTaskDelay(period);
    int64_t offset;
    uint64_t tick_us;
    ReadResult read = this->simulate_wedge_.load(std::memory_order_relaxed) ? ReadResult::FAILED
                                                                             : read_offset(&offset, &tick_us);
    if (read == ReadResult::FAILED) {
      // The handshake stays wedged: every other esp_timer reader is spinning on it too, so the
      // board is stuck until a watchdog fires. Abort sooner, without logging (the logger reads
      // the clock): the board restarts with an "exception/panic" crash record naming the cause.
      if (++failed_checks >= WEDGE_ABORT_CHECKS)
        esp_system_abort("systimer_guard: SYSTIMER snapshot handshake wedged (value-valid never set)");
      continue;
    }
    failed_checks = 0;
    if (read == ReadResult::RECOVERED)
      this->snapshot_recoveries_.fetch_add(1, std::memory_order_relaxed);
    if (prev_tick_us != 0 && tick_us < prev_tick_us) {
      // The tick counter itself went backward: it is no reference now, so re-learn the offset.
      this->tick_counter_jumps_.fetch_add(1, std::memory_order_relaxed);
      base = offset;
      prev_tick_us = tick_us;
      continue;
    }
    prev_tick_us = tick_us;
    if (base == INT64_MAX || (offset < base && base - offset <= threshold)) {
      base = offset;  // first reading, or a slightly better one
    } else if (base - offset > threshold) {
      // esp_timer lost time (jumped back or stopped): step it forward by exactly that much.
      int64_t lost = base - offset;
      esp_timer_private_advance(lost);
      esp_timer_start_once(this->kick_timer_, 1);
      // Corrections less than a second apart are one event (a counter that stops rather than
      // jumps is stepped forward every few milliseconds until it runs again).
      uint32_t lost_ms = uint32_t(lost / 1000);
      if (last_correction_tick_us == 0 || tick_us - last_correction_tick_us > 1000000) {
        this->last_jump_ms_.store(lost_ms, std::memory_order_relaxed);
        this->corrections_.fetch_add(1, std::memory_order_relaxed);
      } else {
        this->last_jump_ms_.fetch_add(lost_ms, std::memory_order_relaxed);
      }
      last_correction_tick_us = tick_us;
      this->recovered_ms_.fetch_add(lost_ms, std::memory_order_relaxed);
    } else if (offset - base > threshold) {
      // esp_timer ahead of the tick counter: not the known fault. Re-learn rather than act.
      base = offset;
    }
  }
}

void SystimerGuard::loop() {
  // Logged from the main loop: the guard task only counts, so it never waits on the logger.
  uint32_t corrections = this->corrections_.load(std::memory_order_relaxed);
  if (corrections != this->logged_corrections_) {
    ESP_LOGW(TAG, "esp_timer lost %.3f s and was stepped forward (%" PRIu32 " corrections, %.1f s in total)",
             this->last_jump_ms_.load(std::memory_order_relaxed) / 1000.0f, corrections,
             this->recovered_ms_.load(std::memory_order_relaxed) / 1000.0f);
    this->logged_corrections_ = corrections;
    this->update();
  }
  uint32_t jumps = this->tick_counter_jumps_.load(std::memory_order_relaxed);
  if (jumps != this->logged_tick_counter_jumps_) {
    ESP_LOGW(TAG, "The tick counter (SYSTIMER counter 1) went backward (%" PRIu32 " times); offset re-learned",
             jumps);
    this->logged_tick_counter_jumps_ = jumps;
    this->update();
  }
  uint32_t recoveries = this->snapshot_recoveries_.load(std::memory_order_relaxed);
  if (recoveries != this->logged_snapshot_recoveries_) {
    ESP_LOGW(TAG, "A counter snapshot had to be re-requested (%" PRIu32 " times)", recoveries);
    this->logged_snapshot_recoveries_ = recoveries;
    this->update();
  }
}

void SystimerGuard::update() {
  if (this->corrections_sensor_ != nullptr)
    this->corrections_sensor_->publish_state(this->corrections_.load(std::memory_order_relaxed));
  if (this->time_recovered_sensor_ != nullptr)
    this->time_recovered_sensor_->publish_state(this->recovered_ms_.load(std::memory_order_relaxed) / 1000.0f);
  if (this->tick_counter_jumps_sensor_ != nullptr)
    this->tick_counter_jumps_sensor_->publish_state(this->tick_counter_jumps_.load(std::memory_order_relaxed));
  if (this->snapshot_recoveries_sensor_ != nullptr)
    this->snapshot_recoveries_sensor_->publish_state(this->snapshot_recoveries_.load(std::memory_order_relaxed));
}

void SystimerGuard::simulate_jump(uint32_t seconds) {
  // Never step below zero: the counter would wrap to ~8.9 years, which the guard (rightly)
  // takes as "ahead", not as the fault. Real faults only clear bits, so they cannot wrap.
  int64_t step = int64_t(seconds) * 1000000LL;
  int64_t limit = esp_timer_get_time() - 10000000LL;
  if (step > limit)
    step = limit;
  if (step <= 0) {
    ESP_LOGW(TAG, "Test: uptime too short to step the clock back");
    return;
  }
  ESP_LOGW(TAG, "Test: stepping esp_timer back %.1f s", step / 1e6f);
  esp_timer_private_advance(-step);
}

void SystimerGuard::simulate_wedge() {
  ESP_LOGW(TAG, "Test: failing the guard's counter reads; expect an abort and restart in about 1 s");
  this->simulate_wedge_.store(true, std::memory_order_relaxed);
}

void SystimerGuard::dump_config() {
  ESP_LOGCONFIG(TAG,
                "SYSTIMER guard:\n"
                "  Running: %s\n"
                "  Threshold: %" PRIu32 " us",
                YESNO(this->running_), this->threshold_us_);
  LOG_SENSOR("  ", "Corrections", this->corrections_sensor_);
  LOG_SENSOR("  ", "Time recovered", this->time_recovered_sensor_);
  LOG_SENSOR("  ", "Tick counter jumps", this->tick_counter_jumps_sensor_);
  LOG_SENSOR("  ", "Snapshot recoveries", this->snapshot_recoveries_sensor_);
}

}  // namespace esphome::systimer_guard
