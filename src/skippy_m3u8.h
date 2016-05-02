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
#include "skippy_hls_priv.h"

// Add extern "C" when using C++ compiler
G_BEGIN_DECLS

struct SkippyM3U8ClientPrivate;

typedef struct _SkippyM3U8Client
{
  struct SkippyM3U8ClientPrivate* priv;
} SkippyM3U8Client;

SkippyM3U8Client *skippy_m3u8_client_new ();
void skippy_m3u8_client_free (SkippyM3U8Client * client);

// Called to get the next fragment
SkippyFragment* skippy_m3u8_client_get_current_fragment (SkippyM3U8Client * client);
SkippyFragment* skippy_m3u8_client_get_fragment (SkippyM3U8Client * client, guint64 sequence_number);
void skippy_m3u8_client_advance_to_next_fragment (SkippyM3U8Client * client);
gboolean skippy_m3u8_client_seek_to (SkippyM3U8Client * client, GstClockTime target);

gchar* skippy_m3u8_client_get_uri(SkippyM3U8Client * client);

// Update/set/identify variant (sub-) playlist by URIs advertised in master playlist
SkippyHlsInternalError skippy_m3u8_client_load_playlist (SkippyM3U8Client * client, const gchar *uri, GstBuffer* playlist_buffer);

gchar *skippy_m3u8_client_get_playlist_for_bitrate (SkippyM3U8Client * client, guint bitrate);
gchar *skippy_m3u8_client_get_current_playlist (SkippyM3U8Client * client);
void skippy_m3u8_client_set_current_playlist (SkippyM3U8Client * client, const gchar *uri);

GstClockTime skippy_m3u8_client_get_total_duration (SkippyM3U8Client * client);
GstClockTime skippy_m3u8_client_get_target_duration (SkippyM3U8Client * client);

gboolean skippy_m3u8_client_has_variant_playlist(SkippyM3U8Client * client);
gboolean skippy_m3u8_client_is_live(SkippyM3U8Client * client);
gboolean skippy_m3u8_client_is_caching_allowed(SkippyM3U8Client * client);

gchar* skippy_m3u8_client_get_current_raw_data (SkippyM3U8Client * client);

G_END_DECLS
