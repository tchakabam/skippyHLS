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

typedef struct _SkippyUriDownloader SkippyUriDownloader;
typedef struct _SkippyUriDownloaderPrivate SkippyUriDownloaderPrivate;
typedef struct _SkippyUriDownloaderClass SkippyUriDownloaderClass;

struct _SkippyUriDownloader
{
  GstObject parent;

  SkippyUriDownloaderPrivate *priv;
};

struct _SkippyUriDownloaderClass
{
  GstObjectClass parent_class;

  /*< private >*/
  gpointer _gst_reserved[GST_PADDING];
};

GType skippy_uri_downloader_get_type (void);

SkippyUriDownloader * skippy_uri_downloader_new (void);
SkippyFragment * skippy_uri_downloader_fetch_uri (SkippyUriDownloader * downloader, const gchar * uri, const gchar * referer, gboolean compress, gboolean refresh, gboolean allow_cache, GError ** err);
SkippyFragment * skippy_uri_downloader_fetch_uri_with_range (SkippyUriDownloader * downloader, const gchar * uri, const gchar * referer, gboolean compress, gboolean refresh, gboolean allow_cache, gint64 range_start, gint64 range_end, GError ** err);
void skippy_uri_downloader_reset (SkippyUriDownloader *downloader);
void skippy_uri_downloader_cancel (SkippyUriDownloader *downloader);
void skippy_uri_downloader_free (SkippyUriDownloader *downloader);

G_END_DECLS
