#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <glib.h>
#include <libsoup/soup.h>
#include <libsoup/soup-websocket.h>
#include <json-glib/json-glib.h>
// #include <wiringPi.h>  // For GPIO control

// GPIO pin definitions for motor control
#define MOTOR_LEFT_FWD  23  // GPIO pins for left motor forward
#define MOTOR_LEFT_REV  24  // GPIO pins for left motor backward
#define MOTOR_RIGHT_FWD 25  // GPIO pins for right motor forward
#define MOTOR_RIGHT_REV 26  // GPIO pins for right motor backward

static GMainLoop               *loop;
static SoupWebsocketConnection *ws_conn = nullptr;
static GstElement              *pipeline = nullptr, *webrtc = nullptr;
static bool                     pipeline_started = false;
static gchar                   *device_id = nullptr;  // Identifier for this device

// Initialize GPIO for motor control
static void init_motor_control() {
    g_print("[TEST] Simulating motor control initialization\n");
}

// Імітація виконання команд
static void control_vehicle(const gchar *command) {
    g_print("[TEST] Vehicle command received: %s\n", command);

    if (g_strcmp0(command, "forward") == 0) {
        g_print("[ACTION] Simulate: Moving forward\n");
    }
    else if (g_strcmp0(command, "backward") == 0) {
        g_print("[ACTION] Simulate: Moving backward\n");
    }
    else if (g_strcmp0(command, "left") == 0) {
        g_print("[ACTION] Simulate: Turning left\n");
    }
    else if (g_strcmp0(command, "right") == 0) {
        g_print("[ACTION] Simulate: Turning right\n");
    }
    else if (g_strcmp0(command, "stop") == 0) {
        g_print("[ACTION] Simulate: Stopping\n");
    }
    else {
        g_print("[WARN] Unknown simulated command: %s\n", command);
    }
}

// Відправка JSON через WebSocket
static void send_json(JsonBuilder *b) {
    JsonGenerator *gen = json_generator_new();
    JsonNode      *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    gchar *text = json_generator_to_data(gen, NULL);
    g_print("[LOG] Sending JSON via WebSocket: %s\n", text);
    soup_websocket_connection_send_text(ws_conn, text);
    g_free(text);
    g_object_unref(gen);
    json_node_free(root);
}

// SDP-offer
static void on_negotiation_needed(GstElement *webrtc_elem, gpointer) {
    g_print("[LOG] on-negotiation-needed called\n");

    GstPromise *promise = gst_promise_new_with_change_func(
      [](GstPromise *p, gpointer user_data) {
        g_print("[LOG] Promise callback: negotiation-reply\n");
        GstElement *webrtc = GST_ELEMENT(user_data);
        const GstStructure *reply = gst_promise_get_reply(p);
        GstWebRTCSessionDescription *offer = nullptr;

        gst_structure_get(reply,
                          "offer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                          &offer,
                          NULL);
        gst_promise_unref(p);

        g_print("[LOG] Received SDP offer, setting local description\n");
        GstPromise *local_promise = gst_promise_new();
        g_signal_emit_by_name(webrtc,
                              "set-local-description",
                              offer,
                              local_promise);
        gst_promise_unref(local_promise);

        g_print("[LOG] Building JSON for SDP offer\n");
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
          // Include device identifier
          json_builder_set_member_name(b, "device");
          json_builder_add_string_value(b, device_id);
          json_builder_set_member_name(b, "sdp");
          gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);
          json_builder_add_string_value(b, sdp_text);
          g_free(sdp_text);
          json_builder_set_member_name(b, "type");
          json_builder_add_string_value(b,
            gst_webrtc_sdp_type_to_string(offer->type));
        json_builder_end_object(b);

        send_json(b);
        g_object_unref(b);
        gst_webrtc_session_description_free(offer);
      },
      webrtc_elem,  // user_data
      nullptr       // no finalize
    );

    g_print("[LOG] Emitting create-offer signal\n");
    g_signal_emit_by_name(webrtc_elem,
                          "create-offer",
                          nullptr,
                          promise);
}

// ICE-candidate
static void on_ice_candidate(GstElement*, guint mline, gchar *cand, gpointer) {
    if (!cand || *cand == '\0') {
      g_print("[LOG] Empty candidate, end of candidates\n");
      return;
    }
    g_print("[LOG] on-ice-candidate: mline=%u candidate=%s\n", mline, cand);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
      // Include device identifier
      json_builder_set_member_name(b, "device");
      json_builder_add_string_value(b, device_id);
      json_builder_set_member_name(b, "candidate");
      json_builder_add_string_value(b, cand);
      json_builder_set_member_name(b, "sdpMLineIndex");
      json_builder_add_int_value(b, mline);
    json_builder_end_object(b);
    send_json(b);
    g_object_unref(b);
}

// Створення та запуск GStreamer-пайплайна
static void start_pipeline() {
    if (pipeline_started) return;
    pipeline_started = true;

    g_print("[LOG] Creating optimized GStreamer pipeline for Pi Zero W\n");
    GError *err = nullptr;
    pipeline = gst_parse_launch(
      "libcamerasrc ! "
      "video/x-raw,width=320,height=240,framerate=15/1 ! "  // Tiny resolution and very low framerate
      "videoconvert ! "
      "x264enc tune=zerolatency bitrate=200 speed-preset=ultrafast ! "  // Absolute minimum bitrate and fastest encoding
      "h264parse ! "
      "rtph264pay config-interval=-1 pt=96 ! "  // Less frequent config packets
      "webrtcbin name=webrtc stun-server=stun://%s:%s/ws latency=10",
      &err);
    if (!pipeline) {
      g_printerr("[ERROR] Failed to create pipeline: %s\n", err->message);
      g_error_free(err);
      g_main_loop_quit(loop);
      return;
    }

    g_print("[LOG] Setting up bus watch\n");
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message::error", G_CALLBACK(+[](GstBus *bus, GstMessage *msg, gpointer) {
      GError *e = nullptr; gchar *dbg = nullptr;
      gst_message_parse_error(msg, &e, &dbg);
      g_printerr("[ERROR] GstError from %s: %s\nDebug: %s\n",
                 GST_OBJECT_NAME(msg->src), e->message, dbg);
      g_error_free(e); g_free(dbg);
    }), nullptr);
    g_signal_connect(bus, "message::warning", G_CALLBACK(+[](GstBus *bus, GstMessage *msg, gpointer) {
      GError *e = nullptr; gchar *dbg = nullptr;
      gst_message_parse_warning(msg, &e, &dbg);
      g_printerr("[WARN] GstWarning from %s: %s\nDebug: %s\n",
                 GST_OBJECT_NAME(msg->src), e->message, dbg);
      g_error_free(e); g_free(dbg);
    }), nullptr);
    gst_object_unref(bus);

    g_print("[LOG] Retrieving webrtcbin element\n");
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    if (!webrtc) {
      g_printerr("[ERROR] Failed to get webrtcbin element\n");
      g_main_loop_quit(loop);
      return;
    }

    g_print("[LOG] Connecting WebRTC signals\n");
    g_signal_connect(webrtc, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), webrtc);
    g_signal_connect(webrtc, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_print("[LOG] Pipeline set to PLAYING\n");
}

// Обробка вхідних WS-повідомлень
static void on_ws_message(SoupWebsocketConnection*, SoupWebsocketDataType, GBytes *msg, gpointer) {
    gsize size;
    const gchar *data = (const gchar*)g_bytes_get_data(msg, &size);
    g_print("[LOG] Received WS message: %.*s\n", (int)size, data);

    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, data, size, NULL);
    JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

    // Filter by device_id
    if (json_object_has_member(obj, "device")) {
      const char *msg_dev = json_object_get_string_member(obj, "device");
      if (g_strcmp0(msg_dev, device_id) != 0) {
        g_print("[LOG] Ignoring message for device '%s'\n", msg_dev);
        g_object_unref(parser);
        return;
      }
    }

    // 1) Browser says "ready" → start pipeline
    if (json_object_has_member(obj, "action")) {
      const gchar *action = json_object_get_string_member(obj, "action");

      if (g_strcmp0(action, "ready") == 0) {
        g_print("[LOG] Received READY from browser — starting pipeline\n");
        start_pipeline();
      }
      // Vehicle control commands
      else if (g_strcmp0(action, "control") == 0 && json_object_has_member(obj, "command")) {
        const gchar *command = json_object_get_string_member(obj, "command");
        g_print("[LOG] Received vehicle control command: %s\n", command);
        control_vehicle(command);
      }
    }
    // 2) Incoming SDP-answer
    else if (json_object_has_member(obj, "type") &&
             g_strcmp0(json_object_get_string_member(obj, "type"), "answer") == 0) {
        g_print("[LOG] Handling incoming SDP answer\n");
        const gchar *sdp_text = json_object_get_string_member(obj, "sdp");
        GstSDPMessage *sdpmsg = nullptr;
        gst_sdp_message_new(&sdpmsg);
        gst_sdp_message_parse_buffer((guint8*)sdp_text,
                                     strlen(sdp_text), sdpmsg);
        GstWebRTCSessionDescription *desc =
          gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpmsg);
        g_print("[LOG] Setting remote description (answer)\n");
        g_signal_emit_by_name(webrtc,
                              "set-remote-description",
                              desc, NULL);
        gst_webrtc_session_description_free(desc);
    }
    // 3) Incoming ICE-candidate
    else if (json_object_has_member(obj, "candidate")) {
        g_print("[LOG] Handling incoming ICE candidate\n");
        const gchar *cand = json_object_get_string_member(obj, "candidate");
        int idx = json_object_get_int_member(obj, "sdpMLineIndex");
        g_print("[LOG] Adding ICE candidate: mline=%d candidate=%s\n", idx, cand);
        g_signal_emit_by_name(webrtc,
                              "add-ice-candidate",
                              idx, cand);
    }

    g_object_unref(parser);
}

// Callback when WebSocket connect completes
static void
on_ws_connected(GObject     *source,
                GAsyncResult *res,
                gpointer      user_data)
{
    g_print("[LOG] on_ws_connected called\n");
    SoupSession *session = SOUP_SESSION(source);
    GError      *error   = nullptr;

    ws_conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error) {
        g_printerr("[ERROR] WebSocket connect failed: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("[LOG] WebSocket connected successfully\n");

    // Відправляємо ready з device_id одразу після підключення
    {
      JsonBuilder *b = json_builder_new();
      json_builder_begin_object(b);
        json_builder_set_member_name(b, "action");
        json_builder_add_string_value(b, "ready");
        json_builder_set_member_name(b, "device");
        json_builder_add_string_value(b, device_id);
      json_builder_end_object(b);
      send_json(b);
      g_object_unref(b);
    }

    // Підключаємо обробник вхідних повідомлень
    g_signal_connect(ws_conn, "message",
                     G_CALLBACK(on_ws_message), nullptr);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    if (argc != 4) {
        g_printerr("Usage: %s <signaling-server-ip> <port> <device-id>\n", argv[0]);
        return 1;
    }
    const char *sig_ip   = argv[1];
    const char *sig_port = argv[2];
    device_id            = g_strdup(argv[3]);

    // Initialize motor control
    init_motor_control();

    gchar *ws_address = g_strdup_printf("ws://%s:%s/ws", sig_ip, sig_port);
    g_print("[LOG] Connecting to signaling server %s\n", ws_address);

    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new(
      SOUP_METHOD_GET,
      ws_address
    );
    g_free(ws_address);

    soup_session_websocket_connect_async(
        session,
        msg,
        NULL,
        NULL,
        G_PRIORITY_DEFAULT,
        NULL,
        on_ws_connected,
        session
    );

    g_print("[LOG] Entering main loop\n");
    g_main_loop_run(loop);

    g_print("[LOG] Cleaning up\n");
    if (pipeline) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
    }
    g_free(device_id);
    g_main_loop_unref(loop);
    return 0;
}
