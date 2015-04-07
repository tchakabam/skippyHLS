/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gstfragment.c:
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

#include <glib.h>
#include <gst/base/gsttypefindhelper.h>
#include <gst/base/gstadapter.h>

#include <skippyhls/skippy_fragment.h>

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

  GST_DEBUG_CATEGORY_INIT (skippy_fragment_debug, "skippyfragment", 0,
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

  if (!fragment->completed) {
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

  g_mutex_unlock (&fragment->priv->lock);
  return TRUE;
}