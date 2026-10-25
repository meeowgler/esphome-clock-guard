"""clock_guard: repairs ESP32-C6 esp_timer backward jumps (espressif/esp-idf#19036).

On some ESP32-C6 chips SYSTIMER counter 0, which esp_timer reads, spontaneously loses
minutes to hours. Counter 1 (the FreeRTOS tick) keeps counting from the same clock, so a
task compares the two every tick and steps esp_timer forward by whatever it lost.
"""

import esphome.codegen as cg
from esphome.components import sensor
from esphome.components.esp32 import VARIANT_ESP32C6, only_on_variant
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_SECOND,
)

CODEOWNERS = []
AUTO_LOAD = ["sensor"]

CONF_THRESHOLD = "threshold"
CONF_CORRECTIONS = "corrections"
CONF_TIME_RECOVERED = "time_recovered"
CONF_TICK_COUNTER_JUMPS = "tick_counter_jumps"

clock_guard_ns = cg.esphome_ns.namespace("clock_guard")
ClockGuard = clock_guard_ns.class_("ClockGuard", cg.PollingComponent)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ClockGuard),
            cv.Optional(CONF_THRESHOLD, default="2ms"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(min=cv.TimePeriod(microseconds=500)),
            ),
            cv.Optional(CONF_CORRECTIONS): sensor.sensor_schema(
                icon="mdi:clock-alert-outline",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_TIME_RECOVERED): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                icon="mdi:clock-plus-outline",
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_TICK_COUNTER_JUMPS): sensor.sensor_schema(
                icon="mdi:clock-alert",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ).extend(cv.polling_component_schema("60s")),
    cv.only_with_framework("esp-idf"),
    only_on_variant(supported=[VARIANT_ESP32C6]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_threshold_us(config[CONF_THRESHOLD].total_microseconds))
    for key, setter in (
        (CONF_CORRECTIONS, var.set_corrections_sensor),
        (CONF_TIME_RECOVERED, var.set_time_recovered_sensor),
        (CONF_TICK_COUNTER_JUMPS, var.set_tick_counter_jumps_sensor),
    ):
        if key in config:
            cg.add(setter(await sensor.new_sensor(config[key])))
