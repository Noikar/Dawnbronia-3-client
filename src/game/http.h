#ifndef ASTONIA_HTTP_H
#define ASTONIA_HTTP_H

#include <stddef.h>

// Minimal HTTPS client for the auto-updater, backed by Windows' WinHTTP
// (TLS via SChannel, no external dependencies). All URLs must be UTF-8; only
// the "https" scheme is expected. These calls block, so run them off the render
// thread.

// Progress callback for downloads. total is 0 when the server sends no
// Content-Length. Return is ignored.
typedef void (*http_progress_cb)(unsigned long long received,
                                 unsigned long long total, void *userdata);

// GET url and return the whole response body as a freshly malloc'd,
// NUL-terminated buffer (caller frees). On success returns the buffer and, when
// out_len is non-NULL, stores the body length (excluding the NUL). Returns NULL
// on any transport error or non-2xx status.
char *http_get(const char *url, size_t *out_len);

// GET url and stream the body to dest_path (created/truncated). progress_cb may
// be NULL. Returns 0 on success (2xx and fully written), -1 otherwise. A failed
// download removes any partial file.
int http_download(const char *url, const char *dest_path,
                  http_progress_cb progress_cb, void *userdata);

#endif
