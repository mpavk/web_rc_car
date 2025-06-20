#ifndef WEBRTC_PIPELINE_H
#define WEBRTC_PIPELINE_H

#include <glib.h>

void start_webrtc(const char *sig_ip, const char *sig_port,
                  const char *device_id, GMainLoop *loop);
void cleanup_webrtc();

#endif // WEBRTC_PIPELINE_H
