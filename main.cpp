#include <gst/gst.h>
#include <glib.h>
#include "gpio_control.h"
#include "pwm_control.h"
#include "webrtc_pipeline.h"

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    if (argc != 4) {
        g_printerr("Usage: %s <signaling-server-ip> <port> <device-id>\n", argv[0]);
        return 1;
    }
    const char *sig_ip   = argv[1];
    const char *sig_port = argv[2];
    const char *device_id= argv[3];

    init_motor_control();
    init_software_pwm();
    start_webrtc(sig_ip, sig_port, device_id, loop);

    g_main_loop_run(loop);

    cleanup_webrtc();
    cleanup_pwm();
    cleanup_motor_control();
    g_main_loop_unref(loop);
    return 0;
}
