/*
 * Copyright 2019 Liviu Nicolescu <nliviu@gmail.com>
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos.h"

#include "mqtt_queue.h"

static void timer_cb(void *arg) {
  double uptime = mgos_uptime();
  unsigned long free_heap = (unsigned long) mgos_get_free_heap_size();
  unsigned long min_free_heap = (unsigned long) mgos_get_min_free_heap_size();
  LOG(LL_INFO, ("Uptime: %.2lf, free_heap: %lu, min_free_heap: %lu", uptime,
                free_heap, min_free_heap));
  int pin = mgos_sys_config_get_board_led1_pin();
  mgos_gpio_toggle(pin);

  struct mqtt_queue *ctx = (struct mqtt_queue *) arg;
  if (ctx) {
    mqtt_queue_send_event_subf(
        ctx, "subfolder", "{uptime: %.2lf, free_heap: %lu, min_free_heap: %lu}",
        uptime, free_heap, min_free_heap);
  }
}

enum mgos_app_init_result mgos_app_init(void) {
  int pin = mgos_sys_config_get_board_led1_pin();
  bool active_high = mgos_sys_config_get_board_led1_active_high();
  mgos_gpio_setup_output(pin, !active_high);

  struct mqtt_queue *ctx = mqtt_queue_create();

  mgos_set_timer(10000 /* ms */, MGOS_TIMER_REPEAT, timer_cb, ctx);
  return MGOS_APP_INIT_SUCCESS;
}
