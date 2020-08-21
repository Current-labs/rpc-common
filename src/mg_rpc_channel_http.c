/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
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

#if defined(MGOS_HAVE_HTTP_SERVER) && MGOS_ENABLE_RPC_CHANNEL_HTTP

#include "mg_rpc_channel_http.h"

#include "mg_rpc.h"
#include "mg_rpc_channel.h"
#include "mg_rpc_channel_tcp_common.h"

#include "common/cs_dbg.h"
#include "common/queue.h"
#include "frozen.h"

#include "mgos_hal.h"

static const char *preflight_method = "OPTIONS";

static const char *s_headers =
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Headers: *";

struct mg_rpc_channel_http_data {
  struct mg_mgr *mgr;
  struct mg_connection *nc;
  struct http_message *hm;
  struct mg_rpc_channel *ch;
  const char *default_auth_domain;
  const char *default_auth_file;
  bool is_keep_alive;
  bool is_rest;
  bool is_open;
  SLIST_ENTRY(mg_rpc_channel_http_data) next;
};

SLIST_HEAD(s_http_chd, mg_rpc_channel_http_data)
s_http_chd = SLIST_HEAD_INITIALIZER(&s_http_chd);

static void ch_closed(void *arg) {
  struct mg_rpc_channel *ch = (struct mg_rpc_channel *) arg;
  ch->ev_handler(ch, MG_RPC_CHANNEL_CLOSED, NULL);
}

/* Connection could've been closed already but we don't get notified of that,
 * so we do the best we can by checking if the pointer is still valid. */
static bool nc_is_valid(struct mg_rpc_channel *ch) {
  struct mg_connection *c;
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  if (chd->nc == NULL) return false;
  for (c = mg_next(chd->mgr, NULL); c != NULL; c = mg_next(chd->mgr, c)) {
    if (c == chd->nc && !(c->flags & MG_F_CLOSE_IMMEDIATELY)) return true;
  }
  chd->nc = NULL;
  mgos_invoke_cb(ch_closed, ch, false /* from_isr */);
  return false;
}

static void mg_rpc_channel_http_ch_connect(struct mg_rpc_channel *ch) {
  (void) ch;
}

static void mg_rpc_channel_http_ch_close(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  if (nc_is_valid(ch)) {
    mg_http_send_error(chd->nc, 400, "Invalid request");
    chd->nc->flags |= MG_F_SEND_AND_CLOSE;
    chd->nc = NULL;
  }
  mgos_invoke_cb(ch_closed, ch, false /* from_isr */);
}

static bool mg_rpc_channel_http_get_authn_info(
    struct mg_rpc_channel *ch, const char *auth_domain, const char *auth_file,
    struct mg_rpc_authn_info *authn) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  bool ret = false;
  struct mg_str *hdr;
  char username_buf[50];
  char *username = username_buf;

  if (auth_domain == NULL || auth_file == NULL) {
    auth_domain = chd->default_auth_domain;
    auth_file = chd->default_auth_file;
  }

  if (auth_domain == NULL || auth_file == NULL) {
    goto clean;
  }

  if (!mg_http_is_authorized(chd->hm, chd->hm->uri, auth_domain, auth_file,
                             MG_AUTH_FLAG_IS_GLOBAL_PASS_FILE)) {
    goto clean;
  }

  /* Parse "Authorization:" header, fail fast on parse error */
  if (chd->hm == NULL ||
      (hdr = mg_get_http_header(chd->hm, "Authorization")) == NULL ||
      mg_http_parse_header2(hdr, "username", &username, sizeof(username_buf)) ==
          0) {
    /* No auth header is present */
    goto clean;
  }

  /* Got username from the Authorization header */
  authn->username = mg_strdup(mg_mk_str(username));

  ret = true;

clean:
  if (username != username_buf) {
    free(username);
  }
  return ret;
}

// Ripped from mg_http.c#mg_http_send_digest_auth_request with sweet mods
void http_send_digest_auth_request(struct mg_connection *c,
                                      const char *domain, bool use_401) {
  // Add CORS headers
  char *status_code;
  if (use_401) {
    status_code = "401 Unauthorized";
  } else {
    status_code = "418 I'm a teapot";
  }
  mg_printf(c,
          "HTTP/1.1 %s\r\n"
          "Access-Control-Allow-Origin: *\r\n"
          "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
          "Access-Control-Expose-Headers: WWW-Authenticate\r\n"
          "WWW-Authenticate: Digest qop=\"auth\", "
          "realm=\"%s\", nonce=\"%lx\"\r\n"
          "Connection: Keep-Alive\r\n"
          "Content-Length: 0\r\n\r\n",
          status_code, domain, (unsigned long) mg_time());
}

static bool mg_rpc_channel_http_cors_preflight_check(struct mg_rpc_channel *ch) {
   struct mg_rpc_channel_http_data *chd =
       (struct mg_rpc_channel_http_data *) ch->channel_data;

   LOG(LL_INFO, ("Received request '%.*s', Preflight method: '%s'",
               (int) chd->hm->method.len, chd->hm->method.p, preflight_method));

   return (mg_vcmp(&chd->hm->method, preflight_method) == 0);
}

void http_send_digest_cors_preflight_response(struct mg_connection *c) {
  // Add CORS headers
  mg_printf(c,
          "HTTP/1.1 200 OK\r\n"
          "Access-Control-Allow-Origin: *\r\n"
          "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
          "Access-Control-Allow-Methods: GET, POST\r\n"
          "Connection: Keep-Alive\r\n"
          "Content-Length: 0\r\n\r\n");
}

static void mg_rpc_channel_http_send_cors_preflight(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;

  http_send_digest_cors_preflight_response(chd->nc);

  /* We sent a response, the channel is no more. */
  chd->nc->flags |= MG_F_SEND_AND_CLOSE;
  chd->nc = NULL;
  mgos_invoke_cb(ch_closed, ch, false /* from_isr */);
  LOG(LL_DEBUG, ("%p sent 200", ch));
}

static void mg_rpc_channel_http_send_not_authorized(struct mg_rpc_channel *ch,
                                                    const char *auth_domain) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;

  if (auth_domain == NULL) {
    auth_domain = chd->default_auth_domain;
  }

  if (auth_domain == NULL) {
    LOG(LL_ERROR,
        ("no auth_domain configured, can't send non_authorized response"));
    return;
  }

  if (!nc_is_valid(ch)) return;

  bool use_401 = true;
  struct http_message *hm = (struct http_message *) chd->hm;
  if (mg_get_http_header(hm, "X-Requested-With")) {
    use_401 = false;
  }

  http_send_digest_auth_request(chd->nc, auth_domain, use_401);
  /* We sent a response, the channel is no more. */
  chd->nc->flags |= MG_F_SEND_AND_CLOSE;
  chd->nc = NULL;
  mgos_invoke_cb(ch_closed, ch, false /* from_isr */);
  LOG(LL_DEBUG, ("%p sent 401", ch));
}

static const char *mg_rpc_channel_http_get_type(struct mg_rpc_channel *ch) {
  (void) ch;
  return "HTTP";
}

static char *mg_rpc_channel_http_get_info(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_http_data *chd = ch->channel_data;
  return (nc_is_valid(ch) ? mg_rpc_channel_tcp_get_info(chd->nc) : NULL);
}

/*
 * Timer callback which emits SENT and CLOSED events to mg_rpc.
 */
static void mg_rpc_channel_http_frame_sent(void *param) {
  struct mg_rpc_channel *ch = param;
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_SENT, (void *) 1);
  if (!chd->is_keep_alive) {
    ch->ev_handler(ch, MG_RPC_CHANNEL_CLOSED, NULL);
  }
}

static void mg_rpc_channel_http_ch_destroy(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  SLIST_REMOVE(&s_http_chd, chd, mg_rpc_channel_http_data, next);
  free(chd);
  free(ch);
}

extern const char *mg_status_message(int status_code);

static bool mg_rpc_channel_http_send_frame(struct mg_rpc_channel *ch,
                                           const struct mg_str f) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  if (!nc_is_valid(ch)) {
    return false;
  }
  struct mg_connection *nc = chd->nc;

  int code = 200;
  struct mg_str body;
  if (chd->is_rest) {
    struct json_token result_tok = JSON_INVALID_TOKEN;
    struct json_token error_tok = JSON_INVALID_TOKEN;
    json_scanf(f.p, f.len, "{result: %T, error: %T}", &result_tok, &error_tok);
    if (error_tok.len != 0) {
      code = 500;
      body = mg_mk_str_n(error_tok.ptr, error_tok.len);
    } else {
      body = mg_mk_str_n(result_tok.ptr, result_tok.len);
    }
  } else {
    body = f;
  }
  mg_send_response_line(nc, code, s_headers);
  mg_printf(nc, "Content-Length: %d\r\n", (int) body.len + 2);
  mg_printf(nc, "Connection: %s\r\n",
            (chd->is_keep_alive ? "keep-alive" : "close"));
  mg_printf(nc, "\r\n%.*s\r\n", (int) body.len, body.p);

  if (!chd->is_keep_alive) {
    nc->flags |= MG_F_SEND_AND_CLOSE;
    chd->nc = NULL;
  }

  /*
   * Schedule a callback which will emit SENT and CLOSED events. mg_rpc expects
   * those to be emitted asynchronously, therefore we can't emit them right
   * here.
   */
  mgos_invoke_cb(mg_rpc_channel_http_frame_sent, ch, false /* from_isr */);

  return true;
}

static bool is_keepalive(struct http_message *hm) {
  struct mg_str *conn_hdr = mg_get_http_header(hm, "Connection");
  return (conn_hdr != NULL && mg_vcasecmp(conn_hdr, "keep-alive") == 0);
}

void mg_rpc_channel_http_recd_frame(struct mg_connection *nc,
                                    struct http_message *hm,
                                    struct mg_rpc_channel *ch,
                                    const struct mg_str frame) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  chd->nc = nc;
  chd->hm = hm;
  chd->is_rest = false;
  chd->is_keep_alive = is_keepalive(hm);

  if (!chd->is_open) {
    chd->is_open = true;
    ch->ev_handler(ch, MG_RPC_CHANNEL_OPEN, NULL);
  }
  ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_RECD, (void *) &frame);
}

void mg_rpc_channel_http_recd_parsed_frame(struct mg_connection *nc,
                                           struct http_message *hm,
                                           struct mg_rpc_channel *ch,
                                           const struct mg_str method,
                                           const struct mg_str args) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  chd->nc = nc;
  chd->hm = hm;
  chd->is_rest = true;
  chd->is_keep_alive = is_keepalive(hm);

  if (mg_vcasecmp(&hm->method, "OPTIONS") == 0) {
    // CORS check.
    mg_send_response_line(chd->nc, 200, s_headers);
    mg_printf(nc, "Content-Length: %d\r\n", 0);
    mg_printf(nc, "Connection: %s\r\n",
              (chd->is_keep_alive ? "keep-alive" : "close"));
    mg_printf(nc, "\r\n");
    if (!chd->is_keep_alive) chd->nc->flags |= MG_F_SEND_AND_CLOSE;
    return;
  }

  /* Prepare "parsed" frame */
  struct mg_rpc_frame frame;
  memset(&frame, 0, sizeof(frame));
  char ids[16];
  snprintf(ids, sizeof(ids), "%lu", (unsigned long) rand());
  frame.method = method;
  frame.args = args;
  frame.id = mg_mk_str(ids);

  /* "Open" the channel and send the frame */
  if (!chd->is_open) {
    chd->is_open = true;
    ch->ev_handler(ch, MG_RPC_CHANNEL_OPEN, NULL);
  }

  ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_RECD_PARSED, &frame);
}

struct mg_rpc_channel *mg_rpc_channel_http(struct mg_connection *nc,
                                           const char *default_auth_domain,
                                           const char *default_auth_file,
                                           bool *is_new) {
  struct mg_rpc_channel *ch = NULL;
  struct mg_rpc_channel_http_data *chd, *chdt;
  SLIST_FOREACH_SAFE(chd, &s_http_chd, next, chdt) {
    if (chd->nc == nc) {
      ch = chd->ch;
    } else {
      // Close channels for which connections may have been closed.
      nc_is_valid(chd->ch);
    }
  }

  if (ch == NULL) {
    ch = (struct mg_rpc_channel *) calloc(1, sizeof(*ch));
    if (ch == NULL) return NULL;
    *is_new = true;
  } else {
    *is_new = false;
  }

  ch->ch_connect = mg_rpc_channel_http_ch_connect;
  ch->send_frame = mg_rpc_channel_http_send_frame;
  ch->ch_close = mg_rpc_channel_http_ch_close;
  ch->ch_destroy = mg_rpc_channel_http_ch_destroy;
  ch->get_type = mg_rpc_channel_http_get_type;
  ch->is_persistent = mg_rpc_channel_false;
  // No broadcasts here, it's a request-response channel.
  ch->is_broadcast_enabled = mg_rpc_channel_false;
  ch->get_authn_info = mg_rpc_channel_http_get_authn_info;
  ch->cors_preflight_check = mg_rpc_channel_http_cors_preflight_check;
  ch->send_cors_preflight = mg_rpc_channel_http_send_cors_preflight;
  ch->send_not_authorized = mg_rpc_channel_http_send_not_authorized;
  ch->get_info = mg_rpc_channel_http_get_info;

  if (*is_new) {
    chd = (struct mg_rpc_channel_http_data *) calloc(1, sizeof(*chd));
  } else {
    chd = ch->channel_data;
  }

  chd->default_auth_domain = default_auth_domain;
  chd->default_auth_file = default_auth_file;
  chd->mgr = nc->mgr;
  ch->channel_data = chd;
  chd->ch = ch;

  if (*is_new) {
    SLIST_INSERT_HEAD(&s_http_chd, chd, next);
  }

  return ch;
}

#endif /* defined(MGOS_HAVE_HTTP_SERVER) && MGOS_ENABLE_RPC_CHANNEL_HTTP */
