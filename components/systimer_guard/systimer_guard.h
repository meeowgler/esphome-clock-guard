#pragma once

#include <atomic>
#include <esp_timer.h>
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome::systimer_guard {

/// Watches esp_timer (SYSTIMER counter 0) against the FreeRTOS tick counter (SYSTIMER counter 1)
/// and steps esp_timer forward when it loses time. See __init__.py and espressif/esp-idf#19036.
class SystimerGuard : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_threshold_us(uint32_t threshold_us) { this->threshold_us_ = threshold_us; }
  void set_corrections_sensor(sensor::Sensor *s) { this->corrections_sensor_ = s; }
  void set_time_recovered_sensor(sensor::Sensor *s) { this->time_recovered_sensor_ = s; }
  void set_tick_counter_jumps_sensor(sensor::Sensor *s) { this->tick_counter_jumps_sensor_ = s; }
  void set_snapshot_recoveries_sensor(sensor::Sensor *s) { this->snapshot_recoveries_sensor_ = s; }

  /// Test hook: step esp_timer back by `seconds`, the way the SYSTIMER fault does.
  void simulate_jump(uint32_t seconds);
  /// Test hook: make the guard's own counter reads fail, as a wedged snapshot handshake would.
  /// The guard then aborts after about a second, so the board restarts with a crash record.
  void simulate_wedge();

 protected:
  static void task_entry_(void *arg);
  void run_();

  uint32_t threshold_us_{2000};
  sensor::Sensor *corrections_sensor_{nullptr};
  sensor::Sensor *time_recovered_sensor_{nullptr};
  sensor::Sensor *tick_counter_jumps_sensor_{nullptr};
  sensor::Sensor *snapshot_recoveries_sensor_{nullptr};
  esp_timer_handle_t kick_timer_{nullptr};
  bool running_{false};

  // Written by the guard task, read by the main loop.
  std::atomic<uint32_t> corrections_{0};  // events: corrections under 1 s apart count once
  std::atomic<uint32_t> recovered_ms_{0};
  std::atomic<uint32_t> last_jump_ms_{0};
  std::atomic<uint32_t> tick_counter_jumps_{0};
  std::atomic<uint32_t> snapshot_recoveries_{0};  // reads that needed the snapshot re-requested
  std::atomic<bool> simulate_wedge_{false};
  uint32_t logged_corrections_{0};
  uint32_t logged_tick_counter_jumps_{0};
  uint32_t logged_snapshot_recoveries_{0};
};

}  // namespace esphome::systimer_guard
