#include "webrtc_pipeline.h"
#include "gpio_control.h"
#include "pwm_control.h"
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <libsoup/soup-websocket.h>
#include <json-glib/json-glib.h>
#include <cstdio>

// --- Глобальні змінні ---
static GMainLoop *gloop = nullptr;
static SoupSession *session = nullptr;
static SoupWebsocketConnection *ws_conn = nullptr;
static GstElement *pipeline = nullptr, *webrtc = nullptr;
static bool pipeline_started = false;
static gchar *g_device_id = nullptr;
static gchar *g_sig_ip = nullptr;
static gchar *g_sig_port = nullptr;
static guint reconnect_timer_id = 0;
static guint connection_check_timer_id = 0;
static bool offer_sent = false;
static bool answer_received = false;
static bool webrtc_connected = false;

// --- Прототипи ---
static void attempt_connection();
static void on_ws_connected(GObject *src, GAsyncResult *res, gpointer data);
static void stop_and_cleanup_pipeline();

// --- Логіка перепідключення ---
static gboolean reconnect_cb(gpointer user_data) {
    g_print("[RECONNECT] Timer fired. Attempting to reconnect...\n");
    attempt_connection();
    return G_SOURCE_CONTINUE;
}

static void schedule_reconnection() {
    if (reconnect_timer_id > 0) return;
    g_print("[RECONNECT] Scheduling reconnection in 30 seconds...\n");
    reconnect_timer_id = g_timeout_add_seconds(30, reconnect_cb, NULL);
}

static void cancel_reconnection() {
    if (reconnect_timer_id > 0) {
        g_print("[RECONNECT] Connection successful. Cancelling reconnection timer.\n");
        g_source_remove(reconnect_timer_id);
        reconnect_timer_id = 0;
    }
}

// --- Нова функція для перевірки стану з'єднання ---
static gboolean check_connection_state(gpointer user_data) {
    if (!webrtc || !pipeline_started) {
        return G_SOURCE_CONTINUE;
    }

    // Перевіряємо стан WebRTC з'єднання
    GstWebRTCPeerConnectionState conn_state;
    g_object_get(webrtc, "connection-state", &conn_state, NULL);

    if (conn_state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED ||
        conn_state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
        conn_state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {

        g_print("[CONNECTION] WebRTC connection state: %d. Stopping pipeline.\n", conn_state);
        webrtc_connected = false;
        stop_and_cleanup_pipeline();

        // Якщо WebSocket ще живий, спробуємо переподключитися
        if (ws_conn && soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN) {
            g_print("[CONNECTION] WebSocket still alive, will restart pipeline on next ready signal.\n");
        } else {
            schedule_reconnection();
        }

        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void start_connection_monitoring() {
    if (connection_check_timer_id > 0) {
        g_source_remove(connection_check_timer_id);
    }
    connection_check_timer_id = g_timeout_add_seconds(5, check_connection_state, NULL);
}

static void stop_connection_monitoring() {
    if (connection_check_timer_id > 0) {
        g_source_remove(connection_check_timer_id);
        connection_check_timer_id = 0;
    }
}

// --- Основна логіка ---
static void on_ws_closed(SoupWebsocketConnection *conn, gpointer user_data) {
    g_printerr("[CONNECTION] WebSocket disconnected. Stopping stream.\n");
    if (ws_conn) {
        g_signal_handlers_disconnect_by_func(ws_conn, (void*)on_ws_closed, user_data);
        g_object_unref(ws_conn);
        ws_conn = nullptr;
    }
    webrtc_connected = false;
    stop_and_cleanup_pipeline();
    stop_connection_monitoring();
    schedule_reconnection();
}

static void send_json(JsonBuilder *b) {
    if (!ws_conn || soup_websocket_connection_get_state(ws_conn) != SOUP_WEBSOCKET_STATE_OPEN) return;
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *text = json_generator_to_data(gen, NULL);
    soup_websocket_connection_send_text(ws_conn, text);
    g_free(text);
    g_object_unref(gen);
}

// --- Обробники стану WebRTC з'єднання ---
static void on_connection_state_change(GstElement *webrtc_elem, GParamSpec *pspec, gpointer user_data) {
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtc_elem, "connection-state", &state, NULL);

    g_print("[WEBRTC] Connection state changed to: %d\n", state);

    switch (state) {
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
            g_print("[WEBRTC] Peer connected successfully\n");
            webrtc_connected = true;
            start_connection_monitoring();
            break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
        case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
        case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
            g_print("[WEBRTC] Peer disconnected (state: %d)\n", state);
            webrtc_connected = false;
            stop_and_cleanup_pipeline();
            break;
        default:
            break;
    }
}

static void on_ice_connection_state_change(GstElement *webrtc_elem, GParamSpec *pspec, gpointer user_data) {
    GstWebRTCICEConnectionState state;
    g_object_get(webrtc_elem, "ice-connection-state", &state, NULL);

    g_print("[WEBRTC] ICE connection state changed to: %d\n", state);

    if (state == GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED ||
        state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED ||
        state == GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED) {
        g_print("[WEBRTC] ICE connection lost\n");
        webrtc_connected = false;
        // Даємо трохи часу для відновлення перед зупинкою
        g_timeout_add_seconds(3, [](gpointer data) -> gboolean {
            if (!webrtc_connected) {
                stop_and_cleanup_pipeline();
            }
            return G_SOURCE_REMOVE;
        }, NULL);
    }
}

static void on_negotiation_needed(GstElement *webrtc_elem, gpointer) {
    if (offer_sent) return;
    offer_sent = true;
    GstPromise *pr = gst_promise_new_with_change_func(
      +[](GstPromise *p, gpointer user_data){
        GstElement *webrtc = GST_ELEMENT(user_data);
        GstWebRTCSessionDescription *offer = nullptr;
        gst_structure_get(gst_promise_get_reply(p), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
        gst_promise_unref(p);
        GstPromise *lp = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-local-description", offer, lp);
        gst_promise_unref(lp);
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "device");
        json_builder_add_string_value(b, g_device_id);
        json_builder_set_member_name(b, "sdp");
        gchar *s = gst_sdp_message_as_text(offer->sdp);
        json_builder_add_string_value(b, s);
        g_free(s);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, gst_webrtc_sdp_type_to_string(offer->type));
        json_builder_end_object(b);
        send_json(b);
        g_object_unref(b);
        gst_webrtc_session_description_free(offer);
      }, webrtc_elem, nullptr);
    g_signal_emit_by_name(webrtc_elem, "create-offer", NULL, pr);
}

static void on_ice_candidate(GstElement*, guint mline, gchar *cand, gpointer){
    if (!cand) return;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
      json_builder_set_member_name(b, "device");
      json_builder_add_string_value(b, g_device_id);
      json_builder_set_member_name(b, "candidate");
      json_builder_add_string_value(b, cand);
      json_builder_set_member_name(b, "sdpMLineIndex");
      json_builder_add_int_value(b, mline);
    json_builder_end_object(b);
    send_json(b);
    g_object_unref(b);
}

static void on_bus_error(GstBus*, GstMessage *msg, gpointer) {
    GError *e; gchar *dbg;
    gst_message_parse_error(msg, &e, &dbg);
    g_printerr("[ERROR] %s: %s\nDebug: %s\n", GST_OBJECT_NAME(msg->src), e->message, dbg);
    g_error_free(e);
    g_free(dbg);

    // При помилці зупиняємо pipeline
    stop_and_cleanup_pipeline();
}

static void on_bus_warning(GstBus*, GstMessage *msg, gpointer) {
    GError *e; gchar *dbg;
    gst_message_parse_warning(msg, &e, &dbg);
    g_printerr("[WARNING] %s: %s\nDebug: %s\n", GST_OBJECT_NAME(msg->src), e->message, dbg);
    g_error_free(e);
    g_free(dbg);
}

static void on_bus_state_changed(GstBus*, GstMessage *msg, gpointer) {
    if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
        g_print("[LOG] Pipeline state changed from %s to %s\n",
                gst_element_state_get_name(old_state),
                gst_element_state_get_name(new_state));
    }
}

// ✅ --- ВИПРАВЛЕНА ФУНКЦІЯ ЗУПИНКИ --- ✅
static void stop_and_cleanup_pipeline() {
    if (!pipeline_started) return;

    g_print("[PIPELINE] Stopping GStreamer pipeline...\n");

    // Зупиняємо моніторинг з'єднання
    stop_connection_monitoring();

    if (pipeline) {
        // Спочатку переводимо в стан READY, потім в NULL
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_READY);
        if (ret == GST_STATE_CHANGE_ASYNC) {
            // Чекаємо завершення зміни стану
            gst_element_get_state(pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
        }

        // Тепер переводимо в NULL
        gst_element_set_state(pipeline, GST_STATE_NULL);
        ret = gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND);

        if (ret == GST_STATE_CHANGE_SUCCESS) {
            g_print("[PIPELINE] Pipeline stopped successfully\n");
        } else {
            g_print("[PIPELINE] Pipeline stop timeout or failure\n");
        }

        // Відключаємо сигнали перед видаленням
        if (webrtc) {
            g_signal_handlers_disconnect_by_func(webrtc, (void*)on_negotiation_needed, webrtc);
            g_signal_handlers_disconnect_by_func(webrtc, (void*)on_ice_candidate, NULL);
            g_signal_handlers_disconnect_by_func(webrtc, (void*)on_connection_state_change, NULL);
            g_signal_handlers_disconnect_by_func(webrtc, (void*)on_ice_connection_state_change, NULL);
        }

        g_object_unref(pipeline);
        pipeline = nullptr;
    }
    webrtc = nullptr;

    // Скидаємо прапори для наступного з'єднання
    pipeline_started = false;
    offer_sent = false;
    answer_received = false;
    webrtc_connected = false;

    g_print("[PIPELINE] Pipeline cleanup completed\n");
}

static void start_pipeline() {
    if (pipeline_started) return;
    GError *error = NULL;

    g_print("[PIPELINE] Starting new pipeline...\n");

    gchar *pipeline_str = g_strdup_printf(
        "udpsrc port=5001 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264\" ! "
        "rtph264depay ! h264parse ! rtph264pay config-interval=1 ! "
        "webrtcbin name=webrtc stun-server=stun://stun.l.google.com:19302"
    );

    g_print("[LOG] Using final UDP-loopback pipeline: %s\n", pipeline_str);
    pipeline = gst_parse_launch(pipeline_str, &error);
    g_free(pipeline_str);

    if (!pipeline || error) {
        g_printerr("[ERROR] Failed to create GStreamer pipeline: %s\n", error ? error->message : "Unknown error");
        if (error) g_error_free(error);
        return;
    }

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error", G_CALLBACK(on_bus_error), NULL);
    g_signal_connect(bus, "message::warning", G_CALLBACK(on_bus_warning), NULL);
    g_signal_connect(bus, "message::state-changed", G_CALLBACK(on_bus_state_changed), NULL);
    gst_object_unref(bus);

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), webrtc);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);

    // ✅ ДОДАЄМО МОНІТОРИНГ СТАНУ WEBRTC З'ЄДНАННЯ
    g_signal_connect(webrtc, "notify::connection-state", G_CALLBACK(on_connection_state_change), NULL);
    g_signal_connect(webrtc, "notify::ice-connection-state", G_CALLBACK(on_ice_connection_state_change), NULL);

    g_print("[LOG] Starting GStreamer pipeline...\n");
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[ERROR] Failed to start pipeline\n");
        stop_and_cleanup_pipeline();
        return;
    }

    pipeline_started = true;
    g_print("[PIPELINE] Pipeline started successfully\n");
}

static void on_ws_message(SoupWebsocketConnection*, SoupWebsocketDataType, GBytes *msg, gpointer) {
    gsize size;
    const gchar *data = (const gchar*)g_bytes_get_data(msg, &size);
    JsonParser *parser = json_parser_new();
    GError *error = nullptr;
    if (!json_parser_load_from_data(parser, data, size, &error)) { g_object_unref(parser); return; }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) { g_object_unref(parser); return; }
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "device") && g_strcmp0(json_object_get_string_member(obj, "device"), g_device_id) != 0) {
        g_object_unref(parser);
        return;
    }

    if (json_object_has_member(obj, "action")) {
        const gchar *action = json_object_get_string_member(obj, "action");
        if (!g_strcmp0(action, "ready")) {
            // Якщо pipeline вже запущений, спочатку зупиняємо його
            if (pipeline_started) {
                g_print("[LOG] Stopping existing pipeline before starting new one\n");
                stop_and_cleanup_pipeline();
            }
            start_pipeline();
        } else if (!g_strcmp0(action, "control")) {
            const gchar *direction = json_object_get_string_member_with_default(obj, "direction", nullptr);
            const gchar *turn = json_object_get_string_member_with_default(obj, "turn", nullptr);
            int speed = json_object_get_int_member_with_default(obj, "speed", 50);
            control_vehicle(direction, turn, speed);
        } else if (!g_strcmp0(action, "stop")) {
            stop_vehicle();
        } else if (!g_strcmp0(action, "disconnect")) {
            // Обробляємо явне відключення клієнта
            g_print("[LOG] Client requested disconnect\n");
            stop_and_cleanup_pipeline();
        }
    } else if (json_object_has_member(obj, "type") && !g_strcmp0(json_object_get_string_member(obj, "type"), "answer")) {
        if (answer_received) { g_object_unref(parser); return; }
        answer_received = true;
        const gchar *sdp_text = json_object_get_string_member(obj, "sdp");
        GstSDPMessage *sdpmsg = nullptr;
        if (gst_sdp_message_new_from_text(sdp_text, &sdpmsg) == GST_SDP_OK) {
            GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpmsg);
            g_signal_emit_by_name(webrtc, "set-remote-description", desc, nullptr);
            gst_webrtc_session_description_free(desc);
        }
    } else if (json_object_has_member(obj, "candidate")) {
        const gchar *cand = json_object_get_string_member(obj, "candidate");
        guint idx = json_object_get_int_member(obj, "sdpMLineIndex");
        g_signal_emit_by_name(webrtc, "add-ice-candidate", idx, cand);
    }
    g_object_unref(parser);
}

static void on_ws_connected(GObject *src, GAsyncResult *res, gpointer) {
    GError *err = NULL;
    SoupSession *local_session = SOUP_SESSION(src);

    ws_conn = soup_session_websocket_connect_finish(local_session, res, &err);

    if (err) {
        g_printerr("[CONNECTION] WebSocket connection failed: %s\n", err->message);
        g_error_free(err);
        ws_conn = nullptr;
        schedule_reconnection();
        return;
    }

    cancel_reconnection();
    g_print("[CONNECTION] WebSocket connected successfully\n");

    soup_websocket_connection_set_keepalive_interval(ws_conn, 15);
    g_signal_connect(ws_conn, "message", G_CALLBACK(on_ws_message), NULL);
    g_signal_connect(ws_conn, "closed", G_CALLBACK(on_ws_closed), NULL);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
      json_builder_set_member_name(b, "action");
      json_builder_add_string_value(b, "ready");
      json_builder_set_member_name(b, "device");
      json_builder_add_string_value(b, g_device_id);
    json_builder_end_object(b);
    send_json(b);
    g_object_unref(b);
}

static void attempt_connection() {
    if (session) {
        soup_session_abort(session);
        g_object_unref(session);
    }
    gchar *addr = g_strdup_printf("ws://%s:%s/ws", g_sig_ip, g_sig_port);
    g_print("[CONNECTION] Attempting to connect to %s\n", addr);

    session = soup_session_new();
    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, addr);
    g_free(addr);

    soup_session_websocket_connect_async(session, msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL, on_ws_connected, session);
}

void start_webrtc(const char *sig_ip, const char *sig_port, const char *device_id, GMainLoop *loop) {
    g_sig_ip = g_strdup(sig_ip);
    g_sig_port = g_strdup(sig_port);
    g_device_id = g_strdup(device_id);
    gloop = loop;
    attempt_connection();
}

// Головна функція очищення
void cleanup_webrtc() {
    cancel_reconnection();
    stop_connection_monitoring();
    stop_and_cleanup_pipeline();

    if (ws_conn) {
        if (soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN) {
             soup_websocket_connection_close(ws_conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        }
        g_object_unref(ws_conn);
        ws_conn = nullptr;
    }
    if (session) {
        soup_session_abort(session);
        g_object_unref(session);
        session = nullptr;
    }

    g_free(g_device_id); g_device_id = nullptr;
    g_free(g_sig_ip); g_sig_ip = nullptr;
    g_free(g_sig_port); g_sig_port = nullptr;
}
