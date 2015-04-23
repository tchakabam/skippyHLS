/* skippyHLS
 *
 * Copyright (C) 2015, SoundCloud Ltd. (http://soundcloud.com)
 *  Author: Stephan Hesse <stephan@soundcloud.com>, SoundCloud Ltd.
 *
 * skippy_fragment.c:
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

#include <string.h>

#include <nettle/aes.h>
#include <nettle/cbc.h>

#include <glib.h>
#include <gst/base/gsttypefindhelper.h>
#include <gst/base/gstadapter.h>

#include <skippyHLS/skippy_fragment.h>
#include <skippyHLS/skippy_uridownloader.h>

#define SKIPPY_FRAGMENT_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TYPE_SKIPPY_FRAGMENT, SkippyFragmentPrivate))

GST_DEBUG_CATEGORY_STATIC (skippy_fragment_debug);
#define GST_CAT_DEFAULT skippy_fragment_debug

struct _SkippyFragmentPrivate
{
  GstBuffer *buffer;
  GstCaps *caps;
  GMutex lock;
};

G_DEFINE_TYPE (SkippyFragment, skippy_fragment, G_TYPE_OBJECT);

static void skippy_fragment_dispose (GObject * object);
static void skippy_fragment_finalize (GObject * object);

static void
skippy_fragment_class_init (SkippyFragmentClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (SkippyFragmentPrivate));

  gobject_class->dispose = skippy_fragment_dispose;
  gobject_class->finalize = skippy_fragment_finalize;

  GST_DEBUG_CATEGORY_INIT (skippy_fragment_debug, "skippyhls-fragment", 0,
      "HLS fragment");
}

static void
skippy_fragment_init (SkippyFragment * fragment)
{
  SkippyFragmentPrivate *priv;

  fragment->priv = priv = SKIPPY_FRAGMENT_GET_PRIVATE (fragment);

  g_mutex_init (&fragment->priv->lock);
  priv->buffer = NULL;

  fragment->download_start_time = gst_util_get_timestamp ();
  fragment->start_time = 0;
  fragment->stop_time = 0;
  fragment->duration = 0;
  fragment->index = 0;
  fragment->range_start = 0;
  fragment->range_end = -1;
  fragment->completed = FALSE;
  fragment->cancelled = FALSE;
  fragment->discontinuous = FALSE;
  fragment->decrypted = TRUE;
}

SkippyFragment *
skippy_fragment_new (const gchar* uri, gchar* key_uri, guint8* iv)
{
  SkippyFragment* fragment;

  g_return_val_if_fail (uri, NULL);
  g_return_val_if_fail (!key_uri || key_uri && iv, NULL);

  fragment = SKIPPY_FRAGMENT (g_object_new (TYPE_SKIPPY_FRAGMENT, NULL));
  fragment->uri = g_strdup (uri);
  if (key_uri) {
    fragment->decrypted = FALSE;
    fragment->key_uri = g_strdup (key_uri);
    memcpy (fragment->iv, iv, sizeof (fragment->iv));
  }
  return fragment;
}

static void
skippy_fragment_finalize (GObject * gobject)
{
  SkippyFragment *fragment = SKIPPY_FRAGMENT (gobject);

  if (fragment->uri) {
    g_free (fragment->uri);
  }

  if (fragment->key_uri) {
    g_free (fragment->key_uri);
  }

  g_mutex_clear (&fragment->priv->lock);

  G_OBJECT_CLASS (skippy_fragment_parent_class)->finalize (gobject);
}

void
skippy_fragment_dispose (GObject * object)
{
  SkippyFragmentPrivate *priv = SKIPPY_FRAGMENT (object)->priv;

  if (priv->buffer != NULL) {
    gst_buffer_unref (priv->buffer);
    priv->buffer = NULL;
  }

  if (priv->caps != NULL) {
    gst_caps_unref (priv->caps);
    priv->caps = NULL;
  }

  G_OBJECT_CLASS (skippy_fragment_parent_class)->dispose (object);
}

GstBuffer *
skippy_fragment_get_buffer (SkippyFragment * fragment)
{
  g_return_val_if_fail (fragment != NULL, NULL);

  if (!fragment->completed)
    return NULL;
  if (!fragment->priv->buffer)
    return NULL;

  gst_buffer_ref (fragment->priv->buffer);
  return fragment->priv->buffer;
}

gsize skippy_fragment_get_buffer_size (SkippyFragment* fragment)
{
  g_return_val_if_fail (fragment != NULL, 0);

  if (!fragment->priv->buffer)
    return 0;

  return gst_buffer_get_size (fragment->priv->buffer);
}

void
skippy_fragment_set_caps (SkippyFragment * fragment, GstCaps * caps)
{
  g_return_if_fail (fragment != NULL);

  g_mutex_lock (&fragment->priv->lock);
  gst_caps_replace (&fragment->priv->caps, caps);
  g_mutex_unlock (&fragment->priv->lock);
}

GstCaps *
skippy_fragment_get_caps (SkippyFragment * fragment)
{
  g_return_val_if_fail (fragment != NULL, NULL);

  if (!fragment->completed || fragment->priv->buffer == NULL) {
    GST_WARNING ("Can't get caps of incomplete fragment");
    return NULL;
  }

  GST_DEBUG ("Getting fragment caps ...");

  g_mutex_lock (&fragment->priv->lock);
  if (fragment->priv->caps == NULL) {
    guint64 offset, offset_end;

    /* FIXME: This is currently necessary as typefinding only
     * works with 0 offsets... need to find a better way to
     * do that */
    offset = GST_BUFFER_OFFSET (fragment->priv->buffer);
    offset_end = GST_BUFFER_OFFSET_END (fragment->priv->buffer);
    GST_BUFFER_OFFSET (fragment->priv->buffer) = GST_BUFFER_OFFSET_NONE;
    GST_BUFFER_OFFSET_END (fragment->priv->buffer) = GST_BUFFER_OFFSET_NONE;
    fragment->priv->caps =
        gst_type_find_helper_for_buffer (NULL, fragment->priv->buffer, NULL);
    GST_BUFFER_OFFSET (fragment->priv->buffer) = offset;
    GST_BUFFER_OFFSET_END (fragment->priv->buffer) = offset_end;
  }
  gst_caps_ref (fragment->priv->caps);
  g_mutex_unlock (&fragment->priv->lock);

  return fragment->priv->caps;
}

gboolean
skippy_fragment_add_buffer (SkippyFragment * fragment, GstBuffer * buffer)
{
  g_return_val_if_fail (fragment != NULL, FALSE);
  g_return_val_if_fail (buffer != NULL, FALSE);

  g_mutex_lock (&fragment->priv->lock);

  if (fragment->completed) {
    GST_WARNING ("Fragment is completed, could not add more buffers");
    g_mutex_unlock (&fragment->priv->lock);
    return FALSE;
  }

  GST_DEBUG ("Adding new buffer to the fragment");
  /* We steal the buffers you pass in */
  if (fragment->priv->buffer == NULL)
    fragment->priv->buffer = buffer;
  else
    fragment->priv->buffer = gst_buffer_append (fragment->priv->buffer, buffer);

  GST_DEBUG ( "Set buffer pts=%" GST_TIME_FORMAT " duration=%" GST_TIME_FORMAT,
    GST_TIME_ARGS (fragment->start_time), GST_TIME_ARGS (fragment->duration));

  /* Update buffer properties */
  GST_BUFFER_DURATION (fragment->priv->buffer) = fragment->duration;
  GST_BUFFER_PTS (fragment->priv->buffer) = fragment->start_time;
  GST_BUFFER_DTS (fragment->priv->buffer) = GST_CLOCK_TIME_NONE;

  g_mutex_unlock (&fragment->priv->lock);
  return TRUE;
}

void
skippy_fragment_clear_buffer (SkippyFragment * fragment)
{
  g_return_if_fail (fragment != NULL);

  g_mutex_lock (&fragment->priv->lock);
  if (fragment->priv->buffer)
    gst_buffer_unref (fragment->priv->buffer);
  fragment->priv->buffer = NULL;
  g_mutex_unlock (&fragment->priv->lock);
}

void
skippy_fragment_complete (SkippyFragment * fragment, struct SkippyUriDownloader* downloader)
{
  g_return_if_fail (fragment != NULL);
  g_return_if_fail (downloader != NULL);

  g_mutex_lock (&fragment->priv->lock);
  GST_DEBUG ("Completing fragment");
  fragment->completed = TRUE;
  fragment->download_stop_time = gst_util_get_timestamp ();
  if (!fragment->decrypted) {
    g_mutex_unlock (&fragment->priv->lock);
    skippy_fragment_decrypt (fragment, downloader);
    g_mutex_lock (&fragment->priv->lock);
  }
  g_mutex_unlock (&fragment->priv->lock);
}

/* Decrypt a buffer */
static gboolean
decrypt_buffer (gsize length,
    const guint8 * encrypted_data, guint8 * decrypted_data,
    const guint8 * key_data, const guint8 * iv_data)
{
  struct CBC_CTX (struct aes_ctx, AES_BLOCK_SIZE) aes_ctx;

  if (length % 16 != 0)
    return FALSE;

  aes_set_decrypt_key (&aes_ctx.ctx, 16, key_data);
  CBC_SET_IV (&aes_ctx, iv_data);

  CBC_DECRYPT (&aes_ctx, aes_decrypt, length, decrypted_data, encrypted_data);

  return TRUE;
}

/* Decrypt a fragment */
gboolean
skippy_fragment_decrypt (SkippyFragment * fragment,
  struct SkippyUriDownloader* downloader)
{
  g_return_val_if_fail (fragment != NULL, FALSE);
  g_return_val_if_fail (downloader != NULL, FALSE);

  g_mutex_lock (&fragment->priv->lock);

  GError* err = NULL;
  SkippyFragment* key_fragment;
  GstBuffer *key_buffer, *encrypted_buffer, *decrypted_buffer;
  GstMapInfo key_info, encrypted_info, decrypted_info;
  gsize unpadded_size;

  if (fragment->decrypted) {
    GST_WARNING ("Fragment already decrypted!");
    goto error;
  }

  GST_INFO ("Fetching key %s", fragment->key_uri);
  key_fragment = skippy_fragment_new (fragment->key_uri, NULL, NULL);
  skippy_uri_downloader_fetch_fragment ( (SkippyUriDownloader*) downloader,
    key_fragment,
    NULL,
    FALSE,
    FALSE,
    TRUE,
    &err
  );
  if (!key_fragment->completed) {
    GST_ERROR ("Failed to fetch key from URI: %s (%s)", key_fragment->uri, err->message);
    goto error;
  }

  key_buffer = skippy_fragment_get_buffer (key_fragment);
  encrypted_buffer = skippy_fragment_get_buffer (fragment);
  decrypted_buffer =
      gst_buffer_new_allocate (NULL, gst_buffer_get_size (encrypted_buffer),
      NULL);

  gst_buffer_map (key_buffer, &key_info, GST_MAP_READ);
  gst_buffer_map (encrypted_buffer, &encrypted_info, GST_MAP_READ);
  gst_buffer_map (decrypted_buffer, &decrypted_info, GST_MAP_WRITE);

  if (key_info.size != 16)
    goto decrypt_error;
  if (!decrypt_buffer (encrypted_info.size,
          encrypted_info.data, decrypted_info.data, key_info.data, fragment->iv))
    goto decrypt_error;

  /* Handle pkcs7 unpadding here */
  unpadded_size =
      decrypted_info.size - decrypted_info.data[decrypted_info.size - 1];
  gst_buffer_resize (decrypted_buffer, 0, unpadded_size);

  skippy_fragment_clear_buffer (fragment);
  skippy_fragment_add_buffer (fragment, decrypted_buffer);
  // we ref here because we will unref it later but in fact in this case we keep ownership
  gst_buffer_ref (fragment->priv->buffer);
  fragment->decrypted = TRUE;

decrypt_error:
  GST_ERROR ("Failed to decrypt fragment");
  g_set_error (&err, GST_STREAM_ERROR, GST_STREAM_ERROR_DECRYPT,
      "Failed to decrypt fragment");

  gst_buffer_unmap (decrypted_buffer, &decrypted_info);
  gst_buffer_unmap (encrypted_buffer, &encrypted_info);
  gst_buffer_unmap (key_buffer, &key_info);

  gst_buffer_unref (key_buffer);
  gst_buffer_unref (encrypted_buffer);
  gst_buffer_unref (decrypted_buffer);

error:
  g_object_unref (key_fragment);
  g_mutex_unlock (&fragment->priv->lock);
  return fragment->decrypted;
}