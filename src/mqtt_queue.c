/*
 * Copyright (c) 2019 Liviu Nicolescu <nliviu@gmail.com>
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

#include "mgos_jstore.h"
#include "mgos_mqtt.h"

struct mqtt_queue {
  const char *jstore_path;
  struct mgos_jstore *jstore;
  char *jstore_err;
  int max_count;

  int interval;
  mgos_timer_id tid;

  bool online;
};

// static struct mqtt_queue *ctx = NULL;

/* clang-format off */
static const char *cloud_type_stringify(enum mgos_cloud_type type) {
  /*
   * cloud types:
   * 0 = MGOS_CLOUD_MQTT
   * 1 = MGOS_CLOUD_DASH
   * 2 = MGOS_CLOUD_AWS
   * 3 = MGOS_CLOUD_AZURE
   * 4 = MGOS_CLOUD_GCP
   * 5 = MGOS_CLOUD_WATSON
   *
   */
  const char *s = "N/A";
  switch (type) {
    case MGOS_CLOUD_MQTT:   s = "MGOS_CLOUD_MQTT";   break;
    case MGOS_CLOUD_DASH:   s = "MGOS_CLOUD_DASH";   break;
    case MGOS_CLOUD_AWS:    s = "MGOS_CLOUD_AWS";    break;
    case MGOS_CLOUD_AZURE:  s = "MGOS_CLOUD_AZURE";  break;
    case MGOS_CLOUD_GCP:    s = "MGOS_CLOUD_GCP";    break;
    case MGOS_CLOUD_WATSON: s = "MGOS_CLOUD_WATSON"; break;
    default: break;
  }
  return s;
}
/* clang-format on */

static void jstore_log_error(const char *caller, const char *func, char **err) {
  if (*err != NULL) {
    LOG(LL_ERROR, ("%s - %s error: %s", caller, func, *err));
    free(*err);
    *err = NULL;
  }
}

static void jstore_play_timer_cb(void *arg) {
  struct mqtt_queue *ctx = (struct mqtt_queue *) arg;
  int count = mgos_jstore_items_cnt(ctx->jstore);
  if (count != 0) {
    const struct mgos_jstore_ref ref = MGOS_JSTORE_REF_BY_INDEX(0);
    struct mg_str data;
    bool ok = mgos_jstore_item_get(ctx->jstore, ref, NULL, &data, NULL, NULL,
                                   &ctx->jstore_err);
    jstore_log_error(__FUNCTION__, "mgos_jstore_item_get", &ctx->jstore_err);
    if (ok) {
      LOG(LL_INFO, ("%s - data: %.*s", __FUNCTION__, data.len, data.p));
      char *topic = NULL;
      char *msg = NULL;
      if (json_scanf(data.p, data.len, "{subfolder: %Q, data: %Q}", &topic,
                     &msg) == 2) {
        mgos_mqtt_pub(topic, msg, strlen(msg), 1, false);
        mgos_jstore_item_remove(ctx->jstore, ref, &ctx->jstore_err);
        jstore_log_error(__FUNCTION__, "mgos_jstore_item_remove",
                         &ctx->jstore_err);
        mgos_jstore_save(ctx->jstore, ctx->jstore_path, &ctx->jstore_err);
        jstore_log_error(__FUNCTION__, "mgos_jstore_save", &ctx->jstore_err);

        free(topic);
        free(msg);
      }
    }
  } else {
    if (ctx->tid != MGOS_INVALID_TIMER_ID) {
      mgos_clear_timer(ctx->tid);
      ctx->tid = MGOS_INVALID_TIMER_ID;
    }
  }
}

static void jstore_check_size(struct mqtt_queue *ctx) {
  int count = mgos_jstore_items_cnt(ctx->jstore);
  bool need_save = false;
  while (count >= ctx->max_count) {
    bool ok = mgos_jstore_item_remove(ctx->jstore, MGOS_JSTORE_REF_BY_INDEX(0),
                                      &ctx->jstore_err);
    jstore_log_error(__FUNCTION__, "mgos_jstore_item_remove", &ctx->jstore_err);
    if (ok) {
      int prev_count = count;
      count = mgos_jstore_items_cnt(ctx->jstore);
      LOG(LL_INFO, ("%s - ctx->max_count: %d, prev_count: %d, count: %d",
                    __FUNCTION__, ctx->max_count, prev_count, count));
      need_save = true;
    }
  }
  if (need_save) {
    mgos_jstore_save(ctx->jstore, ctx->jstore_path, &ctx->jstore_err);
    jstore_log_error(__FUNCTION__, "mgos_jstore_save", &ctx->jstore_err);
  }
}

static int jstore_add(struct mqtt_queue *ctx, const char *subfolder,
                      const char *json_fmt, va_list ap) {
  jstore_check_size(ctx);
  char *json = json_vasprintf(json_fmt, ap);
  char *data = json_asprintf("{subfolder: %Q, data: %s}", subfolder, json);
  free(json);
  struct mg_str id = mgos_jstore_item_add(
      ctx->jstore, mg_mk_str(NULL), mg_mk_str(data), MGOS_JSTORE_OWN_COPY,
      MGOS_JSTORE_OWN_RETAIN, NULL, NULL, &ctx->jstore_err);
  jstore_log_error(__FUNCTION__, "mgos_jstore_item_add", &ctx->jstore_err);

  LOG(LL_INFO, ("%s - added: <%.*s>", __FUNCTION__, id.len, id.p));
  mgos_jstore_save(ctx->jstore, ctx->jstore_path, &ctx->jstore_err);
  jstore_log_error(__FUNCTION__, "mgos_jstore_save", &ctx->jstore_err);
  return id.len;
}

static void cloud_cb(int ev, void *evd, void *arg) {
  const struct mgos_cloud_arg *ca = (const struct mgos_cloud_arg *) evd;
  const char *cloud_type = cloud_type_stringify(ca->type);
  struct mqtt_queue *ctx = (struct mqtt_queue *) arg;
  switch (ev) {
    case MGOS_EVENT_CLOUD_CONNECTED: {
      LOG(LL_INFO, ("%s - Cloud connected (%s)", __FUNCTION__, cloud_type));
      ctx->online = true;
      ctx->tid = mgos_set_timer(ctx->interval, MGOS_TIMER_REPEAT,
                                jstore_play_timer_cb, ctx);
      LOG(LL_INFO, ("%s - ctx->interval: %d", __FUNCTION__, ctx->interval));
      break;
    }
    case MGOS_EVENT_CLOUD_DISCONNECTED: {
      LOG(LL_INFO, ("%s - Cloud disconnected (%s)", __FUNCTION__, cloud_type));
      ctx->online = false;
      break;
    }
  }
}

bool mqtt_queue_send_event_subf(struct mqtt_queue *ctx, const char *subfolder,
                                const char *json_fmt, ...) {
  bool res = false;
  va_list ap;
  va_start(ap, json_fmt);

  if (!ctx->online) {
    LOG(LL_INFO, ("Cloud not connected. Queueing message."));
    jstore_add(ctx, subfolder, json_fmt, ap);
  } else {
    LOG(LL_INFO, ("Cloud connected. Sending message."));
    mgos_mqtt_pubv(subfolder, 1, false, json_fmt, ap);
  }

  va_end(ap);
  return res;
}

struct mqtt_queue *mqtt_queue_create(void) {
  struct mqtt_queue *ctx = (struct mqtt_queue *) calloc(1, sizeof(*ctx));
  const char *jstore_path = mgos_sys_config_get_mqtt_queue_json_path();
  ctx->jstore_path = (jstore_path != NULL) ? jstore_path : "default.json";
  int max_count = mgos_sys_config_get_mqtt_queue_max_count();
  ctx->max_count = (max_count <= 0) ? 10 : max_count;
  ctx->jstore = mgos_jstore_create(ctx->jstore_path, &ctx->jstore_err);
  jstore_log_error(__FUNCTION__, "mgos_jstore_create", &ctx->jstore_err);
  ctx->tid = MGOS_INVALID_TIMER_ID;
  ctx->interval = mgos_sys_config_get_mqtt_queue_interval();

  mgos_event_add_handler(MGOS_EVENT_CLOUD_CONNECTED, cloud_cb, ctx);
  mgos_event_add_handler(MGOS_EVENT_CLOUD_DISCONNECTED, cloud_cb, ctx);
  return ctx;
}

bool mqtt_queue_init(void) {
  return true;
}