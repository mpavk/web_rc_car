#include "webrtc_pipeline.h"
#include "gpio_control.h"
#include "pwm_control.h"
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <libsoup/soup-websocket.h>
#include <json-glib/json-glib.h>

static GMainLoop *gloop = nullptr;
static SoupSession *session = nullptr;
static SoupWebsocketConnection *ws_conn = nullptr;
static GstElement *pipeline = nullptr, *webrtc = nullptr;
static bool pipeline_started = false;
static gchar *g_device_id = nullptr;

static void send_json(JsonBuilder *b) {
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, json_builder_get_root(b));
    gchar *text = json_generator_to_data(gen, NULL);
    soup_websocket_connection_send_text(ws_conn, text);
    g_free(text); g_object_unref(gen);
}

static void on_negotiation_needed(GstElement *webrtc_elem, gpointer) {
    GstPromise *pr = gst_promise_new_with_change_func(
      +[](GstPromise *p, gpointer user_data){
        GstElement *webrtc = GST_ELEMENT(user_data);
        GstWebRTCSessionDescription *offer = nullptr;
        gst_structure_get(gst_promise_get_reply(p), "offer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
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
        json_builder_add_string_value(b,
          gst_webrtc_sdp_type_to_string(offer->type));
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
    send_json(b); g_object_unref(b);
}

static void start_pipeline() {
    if (pipeline_started) return;
    pipeline_started = true;
    pipeline = gst_parse_launch(
      "libcamerasrc ! video/x-raw,width=320,height=240,framerate=15/1 ! "
      "videoconvert ! x264enc tune=zerolatency bitrate=200 speed-preset=ultrafast ! "
      "h264parse ! rtph264pay config-interval=-1 pt=96 ! "
      "webrtcbin name=webrtc stun-server=stun://stun.l.google.com:19302",
      NULL
    );
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error",
      G_CALLBACK(+[](GstBus*, GstMessage *msg, gpointer){
        GError *e; gchar *dbg;
        gst_message_parse_error(msg, &e, &dbg);
        g_printerr("[ERROR] %s: %s\nDebug: %s\n",
                   GST_OBJECT_NAME(msg->src), e->message, dbg);
        g_error_free(e); g_free(dbg);
      }), NULL);
    gst_object_unref(bus);

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    g_signal_connect(webrtc, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), webrtc);
    g_signal_connect(webrtc, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

static void on_ws_message(SoupWebsocketConnection*, SoupWebsocketDataType, GBytes *msg, gpointer) {
    gsize size;
    const gchar *data = (const gchar*)g_bytes_get_data(msg, &size);
    g_print("[LOG] Received WS message: %.*s\n", (int)size, data);

    // Парсимо JSON
    JsonParser *parser = json_parser_new();
    GError *error = nullptr;
    if (!json_parser_load_from_data(parser, data, size, &error)) {
        g_printerr("[ERROR] Failed to parse JSON message: %s\n", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_printerr("[ERROR] JSON root is not an object\n");
        g_object_unref(parser);
        return;
    }
    JsonObject *obj = json_node_get_object(root);

    // Ігноруємо повідомлення не для нашого пристрою
    if (json_object_has_member(obj, "device")) {
        const gchar *msg_dev = json_object_get_string_member(obj, "device");
        if (!msg_dev || g_strcmp0(msg_dev, g_device_id) != 0) {
            g_print("[LOG] Ignoring message for device '%s'\n", msg_dev ? msg_dev : "(null)");
            g_object_unref(parser);
            return;
        }
    }

    // 1) Обробка "action"
    if (json_object_has_member(obj, "action")) {
        const gchar *action = json_object_get_string_member(obj, "action");
        if (action) {
            if (g_strcmp0(action, "ready") == 0) {
                g_print("[LOG] Action=ready → start_pipeline()\n");
                start_pipeline();
            }
            else if (g_strcmp0(action, "control") == 0) {
                const gchar *direction = json_object_has_member(obj, "direction")
                    ? json_object_get_string_member(obj, "direction") : nullptr;
                const gchar *turn = json_object_has_member(obj, "turn")
                    ? json_object_get_string_member(obj, "turn") : nullptr;
                int speed = json_object_has_member(obj, "speed")
                    ? json_object_get_int_member(obj, "speed") : 50;

                g_print("[LOG] Action=control → dir=%s, turn=%s, speed=%d\n",
                        direction?direction:"none",
                        turn?turn:"none",
                        speed);
                control_vehicle(direction, turn, speed);
            }
            else if (g_strcmp0(action, "stop") == 0) {
                g_print("[LOG] Action=stop → stop_vehicle()\n");
                stop_vehicle();
            }
        }
    }
    // 2) Обробка SDP-answer
    else if (json_object_has_member(obj, "type") &&
             g_strcmp0(json_object_get_string_member(obj, "type"), "answer") == 0 &&
             json_object_has_member(obj, "sdp"))
    {
        const gchar *sdp_text = json_object_get_string_member(obj, "sdp");
        g_print("[LOG] Received SDP-answer, parsing...\n");

        GstSDPMessage *sdpmsg = nullptr;
        if (gst_sdp_message_new(&sdpmsg) == GST_SDP_OK &&
            gst_sdp_message_parse_buffer((guint8*)sdp_text,
                                         strlen(sdp_text),
                                         sdpmsg) == GST_SDP_OK)
        {
            GstWebRTCSessionDescription *desc =
                gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpmsg);
            // ! Ось тут — ownership of sdpmsg передається в desc,
            //    тому НЕ викликаємо gst_sdp_message_free(sdpmsg) !

            g_print("[LOG] Setting remote description (answer)\n");
            g_signal_emit_by_name(webrtc,
                                  "set-remote-description",
                                  desc,
                                  nullptr);
            gst_webrtc_session_description_free(desc);
        } else {
            g_printerr("[ERROR] Failed to parse SDP-answer buffer\n");
            if (sdpmsg)
                gst_sdp_message_free(sdpmsg);
        }
    }
    // 3) Обробка ICE-candidate
    else if (json_object_has_member(obj, "candidate")) {
        const gchar *cand = json_object_get_string_member(obj, "candidate");
        if (cand && *cand != '\0') {
            int idx = json_object_has_member(obj, "sdpMLineIndex")
                ? json_object_get_int_member(obj, "sdpMLineIndex") : 0;

            g_print("[LOG] Adding ICE candidate: mline=%d candidate=%s\n", idx, cand);
            g_signal_emit_by_name(webrtc,
                                  "add-ice-candidate",
                                  idx,
                                  cand);
        } else {
            g_print("[LOG] Ignoring empty ICE candidate\n");
        }
    }

    g_object_unref(parser);
}


static void on_ws_connected(GObject *src, GAsyncResult *res, gpointer) {
    GError *err = NULL;
    ws_conn = soup_session_websocket_connect_finish(session, res, &err);
    if (err) { g_printerr("[ERROR] WS failed: %s\n", err->message); return; }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
      json_builder_set_member_name(b, "action");
      json_builder_add_string_value(b, "ready");
      json_builder_set_member_name(b, "device");
      json_builder_add_string_value(b, g_device_id);
    json_builder_end_object(b);
    send_json(b); g_object_unref(b);
    g_signal_connect(ws_conn, "message",
                     G_CALLBACK(on_ws_message), NULL);
}

void start_webrtc(const char *sig_ip, const char *sig_port,
                  const char *device_id, GMainLoop *loop) {
    gloop = loop;
    g_device_id = g_strdup(device_id);
    gchar *addr = g_strdup_printf("ws://%s:%s/ws", sig_ip, sig_port);
    session = soup_session_new();
    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, addr);
    g_free(addr);
    soup_session_websocket_connect_async(
        session, msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL,
        on_ws_connected, session);
}

void cleanup_webrtc() {
    if (pipeline) { gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline); pipeline = nullptr; }
    if (ws_conn) { g_object_unref(ws_conn); ws_conn = nullptr; }
    if (session) { g_object_unref(session); session = nullptr; }
    g_free(g_device_id); g_device_id = nullptr;
}
