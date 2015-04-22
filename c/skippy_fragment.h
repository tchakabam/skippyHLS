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

struct _SkippyFragment
{
  GObject parent;

  gchar * name;                 /* Name of the fragment */
  gboolean completed;           /* Whether the fragment is complete or not */
  guint64 download_start_time;  /* Epoch time when the download started */
  guint64 download_stop_time;   /* Epoch time when the download finished */
  guint64 start_time;           /* Start time of the fragment */
  guint64 stop_time;            /* Stop time of the fragment */
  gboolean index;               /* Index of the fragment */
  gboolean discontinuous;       /* Whether this fragment is discontinuous or not */
  gboolean cached;

  SkippyFragmentPrivate *priv;
};

struct _SkippyFragmentClass
{
  GObjectClass parent_class;
};

GType skippy_fragment_get_type (void);

GstBuffer * skippy_fragment_get_buffer (SkippyFragment *fragment);
void skippy_fragment_set_caps (SkippyFragment * fragment, GstCaps * caps);
GstCaps * skippy_fragment_get_caps (SkippyFragment * fragment);
gboolean skippy_fragment_add_buffer (SkippyFragment *fragment, GstBuffer *buffer);
SkippyFragment * skippy_fragment_new (void);

G_END_DECLS
