// sk_http.h — one-shot GETs to the Signal K server the stream is connected to, for data that
// isn't in the stream (tide predictions, position history). Blocking: call from a background
// task, never the UI task.
#pragma once

// GET http://<server><path> into a NUL-terminated PSRAM buffer of at most max_bytes (caller
// frees). Follows redirects. NULL on any failure (logged under `tag`).
char *sk_http_get(const char *path, int max_bytes, const char *tag);
