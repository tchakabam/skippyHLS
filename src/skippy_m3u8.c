/* skippyHLS
 *
 * Copyright (C) 2015, SoundCloud Ltd. (http://soundcloud.com)
 *  Author: Stephan Hesse <stephan@soundcloud.com>, SoundCloud Ltd.
 *
 * skippy_hlsdemux.c:
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

 #include "skippyhls/skippy_m3u8.h"

#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <glib.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (skippy_m3u8_debug);
#define GST_CAT_DEFAULT skippy_m3u8_debug

static GOnce init_once = G_ONCE_INIT;

static SkippyM3U8 *skippy_m3u8_new (void);
static void skippy_m3u8_free (SkippyM3U8 * m3u8);
static gboolean skippy_m3u8_update (SkippyM3U8 * m3u8, gchar * data,
    gboolean * updated);
static SkippyM3U8MediaFile *skippy_m3u8_media_file_new (gchar * uri,
    gchar * title, GstClockTime duration, guint sequence);
static void skippy_m3u8_media_file_free (SkippyM3U8MediaFile * self);

static gchar *uri_join (const gchar * uri, const gchar * path);

static gpointer
skippy_m3u8_init_once (gpointer user_data)
{
  GST_DEBUG_CATEGORY_INIT (skippy_m3u8_debug, "skippyhls-m3u8", 0, "M3U8 client");
  return NULL;
}

static SkippyM3U8 *
skippy_m3u8_new (void)
{
  SkippyM3U8 *m3u8;
  m3u8 = g_new0 (SkippyM3U8, 1);
  return m3u8;
}

static void
skippy_m3u8_set_uri (SkippyM3U8 * self, gchar * uri)
{
  g_return_if_fail (self != NULL);

  if (self->uri)
    g_free (self->uri);
  self->uri = uri;
}

static void
skippy_m3u8_free (SkippyM3U8 * self)
{
  g_return_if_fail (self != NULL);

  g_free (self->uri);
  g_free (self->codecs);
  g_free (self->key);

  g_list_foreach (self->files, (GFunc) skippy_m3u8_media_file_free, NULL);
  g_list_free (self->files);

  g_free (self->last_data);
  g_list_foreach (self->lists, (GFunc) skippy_m3u8_free, NULL);
  g_list_free (self->lists);

  g_free (self);
}

static SkippyM3U8MediaFile *
skippy_m3u8_media_file_new (gchar * uri, gchar * title, GstClockTime duration,
    guint sequence)
{
  SkippyM3U8MediaFile *file;

  file = g_new0 (SkippyM3U8MediaFile, 1);
  file->uri = uri;
  file->title = title;
  file->duration = duration;
  file->sequence = sequence;

  return file;
}

static void
skippy_m3u8_media_file_free (SkippyM3U8MediaFile * self)
{
  g_return_if_fail (self != NULL);

  g_free (self->title);
  g_free (self->uri);
  g_free (self->key);
  g_free (self);
}

static gboolean
int_from_string (gchar * ptr, gchar ** endptr, gint * val)
{
  gchar *end;
  gint64 ret;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (val != NULL, FALSE);

  errno = 0;
  ret = g_ascii_strtoll (ptr, &end, 10);
  if ((errno == ERANGE && (ret == G_MAXINT64 || ret == G_MININT64))
      || (errno != 0 && ret == 0)) {
    GST_WARNING ("%s", g_strerror (errno));
    return FALSE;
  }

  if (ret > G_MAXINT || ret < G_MININT) {
    GST_WARNING ("%s", g_strerror (ERANGE));
    return FALSE;
  }

  if (endptr)
    *endptr = end;

  *val = (gint) ret;

  return end != ptr;
}

static gboolean
int64_from_string (gchar * ptr, gchar ** endptr, gint64 * val)
{
  gchar *end;
  gint64 ret;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (val != NULL, FALSE);

  errno = 0;
  ret = g_ascii_strtoll (ptr, &end, 10);
  if ((errno == ERANGE && (ret == G_MAXINT64 || ret == G_MININT64))
      || (errno != 0 && ret == 0)) {
    GST_WARNING ("%s", g_strerror (errno));
    return FALSE;
  }

  if (endptr)
    *endptr = end;

  *val = ret;

  return end != ptr;
}

static gboolean
double_from_string (gchar * ptr, gchar ** endptr, gdouble * val)
{
  gchar *end;
  gdouble ret;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (val != NULL, FALSE);

  errno = 0;
  ret = g_ascii_strtod (ptr, &end);
  if ((errno == ERANGE && (ret == HUGE_VAL || ret == -HUGE_VAL))
      || (errno != 0 && ret == 0)) {
    GST_WARNING ("%s", g_strerror (errno));
    return FALSE;
  }

  if (!isfinite (ret)) {
    GST_WARNING ("%s", g_strerror (ERANGE));
    return FALSE;
  }

  if (endptr)
    *endptr = end;

  *val = (gdouble) ret;

  return end != ptr;
}

static gboolean
parse_attributes (gchar ** ptr, gchar ** a, gchar ** v)
{
  gchar *end = NULL, *p;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (*ptr != NULL, FALSE);
  g_return_val_if_fail (a != NULL, FALSE);
  g_return_val_if_fail (v != NULL, FALSE);

  /* [attribute=value,]* */

  *a = *ptr;
  end = p = g_utf8_strchr (*ptr, -1, ',');
  if (end) {
    gchar *q = g_utf8_strchr (*ptr, -1, '"');
    if (q && q < end) {
      /* special case, such as CODECS="avc1.77.30, mp4a.40.2" */
      q = g_utf8_next_char (q);
      if (q) {
        q = g_utf8_strchr (q, -1, '"');
      }
      if (q) {
        end = p = g_utf8_strchr (q, -1, ',');
      }
    }
  }
  if (end) {
    do {
      end = g_utf8_next_char (end);
    } while (end && *end == ' ');
    *p = '\0';
  }

  *v = p = g_utf8_strchr (*ptr, -1, '=');
  if (*v) {
    *v = g_utf8_next_char (*v);
    *p = '\0';
  } else {
    GST_WARNING ("missing = after attribute");
    return FALSE;
  }

  *ptr = end;
  return TRUE;
}

static gint
_m3u8_compare_uri (SkippyM3U8 * a, gchar * uri)
{
  g_return_val_if_fail (a != NULL, 0);
  g_return_val_if_fail (uri != NULL, 0);

  return g_strcmp0 (a->uri, uri);
}

static gint
skippy_m3u8_compare_playlist_by_bitrate (gconstpointer a, gconstpointer b)
{
  return ((SkippyM3U8 *) (a))->bandwidth - ((SkippyM3U8 *) (b))->bandwidth;
}

/*
 * @data: a m3u8 playlist text data, taking ownership
 */
static gboolean
skippy_m3u8_update (SkippyM3U8 * self, gchar * data, gboolean * updated)
{
  gint val;
  GstClockTime duration;
  gchar *title, *end;
  gboolean discontinuity = FALSE;
  SkippyM3U8 *list;
  gboolean have_iv = FALSE;
  guint8 iv[16] = { 0, };
  gint64 size = -1, offset = -1;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (updated != NULL, FALSE);

  *updated = TRUE;

  /* check if the data changed since last update */
  if (self->last_data && g_str_equal (self->last_data, data)) {
    GST_DEBUG ("Playlist is the same as previous one");
    *updated = FALSE;
    g_free (data);
    return TRUE;
  }

  if (!g_str_has_prefix (data, "#EXTM3U")) {
    GST_WARNING ("Data doesn't start with #EXTM3U");
    *updated = FALSE;
    g_free (data);
    return FALSE;
  }

  g_free (self->last_data);
  self->last_data = data;

  if (self->files) {
    g_list_foreach (self->files, (GFunc) skippy_m3u8_media_file_free, NULL);
    g_list_free (self->files);
    self->files = NULL;
  }

  /* By default, allow caching */
  self->allowcache = TRUE;

  list = NULL;
  duration = 0;
  title = NULL;
  data += 7;
  while (TRUE) {
    gchar *r;

    end = g_utf8_strchr (data, -1, '\n');
    if (end)
      *end = '\0';

    r = g_utf8_strchr (data, -1, '\r');
    if (r)
      *r = '\0';

    if (data[0] != '#' && data[0] != '\0') {
      if (duration <= 0 && list == NULL) {
        GST_LOG ("%s: got line without EXTINF or EXTSTREAMINF, dropping", data);
        goto next_line;
      }

      data = uri_join (self->uri, data);
      if (data == NULL)
        goto next_line;

      if (list != NULL) {
        if (g_list_find_custom (self->lists, data,
                (GCompareFunc) _m3u8_compare_uri)) {
          GST_DEBUG ("Already have a list with this URI");
          skippy_m3u8_free (list);
          g_free (data);
        } else {
          skippy_m3u8_set_uri (list, data);
          self->lists = g_list_append (self->lists, list);
        }
        list = NULL;
      } else {
        SkippyM3U8MediaFile *file;
        file =
            skippy_m3u8_media_file_new (data, title, duration,
            self->mediasequence++);

        /* set encryption params */
        file->key = g_strdup (self->key);
        if (file->key) {
          if (have_iv) {
            memcpy (file->iv, iv, sizeof (iv));
          } else {
            guint8 *iv = file->iv + 12;
            GST_WRITE_UINT32_BE (iv + 12, file->sequence);
          }
        }

        if (size != -1) {
          file->size = size;
          if (offset != -1) {
            file->offset = offset;
          } else {
            SkippyM3U8MediaFile *prev =
                self->files ? g_list_last (self->files)->data : NULL;

            if (!prev) {
              offset = 0;
            } else {
              offset = prev->offset + prev->size;
            }
          }
        } else {
          file->size = -1;
          file->offset = 0;
        }

        file->discont = discontinuity;

        duration = 0;
        title = NULL;
        discontinuity = FALSE;
        size = offset = -1;
        self->files = g_list_append (self->files, file);
      }

    } else if (g_str_has_prefix (data, "#EXT-X-ENDLIST")) {
      self->endlist = TRUE;
    } else if (g_str_has_prefix (data, "#EXT-X-VERSION:")) {
      if (int_from_string (data + 15, &data, &val))
        self->version = val;
    } else if (g_str_has_prefix (data, "#EXT-X-STREAM-INF:")) {
      gchar *v, *a;

      if (list != NULL) {
        GST_WARNING ("Found a list without a uri..., dropping");
        skippy_m3u8_free (list);
      }

      list = skippy_m3u8_new ();
      data = data + 18;
      while (data && parse_attributes (&data, &a, &v)) {
        if (g_str_equal (a, "BANDWIDTH")) {
          if (!int_from_string (v, NULL, &list->bandwidth))
            GST_WARNING ("Error while reading BANDWIDTH");
        } else if (g_str_equal (a, "PROGRAM-ID")) {
          if (!int_from_string (v, NULL, &list->program_id))
            GST_WARNING ("Error while reading PROGRAM-ID");
        } else if (g_str_equal (a, "CODECS")) {
          g_free (list->codecs);
          list->codecs = g_strdup (v);
        } else if (g_str_equal (a, "RESOLUTION")) {
          if (!int_from_string (v, &v, &list->width))
            GST_WARNING ("Error while reading RESOLUTION width");
          if (!v || *v != 'x') {
            GST_WARNING ("Missing height");
          } else {
            v = g_utf8_next_char (v);
            if (!int_from_string (v, NULL, &list->height))
              GST_WARNING ("Error while reading RESOLUTION height");
          }
        }
      }
    } else if (g_str_has_prefix (data, "#EXT-X-TARGETDURATION:")) {
      if (int_from_string (data + 22, &data, &val))
        self->targetduration = val * GST_SECOND;
    } else if (g_str_has_prefix (data, "#EXT-X-MEDIA-SEQUENCE:")) {
      if (int_from_string (data + 22, &data, &val))
        self->mediasequence = val;
    } else if (g_str_has_prefix (data, "#EXT-X-DISCONTINUITY")) {
      discontinuity = TRUE;
    } else if (g_str_has_prefix (data, "#EXT-X-PROGRAM-DATE-TIME:")) {
      /* <YYYY-MM-DDThh:mm:ssZ> */
      GST_DEBUG ("FIXME parse date");
    } else if (g_str_has_prefix (data, "#EXT-X-ALLOW-CACHE:")) {
      self->allowcache = g_ascii_strcasecmp (data + 19, "YES") == 0;
    } else if (g_str_has_prefix (data, "#EXT-X-KEY:")) {
      gchar *v, *a;

      data = data + 11;

      /* IV and KEY are only valid until the next #EXT-X-KEY */
      have_iv = FALSE;
      g_free (self->key);
      self->key = NULL;
      while (data && parse_attributes (&data, &a, &v)) {
        if (g_str_equal (a, "URI")) {
          gchar *key = g_strdup (v);
          gchar *keyp = key;
          int len = strlen (key);

          /* handle the \"key\" case */
          if (key[len - 1] == '"')
            key[len - 1] = '\0';
          if (key[0] == '"')
            key += 1;

          self->key = uri_join (self->uri, key);
          g_free (keyp);
        } else if (g_str_equal (a, "IV")) {
          gchar *ivp = v;
          gint i;

          if (strlen (ivp) < 32 + 2 || (!g_str_has_prefix (ivp, "0x")
                  && !g_str_has_prefix (ivp, "0X"))) {
            GST_WARNING ("Can't read IV");
            continue;
          }

          ivp += 2;
          for (i = 0; i < 16; i++) {
            gint h, l;

            h = g_ascii_xdigit_value (*ivp);
            ivp++;
            l = g_ascii_xdigit_value (*ivp);
            ivp++;
            if (h == -1 || l == -1) {
              i = -1;
              break;
            }
            iv[i] = (h << 4) | l;
          }

          if (i == -1) {
            GST_WARNING ("Can't read IV");
            continue;
          }
          have_iv = TRUE;
        } else if (g_str_equal (a, "METHOD")) {
          if (!g_str_equal (v, "AES-128")) {
            GST_WARNING ("Encryption method %s not supported", v);
            continue;
          }
        }
      }
    } else if (g_str_has_prefix (data, "#EXTINF:")) {
      gdouble fval;
      if (!double_from_string (data + 8, &data, &fval)) {
        GST_WARNING ("Can't read EXTINF duration");
        goto next_line;
      }
      duration = fval * (gdouble) GST_SECOND;
      if (duration > self->targetduration)
        GST_WARNING ("EXTINF duration > TARGETDURATION");
      if (!data || *data != ',')
        goto next_line;
      data = g_utf8_next_char (data);
      if (data != end) {
        g_free (title);
        title = g_strdup (data);
      }
    } else if (g_str_has_prefix (data, "#EXT-X-BYTERANGE:")) {
      gchar *v = data + 17;

      if (int64_from_string (v, &v, &size)) {
        if (*v == '@' && !int64_from_string (v + 1, &v, &offset))
          goto next_line;
      } else {
        goto next_line;
      }
    } else {
      GST_LOG ("Ignored line: %s", data);
    }

  next_line:
    if (!end)
      break;
    data = g_utf8_next_char (end);      /* skip \n */
  }

  /* redorder playlists by bitrate */
  if (self->lists) {
    gchar *top_variant_uri = NULL;

    if (!self->current_variant)
      top_variant_uri = SKIPPY_M3U8 (self->lists->data)->uri;
    else
      top_variant_uri = SKIPPY_M3U8 (self->current_variant->data)->uri;

    self->lists =
        g_list_sort (self->lists,
        (GCompareFunc) skippy_m3u8_compare_playlist_by_bitrate);

    self->current_variant = g_list_find_custom (self->lists, top_variant_uri,
        (GCompareFunc) _m3u8_compare_uri);
  }

  return TRUE;
}

SkippyM3U8Client *
skippy_m3u8_client_new (const gchar * uri)
{
  g_once (&init_once, skippy_m3u8_init_once, NULL);

  SkippyM3U8Client *client;

  g_return_val_if_fail (uri != NULL, NULL);

  client = g_new0 (SkippyM3U8Client, 1);
  client->main = skippy_m3u8_new ();
  client->current = NULL;
  client->sequence = -1;
  client->sequence_position = 0;
  g_mutex_init (&client->lock);
  skippy_m3u8_set_uri (client->main, g_strdup (uri));

  return client;
}

void
skippy_m3u8_client_free (SkippyM3U8Client * self)
{
  g_return_if_fail (self != NULL);

  skippy_m3u8_free (self->main);
  g_mutex_clear (&self->lock);
  g_free (self);
}

void
skippy_m3u8_client_set_current (SkippyM3U8Client * self, SkippyM3U8 * m3u8)
{
  g_return_if_fail (self != NULL);

  SKIPPY_M3U8_CLIENT_LOCK (self);
  if (m3u8 != self->current) {
    self->current = m3u8;
  }
  SKIPPY_M3U8_CLIENT_UNLOCK (self);
}

gboolean
skippy_m3u8_client_update (SkippyM3U8Client * self, gchar * data)
{
  SkippyM3U8 *m3u8;
  gboolean updated = FALSE;
  gboolean ret = FALSE;

  g_return_val_if_fail (self != NULL, FALSE);

  SKIPPY_M3U8_CLIENT_LOCK (self);
  m3u8 = self->current ? self->current : self->main;

  if (!skippy_m3u8_update (m3u8, data, &updated))
    goto out;

  if (!updated) {
    goto out;
  }

  if (self->current && !self->current->files) {
    GST_ERROR ("Invalid media playlist, it does not contain any media files");
    goto out;
  }

  /* select the first playlist, for now */
  if (!self->current) {
    if (self->main->lists) {
      self->current = self->main->current_variant->data;
    } else {
      self->current = self->main;
    }
  }

  if (m3u8->files && self->sequence == -1) {
    self->sequence =
        SKIPPY_M3U8_MEDIA_FILE (g_list_first (m3u8->files)->data)->sequence;
    self->sequence_position = 0;
    GST_DEBUG ("Setting first sequence at %d", self->sequence);
  }

  ret = TRUE;
out:
  SKIPPY_M3U8_CLIENT_UNLOCK (self);
  return ret;
}

static gint
_find_current (SkippyM3U8MediaFile * file, SkippyM3U8Client * client)
{
  return file->sequence == client->sequence;
}

static gboolean
_find_next (SkippyM3U8MediaFile * file, SkippyM3U8Client * client)
{
  GST_DEBUG ("Found fragment %d", file->sequence);
  if (file->sequence >= client->sequence)
    return FALSE;
  return TRUE;
}

SkippyFragment*
skippy_m3u8_client_get_next_fragment (SkippyM3U8Client * client)
{
  GList *l;
  SkippyM3U8MediaFile *file;
  SkippyFragment* fragment;

  g_return_val_if_fail (client != NULL, FALSE);
  g_return_val_if_fail (client->current != NULL, FALSE);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  GST_DEBUG ("Looking for fragment %d", client->sequence);
  l = g_list_find_custom (client->current->files, client,
      (GCompareFunc) _find_next);
  if (l == NULL) {
    SKIPPY_M3U8_CLIENT_UNLOCK (client);
    return NULL;
  }

  file = SKIPPY_M3U8_MEDIA_FILE (l->data);
  GST_DEBUG ("Got fragment with sequence %u (client sequence %u)",
      file->sequence, client->sequence);

  fragment = skippy_fragment_new (file->uri, file->key, file->iv);
  fragment->start_time = client->sequence_position;
  fragment->stop_time = client->sequence_position + file->duration;
  fragment->duration = file->duration;
  fragment->discontinuous = client->sequence != file->sequence || file->discont;
  fragment->range_start = file->offset;
  fragment->range_end = file->size != -1 ? file->offset + file->size - 1 : -1;

  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return fragment;
}

void
skippy_m3u8_client_advance_fragment (SkippyM3U8Client * client)
{
  GList *l;
  SkippyM3U8MediaFile *file;

  g_return_if_fail (client != NULL);
  g_return_if_fail (client->current != NULL);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  GST_DEBUG ("Looking for fragment %d", client->sequence);
  l = g_list_find_custom (client->current->files, client,
      (GCompareFunc) _find_next);
  if (l == NULL) {
    GST_ERROR ("Could not find current fragment");
    SKIPPY_M3U8_CLIENT_UNLOCK (client);
    return;
  }

  file = SKIPPY_M3U8_MEDIA_FILE (l->data);
  GST_DEBUG ("Advancing from sequence %u", file->sequence);
  client->sequence = file->sequence + 1;
  client->sequence_position += file->duration;
  SKIPPY_M3U8_CLIENT_UNLOCK (client);
}

static void
_sum_duration (SkippyM3U8MediaFile * self, GstClockTime * duration)
{
  *duration += self->duration;
}

GstClockTime
skippy_m3u8_client_get_duration (SkippyM3U8Client * client)
{
  GstClockTime duration = 0;

  g_return_val_if_fail (client != NULL, GST_CLOCK_TIME_NONE);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  /* We can only get the duration for on-demand streams */
  if (!client->current || !client->current->endlist) {
    GST_WARNING ("Can't determine playlist duration");
    SKIPPY_M3U8_CLIENT_UNLOCK (client);
    return GST_CLOCK_TIME_NONE;
  }

  if (client->current->files)
    g_list_foreach (client->current->files, (GFunc) _sum_duration, &duration);

  GST_DEBUG ("Determined M3U8 duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));

  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return duration;
}

GstClockTime
skippy_m3u8_client_get_target_duration (SkippyM3U8Client * client)
{
  GstClockTime duration = 0;

  g_return_val_if_fail (client != NULL, GST_CLOCK_TIME_NONE);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  duration = client->current->targetduration;
  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return duration;
}

const gchar *
skippy_m3u8_client_get_uri (SkippyM3U8Client * client)
{
  const gchar *uri;

  g_return_val_if_fail (client != NULL, NULL);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  uri = client->main->uri;
  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return uri;
}

const gchar *
skippy_m3u8_client_get_current_uri (SkippyM3U8Client * client)
{
  const gchar *uri;

  g_return_val_if_fail (client != NULL, NULL);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  uri = client->current->uri;
  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return uri;
}

gboolean
skippy_m3u8_client_has_variant_playlist (SkippyM3U8Client * client)
{
  gboolean ret;

  g_return_val_if_fail (client != NULL, FALSE);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  ret = (client->main->lists != NULL);
  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return ret;
}

gboolean
skippy_m3u8_client_is_live (SkippyM3U8Client * client)
{
  gboolean ret;

  g_return_val_if_fail (client != NULL, FALSE);

  SKIPPY_M3U8_CLIENT_LOCK (client);
  if (!client->current || client->current->endlist)
    ret = FALSE;
  else
    ret = TRUE;
  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return ret;
}

SkippyM3U8 *
skippy_m3u8_client_get_playlist_for_bitrate (SkippyM3U8Client * client, guint bitrate)
{
  GList *list, *current_variant;

  SKIPPY_M3U8_CLIENT_LOCK (client);
  current_variant = client->main->current_variant;

  /*  Go to the highest possible bandwidth allowed */
  while (SKIPPY_M3U8 (current_variant->data)->bandwidth <= bitrate) {
    list = g_list_next (current_variant);
    if (!list)
      break;
    current_variant = list;
  }

  while (SKIPPY_M3U8 (current_variant->data)->bandwidth > bitrate) {
    list = g_list_previous (current_variant);
    if (!list)
      break;
    current_variant = list;
  }
  SKIPPY_M3U8_CLIENT_UNLOCK (client);

  return SKIPPY_M3U8 (current_variant->data);
}

SkippyM3U8 *
skippy_m3u8_client_get_current_variant (SkippyM3U8Client * self)
{
  GList* variant;
  SKIPPY_M3U8_CLIENT_LOCK (self);
  variant = self->main->current_variant->data;
  SKIPPY_M3U8_CLIENT_UNLOCK (self);

  return SKIPPY_M3U8 (variant->data);
}

gchar *
uri_join (const gchar * uri1, const gchar * uri2)
{
  gchar *uri_copy, *tmp, *ret = NULL;

  if (gst_uri_is_valid (uri2))
    return g_strdup (uri2);

  uri_copy = g_strdup (uri1);
  if (uri2[0] != '/') {
    /* uri2 is a relative uri2 */
    /* look for query params */
    tmp = g_utf8_strchr (uri_copy, -1, '?');
    if (tmp) {
      /* find last / char, ignoring query params */
      tmp = g_utf8_strrchr (uri_copy, tmp - uri_copy, '/');
    } else {
      /* find last / char in URL */
      tmp = g_utf8_strrchr (uri_copy, -1, '/');
    }
    if (!tmp) {
      GST_WARNING ("Can't build a valid uri_copy");
      goto out;
    }

    *tmp = '\0';
    ret = g_strdup_printf ("%s/%s", uri_copy, uri2);
  } else {
    /* uri2 is an absolute uri2 */
    char *scheme, *hostname;

    scheme = uri_copy;
    /* find the : in <scheme>:// */
    tmp = g_utf8_strchr (uri_copy, -1, ':');
    if (!tmp) {
      GST_WARNING ("Can't build a valid uri_copy");
      goto out;
    }

    *tmp = '\0';

    /* skip :// */
    hostname = tmp + 3;

    tmp = g_utf8_strchr (hostname, -1, '/');
    if (tmp)
      *tmp = '\0';

    ret = g_strdup_printf ("%s://%s%s", scheme, hostname, uri2);
  }

out:
  g_free (uri_copy);
  return ret;
}

guint64
skippy_m3u8_client_get_current_fragment_duration (SkippyM3U8Client * client)
{
  guint64 dur;
  GList *list;

  g_return_val_if_fail (client != NULL, 0);

  SKIPPY_M3U8_CLIENT_LOCK (client);

  list = g_list_find_custom (client->current->files, client,
      (GCompareFunc) _find_current);
  if (list == NULL) {
    dur = -1;
  } else {
    dur = SKIPPY_M3U8_MEDIA_FILE (list->data)->duration;
  }

  SKIPPY_M3U8_CLIENT_UNLOCK (client);
  return dur;
}

void
skippy_m3u8_client_update_playlist_position (SkippyM3U8Client * client, guint64 target_pos, gboolean* need_segment)
{
  GstClockTime current_pos;
  guint sequence, last_sequence = 0;
  GList *walk;
  SkippyM3U8MediaFile *file;

  /* If it's a live source, do not let the sequence number go beyond
   * three fragments before the end of the list */
  if (client->current && skippy_m3u8_client_is_live (client)) {
    SKIPPY_M3U8_CLIENT_LOCK (client);
    last_sequence =
        SKIPPY_M3U8_MEDIA_FILE (g_list_last (client->current->files)->
        data)->sequence;

    if (client->sequence >= last_sequence - 3) {
      GST_DEBUG_OBJECT (client, "Sequence is beyond playlist. Moving back to %d",
          last_sequence - 3);
      *need_segment = TRUE;
      client->sequence = last_sequence - 3;
    }
    SKIPPY_M3U8_CLIENT_UNLOCK (client);
  } else if (client->current && !skippy_m3u8_client_is_live (client)) {
    /* Sequence numbers are not guaranteed to be the same in different
     * playlists, so get the correct fragment here based on the current
     * position
     */
    SKIPPY_M3U8_CLIENT_LOCK (client);
    current_pos = 0;
    for (walk = client->current->files; walk; walk = walk->next) {
      file = walk->data;
      sequence = file->sequence;
      if (current_pos <= target_pos
          && target_pos < current_pos + file->duration) {
        break;
      }
      current_pos += file->duration;
    }
    /* End of playlist */
    if (!walk)
      sequence++;
    client->sequence = sequence;
    client->sequence_position = current_pos;
    SKIPPY_M3U8_CLIENT_UNLOCK (client);
  }
}

static gchar *
buf_to_utf8_playlist (GstBuffer * buf)
{
  GstMapInfo info;
  gchar *playlist;

  if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
    return NULL;
  }

  if (!g_utf8_validate ((gchar *) info.data, info.size, NULL)) {
    gst_buffer_unmap (buf, &info);
    return NULL;
  }

  /* alloc size + 1 to end with a null character */
  playlist = g_malloc0 (info.size + 1);
  memcpy (playlist, info.data, info.size);

  gst_buffer_unmap (buf, &info);
  return playlist;
}

gboolean
skippy_m3u8_client_load_playlist (SkippyM3U8Client * client, GstBuffer* playlist_buffer)
{
  gchar* playlist = buf_to_utf8_playlist (playlist_buffer);
  if (playlist == NULL) {
    GST_WARNING ("Error converting playlist from raw buffer to UTF8");
  } else if (!skippy_m3u8_client_update (client, playlist)) {
    return FALSE;
  }
  return TRUE;
}

