/* skippyHLS
 *
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *  Author: Youness Alaoui <youness.alaoui@collabora.co.uk>, Collabora Ltd.
 *  Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 * Copyright (C) 2015, SoundCloud Ltd. (http://soundcloud.com)
 *  Author: Stephan Hesse <stephan@soundcloud.com>, SoundCloud Ltd.
 *
 * skippy_uridownloader.c:
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

#include "skippyHLS/skippy_fragment.h"
#include "skippyHLS/skippy_uridownloader.h"

#include <glib.h>

#define GST_CAT_DEFAULT uridownloader_debug
GST_DEBUG_CATEGORY (uridownloader_debug);

G_DEFINE_TYPE (SkippyUriDownloader, skippy_uri_downloader, GST_TYPE_BIN);

#define SKIPPY_URI_DOWNLOADER_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    TYPE_SKIPPY_URI_DOWNLOADER, SkippyUriDownloaderPrivate))

struct _SkippyUriDownloaderPrivate
{
  SkippyFragment *fragment;

  GstElement *urisrc, *typefind;
  GstBus *bus;
  GstPad *srcpad;
  GstBuffer *buffer;
  GstSegment segment;
  GError *err;

  GCond cond;
  GMutex download_lock;

  gboolean set_uri;
  gboolean fetching;
  gboolean discont;

  gsize bytes_loaded;
  gsize bytes_total;

  gulong urisrcpad_probe_id;
};

static GstStaticPadTemplate srcpadtemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static void skippy_uri_downloader_finalize (GObject * object);
static void skippy_uri_downloader_dispose (GObject * object);
static void skippy_uri_downloader_reset (SkippyUriDownloader * downloader);
static GstPadProbeReturn skippy_uri_downloader_src_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static GstBusSyncReply skippy_uri_downloader_bus_handler (GstBus * bus, GstMessage * message, gpointer data);
static void skippy_uri_downloader_complete (SkippyUriDownloader * downloader);
static gboolean skippy_uri_downloader_create_src (SkippyUriDownloader * downloader, gchar* uri);

// Define class
static void
skippy_uri_downloader_class_init (SkippyUriDownloaderClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  g_type_class_add_private (klass, sizeof (SkippyUriDownloaderPrivate));

  gobject_class->dispose = skippy_uri_downloader_dispose;
  gobject_class->finalize = skippy_uri_downloader_finalize;

  GST_DEBUG_CATEGORY_INIT (uridownloader_debug, "skippyhls-uridownloader", 0, "URI downloader");
}

// Constructor
SkippyUriDownloader*
skippy_uri_downloader_new ()
{
  SkippyUriDownloader* downloader = g_object_new (TYPE_SKIPPY_URI_DOWNLOADER, NULL);
  return downloader;
}

// GObject init function
static void
skippy_uri_downloader_init (SkippyUriDownloader * downloader)
{
  GstPad* typefindsrcpad;
  GstPadTemplate* templ;

  downloader->priv = SKIPPY_URI_DOWNLOADER_GET_PRIVATE (downloader);

  // set this to NULL explicitely
  downloader->priv->urisrc = NULL;
  downloader->priv->buffer = NULL;
  downloader->priv->bus = NULL;
  downloader->priv->fetching = FALSE;
  downloader->priv->typefind = gst_element_factory_make ("typefind", NULL);

  // Add typefind
  gst_bin_add (GST_BIN(downloader), GST_ELEMENT (downloader->priv->typefind));
  typefindsrcpad = gst_element_get_static_pad (downloader->priv->typefind, "src");
  // Add external source pad as ghost pad to typefind src pad
  templ = gst_static_pad_template_get (&srcpadtemplate);
  downloader->priv->srcpad = gst_ghost_pad_new_from_template ("src", typefindsrcpad, templ);
  // Configure external source pad
  gst_pad_set_active (downloader->priv->srcpad, TRUE);
  // Add pad to element
  gst_element_add_pad (GST_ELEMENT(downloader), downloader->priv->srcpad);
  gst_element_no_more_pads (GST_ELEMENT (downloader));
  gst_object_unref (typefindsrcpad);
  gst_object_unref (templ);

  // Init wait cond for blocking fetch call
  g_cond_init (&downloader->priv->cond);
  g_mutex_init (&downloader->priv->download_lock);

  // Reset segment completely
  gst_segment_init (&downloader->priv->segment, GST_FORMAT_TIME);

  // Reset private state fields
  skippy_uri_downloader_reset (downloader);
}

// Reset object - can not be called concurrently with fetch or getters/setters functions
// Will cancel any ongoing download and block until it's finished (i.e until fetch function has exited)
//
// MT-safe
static void
skippy_uri_downloader_reset (SkippyUriDownloader * downloader)
{
  g_return_if_fail (downloader != NULL);

  // Cancel anything ongoing just in case
  skippy_uri_downloader_cancel (downloader);

  g_mutex_lock (&downloader->priv->download_lock);

  downloader->priv->bytes_loaded = 0;
  downloader->priv->bytes_total = 0;
  downloader->priv->discont = FALSE;
  downloader->priv->set_uri = FALSE;

  // Clear error when present
  g_clear_error (&downloader->priv->err);

  // Unref fragment model
  if (downloader->priv->fragment) {
    g_object_unref (downloader->priv->fragment);
    downloader->priv->fragment = NULL;
  }

  // Reset our own buffer where we'll concatenate all the download into
  if (downloader->priv->buffer) {
    gst_buffer_unref (downloader->priv->buffer);
    downloader->priv->buffer = NULL;
  }

  g_mutex_unlock (&downloader->priv->download_lock);

  GST_TRACE ("Reset done");
}

// Dispose function - frees all resources
static void
skippy_uri_downloader_dispose (GObject * object)
{
  g_return_if_fail (object);

  GST_DEBUG ("Disposing ...");

  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  g_return_if_fail (downloader->priv);

  // Let's reset first (this is flushing the message bus and unref-ing any owned download)
  skippy_uri_downloader_reset (downloader);

  // Get rid of private bus and buffer
  if (downloader->priv->bus) {
    gst_object_unref (downloader->priv->bus);
    downloader->priv->bus = NULL;
  }

  // Put element explicitely to NULL state
  if (downloader->priv->urisrc) {
    gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
  }

  // Dispose base class
  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->dispose (object);

  GST_DEBUG ("Done cleaning up.");
}

// Finalize, frees cond and mutex
static void
skippy_uri_downloader_finalize (GObject * object)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  g_cond_clear (&downloader->priv->cond);
  g_mutex_clear (&downloader->priv->download_lock);

  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->finalize (object);
}

// Prepare function - sets up an appropriate source before starting the first download
//
// MT-safe
void
skippy_uri_downloader_prepare (SkippyUriDownloader * downloader, gchar* uri)
{
  g_mutex_lock (&downloader->priv->download_lock);
  skippy_uri_downloader_create_src (downloader, uri);
  g_mutex_unlock (&downloader->priv->download_lock);
}

// Getter for buffer - can not be called concurrently with fetch & prepare
//
// MT-safe
GstBuffer* skippy_uri_downloader_get_buffer (SkippyUriDownloader *downloader)
{
  GstBuffer* buf;
  g_mutex_lock (&downloader->priv->download_lock);
  buf = gst_buffer_ref(downloader->priv->buffer);
  g_mutex_unlock (&downloader->priv->download_lock);
  return buf;
}

// Setter for segment - can not be called concurrently with fetch & prepare
//
// MT-safe
void skippy_uri_downloader_set_segment (SkippyUriDownloader *downloader, GstSegment segment)
{
  g_mutex_lock (&downloader->priv->download_lock);
  downloader->priv->segment = segment;
  g_mutex_unlock (&downloader->priv->download_lock);
}

// Getter for segment - can not be called concurrently with fetch & prepare
//
// MT-safe
GstSegment skippy_uri_downloader_get_segment (SkippyUriDownloader *downloader)
{
  GstSegment segment;
  g_mutex_lock (&downloader->priv->download_lock);
  segment = downloader->priv->segment;
  g_mutex_unlock (&downloader->priv->download_lock);
  return segment;
}

// Handles received bytes: Called by URL source element streaming thread
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_handle_bytes_received (SkippyUriDownloader* downloader,
  guint64 start_time, guint64 stop_time,
  gsize bytes_loaded, gsize bytes_total)
{
  GstStructure* s;
  float percentage = 100.0f * bytes_loaded / bytes_total;

  GST_TRACE ("Loaded %ld bytes of %ld -> %f percent of media interval %f to %f seconds",
    (long int) bytes_loaded,
    (long int) bytes_total,
    percentage,
    ((float) (start_time)) / GST_SECOND,
    ((float) (stop_time)) / GST_SECOND
  );

  // Post downloading message on the bus
  s = gst_structure_new (SKIPPY_HLS_DEMUX_DOWNLOADING_MSG_NAME,
    "fragment-start-time", G_TYPE_UINT64, start_time,
    "fragment-stop-time", G_TYPE_UINT64, stop_time,
    "loaded-bytes", G_TYPE_UINT64, (guint64) bytes_loaded,
    "total-bytes", G_TYPE_UINT64, (guint64) bytes_total,
    NULL
  );

  gst_element_post_message (GST_ELEMENT (downloader),
    gst_message_ref (gst_message_new_element (GST_OBJECT(downloader), s))
  );
}

// Handles an EOS from the data source thread. Aquires the object lock and signals the condition to unblock fetch function.
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_handle_eos (SkippyUriDownloader* downloader)
{
  GST_DEBUG_OBJECT (downloader, "Got EOS on the data source");
  if (downloader->priv->err) {
    GST_WARNING ("Got EOS but error: %s", downloader->priv->err->message);
  } else {

    downloader->priv->fragment->download_stop_time = gst_util_get_timestamp ();

    // Updating current data segment properties
    downloader->priv->segment.position += downloader->priv->fragment->duration;
    // FIXME: seems when data comes from filesystem caches we can get less data than the segment advertises (encryption padding?)
    // Make sure we send a 100% callback and have a valid byte number
    if (downloader->priv->bytes_loaded != downloader->priv->bytes_total) {
      downloader->priv->bytes_loaded = downloader->priv->bytes_total;
      skippy_uri_downloader_handle_bytes_received (downloader,
        downloader->priv->fragment->start_time, downloader->priv->fragment->stop_time,
        downloader->priv->bytes_loaded, downloader->priv->bytes_total
      );
    }

    skippy_uri_downloader_complete (downloader);
  }
}

// Handles segment events from URI data source thread
// Download mutex is locked when this is called (only while fetch executes).
static gboolean
skippy_uri_downloader_handle_data_segment (SkippyUriDownloader* downloader, GstSegment* segment)
{
  if (segment->format == GST_FORMAT_BYTES) {

    GST_DEBUG ("Handling data segment for fragment at %ld - %ld ms",
      (long int) downloader->priv->fragment->start_time / GST_MSECOND,
      (long int) downloader->priv->fragment->stop_time / GST_MSECOND
    );

    downloader->priv->bytes_loaded = segment->position;
    downloader->priv->bytes_total = segment->duration;

  } else {
    GST_WARNING ("Data segment event does not have bytes format!");
    return FALSE;
  }
  return TRUE;
}

// Handles errors from message bus sync handler of URI src (runs in it's streaming thread)
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_handle_error (SkippyUriDownloader *downloader,
  GstMessage* message)
{
  GError *err = NULL;

  // Set current error if not yet set (if there are several recurrent errors we will only store the first one)
  if (downloader->priv->err) {
    return;
  }
  gst_message_parse_error (message, &err, NULL);
  downloader->priv->err = err;

  // Log error
  GST_INFO_OBJECT (downloader,
      "URI source error: '%s' from %s, the download will be cancelled",
      err->message, GST_OBJECT_NAME (message->src));

  // Cancel ongoing download
  skippy_uri_downloader_cancel (downloader);
}

// Handles warnings from message bus sync handler of URI src (runs in it's streaming thread)
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_handle_warning (SkippyUriDownloader *downloader,
  GstMessage* message)
{
  GError *err = NULL;
  gchar *dbg_info = NULL;

  gst_message_parse_warning (message, &err, &dbg_info);
  GST_WARNING_OBJECT (downloader,
      "Received warning: %s from %s",
      GST_OBJECT_NAME (message->src), err->message);
  GST_DEBUG ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
  g_error_free (err);
  g_free (dbg_info);
}

// Bus sync handler. Called from URI src streaming thread.
// Download mutex is locked when this is called (only while fetch executes).
static GstBusSyncReply
skippy_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data)
{
  SkippyUriDownloader *downloader = (SkippyUriDownloader *) (data);

  //GST_DEBUG ("Got %" GST_PTR_FORMAT, message);

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
    skippy_uri_downloader_handle_error (downloader,
                                        message);

  } else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_WARNING) {
    skippy_uri_downloader_handle_warning (downloader,
                                        message);

  } else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ELEMENT) {
    gst_element_post_message (GST_ELEMENT(downloader), gst_message_ref(message));
  }
  // Drop the message
  gst_message_unref (message);
  return GST_BUS_DROP;
}

// Probe buffers from URI src streaming thread
// Download mutex is locked when this is called (only while fetch executes).
static GstPadProbeReturn
skippy_uri_downloader_src_probe_buffer (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  gsize bytes;
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;

  GST_DEBUG ("Got %" GST_PTR_FORMAT, buf);

  /* NOTE: HTTP errors (404, 500, etc...) are also pushed through this pad as
   * response but the source element will also post a warning or error message
   * in the bus, which is handled synchronously cancelling the download.
   */

  // There was an error downloading, quit quietly
  if (downloader->priv->err) {
    GST_DEBUG ("Detected error, dropping item");
    return GST_PAD_PROBE_DROP;
  }

  // Get size of buffer
  bytes = gst_buffer_get_size (buf);

  GST_TRACE ("The uri fetcher received a new buffer of size %" G_GSIZE_FORMAT, bytes);

  // Increment size on fragment model
  downloader->priv->fragment->size += bytes;
  // Count bytes and trigger callback
  downloader->priv->bytes_loaded += bytes;

  skippy_uri_downloader_handle_bytes_received (downloader,
    downloader->priv->fragment->start_time, downloader->priv->fragment->stop_time,
    downloader->priv->bytes_loaded, downloader->priv->bytes_total);

  if (!downloader->priv->discont) {
    GST_DEBUG ("Marking buffer as discontinuous");
    GST_BUFFER_PTS (buf) = downloader->priv->segment.position;
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    // We should do this only once per downloaded fragment
    downloader->priv->discont = TRUE;
  } else {
    GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  if (!gst_pad_is_linked (downloader->priv->srcpad)) {
    // Copy and append buffer to download aggregate
    if (downloader->priv->buffer == NULL) {
      downloader->priv->buffer = gst_buffer_new ();
    }
    // Copy the buffer into the internal storage
    downloader->priv->buffer = gst_buffer_append (downloader->priv->buffer, gst_buffer_copy(buf));
    // Drop this buffer (this will return FLOW_OK to internal src)
    ret = GST_PAD_PROBE_DROP;
  }

  return ret;
}

// Probe events from URI src streaming thread
// Download mutex is locked when this is called (only while fetch executes).
static GstPadProbeReturn
skippy_uri_downloader_src_probe_event (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (user_data);
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  GstSegment bytes_segment;
  GstEvent* segment_event;

  GST_DEBUG ("Got %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_SEGMENT:
    // Check for current fragment download and replace event data if possible
    // Copy segment event from URI src
    gst_event_copy_segment (event, &bytes_segment);
    // Create new segment event from our own segment (time format)
    segment_event = gst_event_new_segment (&downloader->priv->segment);
    gst_event_set_seqnum (segment_event, gst_event_get_seqnum(event));
    // Replace data and drop old event
    GST_PAD_PROBE_INFO_DATA(info) = segment_event;
    GST_DEBUG ("Replaced by %" GST_PTR_FORMAT, segment_event);
    gst_event_unref(event);
    // Update bytes counter
    skippy_uri_downloader_handle_data_segment (downloader, &bytes_segment);
    return GST_PAD_PROBE_OK;
  case GST_EVENT_EOS:
    skippy_uri_downloader_handle_eos (downloader);
    return GST_PAD_PROBE_DROP;
  default:
    return GST_PAD_PROBE_OK;
  }
}

// Probe URI src streaming thread
// Download mutex is locked when this is called (only while fetch executes).
static GstPadProbeReturn
skippy_uri_downloader_src_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  if (GST_IS_BUFFER (info->data)) {
    return skippy_uri_downloader_src_probe_buffer (pad, info, user_data);
  } else if (GST_IS_EVENT (info->data)) {
    return skippy_uri_downloader_src_probe_event (pad, info, user_data);
  }

  return GST_PAD_PROBE_OK;
}

// Creates source element and pad: Only called when download mutex locked by fetch and prepare
// (can not be called concurrently with getters/setters). Download is not yet running in both cases
// so private fields can be accessed safely.
//
// @return TRUE when we did create a source or one was present, FALSE in case of error (no source)
static gboolean
skippy_uri_downloader_create_src (SkippyUriDownloader * downloader, gchar* uri)
{
  GError* err = NULL;
  GstPad *urisrcpad;

  if (downloader->priv->urisrc) {
    goto relink;
  }

  // Create HTTP src
  downloader->priv->urisrc = gst_element_make_from_uri (GST_URI_SRC, uri, NULL, &err);
  if (err) {
    GST_ERROR ("Could not create HTTP source: %s", err->message);
    g_clear_error (&err);
    return FALSE;
  }
  /* Create a bus to handle error and warning message from the source element */
  downloader->priv->bus = gst_bus_new ();

  // Add URI source element to bin
  GST_DEBUG ("Added source: %s", GST_ELEMENT_NAME (downloader->priv->urisrc));

  gst_bin_add (GST_BIN(downloader), GST_ELEMENT (downloader->priv->urisrc));

  // We want to lock the state of the URI src to manage stuff ourselves
  gst_element_set_locked_state (GST_ELEMENT (downloader->priv->urisrc), TRUE);
  gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
  // add a sync handler for the bus messages to detect errors in the download
  gst_element_set_bus (GST_ELEMENT (downloader->priv->urisrc), downloader->priv->bus);
  gst_bus_set_sync_handler (downloader->priv->bus, skippy_uri_downloader_bus_handler, downloader, NULL);

relink:
  if (!downloader->priv->set_uri) {
    // Link URI src with typefind
    gst_element_link (downloader->priv->urisrc, downloader->priv->typefind);
    // Get static src pad of URI src
    urisrcpad = gst_element_get_static_pad (downloader->priv->urisrc, "src");
    // Add probe to URI src pad
    downloader->priv->urisrcpad_probe_id = gst_pad_add_probe (urisrcpad, GST_PAD_PROBE_TYPE_ALL_BOTH,
      skippy_uri_downloader_src_probe, downloader, NULL);
    gst_object_unref (urisrcpad);
    downloader->priv->set_uri = TRUE;
  }
  return TRUE;
}

// Set URI src range
// Download mutex is locked when this is called (only while fetch executes).
static gboolean
skippy_uri_downloader_set_range (SkippyUriDownloader * downloader,
    gint64 range_start, gint64 range_end)
{
  GstEvent *seek;

  g_return_val_if_fail (range_start >= 0, FALSE);
  g_return_val_if_fail (range_end >= -1, FALSE);

  if (range_start || (range_end >= 0)) {
    GST_DEBUG_OBJECT (downloader, "Setting range to %d - %d (Creating seek event on URI src)", (int) range_start, (int) range_end);
    seek = gst_event_new_seek (1.0, GST_FORMAT_BYTES, GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET, range_start, GST_SEEK_TYPE_SET, range_end);
    return gst_element_send_event (downloader->priv->urisrc, seek);
  }
  return TRUE;
}

// Setup URI source
// Download mutex is locked when this is called (only while fetch executes).
static gboolean
skippy_uri_downloader_set_uri (SkippyUriDownloader * downloader, const gchar * uri,
    const gchar * referer, gboolean compress, gboolean refresh,
    gboolean allow_cache)
{
  GError* err = NULL;
  GObjectClass *klass = G_OBJECT_GET_CLASS (downloader->priv->urisrc);

  // Validate the URI
  if (!gst_uri_is_valid (uri)) {
    GST_WARNING ("URI is invalid");
    return FALSE;
  }

  GST_DEBUG_OBJECT (downloader, "Setting URI: %s", uri);

  // Set URI
  if (!gst_uri_handler_set_uri (GST_URI_HANDLER (downloader->priv->urisrc), uri, &err)) {
    GST_ERROR_OBJECT (downloader, "Failed set URI on source element for URI: %s", err->message);
    g_clear_error (&err);
    return FALSE;
  }

  GST_DEBUG ("URI has been applied to handler interface, configuring data source now");

  // Configure source element accordingly
  if (g_object_class_find_property (klass, "compress"))
    g_object_set (downloader->priv->urisrc, "compress", compress, NULL);
  if (g_object_class_find_property (klass, "keep-alive"))
    g_object_set (downloader->priv->urisrc, "keep-alive", TRUE, NULL);
  if (g_object_class_find_property (klass, "extra-headers")) {
    if (referer || refresh || !allow_cache) {
      GstStructure *extra_headers = gst_structure_new_empty ("headers");
      if (referer) {
        gst_structure_set (extra_headers, "Referer", G_TYPE_STRING, referer,
            NULL);
      }
      if (!allow_cache) {
        gst_structure_set (extra_headers, "Cache-Control", G_TYPE_STRING,
            "no-cache", NULL);
      } else if (refresh) {
        gst_structure_set (extra_headers, "Cache-Control", G_TYPE_STRING,
            "max-age=0", NULL);
      }
      g_object_set (downloader->priv->urisrc, "extra-headers", extra_headers,
          NULL);
      gst_structure_free (extra_headers);
    } else {
      g_object_set (downloader->priv->urisrc, "extra-headers", NULL, NULL);
    }
  }

  return TRUE;
}

// Unset URI source
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_deinit_uri_src (SkippyUriDownloader * downloader)
{
  GstPad* urisrcpad;
  GST_DEBUG ("Unsetting URI source");
  // Flush bus - this will trigger handlers for all remaining messages on the bus and flush any new one incoming
  gst_bus_set_flushing (downloader->priv->bus, TRUE);
  // Unlink element
  gst_element_unlink (downloader->priv->typefind, downloader->priv->urisrc);
  //  Remove prope
  urisrcpad = gst_element_get_static_pad (downloader->priv->urisrc, "src");
  gst_pad_remove_probe (urisrcpad, downloader->priv->urisrcpad_probe_id);
  gst_object_unref (urisrcpad);
  // Now we can shut down the element
  GST_DEBUG ("Setting source element to READY state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
  gst_element_set_state (downloader->priv->urisrc, GST_STATE_READY);
  gst_bus_set_flushing (downloader->priv->bus, FALSE);
  // In case of error set element to NULL
  if (downloader->priv->err) {
    /* set the element state to NULL if there was an error otherwise go back to READY state */
    GST_DEBUG ("Setting source element to NULL state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
    gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
  }
  // Set state flag that we have unset URI source
  downloader->priv->set_uri = FALSE;
}

// Handle failure downloading in fetch function
// Download mutex is locked when this is called (only while fetch executes).
static SkippyUriDownloaderFetchReturn
skippy_uri_downloader_handle_failure (SkippyUriDownloader * downloader, GError ** err)
{
  // Only deinit data source if it is still set up
  if (downloader->priv->set_uri) {
    skippy_uri_downloader_deinit_uri_src (downloader);
  }
  // Check for error from internal bus
  if (downloader->priv->err) {
    // Copy error but our own one for internal processing
    GST_ERROR_OBJECT (downloader, "Error fetching URI: %s", downloader->priv->err->message);
    *err = g_error_copy (downloader->priv->err);
  }
  return SKIPPY_URI_DOWNLOADER_FAILED;
}

// Fetch function: can not be called concurrently with setters&getters or prepare function
// Blocks until download is finished
//
// MT-safe
SkippyUriDownloaderFetchReturn skippy_uri_downloader_fetch_fragment (SkippyUriDownloader * downloader, SkippyFragment* fragment,
  const gchar * referer, gboolean compress, gboolean refresh, gboolean allow_cache, GError ** err)
{
  GstStateChangeReturn ret;

  g_return_val_if_fail (downloader, SKIPPY_URI_DOWNLOADER_FAILED);
  g_return_val_if_fail (fragment, SKIPPY_URI_DOWNLOADER_FAILED);
  g_return_val_if_fail (*err == NULL, SKIPPY_URI_DOWNLOADER_FAILED);

  // Let's first make sure we are completely reset
  skippy_uri_downloader_reset (downloader);

  // Aquire download lock
  g_mutex_lock (&downloader->priv->download_lock);

  // Storing the current fragment info
  downloader->priv->fragment = g_object_ref (fragment);

  // Make sure we have our data source component set up and wired
  if (!skippy_uri_downloader_create_src (downloader, fragment->uri)) {
    g_mutex_unlock (&downloader->priv->download_lock);
    return SKIPPY_URI_DOWNLOADER_FAILED;
  }

  // Setup URL & range
  if (! (skippy_uri_downloader_set_uri (downloader, fragment->uri, referer, compress, refresh, allow_cache)
    && skippy_uri_downloader_set_range (downloader, fragment->range_start, fragment->range_end))) {
    GST_WARNING_OBJECT (downloader, "Failed to set URL or byte-range on data source");
    g_mutex_unlock (&downloader->priv->download_lock);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  // Let data flow ...
  ret = gst_element_set_state (downloader->priv->urisrc, GST_STATE_PLAYING);
  GST_TRACE ("Setting URI data source to PLAYING: %s", gst_element_state_change_return_get_name (ret));
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_mutex_unlock (&downloader->priv->download_lock);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  // From here we expect the streaming thread to call into our event & sync message handlers.
  // This means we have to protect any shared data between our cond wait block and these handlers.
  GST_DEBUG_OBJECT (downloader, "Waiting to fetch the URI %s", fragment->uri);
  // We protect the downloaded fragment metadata we share with the 'cancel' function using the object lock here.
  GST_OBJECT_LOCK (downloader);
  /* wait until:
   *   - the download succeed (EOS in the src pad)
   *   - the download failed (Error message on the fetcher bus)
   */
  while (!(fragment->cancelled || fragment->completed)) {
    // Indicate we are downloading
    downloader->priv->fetching = TRUE;
    g_cond_wait (&downloader->priv->cond, GST_OBJECT_GET_LOCK (downloader));
    GST_DEBUG ("Condition has been signalled");
  }
  downloader->priv->fetching = FALSE;
  GST_OBJECT_UNLOCK (downloader);
  // Now we disconnect everything from the data source
  skippy_uri_downloader_deinit_uri_src  (downloader);
  // After this we are sure the streaming thread of the data source will not push any more data or events
  // and all messages from the URI src element are flushed (in sync with this call)

  GST_DEBUG ("Done fetching");

  // Handle errors
  if (downloader->priv->err) {
    fragment->completed = FALSE;
    g_mutex_unlock (&downloader->priv->download_lock);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }
  // Cancellation
  if (fragment->cancelled || downloader->priv->err) {
    g_mutex_unlock (&downloader->priv->download_lock);
    return SKIPPY_URI_DOWNLOADER_CANCELLED;
  }
  // Success completion
  g_mutex_unlock (&downloader->priv->download_lock);
  return SKIPPY_URI_DOWNLOADER_COMPLETED;
}

static void
skippy_uri_downloader_complete (SkippyUriDownloader * downloader)
{
  GST_OBJECT_LOCK (downloader);
  // We share this with the cond wait
  downloader->priv->fragment->completed = TRUE;
  GST_DEBUG_OBJECT (downloader, "Signaling wait condition");
  g_cond_signal (&downloader->priv->cond);
  GST_OBJECT_UNLOCK (downloader);
}

// Cancel function: will unblock fetch function as quick as possible and mark download as failure
// The fetch and cancel function access the 'fetching' and 'cancelled' fields concurrently but this is protected
// by the object lock here.
void skippy_uri_downloader_cancel (SkippyUriDownloader * downloader)
{
  GST_OBJECT_LOCK (downloader);
  if (downloader->priv->fetching) {
    GST_DEBUG ("Cancelling ongoing download");
    downloader->priv->fragment->cancelled = TRUE;
    g_cond_signal (&downloader->priv->cond);
  } else {
    GST_TRACE ("There is no ongoing download to cancel");
  }
  GST_OBJECT_UNLOCK (downloader);
}
