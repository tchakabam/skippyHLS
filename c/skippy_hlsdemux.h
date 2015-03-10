/* GStreamer
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gsthlsdemux.h:
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


#ifndef __GST_HLS_DEMUX_H__
#define __GST_HLS_DEMUX_H__

#include <gst/gst.h>
#include <gst/base/gstadapter.h>

#include "skippy_m3u8.h"
#include "skippy_uridownloader.h"

G_BEGIN_DECLS
#define TYPE_SKIPPY_HLS_DEMUX \
  (skippy_hls_demux_get_type())
#define SKIPPY_HLS_DEMUX(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),TYPE_SKIPPY_HLS_DEMUX,SkippyHLSDemux))
#define SKIPPY_HLS_DEMUX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),TYPE_SKIPPY_HLS_DEMUX,SkippyHLSDemuxClass))
#define IS_SKIPPY_HLS_DEMUX(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),TYPE_SKIPPY_HLS_DEMUX))
#define IS_SKIPPY_HLS_DEMUX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),TYPE_SKIPPY_HLS_DEMUX))
#define SKIPPY_HLS_DEMUX_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj),TYPE_HLS_DEMUX,SkippyHLSDemuxClass))
typedef struct _SkippyHLSDemux SkippyHLSDemux;
typedef struct _SkippyHLSDemuxClass SkippyHLSDemuxClass;

/**
 * SkippyHLSDemux:
 *
 * Opaque #SkippyHLSDemux data structure.
 */
struct _SkippyHLSDemux
{
  GstElement parent;

  GstPad *srcpad;
  GstPad *sinkpad;

  gboolean have_group_id;
  guint group_id;

  GstBuffer *playlist;
  GstCaps *input_caps;
  SkippyUriDownloader *downloader;
  SkippyM3U8Client *client;        /* M3U8 client */
  GQueue *queue;                /* Queue storing the fetched fragments */
  gboolean need_cache;          /* Wheter we need to cache some fragments before starting to push data */
  gboolean end_of_playlist;
  gboolean do_typefind;         /* Whether we need to typefind the next buffer */

  /* Properties */
  guint fragments_cache;        /* number of fragments needed to be cached to start playing */
  gfloat bitrate_limit;         /* limit of the available bitrate to use */
  guint connection_speed;       /* Network connection speed in kbps (0 = unknown) */

  /* Streaming task */
  GstTask *stream_task;
  GRecMutex stream_lock;
  gboolean stop_stream_task;

  /* Updates task */
  GstTask *updates_task;
  GRecMutex updates_lock;
  GMutex updates_timed_lock;
  GTimeVal next_update;         /* Time of the next update */
  gboolean cancelled;

  /* Position in the stream */
  GstClockTime position_shift;
  gboolean need_segment;
};

struct _SkippyHLSDemuxClass
{
  GstElementClass parent_class;
};

G_GNUC_INTERNAL
GType skippy_hls_demux_get_type (void);

G_GNUC_INTERNAL
void skippy_hlsdemux_setup (void);

G_END_DECLS
#endif /* __GST_HLS_DEMUX_H__ */
