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
  gchar *allowcache;            /* last EXT-X-ALLOWCACHE */
  gchar *key;

  gint bandwidth;
  gint program_id;
  gchar *codecs;
  gint width;
  gint height;
  GList *files;

  /*< private > */
  gchar *last_data;
  GList *lists;                 /* list of SkippyM3U8 from the main playlist */
  GList *current_variant;       /* Current variant playlist used */
  SkippyM3U8 *parent;              /* main playlist (if any) */
  guint mediasequence;          /* EXT-X-MEDIA-SEQUENCE & increased with new media file */
};

struct _SkippyM3U8MediaFile
{
  gchar *title;
  GstClockTime duration;
  gchar *uri;
  guint sequence;               /* the sequence nb of this file */
  gchar *key;
  guint8 iv[16];
};

struct _SkippyM3U8Client
{
  SkippyM3U8 *main;                /* main playlist */
  SkippyM3U8 *current;
  guint update_failed_count;
  gint sequence;                /* the next sequence for this client */
  GMutex lock;
};


SkippyM3U8Client *skippy_m3u8_client_new (const gchar * uri);
void skippy_m3u8_client_free (SkippyM3U8Client * client);
gboolean skippy_m3u8_client_update (SkippyM3U8Client * client, gchar * data);
void skippy_m3u8_client_set_current (SkippyM3U8Client * client, SkippyM3U8 * m3u8);
gboolean skippy_m3u8_client_get_next_fragment (SkippyM3U8Client * client,
    gboolean * discontinuity, const gchar ** uri, GstClockTime * duration,
    GstClockTime * timestamp, const gchar ** key, const guint8 ** iv);
void skippy_m3u8_client_get_current_position (SkippyM3U8Client * client,
    GstClockTime * timestamp);
GstClockTime skippy_m3u8_client_get_duration (SkippyM3U8Client * client);
GstClockTime skippy_m3u8_client_get_target_duration (SkippyM3U8Client * client);
const gchar *skippy_m3u8_client_get_uri(SkippyM3U8Client * client);
const gchar *skippy_m3u8_client_get_current_uri(SkippyM3U8Client * client);
gboolean skippy_m3u8_client_has_variant_playlist(SkippyM3U8Client * client);
gboolean skippy_m3u8_client_is_live(SkippyM3U8Client * client);
GList * skippy_m3u8_client_get_playlist_for_bitrate (SkippyM3U8Client * client,
    guint bitrate);

G_END_DECLS