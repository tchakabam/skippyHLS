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

#include "skippyhls/skippy_fragment.h"
#include "skippyhls/skippy_uridownloader.h"

#include <glib.h>

#define GST_CAT_DEFAULT uridownloader_debug
GST_DEBUG_CATEGORY (uridownloader_debug);

#define SKIPPY_URI_DOWNLOADER_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    TYPE_SKIPPY_URI_DOWNLOADER, SkippyUriDownloaderPrivate))

struct _SkippyUriDownloaderPrivate
{
  /* Fragments fetcher */
  GstElement *urisrc;
  GstBus *bus;
  GstPad *pad;
  GTimeVal *timeout;

  SkippyFragment *download;
  GError *err;
  GCond cond;

  gboolean fetching;

  SkippyUriDownloaderCallback callback;
  gsize bytes_loaded;
  gsize bytes_total;
};

static void skippy_uri_downloader_finalize (GObject * object);
static void skippy_uri_downloader_dispose (GObject * object);

static GstFlowReturn skippy_uri_downloader_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean skippy_uri_downloader_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static GstBusSyncReply skippy_uri_downloader_bus_handler (GstBus * bus,
    GstMessage * message, gpointer data);

static GstStaticPadTemplate sinkpadtemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define _do_init \
{ \
  GST_DEBUG_CATEGORY_INIT (uridownloader_debug, "skippyhls-uridownloader", 0, "URI downloader"); \
}

G_DEFINE_TYPE_WITH_CODE (SkippyUriDownloader, skippy_uri_downloader, GST_TYPE_OBJECT,
    _do_init);

static void
skippy_uri_downloader_class_init (SkippyUriDownloaderClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  g_type_class_add_private (klass, sizeof (SkippyUriDownloaderPrivate));

  gobject_class->dispose = skippy_uri_downloader_dispose;
  gobject_class->finalize = skippy_uri_downloader_finalize;
}

static void
skippy_uri_downloader_init (SkippyUriDownloader * downloader)
{
  downloader->priv = SKIPPY_URI_DOWNLOADER_GET_PRIVATE (downloader);

  /* Initialize the sink pad. This pad will be connected to the src pad of the
   * element created with gst_element_make_from_uri and will handle the download */
  downloader->priv->pad =
      gst_pad_new_from_static_template (&sinkpadtemplate, "sink");
  gst_pad_set_chain_function (downloader->priv->pad,
      GST_DEBUG_FUNCPTR (skippy_uri_downloader_chain));
  gst_pad_set_event_function (downloader->priv->pad,
      GST_DEBUG_FUNCPTR (skippy_uri_downloader_sink_event));
  gst_pad_set_element_private (downloader->priv->pad, downloader);
  gst_pad_set_active (downloader->priv->pad, TRUE);

  /* Create a bus to handle error and warning message from the source element */
  downloader->priv->bus = gst_bus_new ();

  // Reset private state fields
  skippy_uri_downloader_reset (downloader);

  g_cond_init (&downloader->priv->cond);
}

static void
skippy_uri_downloader_dispose (GObject * object)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  // Let's reset first (this is flushing the message bus and unref-ing any owned download)
  skippy_uri_downloader_reset (downloader);

  // Shutdown everything
  if (downloader->priv->urisrc != NULL) {
    gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
    gst_object_unref (downloader->priv->urisrc);
    downloader->priv->urisrc = NULL;
  }

  // Get rid of bus
  if (downloader->priv->bus != NULL) {
    gst_object_unref (downloader->priv->bus);
    downloader->priv->bus = NULL;
  }

  // Get rid of pad
  if (downloader->priv->pad) {
    gst_object_unref (downloader->priv->pad);
    downloader->priv->pad = NULL;
  }

  // Dispose base class
  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->dispose (object);
}

void
skippy_uri_downloader_reset (SkippyUriDownloader * downloader)
{
  g_return_if_fail (downloader != NULL);

  // Cancel anything ongoing just in case
  skippy_uri_downloader_cancel (downloader);

  GST_OBJECT_LOCK (downloader);
  // When there was an error we get rid of the source (should already be in NULL state)
  if (downloader->priv->err && downloader->priv->urisrc) {
    gst_object_unref (downloader->priv->urisrc);
    downloader->priv->urisrc = NULL;
  }
  // Reset state
  if (downloader->priv->err) {
    //g_error_free (downloader->priv->err);
  }
  downloader->priv->err = NULL;
  downloader->priv->bytes_loaded = 0;
  downloader->priv->bytes_total = 0;
  downloader->priv->fetching = FALSE;
  // Unref download
  if (downloader->priv->download) {
    g_object_unref (downloader->priv->download);
    downloader->priv->download = NULL;
  }
  // Flush bus
  gst_bus_set_flushing (downloader->priv->bus, FALSE);
  GST_OBJECT_UNLOCK (downloader);
}

static void
skippy_uri_downloader_finalize (GObject * object)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  g_cond_clear (&downloader->priv->cond);

  G_OBJECT_CLASS (skippy_uri_downloader_parent_class)->finalize (object);
}

SkippyUriDownloader *
skippy_uri_downloader_new (SkippyUriDownloaderCallback callback)
{
  SkippyUriDownloader* downloader = g_object_new (TYPE_SKIPPY_URI_DOWNLOADER, NULL);
  downloader->priv->callback = callback;
  return downloader;
}

static void
skippy_uri_downloader_handle_eos (SkippyUriDownloader* downloader)
{
  GST_OBJECT_LOCK (downloader);
  GST_DEBUG_OBJECT (downloader, "Got EOS on the fetcher pad");
  if (downloader->priv->err) {
    GST_WARNING ("Got EOS but error: %s", downloader->priv->err->message);
  } else if (downloader->priv->download != NULL) {
    skippy_fragment_complete (downloader->priv->download, (struct SkippyUriDownloader *) downloader);
    // FIXME: seems when data comes from filesystem caches we can get less data than the segment advertises (encryption padding?)
    // Make sure we send a 100% callback and have a valid byte number
    downloader->priv->bytes_loaded = downloader->priv->bytes_total;
    downloader->priv->callback (downloader,
      downloader->priv->download->start_time, downloader->priv->download->stop_time,
      downloader->priv->bytes_loaded, downloader->priv->bytes_total
    );
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
skippy_uri_downloader_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  const GstSegment* segment = NULL;
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (gst_pad_get_element_private (pad));

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_EOS:{
      skippy_uri_downloader_handle_eos (downloader);
      gst_event_unref (event); // swallow event
      return FALSE;
    }
    case GST_EVENT_SEGMENT: {
      gst_event_parse_segment (event, &segment);
      // it's important we ALWAYS execute the default event handler here
      // therefore this call needs to be here FIRST, before the actual handler
      return gst_pad_event_default (pad, parent, event)
              && skippy_uri_downloader_handle_data_segment (downloader, segment);
      // falls through to default
    }
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
  }
  // Drop the message
  gst_message_unref (message);
  return GST_BUS_DROP;
}

static GstFlowReturn
skippy_uri_downloader_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  SkippyUriDownloader *downloader;
  gsize bytes;

  downloader = SKIPPY_URI_DOWNLOADER (gst_pad_get_element_private (pad));

  /* HTTP errors (404, 500, etc...) are also pushed through this pad as
   * response but the source element will also post a warning or error message
   * in the bus, which is handled synchronously cancelling the download.
   */
  GST_OBJECT_LOCK (downloader);

  // There was an error downloading, quit
  if (downloader->priv->err) {
    goto done;
  }

  bytes = gst_buffer_get_size (buf);

  GST_TRACE_OBJECT (downloader, "The uri fetcher received a new buffer "
      "of size %" G_GSIZE_FORMAT, bytes);

  downloader->priv->bytes_loaded += bytes;
  downloader->priv->callback (downloader,
    downloader->priv->download->start_time, downloader->priv->download->stop_time,
    downloader->priv->bytes_loaded, downloader->priv->bytes_total);

  if (!skippy_fragment_add_buffer (downloader->priv->download, buf))
    GST_WARNING_OBJECT (downloader, "Could not add buffer to fragment");

done:
  {
    GST_OBJECT_UNLOCK (downloader);
    return GST_FLOW_OK;
  }
}

void
skippy_uri_downloader_cancel (SkippyUriDownloader * downloader)
{
  GST_OBJECT_LOCK (downloader);
  GST_DEBUG ("Checking for ongoing download");
  if (downloader->priv->fetching) {
    GST_DEBUG ("Cancelling ongoing download");
    downloader->priv->download->cancelled = TRUE;
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
  g_return_val_if_fail (range_start >= 0, FALSE);
  g_return_val_if_fail (range_end >= -1, FALSE);

  GST_INFO_OBJECT (downloader, "Setting range to %d - %d", (int) range_start, (int) range_end);

  if (range_start || (range_end >= 0)) {

    GST_DEBUG ("Creating seek event");

    GstEvent *seek;

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
  GstPad *pad;
  GObjectClass *gobject_class;
  GError* err = NULL;

  GST_DEBUG_OBJECT (downloader, "Fetching URI %s", uri);

  if (!gst_uri_is_valid (uri)) {
    return FALSE;
  }

  if (!downloader->priv->urisrc) {
    GST_DEBUG_OBJECT (downloader, "Creating source element for the URI: %s", uri);
    downloader->priv->urisrc =
        gst_element_make_from_uri (GST_URI_SRC, uri, NULL, NULL);
  } else {
    GST_DEBUG_OBJECT (downloader, "Re-using old source element");
    if (!gst_uri_handler_set_uri (GST_URI_HANDLER (downloader->priv->urisrc), uri, &err)) {
      GST_ERROR_OBJECT (downloader, "Failed to re-use old source element: %s", err->message);
      g_clear_error (&err);
      gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
      gst_object_unref (downloader->priv->urisrc);
      downloader->priv->urisrc = NULL;
    }
  }
  if (!downloader->priv->urisrc) {
    return FALSE;
  }
  GST_DEBUG ("Got source element: %s", GST_ELEMENT_NAME (downloader->priv->urisrc));

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

  /* add a sync handler for the bus messages to detect errors in the download */
  gst_element_set_bus (GST_ELEMENT (downloader->priv->urisrc),
      downloader->priv->bus);

  gst_bus_set_sync_handler (downloader->priv->bus,
      skippy_uri_downloader_bus_handler, downloader, NULL);

  pad = gst_element_get_static_pad (downloader->priv->urisrc, "src");
  if (!pad)
    return FALSE;
  gst_pad_link (pad, downloader->priv->pad);
  gst_object_unref (pad);
  return TRUE;
}

static void
skippy_uri_downloader_unset_uri (SkippyUriDownloader * downloader)
{
  GstPad *pad;
  GstElement *urisrc;

  GST_OBJECT_LOCK (downloader);

  urisrc = downloader->priv->urisrc;

  /* remove the bus sync handler */
  gst_bus_set_sync_handler (downloader->priv->bus, NULL, NULL, NULL);
  gst_bus_set_flushing (downloader->priv->bus, TRUE);

  GST_OBJECT_UNLOCK (downloader);
  if (downloader->priv->err) {
    /* set the element state to NULL if there was an error
    otherwise go back to READY state */
    GST_DEBUG ("Setting source element to NULL state (%s)", GST_ELEMENT_NAME (urisrc));
    gst_element_set_state (urisrc, GST_STATE_NULL);
  } else {
    GST_DEBUG ("Setting source element to READY state (%s)", GST_ELEMENT_NAME (urisrc));
    gst_element_set_state (urisrc, GST_STATE_READY);
  }
  GST_OBJECT_LOCK (downloader);

  // Get rid of bus
  gst_element_set_bus (urisrc, NULL);

  /* unlink the source element from the internal pad */
  pad = gst_pad_get_peer (downloader->priv->pad);
  if (pad) {
    gst_pad_unlink (pad, downloader->priv->pad);
    gst_object_unref (pad);
  }
  GST_OBJECT_UNLOCK (downloader);
}

/**
 * skippy_uri_downloader_fetch_fragment:
 * Downloads a SkippyFragment (downloader takes ownership of fragment)
 */

 static SkippyUriDownloaderFetchReturn
 skippy_uri_downloader_handle_failure (SkippyUriDownloader * downloader, GError ** err)
 {
  skippy_uri_downloader_unset_uri (downloader);
  GST_OBJECT_LOCK (downloader);
  if (downloader->priv->err) {
    // Handle error
    GST_ERROR_OBJECT (downloader, "Error fetching URI: %s", downloader->priv->err->message);
    g_propagate_error (err, downloader->priv->err);
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
  downloader->priv->download = fragment;

  // Set URL
  if (!skippy_uri_downloader_set_uri (downloader, fragment->uri, referer, compress, refresh, allow_cache)) {
    GST_WARNING_OBJECT (downloader, "Failed to set URI");
    GST_OBJECT_UNLOCK (downloader);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  // Bootup source into READY state
  GST_OBJECT_UNLOCK (downloader);
  ret = gst_element_set_state (downloader->priv->urisrc, GST_STATE_READY);
  GST_DEBUG ("State change return: %s", gst_element_state_change_return_get_name (ret));
  GST_OBJECT_LOCK (downloader);

  // Check for failure
  if (ret == GST_STATE_CHANGE_FAILURE || downloader->priv->download == NULL) {
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

  GST_OBJECT_UNLOCK (downloader);
  skippy_uri_downloader_unset_uri (downloader);
  GST_OBJECT_LOCK (downloader);

  if (downloader->priv->err) {
    g_return_val_if_fail (!fragment->completed, SKIPPY_URI_DOWNLOADER_FAILED);
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
