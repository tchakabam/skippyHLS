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

#include <string.h>

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
  GstPad *srcpad;
  GstBuffer *buffer;
  GstSegment segment;
  GError *err;

  GCond cond;
  GMutex download_lock;

  gboolean previous_was_interrupted;
  gboolean set_uri;
  gboolean fetching;
  gboolean need_segment, need_stream_start, got_segment;

  gsize bytes_loaded;
  gsize bytes_total;

  gulong urisrcpad_probe_id;
};

static GstStaticPadTemplate srcpadtemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static void skippy_uri_downloader_finalize (GObject * object);
static void skippy_uri_downloader_dispose (GObject * object);
static void skippy_uri_downloader_reset (SkippyUriDownloader * downloader, SkippyFragment* fragment);
static GstPadProbeReturn skippy_uri_downloader_src_probe (GstPad *pad, GstPadProbeInfo *info, gpointer user_data);
static void skippy_uri_downloader_complete (SkippyUriDownloader * downloader);
static gboolean skippy_uri_downloader_create_src (SkippyUriDownloader * downloader, gchar* uri);
static GstStateChangeReturn skippy_uri_downloader_change_state (GstElement *element, GstStateChange transition);
static void skippy_uri_downloader_handle_message (GstBin * bin, GstMessage * msg);

// Define class
static void
skippy_uri_downloader_class_init (SkippyUriDownloaderClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass*) klass;
  GstBinClass *gstbin_class = (GstBinClass*) klass;

  gstbin_class = (GstBinClass *) klass;

  g_type_class_add_private (klass, sizeof (SkippyUriDownloaderPrivate));

  gobject_class->dispose = skippy_uri_downloader_dispose;
  gobject_class->finalize = skippy_uri_downloader_finalize;

  //gstelement_class->change_state = skippy_uri_downloader_change_state;
  gstbin_class->handle_message = skippy_uri_downloader_handle_message;

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

  // Element state flags
  downloader->priv->fetching = FALSE;
  downloader->priv->set_uri = FALSE;
  downloader->priv->need_segment = TRUE;
  downloader->priv->need_stream_start = TRUE;

  // Add typefind
  downloader->priv->typefind = gst_element_factory_make ("typefind", NULL);
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
  skippy_uri_downloader_reset (downloader, NULL);
}

// This will compare two URIs by their string representation without the query part
// We use this to compare URIs without considering time or user-auth-dependent CDN tokens in order to enable
// caching or resuming for either resource whenever it is attempted to download it.
static gboolean
compare_uri_resource_path (gchar* uri1, gchar *uri2)
{
  gboolean ret;
  GstUri *prev_uri, *next_uri;
  gchar *prev_uri_no_query, *next_uri_no_query;

  prev_uri = gst_uri_from_string (uri1);
  next_uri = gst_uri_from_string (uri2);
  gst_uri_set_query_string (prev_uri, "");
  gst_uri_set_query_string (next_uri, "");
  prev_uri_no_query = gst_uri_to_string (prev_uri);
  next_uri_no_query = gst_uri_to_string (next_uri);

  ret = strcmp(prev_uri_no_query, next_uri_no_query) == 0;

  gst_uri_unref (prev_uri);
  gst_uri_unref (next_uri);
  g_free (prev_uri_no_query);
  g_free (next_uri_no_query);
  return ret;
}

// Reset object - can not be called concurrently with fetch or getters/setters functions
// Will cancel any ongoing download and block until it's finished (i.e until fetch function has exited)
//
// MT-safe
static void
skippy_uri_downloader_reset (SkippyUriDownloader * downloader, SkippyFragment* next_fragment)
{
  g_return_if_fail (downloader != NULL);

  // Cancel anything ongoing just in case
  skippy_uri_downloader_cancel (downloader);

  g_mutex_lock (&downloader->priv->download_lock);

  // Is this a retrial from an unfinished download?
  // We can check that its the same by comparing the URIs and the loaded bytes count
  // NOTE: we do the string comparison only after the bytes count is off as its more expensive
  // If we are seeking in some way (i.e will issue a segment event to the stream) then wont consider resuming at all.
  if (G_UNLIKELY(!downloader->priv->need_segment
    // Did we not complete the previous download?
     && downloader->priv->bytes_loaded != downloader->priv->bytes_total
    // reset might not be called to prepare a fetch and/or there might be no previous download
     && next_fragment && downloader->priv->fragment
     // Is it the same URI?
     && compare_uri_resource_path (next_fragment->uri, downloader->priv->fragment->uri))) {
    // If the previous download was not completed and we are currently retrying the same
    // we are not resetting the fields and will later perform a range request to get only
    // the missing stuff.
    GST_DEBUG ("Previous download interruption detected");
    downloader->priv->previous_was_interrupted = TRUE;
  } else {
    // If above condition does not hold: Fresh new download state
    downloader->priv->bytes_loaded = 0;
    downloader->priv->bytes_total = 0;
    downloader->priv->previous_was_interrupted = FALSE;
  }

  downloader->priv->got_segment = FALSE;

  // Clear error when present
  g_clear_error (&downloader->priv->err);

  // Unref fragment model
  if (downloader->priv->fragment) {
    g_object_unref (downloader->priv->fragment);
    downloader->priv->fragment = NULL;
  }

  // Reset our own buffer where we'll concatenate all the download into
  // Only when we were not interrupted before (or this is a fresh start with previous fragment == NULL)
  if (downloader->priv->buffer && !downloader->priv->previous_was_interrupted) {
    gst_buffer_unref (downloader->priv->buffer);
    downloader->priv->buffer = NULL;
  }

  g_mutex_unlock (&downloader->priv->download_lock);

  GST_TRACE ("Reset done");
}

/*
static GstStateChangeReturn
skippy_uri_downloader_change_state (GstElement *element, GstStateChange transition)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (element);

  GST_DEBUG_OBJECT (element, "Performing transition: %s -> %s",
    gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT(transition)),
    gst_element_state_get_name (GST_STATE_TRANSITION_NEXT(transition)));

  // Call parent
  return GST_ELEMENT_CLASS (skippy_uri_downloader_parent_class)->change_state (element, transition);
}
*/

// Dispose function - frees all resources
static void
skippy_uri_downloader_dispose (GObject * object)
{
  g_return_if_fail (object);

  GST_DEBUG ("Disposing ...");

  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (object);

  g_return_if_fail (downloader->priv);

  // Let's reset first (this is flushing the message bus and unref-ing any owned download)
  skippy_uri_downloader_reset (downloader, NULL);

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
  // We updated the segment so we need to set this to TRUE to make sure before we send the next buffer
  // through a segment event gets sent
  downloader->priv->need_segment = TRUE;
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

// Handles received bytes info: Called by URL source element streaming thread. Triggers custom message about media byte-interval loaded.
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_handle_bytes_received (SkippyUriDownloader* downloader,
  guint64 start_time, guint64 stop_time,
  gsize bytes_loaded, gsize bytes_total)
{
  GstStructure* s;
  float percentage = 100.0f * bytes_loaded / bytes_total;

  // Be silent if we are not linked
  if (!gst_pad_is_linked (downloader->priv->srcpad)) {
    return;
  }

  GST_TRACE ("Loaded %" G_GSIZE_FORMAT " bytes of %" G_GSIZE_FORMAT " -> %f percent of media interval %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT " seconds",
    bytes_loaded,
    bytes_total,
    percentage,
    GST_TIME_ARGS (start_time),
    GST_TIME_ARGS (stop_time)
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

// Handles segment events from URI data source thread - this tells us about the size of the next chunk we are loading
// Download mutex is locked when this is called (only while fetch executes).
static gboolean
skippy_uri_downloader_handle_data_segment (SkippyUriDownloader* downloader, const GstSegment* segment)
{
  if (segment->format == GST_FORMAT_BYTES) {

    GST_DEBUG ("Handling data segment for fragment at %d - %d ms (duration = %d ms)",
      (int) (downloader->priv->fragment->start_time / GST_MSECOND),
      (int) (downloader->priv->fragment->stop_time / GST_MSECOND),
      (int) (downloader->priv->fragment->duration / GST_MSECOND)
    );

    if (!downloader->priv->got_segment) {
      // Guard from HTTP client internally retrying a broken connection
      // and giving us data we already got from the previous attempt.
      // In this case we will just cancel and resume the download
      // in a smart way ourselves if necessary.
      if (segment->position < downloader->priv->bytes_loaded) {
        skippy_uri_downloader_cancel (downloader);
        return TRUE;
      }
      // Update total bytes and reset counter
      downloader->priv->bytes_loaded = segment->position;
      downloader->priv->bytes_total = segment->duration;
      downloader->priv->got_segment = TRUE;
    }

  } else {
    GST_WARNING ("Data segment event does not have bytes format!");
    return FALSE;
  }
  return TRUE;
}

// Handles errors from message bus sync handler of URI src (runs in it's streaming thread)
// Download mutex is locked when this is called (only while fetch executes).
static void
skippy_uri_downloader_handle_error (SkippyUriDownloader *downloader, GError* err)
{
  // Set current error if not yet set (if there are several recurrent errors we will only store the first one)
  if (downloader->priv->err) {
    return;
  }
  downloader->priv->err = err;

  // Log error
  GST_INFO_OBJECT (downloader, "Downloader error: '%s', the download will be cancelled", err->message);

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
      "Downloader warning: %s from %s",
      GST_OBJECT_NAME (message->src), err->message);
  GST_DEBUG ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
  g_error_free (err);
  g_free (dbg_info);
}

static void skippy_uri_downloader_handle_message (GstBin * bin, GstMessage * message)
{
  GError *err = NULL;
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (bin);

  GST_DEBUG ("Got %" GST_PTR_FORMAT, message);

  // Download mutex is locked when this is called (only while fetch executes).
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {

    gst_message_parse_error (message, &err, NULL);
    skippy_uri_downloader_handle_error (downloader, err);
    gst_message_unref (message);

  } else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_WARNING) {

    skippy_uri_downloader_handle_warning (downloader, message);
    gst_message_unref (message);

  } else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ELEMENT) {
    // Handle any other message (mostly state-changed notifications)
    GST_BIN_CLASS (skippy_uri_downloader_parent_class)->handle_message (bin, message);
  }
  else {
    gst_message_unref (message);
  }
}

void skippy_uri_downloader_update_downstream_events (SkippyUriDownloader *downloader, gboolean stream_start, gboolean segment)
{
  gboolean have_group_id;
  guint group_id;
  gchar* stream_id = NULL;
  GstEvent* event = NULL;
  GstPad *sink = gst_element_get_static_pad (downloader->priv->typefind, "sink");

  GST_DEBUG ("Need segment: %d / Need stream-start: %d", downloader->priv->need_segment, downloader->priv->need_stream_start);

  if (stream_start && G_UNLIKELY(downloader->priv->need_stream_start)) {
    // Sending stream start event that we got on sink pad (sticky event)
    stream_id = gst_pad_create_stream_id (downloader->priv->srcpad, GST_ELEMENT_CAST (downloader), NULL);
    // Create stream start event from stream ID that we parsed from sink pad
    event = gst_event_new_stream_start (stream_id);
    gst_event_set_group_id (event, gst_util_group_id_next ());
    downloader->priv->need_stream_start = FALSE;
    GST_DEBUG ("Sending %" GST_PTR_FORMAT, event);
    gst_pad_send_event (sink, event);
  }

  // This is TRUE if we have modified the segment or if its the very first buffer we issue
  if (segment && G_UNLIKELY(downloader->priv->need_segment)) {
    // Create new segment event from our own segment (time format)
    downloader->priv->segment.position = downloader->priv->fragment->start_time;
    event = gst_event_new_segment (&downloader->priv->segment);
    GST_OBJECT_LOCK (downloader);
    downloader->priv->need_segment = FALSE;
    GST_OBJECT_UNLOCK (downloader);
    GST_DEBUG ("Sending %" GST_PTR_FORMAT, event);
    gst_pad_send_event (sink, event);
  }

  gst_object_unref (sink);
  g_free (stream_id);
}

// Probe buffers from URI src streaming thread
// Download mutex is locked when this is called (only while fetch executes).
static GstPadProbeReturn
skippy_uri_downloader_src_probe_buffer (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  gsize bytes = gst_buffer_get_size (buf);

  GST_TRACE_OBJECT (downloader, "Got %" GST_PTR_FORMAT " of size %" G_GSIZE_FORMAT, buf, bytes);

  /* NOTE: HTTP errors (404, 500, etc...) are also pushed through this pad as
   * response but the source element will also post a warning or error message
   * in the bus, which is handled synchronously cancelling the download.
   */

  // There was an error downloading, quit quietly
  if (downloader->priv->err) {
    GST_WARNING ("Detected error, dropping item");
    return GST_PAD_PROBE_DROP;
  }

  // Increment size on fragment model
  downloader->priv->fragment->size += bytes;
  // Count bytes up
  downloader->priv->bytes_loaded += bytes;

  // Triggers message
  skippy_uri_downloader_handle_bytes_received (downloader,
    downloader->priv->fragment->start_time, downloader->priv->fragment->stop_time,
    downloader->priv->bytes_loaded, downloader->priv->bytes_total);

  // This is only if we are not linked: Drop the buffer and append to our own
  // internal buffer.
  if (!gst_pad_is_linked (downloader->priv->srcpad)) {

    // Copy and append buffer to download aggregate
    if (downloader->priv->buffer == NULL) {
      downloader->priv->buffer = gst_buffer_new ();
    }
    // Append the buffer into the internal storage (transfers full ownership of the copy)
    // We rather copy because the original is still owned by the pad at this point
    downloader->priv->buffer = gst_buffer_append (downloader->priv->buffer, gst_buffer_copy(buf));
    // Drop this buffer (this will return FLOW_OK to internal src)
    return GST_PAD_PROBE_DROP;
  }
  // Otherwise keep the probe and go one with regular streaming

  // This sets the PTS on the buffer when we send a segment event
  if (downloader->priv->need_segment) {
    GST_LOG ("Marking buffer at %" GST_TIME_FORMAT " as discontinuous",
      GST_TIME_ARGS (downloader->priv->fragment->start_time));
    GST_BUFFER_PTS (buf) = downloader->priv->fragment->start_time;
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT );
  }

  // Make sure we are updating the pipeline with the necessary information before we push any data
  // In this case we need to check and send a segment event if needed
  skippy_uri_downloader_update_downstream_events (downloader, TRUE, TRUE);
  return GST_PAD_PROBE_OK;
}

// There are cases where the downstream elements get reset and loose their segment
// event, in this case we need to update them (called from urisrc event probe)
void skippy_uri_downloader_check_for_sticky_segment_event (SkippyUriDownloader *downloader, GstPad *pad)
{
  GstPad *sink = gst_pad_get_peer (pad);
  GstEvent *sticky_event = gst_pad_get_sticky_event (sink, GST_EVENT_SEGMENT, 0);
  if (sticky_event) {
    gst_event_unref (sticky_event);
  } else {
    GST_DEBUG ("Sticky segment event not found");
    GST_OBJECT_LOCK (downloader);
    downloader->priv->need_segment = TRUE;
    GST_OBJECT_UNLOCK (downloader);
  }
  gst_object_unref (sink);
}

// Probe events from URI src streaming thread
// Download mutex is locked when this is called (only while fetch executes).
static GstPadProbeReturn
skippy_uri_downloader_src_probe_event (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  SkippyUriDownloader *downloader = SKIPPY_URI_DOWNLOADER (user_data);
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  GstSegment bytes_segment;

  GST_TRACE ("Got %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE(event)) {
  case GST_EVENT_SEGMENT:
    skippy_uri_downloader_check_for_sticky_segment_event (downloader, pad);
    // Check for current fragment download and replace event data if possible
    // Copy segment event from URI src
    gst_event_copy_segment (event, &bytes_segment);
    // Reset bytes counter & update our time segment
    skippy_uri_downloader_handle_data_segment (downloader, &bytes_segment);
    return GST_PAD_PROBE_DROP;
  case GST_EVENT_EOS:
    skippy_uri_downloader_handle_eos (downloader);
    return GST_PAD_PROBE_DROP;
  case GST_EVENT_CAPS:
    // Update stream start event before we pass on a caps event here
    skippy_uri_downloader_update_downstream_events (downloader, TRUE, FALSE);
    return GST_PAD_PROBE_OK;
  default:
    // Dropping everything else here (like stream-start, flush-start/stop etc)
    return GST_PAD_PROBE_DROP;
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
    downloader->priv->set_uri = TRUE;
    return TRUE;
  }

  // Create HTTP src
  downloader->priv->urisrc = gst_element_make_from_uri (GST_URI_SRC, uri, NULL, &err);
  if (err) {
    GST_ERROR ("Could not create HTTP source: %s", err->message);
    g_clear_error (&err);
    return FALSE;
  }
  // Add URI source element to bin
  GST_DEBUG ("Added source: %s", GST_ELEMENT_NAME (downloader->priv->urisrc));

  gst_bin_add (GST_BIN(downloader), GST_ELEMENT (downloader->priv->urisrc));

  // We want to lock the state of the URI src to manage stuff ourselves
  gst_element_set_locked_state (GST_ELEMENT (downloader->priv->urisrc), TRUE);
  gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);

  if (!downloader->priv->set_uri) {
    // Link URI src with typefind
    gst_element_link (downloader->priv->urisrc, downloader->priv->typefind);
    // Get static src pad of URI src
    urisrcpad = gst_element_get_static_pad (downloader->priv->urisrc, "src");
    // Add probe to URI src pad
    downloader->priv->urisrcpad_probe_id = gst_pad_add_probe (urisrcpad, GST_PAD_PROBE_TYPE_ALL_BOTH, skippy_uri_downloader_src_probe, downloader, NULL);
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
    GST_DEBUG_OBJECT (downloader, "Setting range to %d - %d (Sending seek event to URI src)", (int) range_start, (int) range_end);
    seek = gst_event_new_seek (1.0,
      GST_FORMAT_BYTES, GST_SEEK_FLAG_NONE,
      GST_SEEK_TYPE_SET, range_start,
      GST_SEEK_TYPE_SET, range_end);
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
        GST_DEBUG ("Cache-Control set to max-age=0");
        gst_structure_set (extra_headers, "Cache-Control", G_TYPE_STRING,
            "max-age=0", NULL);
      }
      g_object_set (downloader->priv->urisrc, "extra-headers", extra_headers, NULL);
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
  if (downloader->priv->set_uri) {
    GstPad* urisrcpad;
    GST_DEBUG ("Unsetting URI source");

    // Flush start
    if (!downloader->priv->err) {
      GST_DEBUG_OBJECT (downloader, "Sending flush start");
      gst_element_send_event (GST_ELEMENT(downloader->priv->urisrc), gst_event_new_flush_start ());

      GST_DEBUG ("Setting source element to PAUSED state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
      gst_element_set_state (downloader->priv->urisrc, GST_STATE_PAUSED);

      GST_DEBUG_OBJECT (downloader, "Sending flush stop");
      gst_element_send_event (GST_ELEMENT(downloader->priv->urisrc), gst_event_new_flush_stop (TRUE));
    }

    // Now we can shut down the element
    GST_DEBUG ("Setting source element to READY state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
    gst_element_set_state (downloader->priv->urisrc, GST_STATE_READY);

    // In case of error set element to NULL
    if (downloader->priv->err) {
      /* set the element state to NULL if there was an error otherwise go back to READY state */
      GST_DEBUG ("Setting source element to NULL state (%s)", GST_ELEMENT_NAME (downloader->priv->urisrc));
      gst_element_set_state (downloader->priv->urisrc, GST_STATE_NULL);
    }
    // Set state flag that we have unset URI source
    downloader->priv->set_uri = FALSE;
  }
}

// Handle failure downloading in fetch function
// Download mutex is locked when this is called (only while fetch executes).
static SkippyUriDownloaderFetchReturn
skippy_uri_downloader_handle_failure (SkippyUriDownloader * downloader, GError ** err)
{
  skippy_uri_downloader_deinit_uri_src (downloader);
  // Check for error from internal bus
  if (downloader->priv->err) {
    // Copy error but our own one for internal processing
    GST_ERROR_OBJECT (downloader, "Error fetching URI: %s", downloader->priv->err->message);
    *err = g_error_copy (downloader->priv->err);

    // Did we miss anything?
    GST_DEBUG ("Error after loading %d bytes, missing %d bytes",
      (int) downloader->priv->bytes_loaded, (int) (downloader->priv->bytes_total - downloader->priv->bytes_loaded));
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

  // Let's first make sure we are completely reset, but pass in the current fragment
  // to eventually prepare to resume a previous broken download ...
  skippy_uri_downloader_reset (downloader, fragment);

  // Aquire download lock
  g_mutex_lock (&downloader->priv->download_lock);

  // Storing the current fragment info
  downloader->priv->fragment = g_object_ref (fragment);

  // Make sure we have our data source component set up and wired
  if (!skippy_uri_downloader_create_src (downloader, fragment->uri)) {
    g_mutex_unlock (&downloader->priv->download_lock);
    return SKIPPY_URI_DOWNLOADER_FAILED;
  }

  // If we were interrupted previously, resume at this point
  if (downloader->priv->previous_was_interrupted) {
    fragment->range_start = downloader->priv->bytes_loaded + 1;
    fragment->range_end = downloader->priv->bytes_total;
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
    GST_ERROR ("Failed setting URI src to PLAYING state");
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

  // Handle errors (even when completed data)
  if (downloader->priv->err) {
    g_mutex_unlock (&downloader->priv->download_lock);
    return skippy_uri_downloader_handle_failure (downloader, err);
  }

  // Cancellation (this is when we have been intendendly cancelled)
  if (fragment->cancelled) {
    g_mutex_unlock (&downloader->priv->download_lock);
    return SKIPPY_URI_DOWNLOADER_CANCELLED;
  }

  // Successful completion
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


