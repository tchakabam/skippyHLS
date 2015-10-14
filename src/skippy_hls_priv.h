#pragma once

#include <glib.h>

#define SKIPPY_HLS_DOWNLOAD_AHEAD "skippy-download-ahead"
#define SKIPPY_HLS_ERROR skippy_hls_error_quark()
G_GNUC_INTERNAL GQuark skippy_hls_error_quark(void);

typedef enum {
  SKIPPY_HLS_ERROR_PLAYLIST_INVALID_UTF_CONTENT,
  SKIPPY_HLS_ERROR_PLAYLIST_INCOMPLETE,
} SkippyHlsError;