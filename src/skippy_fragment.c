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

#define SKIPPY_FRAGMENT_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TYPE_SKIPPY_FRAGMENT, SkippyFragmentPrivate))

GST_DEBUG_CATEGORY_STATIC (skippy_fragment_debug);
#define GST_CAT_DEFAULT skippy_fragment_debug

struct _SkippyFragmentPrivate
{
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
  fragment->size = 0;
}

SkippyFragment *
skippy_fragment_new (const gchar* uri)
{
  SkippyFragment* fragment;

  g_return_val_if_fail (uri, NULL);

  fragment = SKIPPY_FRAGMENT (g_object_new (TYPE_SKIPPY_FRAGMENT, NULL));
  fragment->uri = g_strdup (uri);
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

  G_OBJECT_CLASS (skippy_fragment_parent_class)->dispose (object);
}

void
skippy_fragment_set_completed (SkippyFragment * fragment)
{
  g_return_if_fail (fragment != NULL);

  g_mutex_lock (&fragment->priv->lock);
  GST_DEBUG ("Completing fragment");
  fragment->completed = TRUE;
  fragment->download_stop_time = gst_util_get_timestamp ();
  g_mutex_unlock (&fragment->priv->lock);
}

#if 0
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
  SkippyFragment* key_fragment = NULL;
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
  if (key_fragment) {
    g_object_unref (key_fragment);
  }
  g_mutex_unlock (&fragment->priv->lock);
  return fragment->decrypted;
}

#endif