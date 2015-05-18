/* GStreamer
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * m3u8.h:
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

#pragma once

#include <gst/gst.h>

#include "skippy_fragment.h"
#include "skippy_uridownloader.h"

G_BEGIN_DECLS

typedef struct _SkippyM3U8 SkippyM3U8;
typedef struct _SkippyM3U8MediaFile SkippyM3U8MediaFile;
typedef struct _SkippyM3U8Client SkippyM3U8Client;

#define SKIPPY_M3U8(m) ((SkippyM3U8*)m)
#define SKIPPY_M3U8_MEDIA_FILE(f) ((SkippyM3U8MediaFile*)f)

#define SKIPPY_M3U8_CLIENT_LOCK(c) g_mutex_lock (&c->lock);
#define SKIPPY_M3U8_CLIENT_UNLOCK(c) g_mutex_unlock (&c->lock);

struct _SkippyM3U8
{
  gchar *uri;

  gboolean endlist;             /* if ENDLIST has been reached */
  gint version;                 /* last EXT-X-VERSION */
  GstClockTime targetduration;  /* last EXT-X-TARGETDURATION */
  gboolean allowcache;          /* last EXT-X-ALLOWCACHE */
  gchar *key;

  gint bandwidth;
  gint program_id;
  gchar *codecs;
  gint width;
  gint height;
  GList *files;                 /* List of media files / segments */

  /*< private > */
  gchar *last_data;
  GList *lists;                 /* List of SkippyM3U8 from the main playlist */
  GList *current_variant;       /* Current variant playlist used */
  SkippyM3U8 *parent;           /* Main playlist (if any) */
  guint mediasequence;          /* EXT-X-MEDIA-SEQUENCE & increased with new media file */
};

struct _SkippyM3U8MediaFile
{
  gchar *title;
  GstClockTime duration;
  gchar *uri;
  guint sequence;               /* the sequence nb of this file */
  gboolean discont;             /* this file marks a discontinuity */
  gchar *key;
  guint8 iv[16];
  gint64 offset, size;
};

struct _SkippyM3U8Client
{
  SkippyM3U8 *main;               /* main playlist */
  SkippyM3U8 *current;            /* currently used playlist */
  gint sequence;                  /* the next sequence for this client */
  GstClockTime sequence_position; /* position of this sequence */
  GMutex lock;
};


SkippyM3U8Client *skippy_m3u8_client_new (const gchar * uri);
void skippy_m3u8_client_free (SkippyM3U8Client * client);
gboolean skippy_m3u8_client_update (SkippyM3U8Client * client, gchar * data);
void skippy_m3u8_client_set_current (SkippyM3U8Client * client, SkippyM3U8 * m3u8);

// Called by SkippyHLSDemux to get the next fragment
SkippyFragment* skippy_m3u8_client_get_next_fragment (SkippyM3U8Client * client);

void skippy_m3u8_client_advance_fragment (SkippyM3U8Client * client);
GstClockTime skippy_m3u8_client_get_duration (SkippyM3U8Client * client);
GstClockTime skippy_m3u8_client_get_target_duration (SkippyM3U8Client * client);
const gchar *skippy_m3u8_client_get_uri(SkippyM3U8Client * client);
const gchar *skippy_m3u8_client_get_current_uri(SkippyM3U8Client * client);
gboolean skippy_m3u8_client_has_variant_playlist(SkippyM3U8Client * client);
gboolean skippy_m3u8_client_is_live(SkippyM3U8Client * client);
SkippyM3U8 * skippy_m3u8_client_get_playlist_for_bitrate (SkippyM3U8Client * client,
    guint bitrate);
SkippyM3U8 * skippy_m3u8_client_get_current_variant (SkippyM3U8Client * client);

guint64 skippy_m3u8_client_get_current_fragment_duration (SkippyM3U8Client * client);

gboolean
skippy_m3u8_client_load_playlist (SkippyM3U8Client * client, GstBuffer* playlist_buffer);

G_END_DECLS