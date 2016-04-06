#pragma once

#include <glib.h>

#define SKIPPY_HLS_DOWNLOAD_AHEAD "skippy-download-ahead"
#define GST_SKIPPY_HLS_ERROR skippy_hls_error_quark()

G_BEGIN_DECLS

GQuark skippy_hls_error_quark(void);

typedef enum {
  GST_SKIPPY_HLS_ERROR_PLAYLIST_INVALID_UTF_CONTENT,
  GST_SKIPPY_HLS_ERROR_PLAYLIST_INCOMPLETE_ON_REFRESH,
  GST_SKIPPY_HLS_ERROR_PLAYLIST_INCOMPLETE_ON_LOAD,
} GstSkippyHlsError;


void skippy_hlsdemux_setup (guint hls_demux_rank);

G_END_DECLS