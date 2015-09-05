/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gsturidownloader.h:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * Youshould have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include "skippy_fragment.h"

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define TYPE_SKIPPY_URI_DOWNLOADER (skippy_uri_downloader_get_type())
#define SKIPPY_URI_DOWNLOADER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),TYPE_SKIPPY_URI_DOWNLOADER,SkippyUriDownloader))
#define SKIPPY_URI_DOWNLOADER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),TYPE_SKIPPY_URI_DOWNLOADER,SkippyUriDownloaderClass))
#define IS_SKIPPY_SURI_DOWNLOADER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),TYPE_SKIPPY_URI_DOWNLOADER))
#define IS_SKIPPY_URI_DOWNLOADER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),TYPE_SKIPPY_URI_DOWNLOADER))

// Constants for custom element message names
#define SKIPPY_HLS_DEMUX_DOWNLOADING_MSG_NAME "skippy-hlsdemux-download"

typedef struct _SkippyUriDownloader SkippyUriDownloader;
typedef struct _SkippyUriDownloaderPrivate SkippyUriDownloaderPrivate;
typedef struct _SkippyUriDownloaderClass SkippyUriDownloaderClass;

typedef void (*SkippyUriDownloaderCallback) (SkippyUriDownloader *downloader, guint64 start_time, guint64 stop_time,
																				gsize bytes_loaded, gsize bytes_total);
typedef enum {
	SKIPPY_URI_DOWNLOADER_VOID,
	SKIPPY_URI_DOWNLOADER_FAILED,
	SKIPPY_URI_DOWNLOADER_CANCELLED,
	SKIPPY_URI_DOWNLOADER_COMPLETED,
} SkippyUriDownloaderFetchReturn;

struct _SkippyUriDownloader
{
  GstBin parent;

  SkippyUriDownloaderPrivate *priv;
};

struct _SkippyUriDownloaderClass
{
  GstBinClass parent_class;

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING];
};

GType skippy_uri_downloader_get_type (void);

// URI can be NULL (then source will be created on demand with first fetch)
SkippyUriDownloader * skippy_uri_downloader_new ();

void skippy_uri_downloader_prepare (SkippyUriDownloader * downloader, gchar* uri);
SkippyUriDownloaderFetchReturn skippy_uri_downloader_fetch_fragment (SkippyUriDownloader * downloader, SkippyFragment* fragment,
	const gchar * referer, gboolean compress, gboolean refresh, gboolean allow_cache, GError ** err);
GstBuffer* skippy_uri_downloader_get_buffer (SkippyUriDownloader *downloader);

void skippy_uri_downloader_interrupt (SkippyUriDownloader * downloader);

void skippy_uri_downloader_continue (SkippyUriDownloader * downloader);

G_END_DECLS
