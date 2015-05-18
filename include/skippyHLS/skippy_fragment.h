/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gstfragment.h:
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

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define TYPE_SKIPPY_FRAGMENT (skippy_fragment_get_type())
#define SKIPPY_FRAGMENT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),TYPE_SKIPPY_FRAGMENT,SkippyFragment))
#define SKIPPY_FRAGMENT_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass),TYPE_SKIPPY_FRAGMENT,SkippyFragmentClass))
#define SKIPPY_IS_FRAGMENT(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),TYPE_SKIPPY_FRAGMENT))
#define SKIPPY_IS_FRAGMENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),TYPE_SKIPPY_FRAGMENT))

typedef struct _SkippyFragment SkippyFragment;
typedef struct _SkippyFragmentPrivate SkippyFragmentPrivate;
typedef struct _SkippyFragmentClass SkippyFragmentClass;

struct SkippyUriDownloader;

struct _SkippyFragment
{
  GObject parent;

  gchar* uri;                    /* URI of the fragment */
  gchar *key_uri;                /* Encryption key */
  guint8 iv[16];                 /* Encryption IV */
  gint64 range_start, range_end; /* Byte range @ URI */
  gboolean completed;            /* Whether the fragment is complete or not */
  gboolean cancelled;            /* Wether the fragment download was cancelled */
  guint64 download_start_time;   /* Epoch time when the download started */
  guint64 download_stop_time;    /* Epoch time when the download finished */
  guint64 start_time;            /* Media start time of the fragment */
  guint64 stop_time;             /* Media stop time of the fragment */
  guint64 duration;              /* Media fragment duration */
  gboolean index;                /* Index of the fragment */
  gboolean discontinuous;        /* Whether this fragment is discontinuous or not */
  gboolean decrypted;
  gsize size;

  SkippyFragmentPrivate *priv;
};

struct _SkippyFragmentClass
{
  GObjectClass parent_class;
};

GType skippy_fragment_get_type (void);
SkippyFragment * skippy_fragment_new (const gchar* uri, gchar* key_uri, guint8* iv);
void skippy_fragment_set_completed (SkippyFragment * fragment);

G_END_DECLS
