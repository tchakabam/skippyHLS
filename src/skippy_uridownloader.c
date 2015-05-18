/* skippyHLS
 *
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

 #define SKIPPY_HLS_DEMUX_DOWNLOADING_MSG_NAME "skippy-hlsdemux-download"

#define GST_CAT_DEFAULT uridownloader_debug
GST_DEBUG_CATEGORY (uridownloader_debug);

G_DEFINE_TYPE (SkippyUriDownloader, skippy_uri_downloader, GST_TYPE_BIN);

#define SKIPPY_URI_DOWNLOADER_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    TYPE_SKIPPY_URI_DOWNLOADER, SkippyUriDownloaderPrivate))

struct _SkippyUriDownloaderPrivate
{
  SkippyFragment *fragment;

  GstElement *urisrc;
  GstBus *bus;
  GstPad *srcpad;
  GstBuffer *buffer;

  GError *err;
  GCond cond;

  gboolean fetching;
  gboolean discont;

  gsize bytes_loaded;
  gsize bytes_total;
};

static void skippy_uri_downloader_finalize (GObject * object);
static void skippy_uri_downloader_dispose (GObject * object);

static GstStateChangeReturn
skippy_uri_downloader_change_state (GstElement * element, GstStateChange transition);

static gboolean skippy_uri_downloader_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);

static GstPadProbeReturn
skippy_uri_downloader_src_buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data);

static GstBusSyncReply skippy_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data);

static GstStaticPadTemplate sinkpadtemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srcpadtemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static void
skippy_uri_downloader_class_init (SkippyUriDownloaderClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = (GstElementClass *) klass;

  g_type_class_add_private (klass, sizeof (SkippyUriDownloaderPrivate));

  gobject_class->dispose = skippy_uri_downloader_dispose;
  gobject_class->finalize = skippy_uri_downloader_finalize;

  element_class->change_state = GST_DEBUG_FUNCPTR (skippy_uri_downloader_change_state);

  GST_DEBUG_CATEGORY_INIT (uridownloader_debug, "skippyhls-uridownloader", 0, "URI downloader");
}

static void
skippy_uri_downloader_init (SkippyUriDownloader * downloader)
{
  GError* err = NULL;
  GstPadTemplate* templ = gst_static_pad_template_get (&srcpadtemplate);
  GstPad* urisrcpad;

  downloader->priv = SKIPPY_URI_DOWNLOADER_GET_PRIVATE (downloader);

  // Create HTTP src
  downloader->priv->urisrc = gst_element_make_from_uri (GST_URI_SRC, "http://", NULL, &err);
  if (err) {
    GST_ERROR ("Could not create HTTP source: %s", err->message);
    g_clear_error (&err);
    return;
  }

  // Add URI source element to bin
  GST_DEBUG ("Added source: %s", GST_ELEMENT_NAME (downloader->priv->urisrc));
  gst_bin_add (GST_BIN(downloader), downloader->priv->urisrc);
  // We want to lock the state of the URI src to manage stuff ourselves
  gst_element_set_locked_state (downloader->priv->urisrc, TRUE);
  gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);

  // Our own buffer where we'll concatenate all the download into
  downloader->priv->buffer = gst_buffer_new ();
  /* Create a bus to handle error and warning message from the source element */
  downloader->priv->bus = gst_bus_new ();
  /* add a sync handler for the bus messages to detect errors in the download */
  gst_element_set_bus (GST_ELEMENT (downloader->priv->urisrc), downloader->priv->bus);
  gst_bus_set_sync_handler (downloader->priv->bus, skippy_uri_downloader_bus_handler, downloader, NULL);

  // Add external source pad as ghost pad to URI src pad
  urisrcpad = gst_element_get_static_pad (downloader->priv->urisrc, "src");
  downloader->priv->srcpad = gst_ghost_pad_new_from_template ("src", urisrcpad, templ);

  // Configure external source pad
  gst_pad_set_active (downloader->priv->srcpad, TRUE);
  gst_pad_set_event_function (downloader->priv->srcpad, skippy_uri_downloader_src_event);
  // Add blocking buffer probe
  /*
  gst_pad_add_probe (downloader->priv->srcpad, GST_PAD_PROBE_TYPE_BUFFER,
    skippy_uri_downloader_src_buffer_probe, downloader, NULL);
*/
  // Add pad to element
  gst_element_add_pad (GST_ELEMENT(downloader), downloader->priv->srcpad);
  gst_element_no_more_pads (GST_ELEMENT (downloader));

  // Reset private state fields
  skippy_uri_downloader_reset (downloader);
  // Init wait cond for blocking fetch call
  g_cond_init (&downloader->priv->cond);

  gst_object_unref (templ);
  gst_object_unref (urisrcpad);
}

void
skippy_uri_downloader_reset (SkippyUriDownloader * downloader)
{
  g_return_if_fail (downloader != NULL);

  // Cancel anything ongoing just in case
  skippy_uri_downloader_cancel (downloader);

  GST_OBJECT_LOCK (downloader);
  downloader->priv->bytes_loaded = 0;
  downloader->priv->bytes_total = 0;
  downloader->priv->fetching = FALSE;
  downloader->priv->discont = FALSE;

  // When there was an error we get rid of the source (should already be in NULL state)
  if (downloader->priv->err && downloader->priv->urisrc) {
    gst_object_unref (downloader->priv->urisrc);
    downloader->priv->urisrc = NULL;
  }
  // Any way, clear error
  g_clear_error (&downloader->priv->err);

  // Unref fragment model & buffer
  if (downloader->priv->fragment) {
    g_object_unref (downloader->priv->fragment);
    downloader->priv->fragment = NULL;
  }
  if (downloader->priv->buffer) {
    gst_buffer_unref (downloader->priv->buffer);
    downloader->priv->buffer = NULL;
  }

  // Flush bus
  gst_bus_set_flushing (downloader->priv->bus, FALSE);
  GST_OBJECT_UNLOCK (downloader);
}

static void
skippy_uri_downloader_dispose (GObject * object)
{
  g_return_if_fail (object);

  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  // Let's reset first (this is flushing the message bus and unref-ing any owned download)
  skippy_uri_downloader_reset (downloader);

  // Get rid of private bus
  if (downloader->priv->bus != NULL) {
    gst_object_unref (downloader->priv->bus);
    downloader->priv->bus = NULL;
  }

  // Dispose base class
  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->dispose (object);
}

static void
skippy_uri_downloader_finalize (GObject * object)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  g_cond_clear (&downloader->priv->cond);

  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->finalize (object);
}

static GstStateChangeReturn
skippy_uri_downloader_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  SkippyUriDownloader *demux = SKIPPY_URI_DOWNLOADER (element);

  GST_DEBUG ("Performing transition: %s -> %s", gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT(transition)),
    gst_element_state_get_name (GST_STATE_TRANSITION_NEXT(transition)));
  GST_DEBUG ("Calling parent class state change handler ...");
  ret = GST_ELEMENT_CLASS (skippy_uri_downloader_parent_class)->change_state (element, transition);
  GST_DEBUG ("State transition result: %s", gst_element_state_change_return_get_name (ret));

  return ret;
}

SkippyUriDownloader *
skippy_uri_downloader_new ()
{
  SkippyUriDownloader* downloader = g_object_new (TYPE_SKIPPY_URI_DOWNLOADER, NULL);
  return downloader;
}

GstPad* skippy_uri_download_get_src_pad (SkippyUriDownloader *downloader)
{
  g_return_val_if_fail (downloader, NULL);

  if (downloader->priv->urisrc) {
    return gst_element_get_static_pad (downloader->priv->urisrc, "src");
  }
  return NULL;
}

static void
skippy_uri_downloader_handle_bytes_received (SkippyUriDownloader* downloader,
  guint64 start_time, guint64 stop_time,
  gsize bytes_loaded, gsize bytes_total)
{
  GstElement* parent;
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
    gst_message_new_element (GST_OBJECT(downloader), s));

}

static void
skippy_uri_downloader_handle_eos (SkippyUriDownloader* downloader)
{
  GST_DEBUG_OBJECT (downloader, "Got EOS on the fetcher pad");
  GST_OBJECT_LOCK (downloader);
  if (downloader->priv->err) {
    GST_WARNING ("Got EOS but error: %s", downloader->priv->err->message);
  } else if (downloader->priv->fragment != NULL) {
    skippy_fragment_set_completed (downloader->priv->fragment);
    // FIXME: seems when data comes from filesystem caches we can get less data than the segment advertises (encryption padding?)
    // Make sure we send a 100% callback and have a valid byte number
    downloader->priv->bytes_loaded = downloader->priv->bytes_total;
    GST_OBJECT_UNLOCK (downloader);
    skippy_uri_downloader_handle_bytes_received (downloader,
      downloader->priv->fragment->start_time, downloader->priv->fragment->stop_time,
      downloader->priv->bytes_loaded, downloader->priv->bytes_total
    );
    GST_OBJECT_LOCK (downloader);
    GST_DEBUG_OBJECT (downloader, "Signaling chain funtion");
    g_cond_signal (&downloader->priv->cond);
  }
  GST_OBJECT_UNLOCK (downloader);
}

static gboolean
skippy_uri_downloader_handle_data_segment (SkippyUriDownloader* downloader, const GstSegment* segment)
{
  GST_OBJECT_LOCK (downloader);
  if (segment->format == GST_FORMAT_BYTES) {
    downloader->priv->bytes_loaded = segment->position;
    downloader->priv->bytes_total = segment->duration;
  } else {
    GST_WARNING ("Data segment event does not have bytes format!");
    GST_OBJECT_UNLOCK (downloader);
    return FALSE;
  }
  GST_OBJECT_UNLOCK (downloader);
  return TRUE;
}

static gboolean
skippy_uri_downloader_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (parent);
  const GstSegment* segment = NULL;
  GstCaps* caps;

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_EOS:
      skippy_uri_downloader_handle_eos (downloader);
      gst_event_unref (event); // swallow event
      return TRUE;
    case GST_EVENT_SEGMENT:
      gst_event_parse_segment (event, &segment);
      // it's important we ALWAYS execute the default event handler here
      // therefore this call needs to be here FIRST, before the actual handler
      return gst_pad_event_default (pad, parent, event)
      && skippy_uri_downloader_handle_data_segment (downloader, segment);
    default:
      return gst_pad_event_default (pad, parent, event);
  }
}

static void
skippy_uri_downloader_handle_error (SkippyUriDownloader *downloader,
  GstMessage* message)
{
  GError *err = NULL;
  gchar *dbg_info = NULL;
  gchar *full_message;

  gst_message_parse_error (message, &err, &dbg_info);

  // Log error
  GST_ERROR_OBJECT (downloader,
      "URI source error: '%s' from %s, the download will be cancelled",
      err->message, GST_OBJECT_NAME (message->src));

  // Update error message with debug info
  if (dbg_info) {
    GST_ERROR_OBJECT (downloader, "Error debug info: %s", dbg_info);
    full_message = g_strdup_printf ("%s\nERROR DEBUGGING INFO: {%s}", err->message, dbg_info);
    g_free (err->message);
    err->message = full_message;
  }
  g_free (dbg_info);

  // Set current error
  downloader->priv->err = err;

  // remove the sync handler to avoid duplicated messages
  gst_bus_set_sync_handler (downloader->priv->bus, NULL, NULL, NULL);

  // Cancel ongoing download
  skippy_uri_downloader_cancel (downloader);
}

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

static GstBusSyncReply
skippy_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data)
{
  SkippyUriDownloader *downloader = (SkippyUriDownloader *) (data);

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

static GstPadProbeReturn
skippy_uri_downloader_src_buffer_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  gsize bytes;

  if (GST_PAD_PROBE_INFO_TYPE(info) != GST_PAD_PROBE_TYPE_BUFFER) {
    return GST_PAD_PROBE_OK;
  }

  GST_OBJECT_LOCK (downloader);

  /* NOTE: HTTP errors (404, 500, etc...) are also pushed through this pad as
   * response but the source element will also post a warning or error message
   * in the bus, which is handled synchronously cancelling the download.
   */
  // There was an error downloading, quit quietly
  if (downloader->priv->err) {
    return GST_PAD_PROBE_DROP;
  }

  // Get size of buffer
  bytes = gst_buffer_get_size (buf);

  GST_TRACE_OBJECT (downloader, "The uri fetcher received a new buffer of size %" G_GSIZE_FORMAT, bytes);

  // Increment size on fragment model
  downloader->priv->fragment->size += bytes;

  // Count bytes and trigger callback
  downloader->priv->bytes_loaded += bytes;

  skippy_uri_downloader_handle_bytes_received (downloader,
    downloader->priv->fragment->start_time, downloader->priv->fragment->stop_time,
    downloader->priv->bytes_loaded, downloader->priv->bytes_total);

  // Flagging buffer as discontinuous if fragment is flagged as discont
  if (downloader->priv->fragment->discontinuous && !downloader->priv->discont) {
    GST_DEBUG ("Marking buffer as discontinuous");
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    // We should do this only once per downloaded fragment
    downloader->priv->discont = TRUE;
  } else {
    GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  // Copy and append buffer to download aggregate
  downloader->priv->buffer = gst_buffer_append (downloader->priv->buffer, gst_buffer_copy(buf));

  GST_OBJECT_UNLOCK (downloader);
  return GST_PAD_PROBE_OK;
}

GstBuffer* skippy_uri_downloader_get_buffer (SkippyUriDownloader *downloader)
{
  GstBuffer* buf;
  GST_OBJECT_LOCK (downloader);
  buf = gst_buffer_ref(downloader->priv->buffer);
  GST_OBJECT_UNLOCK (downloader);
  return buf;
}

void
skippy_uri_downloader_cancel (SkippyUriDownloader * downloader)
{
  GST_OBJECT_LOCK (downloader);
  GST_DEBUG ("Checking for ongoing download");
  if (downloader->priv->fetching) {
    GST_DEBUG ("Cancelling ongoing download");
    downloader->priv->fragment->cancelled = TRUE;
    GST_DEBUG ("Signaling chain funtion");
    g_cond_signal (&downloader->priv->cond);
  } else {
    GST_INFO ("There is no ongoing download to cancel");
  }
  GST_OBJECT_UNLOCK (downloader);
}

static gboolean
skippy_uri_downloader_set_range (SkippyUriDownloader * downloader,
    gint64 range_start, gint64 range_end)
{
  GstEvent *seek;

  g_return_val_if_fail (range_start >= 0, FALSE);
  g_return_val_if_fail (range_end >= -1, FALSE);

  GST_INFO_OBJECT (downloader, "Setting range to %d - %d", (int) range_start, (int) range_end);

  if (range_start || (range_end >= 0)) {
    GST_DEBUG ("Creating seek event on URI src");
    seek = gst_event_new_seek (1.0, GST_FORMAT_BYTES, GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET, range_start, GST_SEEK_TYPE_SET, range_end);
    return gst_element_send_event (downloader->priv->urisrc, seek);
  }
  return TRUE;
}

static gboolean
skippy_uri_downloader_set_uri (SkippyUriDownloader * downloader, const gchar * uri,
    const gchar * referer, gboolean compress, gboolean refresh,
    gboolean allow_cache)
{
  GError* err = NULL;
  GObjectClass *gobject_class;

  GST_DEBUG_OBJECT (downloader, "Setting URI: %s", uri);

  // Validate the URI
  if (!gst_uri_is_valid (uri)) {
    GST_WARNING ("URI is invalid");
    return FALSE;
  }

  // Set URI
  if (!gst_uri_handler_set_uri (GST_URI_HANDLER (downloader->priv->urisrc), uri, &err)) {
    GST_ERROR_OBJECT (downloader, "Failed set URI on source element for URI: %s", err->message);
    g_clear_error (&err);
    return FALSE;
  }

  // Configure source element accordingly
  gobject_class = G_OBJECT_GET_CLASS (downloader->priv->urisrc);
  if (g_object_class_find_property (gobject_class, "compress"))
    g_object_set (downloader->priv->urisrc, "compress", compress, NULL);
  if (g_object_class_find_property (gobject_class, "keep-alive"))
    g_object_set (downloader->priv->urisrc, "keep-alive", TRUE, NULL);
  if (g_object_class_find_property (gobject_class, "extra-headers")) {
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

static void
skippy_uri_downloader_unset_uri (SkippyUriDownloader * downloader)
{
  if (downloader->priv->err) {
    /* set the element state to NULL if there was an error otherwise go back to READY state */
    GST_DEBUG ("Setting source element to NULL state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
    gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
    return;
  }
  GST_DEBUG ("Setting source element to READY state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
  gst_element_set_state (downloader->priv->urisrc, GST_STATE_READY);
}

static SkippyUriDownloaderFetchReturn
skippy_uri_downloader_handle_failure (SkippyUriDownloader * downloader, GError ** err)
{
  skippy_uri_downloader_unset_uri (downloader);
  GST_OBJECT_LOCK (downloader);
  // Check for error from internal bus
  if (downloader->priv->err) {
    // Copy error but our own one for internal processing
    GST_ERROR_OBJECT (downloader, "Error fetching URI: %s", downloader->priv->err->message);
    *err = g_error_copy (downloader->priv->err);
  }
  GST_OBJECT_UNLOCK (downloader);
  return SKIPPY_URI_DOWNLOADER_FAILED;
}

SkippyUriDownloaderFetchReturn
skippy_uri_downloader_fetch_fragment (SkippyUriDownloader * downloader, SkippyFragment* fragment,
  const gchar * referer, gboolean compress, gboolean refresh, gboolean allow_cache, GError ** err)
{
  GstStateChangeReturn ret;

  g_return_val_if_fail (fragment, SKIPPY_URI_DOWNLOADER_FAILED);
  g_return_val_if_fail (fragment, SKIPPY_URI_DOWNLOADER_FAILED);

  // We spare our consumer to explicitely reset us for re-usal
  skippy_uri_downloader_reset (downloader);

  GST_OBJECT_LOCK (downloader);

  g_object_ref (fragment);
  // Storing the current fragment for cancellation
  // Taking ownership, fragment should then only be accessed via the public functions of this object for MT-safety)
  downloader->priv->fragment = fragment;

  // Set URL
  if (!skippy_uri_downloader_set_uri (downloader, fragment->uri, referer, compress, refresh, allow_cache)) {
    GST_WARNING_OBJECT (downloader, "Failed to set URI");
    GST_OBJECT_UNLOCK (downloader);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  GST_DEBUG ("Preparing data source ...");
  // Bootup source into READY state
  GST_OBJECT_UNLOCK (downloader);
  ret = gst_element_set_state (downloader->priv->urisrc, GST_STATE_READY);
  GST_DEBUG ("State change return: %s", gst_element_state_change_return_get_name (ret));
  GST_OBJECT_LOCK (downloader);

  // Check for failure
  if (ret == GST_STATE_CHANGE_FAILURE || downloader->priv->fragment == NULL) {
    GST_WARNING_OBJECT (downloader, "Failed to set src to READY");
    GST_OBJECT_UNLOCK (downloader);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  // Set range
  if (!skippy_uri_downloader_set_range (downloader, fragment->range_start, fragment->range_end)) {
    GST_WARNING_OBJECT (downloader, "Failed to set range");
    GST_OBJECT_UNLOCK (downloader);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  GST_DEBUG ("Ready to request data");

  // Let data flow!
  GST_OBJECT_UNLOCK (downloader);
  ret = gst_element_set_state (downloader->priv->urisrc, GST_STATE_PLAYING);
  GST_DEBUG ("State change return: %s", gst_element_state_change_return_get_name (ret));
  GST_OBJECT_LOCK (downloader);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    GST_OBJECT_UNLOCK (downloader);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  /* wait until:
   *   - the download succeed (EOS in the src pad)
   *   - the download failed (Error message on the fetcher bus)
   *   - the download was canceled
   */
  GST_DEBUG_OBJECT (downloader, "Waiting to fetch the URI %s", fragment->uri);

  // Indicating we are downloading
  downloader->priv->fetching = TRUE;
  while (!fragment->cancelled && !fragment->completed && !downloader->priv->err) {
    g_cond_wait (&downloader->priv->cond, GST_OBJECT_GET_LOCK (downloader));
    GST_DEBUG ("Condition has been signalled");
  }
  downloader->priv->fetching = FALSE;

  skippy_uri_downloader_unset_uri (downloader);

  if (downloader->priv->err) {
    fragment->completed = FALSE;
    GST_OBJECT_UNLOCK (downloader);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  if (fragment->cancelled) {
    GST_OBJECT_UNLOCK (downloader);
    return SKIPPY_URI_DOWNLOADER_CANCELLED;
  }

  GST_OBJECT_UNLOCK (downloader);
  return SKIPPY_URI_DOWNLOADER_COMPLETED;
}
