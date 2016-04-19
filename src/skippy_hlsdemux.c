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

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "skippy_hlsdemux.h"
#include "skippyHLS/skippy_hls.h"
#include "skippy_hls_priv.h"
#include "glib.h"

#define RETRY_TIME_BASE (500*GST_MSECOND)
#define RETRY_THRESHOLD 6 // when we switch from constant to exponential backoff retrial
#define RETRY_MAX_TIME_UNTIL (60*GST_SECOND)

// Must be doubles above zero
#define BUFFER_WATERMARK_HIGH_RATIO 0.5
#define BUFFER_WATERMARK_LOW_RATIO 0.5

#define DEFAULT_BUFFER_DURATION (30*GST_SECOND)
#define MIN_BUFFER_DURATION (10*GST_SECOND)

#define OPUS_FORMAT_PARAM "hls_opus_64_url"
#define MP3_FORMAT_PARAM "hls_mp3_128_url"
#define FORMAT_PARAM "format"
#define FORMAT_OPUS_PARAM "format=hls_opus_64_url"

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-hls"));

GST_DEBUG_CATEGORY_STATIC (skippy_hls_demux_debug);
#define GST_CAT_DEFAULT skippy_hls_demux_debug
#define SKIPPY_HLS_DOWNLOAD_AHEAD "skippy-download-ahead"

//NOTE: if an error is considered recoverable from category will be STREAM, code DEMUX.
#define REPORT_NON_FATAL_ERROR(el, text, debug) GST_ELEMENT_WARNING(el, STREAM, DEMUX, text, debug)
#define REPORT_FATAL_ERROR(el, domain, code, text, debug)  \
G_STMT_START {                                             \
  if (G_UNLIKELY(GST_ ## domain ## _ERROR == GST_STREAM_ERROR && GST_ ## domain ## _ERROR_ ## code == GST_STREAM_ERROR_DEMUX)) {     \
    g_warning ("For fatal errors please do not use domain STREAM and code DEMUX"); \
  }                                                         \
  else {                                                    \
    GST_ELEMENT_ERROR(el, domain, code, text, debug);       \
  }                                                         \
} G_STMT_END                                                \



typedef enum
{
  STAT_TIME_OF_FIRST_PLAYLIST,
  STAT_TIME_TO_PLAYLIST,
  STAT_TIME_TO_DOWNLOAD_FRAGMENT,
  STAT_CODEC_TYPE
} SkippyHLSDemuxStats;

/* GObject */
static void skippy_hls_demux_dispose (GObject * obj);
static void skippy_hls_demux_finalize (GObject * obj);

/* GstElement */
static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition);
static void
skippy_hls_demux_set_context (GstElement *element, GstContext *context);

/* SkippyHLSDemux */
static GstFlowReturn skippy_hls_demux_sink_data (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query);
static gboolean skippy_hls_demux_handle_seek (SkippyHLSDemux *demux, GstEvent * event);
static void skippy_hls_demux_stream_loop (SkippyHLSDemux * demux);
static void skippy_hls_demux_stop (SkippyHLSDemux * demux);
static void skippy_hls_demux_pause (SkippyHLSDemux * demux);
static void skippy_hls_demux_reset (SkippyHLSDemux * demux);
static void skippy_hls_demux_link_pads (SkippyHLSDemux * demux);
static gboolean skippy_hls_demux_refresh_playlist (SkippyHLSDemux * demux);
static GstFlowReturn skippy_hls_demux_proxy_pad_chain (GstPad *pad, GstObject *parent, GstBuffer *buffer);
static gboolean skippy_hls_demux_proxy_pad_event (GstPad *pad, GstObject *parent, GstEvent *event);

/* Utility functions */
static void skippy_hls_demux_append_query_param_to_hls_url (gchar **url, const gchar* query_param_name, const gchar* query_param_value);
static void http_replace_query_parameter(gchar **url, const gchar* query_param_name, const gchar* query_param_value);


#define skippy_hls_demux_parent_class parent_class
G_DEFINE_TYPE (SkippyHLSDemux, skippy_hls_demux, GST_TYPE_BIN);

// Set up our class
static void
skippy_hls_demux_class_init (SkippyHLSDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  gobject_class->dispose = skippy_hls_demux_dispose;
  gobject_class->finalize = skippy_hls_demux_finalize;

  element_class->change_state = GST_DEBUG_FUNCPTR (skippy_hls_demux_change_state);
  element_class->set_context = skippy_hls_demux_set_context;

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sinktemplate));

  gst_element_class_set_static_metadata (element_class,
      "HLS Client/Demuxer",
      "Codec/Demuxer/Adaptive",
      "HTTP Live Streaming Demuxer",
      "Stephan Hesse <stephan@soundcloud.com>\n"
      "Marc-Andre Lureau <marcandre.lureau@gmail.com>\n"
      "Andoni Morales Alastruey <ylatuya@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (skippy_hls_demux_debug, "skippyhlsdemux", 0,
      "Skippy HLS client");
}

// Creating the element with all the internal components whose lifetime will span across states
// See: _dispose and _finalize functions where we free all of this
static void
skippy_hls_demux_init (SkippyHLSDemux * demux)
{
  // Pads
  demux->srcpad = NULL;
  demux->sinkpad = gst_pad_new_from_static_template (&sinktemplate, "sink");

  demux->out_adapter = gst_adapter_new();

  // Configure sink pad
  gst_pad_set_chain_function (demux->sinkpad, GST_DEBUG_FUNCPTR (skippy_hls_demux_sink_data));
  gst_pad_set_event_function (demux->sinkpad, GST_DEBUG_FUNCPTR (skippy_hls_demux_sink_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  // Member objects
  demux->client = skippy_m3u8_client_new ();
  demux->playlist = NULL; // Storage for initial playlist
  demux->caps = NULL;

  // Internal elements
  demux->download_queue = gst_element_factory_make ("queue2", "skippyhlsdemux-download-queue");
  demux->queue_sinkpad = gst_element_get_static_pad (demux->download_queue, "sink");
  demux->downloader = skippy_uri_downloader_new (TRUE);
  demux->playlist_downloader = skippy_uri_downloader_new (FALSE);

  demux->queue_proxy_pad = gst_pad_new ("skippyhlsdemux-queue-proxy-pad", GST_PAD_SINK);
  gst_pad_set_element_private (demux->queue_proxy_pad, demux);
  gst_pad_set_chain_function (demux->queue_proxy_pad, skippy_hls_demux_proxy_pad_chain);
  gst_pad_set_event_function (demux->queue_proxy_pad, skippy_hls_demux_proxy_pad_event);

  // Add bin elements
  gst_bin_add (GST_BIN (demux), demux->download_queue);
  gst_bin_add (GST_BIN (demux), GST_ELEMENT(demux->downloader));
  gst_bin_add (GST_BIN (demux), GST_ELEMENT(demux->playlist_downloader));

  demux->need_segment = TRUE;
  demux->need_stream_start = TRUE;
  gst_segment_init (&demux->segment, GST_FORMAT_TIME);

  demux->download_ahead = DEFAULT_BUFFER_DURATION;
  demux->force_secure_hls = FALSE;
  
  demux->dataCodec = UNKNOWN;
  demux->opus_init_data = g_malloc (129);
  demux->opus_init_data_written = 0;
  demux->opus_0_fragment_cached = FALSE;

  // Thread
  g_cond_init (&demux->wait_cond);
  g_rec_mutex_init (&demux->stream_lock);
  demux->stream_task = gst_task_new ((GstTaskFunction) skippy_hls_demux_stream_loop, demux, NULL);
  gst_task_set_lock (demux->stream_task, &demux->stream_lock);
}

// Dispose: Remove everything we allocated in _init
// NOTE: Elements and pads that have been added to the element/bin are owned by the element itself (unreffed by the parent handler)
static void
skippy_hls_demux_dispose (GObject * obj)
{
  GST_DEBUG ("Disposing ...");

  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);

  skippy_hls_demux_reset (demux);
  skippy_hls_demux_stop (demux);

  G_OBJECT_CLASS (parent_class)->dispose (obj);

  // Remove M3U8 client
  if (demux->client) {
    skippy_m3u8_client_free (demux->client);
    demux->client = NULL;
  }

  // Release ref to queue sinkpad
  if (demux->queue_sinkpad) {
    g_object_unref (demux->queue_sinkpad);
    demux->queue_sinkpad = NULL;
  }

  // Clean up thread
  if (demux->stream_task) {
    gst_object_unref (demux->stream_task);
    demux->stream_task = NULL;
  }

  if (demux->queue_proxy_pad) {
    gst_object_unref (demux->queue_proxy_pad);
    demux->queue_proxy_pad = NULL;
  }

  if (demux->caps) {
    gst_caps_unref (demux->caps);
    demux->caps = NULL;
  }
  
  if (demux->opus_init_data) {
    g_free (demux->opus_init_data);
  }

  GST_DEBUG ("Done cleaning up.");
}

static void
skippy_hls_demux_finalize (GObject * obj)
{
  GST_DEBUG ("Finalizing ...");
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);
  g_rec_mutex_clear (&demux->stream_lock);
  g_cond_clear (&demux->wait_cond);
  G_OBJECT_CLASS (parent_class)->finalize (obj);
  GST_DEBUG ("Finalized.");
}

// This performs the transition from NULL -> READY (re-)allocating all the resources of this element,
// cleaning up from eventual previous state cycles
// and reconfigures the internal elements.
//
// MT-safe
static void
skippy_hls_demux_reset (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Re-setting element");

  GST_OBJECT_LOCK (demux);
  // Reset all our state fields
  demux->position = 0;
  demux->position_downloaded = 0;
  demux->download_failed_count = 0;
  demux->continuing = FALSE;

  // Get rid of eventual playlist data
  if (demux->playlist) {
    gst_buffer_unref (demux->playlist);
    demux->playlist = NULL;
  }

  if (demux->download_queue) {
    GST_OBJECT_UNLOCK (demux);
    // Download queue is unlimited
    g_object_set (demux->download_queue,
      "max-size-buffers", 0,
      "max-size-bytes", 0,
      "max-size-time", 0,
      "use-buffering", FALSE,
    NULL);
    GST_OBJECT_LOCK (demux);
  }
  GST_OBJECT_UNLOCK (demux);
}

// Called for state change from PAUSED -> READY and during seek handling
// Schedules the streaming thread to paused state, cancels all ongoing downloads,
// Locks&unlocks the streaming thread in order for this function to block until the
// thread is actually in paused state (task function exited)
//
// MT-safe
static void
skippy_hls_demux_pause (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Pausing task ...");
  // Pause the task
  gst_task_pause (demux->stream_task);
  // Signal the thread in case it's waiting
  GST_OBJECT_LOCK (demux);
  demux->continuing = TRUE;
  demux->download_failed_count = 0;
  GST_TASK_SIGNAL (demux->stream_task);
  g_cond_signal (&demux->wait_cond);
  GST_OBJECT_UNLOCK (demux);
  GST_DEBUG ("Checking for ongoing downloads to cancel ...");
  // Now cancel all downloads to make the stream function exit quickly in case there are some
  skippy_uri_downloader_interrupt (demux->downloader);
  skippy_uri_downloader_interrupt (demux->playlist_downloader);
  // Block until we're done cancelling
  g_rec_mutex_lock (&demux->stream_lock);
  g_rec_mutex_unlock (&demux->stream_lock);
  // Make sure these will handle the next download requested
  skippy_uri_downloader_continue (demux->downloader);
  skippy_uri_downloader_continue (demux->playlist_downloader);
  GST_DEBUG ("Paused streaming task");
}

// Called for state change from READY -> NULL
// This will stop & join the task (given it's not stopped yet) in a blocking way
// Which means the function will block until the task is stopped i.e streaming thread is joined.
// This function assumes the task is already in paused state (i.e task loop not running)
// Therefore calling this function should not imply any waiting for the task function to exit
// but is simply to allow eventual further deallocation of the task resources (unref'ing the task).
// However we can also just restart the task once it has been stopped.
//
// MT-safe
static void
skippy_hls_demux_stop (SkippyHLSDemux *demux)
{
  // Let's join the streaming thread
  GST_DEBUG ("Stopping task ...");
  if (gst_task_get_state (demux->stream_task) != GST_TASK_PAUSED) {
    skippy_hls_demux_pause(demux);
  }

  g_return_if_fail(gst_task_get_state (demux->stream_task) == GST_TASK_PAUSED);

  if (gst_task_get_state (demux->stream_task) != GST_TASK_STOPPED) {
    gst_task_join (demux->stream_task);
  }
  GST_DEBUG ("Stopped streaming task");
}

// The state change handler: simply calls the pause, stop and reset functions above
// and afterwards the parent handler
//
// MT-safe
static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (element);

  GST_DEBUG ("Performing transition: %s -> %s", gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT(transition)),
    gst_element_state_get_name (GST_STATE_TRANSITION_NEXT(transition)));

  switch (transition) {
    // Boot up
    case GST_STATE_CHANGE_NULL_TO_READY:
      // When we go from NULL to READY there is no concurrent function running with reset
      // Therefore no need to lock the object here
      skippy_hls_demux_reset (demux);
      break;
    // Start streaming thread
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    // Interrupt streaming thread
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      skippy_hls_demux_pause (demux);
      // Can be called while streaming thread is running
      break;
    // Shut down
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
skippy_hls_demux_set_context (GstElement *element, GstContext *context)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (element);

  const GstStructure* context_structure = gst_context_get_structure (context);

  GstClockTime buffer_ahead = 0;
  if (gst_structure_get_uint64 (context_structure, SKIPPY_HLS_DOWNLOAD_AHEAD, &buffer_ahead)) {
    demux->download_ahead = buffer_ahead;
  }

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

// Posts HLS statistics messages on the element bus
//
// MT-safe
static void
skippy_hls_demux_post_stat_msg (SkippyHLSDemux * demux, SkippyHLSDemuxStats metric, guint64 time_val, gsize size)
{
  GstStructure * structure = NULL;

  // Create message data
  switch (metric) {
  case STAT_TIME_TO_DOWNLOAD_FRAGMENT:
    GST_TRACE ("Statistic: STAT_TIME_TO_DOWNLOAD_FRAGMENT");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "fragment-download-time", G_TYPE_UINT64, time_val,
      "fragment-size", G_TYPE_UINT64, (guint64) size,
      NULL);
    break;
  case STAT_TIME_TO_PLAYLIST:
    GST_TRACE ("Statistic: STAT_TIME_TO_PLAYLIST");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "time-to-playlist", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  case STAT_TIME_OF_FIRST_PLAYLIST:
    GST_TRACE ("Statistic: STAT_TIME_OF_FIRST_PLAYLIST");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "manifest-download-start", GST_TYPE_CLOCK_TIME, GST_CLOCK_TIME_NONE,
      "manifest-download-stop", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
    case STAT_CODEC_TYPE:
      GST_TRACE ("Statistic: STAT_CODEC_TYPE");
      structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "codec-type", G_TYPE_UINT, size,
      NULL);
      break;
  default:
    GST_ERROR ("Can't post unknown stats type");
    return;
  }

  // Post the message on the bus
  gst_element_post_message (GST_ELEMENT_CAST (demux),
    gst_message_new_element (GST_OBJECT_CAST (demux), structure)
  );
}

// Queries current source URI from upstream element
// Returns NULL when query was not successful
// Caller owns returned pointer
//
// MT-safe
static gchar*
skippy_hls_demux_query_location (SkippyHLSDemux * demux)
{
  GstQuery* query = gst_query_new_uri ();
  gboolean ret = gst_pad_peer_query (demux->sinkpad, query);
  gchar *uri = NULL;

  if (ret) {
    gst_query_parse_uri (query, &uri);
  }
  gst_query_unref (query);
  return uri;
}

// Queries current playback position from downstream element
// Returns MAXINT64 when query was not successful (which might happen in some pipeline states)
//
// MT-safe
static gint64
skippy_hls_demux_query_position (SkippyHLSDemux * demux)
{
  GstFormat format;
  gint64 pos;
  GstQuery* query;
  gboolean query_ret;

  // Query current playback position from downstream
  query = gst_query_new_position (GST_FORMAT_TIME);
  query_ret = gst_pad_peer_query (demux->srcpad, query);
  if (query_ret) {
    gst_query_parse_position (query, &format, &pos);
    if (format != GST_FORMAT_TIME) {
      GST_ERROR ("Position query result is not in TIME format");
      query_ret = FALSE;
    }
  }
  gst_query_unref (query);

  if (!query_ret) {
    // If we didn't get a proper position we could be anywhere in the stream
    // and should assume it's MAX int in order to keep re-buffering going
    pos = GST_CLOCK_TIME_NONE;
  }
  GST_TRACE ("Position query result: %" GST_TIME_FORMAT, GST_TIME_ARGS ((GstClockTime) pos));
  return pos;
}

// Sets the duration field of our object according to the M3U8 parser output
// This function is called only from the first playlist handler. So only in the source thread.
//
// MT-safe
static void
skippy_hls_demux_update_duration (SkippyHLSDemux * demux)
{
  GstClockTime duration;

  // Only post duration message if non-live
  if (skippy_m3u8_client_is_live (demux->client)) {
    return;
  }
  duration = skippy_m3u8_client_get_total_duration (demux->client);

  GST_DEBUG_OBJECT (demux, "Playlist duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));

  // Post message
  if (duration == GST_CLOCK_TIME_NONE) {
    GST_ERROR ("Duration has invalid value, not posting message to pipeline");
    return;
  }
  gst_element_post_message (GST_ELEMENT (demux), gst_message_new_duration_changed (GST_OBJECT (demux)));
}

// This is called by the URL source (sinkpad) event handler on EOS to handle the initial playlist data
//
// MT-safe
static void
skippy_hls_demux_handle_first_playlist (SkippyHLSDemux* demux)
{
  gchar* uri = NULL;
  guint64 timestamp = (guint64) gst_util_get_timestamp ();
  SkippyHlsInternalError result = NO_ERROR;

  // Query the playlist URI
  uri = skippy_hls_demux_query_location (demux);
  if (!uri) {
    GST_ELEMENT_ERROR (demux, RESOURCE, NOT_FOUND, ("Failed querying the playlist URI"), (NULL));
    goto error;
  }
  GST_INFO_OBJECT (demux, "M3U8 location: %s", uri);

  // Parse main playlist - lock the object for this
  GST_OBJECT_LOCK (demux);

  if (G_UNLIKELY(demux->playlist == NULL)) {
    GST_OBJECT_UNLOCK (demux);
    GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("First playlist: Invalid M3U8 data (buffer=%p)", demux->playlist), (NULL));
    goto error;
  }

  result = skippy_m3u8_client_load_playlist (demux->client, uri, demux->playlist);

  switch (result) {
    case PLAYLIST_INCOMPLETE:
      GST_OBJECT_UNLOCK (demux);
      GST_ELEMENT_WARNING (demux, SKIPPY_HLS, PLAYLIST_INCOMPLETE_ON_LOAD, ("First playlist: Incomplete M3U8 data."), ("%s", skippy_m3u8_client_get_current_raw_data (demux->client)));
      goto error;
      break;
    case PLAYLIST_INVALID_UTF_CONTENT:
      GST_OBJECT_UNLOCK (demux);
      GST_ELEMENT_ERROR (demux, SKIPPY_HLS, PLAYLIST_INVALID_UTF_CONTENT, ("First playlist: Invalid M3U8 data (buffer=%p)", demux->playlist), (NULL));
      goto error;
      break;
    case NO_ERROR:
      break;
    default:
      g_warning ("Not expected return code. Please implement case branch for new return code");
      GST_OBJECT_UNLOCK (demux);
      goto error;
      break;
  }

  GST_OBJECT_UNLOCK (demux);

  // Sending stats message about first playlist fetch
  skippy_hls_demux_post_stat_msg (demux, STAT_TIME_OF_FIRST_PLAYLIST, timestamp, 0);

  // Updates duration field and posts message to bus
  skippy_hls_demux_update_duration (demux);

  GST_DEBUG_OBJECT (demux, "Finished setting up playlist");

  // Make sure URI downloaders are ready asap
  skippy_uri_downloader_prepare (demux->downloader, uri);
  skippy_uri_downloader_prepare (demux->playlist_downloader, uri);

  skippy_hls_demux_link_pads (demux);

  gst_task_start (demux->stream_task);
  GST_LOG ("Task started");

error:
  g_free (uri);
  return;
}

void skippy_hls_demux_update_downstream_events (SkippyHLSDemux *demux, gboolean stream_start, gboolean segment)
{
  gchar* stream_id = NULL;
  GstEvent* event = NULL;

  //GST_DEBUG ("Need segment: %d / Need stream-start: %d", demux->need_segment, demux->need_stream_start);

  if (stream_start && G_UNLIKELY(demux->need_stream_start)) {
    // Sending stream start event that we got on sink pad (sticky event)
    stream_id = gst_pad_create_stream_id (demux->srcpad, GST_ELEMENT_CAST (demux), NULL);
    // Create stream start event from stream ID that we parsed from sink pad
    event = gst_event_new_stream_start (stream_id);
    gst_event_set_group_id (event, gst_util_group_id_next ());
    demux->need_stream_start = FALSE;
    GST_DEBUG ("Sending %" GST_PTR_FORMAT, event);
    gst_pad_send_event (demux->queue_sinkpad, event);
    if (demux->caps) {
      skippy_hls_demux_post_stat_msg (demux, STAT_CODEC_TYPE, 0, demux->dataCodec);
    }
  }

  // This is TRUE if we have modified the segment or if its the very first buffer we issue
  if (segment && G_UNLIKELY(demux->need_segment)) {

    g_return_if_fail (demux->caps);

    GST_OBJECT_LOCK (demux);
    event = gst_event_new_caps (demux->caps);
    GST_OBJECT_UNLOCK (demux);
    gst_pad_send_event (demux->queue_sinkpad, event);

    event = gst_event_new_segment (&demux->segment);
    GST_OBJECT_LOCK (demux);
    demux->need_segment = FALSE;
    GST_OBJECT_UNLOCK (demux);
    GST_DEBUG ("Sending %" GST_PTR_FORMAT, event);
    gst_pad_send_event (demux->queue_sinkpad, event);
  }

  g_free (stream_id);
}

// Called when we want to link the downloader to the queue pads
//
// MT-safe
static void
skippy_hls_demux_link_pads (SkippyHLSDemux * demux)
{
  GstPad *downloader_srcpad, *queue_srcpad, *srcpad;
  GstPadTemplate* templ;

  GST_DEBUG ("Linking pads...");

  // Link downloader -> floating sink pad
  downloader_srcpad = gst_element_get_static_pad (GST_ELEMENT(demux->downloader), "src");
  if (gst_pad_link (downloader_srcpad, demux->queue_proxy_pad) != GST_PAD_LINK_OK) {
    // In case this ever happens: This will abort the process in test conditions
    g_critical ("Error while linking downloader src pad to floating sink pad");
  }
  gst_object_unref (downloader_srcpad);

  // Set our srcpad
  queue_srcpad = gst_element_get_static_pad (demux->download_queue, "src");
  templ = gst_static_pad_template_get (&srctemplate);
  srcpad = gst_ghost_pad_new_from_template ("src_0", queue_srcpad, templ);
  gst_object_unref (queue_srcpad);
  gst_object_unref (templ);

  // Configure external source pad
  gst_pad_set_active (srcpad, TRUE);

  gst_pad_set_active (demux->queue_proxy_pad, TRUE);
  // Set event & query handlers for downstream pads
  gst_pad_set_event_function (srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_event));
  gst_pad_set_query_function (srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_query));
  // Add pad to element
  gst_element_add_pad (GST_ELEMENT (demux), srcpad);
  gst_element_no_more_pads (GST_ELEMENT (demux));

  // Only set this once the pad is fully set up
  GST_OBJECT_LOCK (demux);
  demux->srcpad = srcpad;
  GST_OBJECT_UNLOCK (demux);

  GST_DEBUG ("Added src pad");
}

/* Handling data from the sink pad
*  @param pad - our sink pad
*  @param parent - our element
*  @param buf - the buffer we are handling
*  @return - TRUE if the event was handled
*
*  Calling thread: source
*
*  This function will not handle data if the streaming thread is running
*  MT-safe
*/
static GstFlowReturn
skippy_hls_demux_sink_data (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  GST_OBJECT_LOCK (demux);
  if (demux->playlist == NULL) {
    demux->playlist = buf;
  } else {
    demux->playlist = gst_buffer_append (demux->playlist, buf);
  }
  GST_OBJECT_UNLOCK (demux);

  return GST_FLOW_OK;
}

/* Handling events from the sink pad
*  @param pad - our sink pad
*  @param parent - our element
*  @param event - the event we are handling
*  @return - TRUE if the event was handled
*
*  Calling thread: source
*
*  This function will not handle events if the streaming thread is running
*  MT-safe
*/
static gboolean
skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux;

  demux = SKIPPY_HLS_DEMUX (parent);

  GST_DEBUG ("Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
  case GST_EVENT_EOS:
    GST_DEBUG ("Got EOS on the sink pad: main playlist fetched");
    // Stream loop should not be running when this is called
    skippy_hls_demux_handle_first_playlist (demux);
    break;
  default:
    break;
  }

  // Swallow new segments, we'll push our own
  gst_event_unref (event);
  return TRUE;
}

// Called upon source pad events i.e seeking
//
// MT-safe
static gboolean
skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  GST_TRACE_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_SEEK:
      return skippy_hls_demux_handle_seek (demux, event);
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

// Handles seek events: Pauses the streaming thread, seeks the M3U8 parser to correct position,
// modifies segment data in downloader element, set seeked flag, sends flush events onto output queue,
// then restarts streaming thread.
//
// MT-safe
static gboolean
skippy_hls_demux_handle_seek (SkippyHLSDemux *demux, GstEvent * event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;

  // Parse seek event
  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
  if (format != GST_FORMAT_TIME) {
    GST_WARNING ("Received seek event not in time format");
    gst_event_unref (event);
    return FALSE;
  }

  GST_INFO ("Handling seek event to: %" GST_TIME_FORMAT, GST_TIME_ARGS(start));

  // NOTE: The order of sending flush start/stop and pausing the task in between in MANDATORY !!

  GST_DEBUG_OBJECT (demux, "Sending flush start");
  gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_start ());

  // Pausing streaming task (blocking)
  skippy_hls_demux_pause (demux);
  // At this point we can be sure the stream loop is paused

  GST_DEBUG_OBJECT (demux, "Sending flush stop");
  gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_stop (TRUE));

  // Seek on M3U8 data model
  skippy_m3u8_client_seek_to (demux->client, (GstClockTime) start);

  // Update downloader segment after seek
  gst_segment_do_seek (&demux->segment, rate, format, flags, start_type, start, stop_type, stop, NULL);

  demux->need_segment = TRUE;
  
  //demux->opus_init_data_written = 0;

  // Restart the streaming task
  GST_DEBUG ("Restarting streaming task");
  gst_task_start (demux->stream_task);

  // Handle and swallow event
  gst_event_unref (event);
  return TRUE;
}

// Handles duration, URI and seeking queries: only access MT-safe M3U8 client to do this
//
// MT-safe
static gboolean
skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);
  gboolean ret = FALSE;
  GstFormat fmt;
  gint64 stop = -1;
  GstClockTime duration;
  gchar* uri;

  GST_TRACE ("Got %" GST_PTR_FORMAT, query);

  if (query == NULL)
    return FALSE;
  switch (query->type) {
    case GST_QUERY_DURATION:
      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt != GST_FORMAT_TIME) {
        GST_WARNING ("Can't process duration query that is not in time format");
        break;
      }
      duration = skippy_m3u8_client_get_total_duration (demux->client);
      if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
        gst_query_set_duration (query, GST_FORMAT_TIME, duration);
        GST_TRACE_OBJECT (demux, "Duration query: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));
        ret = TRUE;
      } else {
        GST_WARNING ("Bad duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (duration));
      }
      break;
    case GST_QUERY_URI:
      uri = skippy_m3u8_client_get_uri (demux->client);
      gst_query_set_uri (query, uri);
      g_free (uri);
      ret = TRUE;
      break;
    case GST_QUERY_SEEKING:
      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      if (fmt != GST_FORMAT_TIME) {
        GST_DEBUG ("Can't process seeking query that is not in time format");
        break;
      }
      duration = skippy_m3u8_client_get_total_duration (demux->client);
      if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
        stop = duration;
        gst_query_set_seeking (query, fmt, !skippy_m3u8_client_is_live (demux->client), 0, stop);
        GST_INFO_OBJECT (demux, "Seeking query stop time: %" GST_TIME_FORMAT, GST_TIME_ARGS (stop));
        ret = TRUE;
      } else {
        GST_DEBUG ("Can't process seeking query that is not in time format");
      }
      break;
    default:
      /* Don't fordward queries upstream because of the special nature of this
       * "demuxer", which relies on the upstream element only to be fed with the
       * first playlist */
      break;
  }
  return ret;
}

// Handles end of playlist: Sets streaming thread to paused state and pushes EOS event
//
// MT-safe
static void
skippy_hls_handle_end_of_playlist (SkippyHLSDemux * demux)
{
  GST_DEBUG_OBJECT (demux, "Reached end of playlist, sending EOS");
  GST_OBJECT_LOCK (demux);
  demux->position = 0;
  demux->position_downloaded = 0;
  GST_OBJECT_UNLOCK (demux);
  gst_task_pause (demux->stream_task);
  gst_pad_send_event (demux->queue_sinkpad, gst_event_new_eos ());
}

// Simply wraps caching allowed flag of M3U8 manifest to eventually add custom policy
//
// MT-safe
static gboolean
skippy_hls_demux_is_caching_allowed (SkippyHLSDemux * demux)
{
  gboolean ret;
  ret = skippy_m3u8_client_is_caching_allowed (demux->client);
  return ret;
}

static GstFlowReturn
skippy_hls_demux_proxy_pad_chain (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  GST_TRACE ("Got %" GST_PTR_FORMAT, buffer);

  GstFlowReturn ret_value = GST_FLOW_ERROR;
  gboolean set_discont = FALSE, first_buffer_processed = FALSE;
  GstClockTime buffer_pts = GST_CLOCK_TIME_NONE;
  GstBuffer *buf;
  gsize avail_out_size = 0;
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (gst_pad_get_element_private (pad));

  GST_OBJECT_LOCK (demux);
  if (G_UNLIKELY(demux->need_segment)) {
    buffer_pts = demux->position;
    if (!demux->need_stream_start) {
      set_discont = TRUE;
    }
  } else {
    set_discont = FALSE;
  }
  // check opus init
  if (demux->dataCodec == OPUS) {
    if (!(demux->opus_0_fragment_cached = (demux->opus_init_data_written >= 129))) {
      if (buffer != NULL) {
        GstMapInfo in_map;
        gst_buffer_map(buffer, &in_map, GST_MAP_READ);
        size_t number_of_bytes_to_write = (129 - demux->opus_init_data_written > in_map.size) ? in_map.size : 129 - demux->opus_init_data_written;
        memcpy (demux->opus_init_data + demux->opus_init_data_written, in_map.data, number_of_bytes_to_write);
        demux->opus_init_data_written += number_of_bytes_to_write;
      }
    }
  }
  GST_OBJECT_UNLOCK (demux);

  // first send eventual events upfront data
  skippy_hls_demux_update_downstream_events (demux, TRUE, TRUE);

  // now push the data chunked
  gst_adapter_push(demux->out_adapter, buffer);
  
  while ((avail_out_size = gst_adapter_available(demux->out_adapter))) {
    buf = gst_adapter_take_buffer(demux->out_adapter, avail_out_size > 4096 ? 4096 : avail_out_size);
    if (G_UNLIKELY(!first_buffer_processed)) {
      first_buffer_processed = TRUE;
      // set proper discont flag and time stamp if needed for the first buffer
      if (set_discont) {
        GstBuffer *fake_buffer = gst_buffer_new();
        GST_BUFFER_FLAG_SET (fake_buffer, GST_BUFFER_FLAG_DISCONT);
        GST_BUFFER_PTS(fake_buffer) = buffer_pts;
        gst_pad_chain (demux->queue_sinkpad, fake_buffer);
        
        //GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
        GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
      } else {
        GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
      }
      // set pts for buf
      GST_BUFFER_PTS(buf) = buffer_pts;
    } else {
      // for all subsequent buffers unset discont flag and mark PTS
      // as GST_CLOCK_TIME_NONE (continous data)
      GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
      GST_BUFFER_PTS(buf) =  GST_CLOCK_TIME_NONE;
    }
    ret_value = gst_pad_chain (demux->queue_sinkpad, buf);
    switch(ret_value) {
    case GST_FLOW_OK:
      GST_LOG ("Proxy pad was %s while invoking queue chain function", gst_flow_get_name (ret_value));
    default:
      break;
    }
  }
  return ret_value;
}

static gboolean
skippy_hls_demux_proxy_pad_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  GST_DEBUG ("Got %" GST_PTR_FORMAT, event);

  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (gst_pad_get_element_private (pad));
  GstCaps *caps;
  static GstStaticCaps opus_caps = GST_STATIC_CAPS ("audio/ogg");
  switch (event->type) {
  case GST_EVENT_CAPS:
    GST_OBJECT_LOCK (demux);
    if (demux->caps) {
      gst_caps_unref (demux->caps);
    }
    gst_event_parse_caps (event, &caps);
    demux->caps = gst_caps_copy (caps);
    
    if (gst_caps_can_intersect (gst_static_caps_get (&opus_caps), demux->caps)) {
      demux->dataCodec = OPUS;
    } else {
      demux->dataCodec = MP3;
    }
    GST_OBJECT_UNLOCK (demux);
  default:
    break;
  }

  gst_event_unref (event);
  return TRUE;
}

// Refreshes playlist - only called from streaming thread
//
// MT-safe
static gboolean
skippy_hls_demux_refresh_playlist (SkippyHLSDemux * demux)
{
  SkippyFragment *download;
  GstBuffer *buf = NULL;
  GError* err = NULL;
  SkippyUriDownloaderFetchReturn fetch_ret;
  gboolean ret = FALSE;
  gchar *current_playlist = skippy_m3u8_client_get_current_playlist (demux->client);
  SkippyHlsInternalError load_playlist_result = NO_ERROR;

  if (!current_playlist) {
    return FALSE;
  }

  if (demux->force_secure_hls) {
    skippy_hls_demux_append_query_param_to_hls_url (&current_playlist, "secure", "true");
  }
  
  if (demux->caps) {
    const char* format = demux->dataCodec == OPUS ? OPUS_FORMAT_PARAM : MP3_FORMAT_PARAM;
    http_replace_query_parameter (&current_playlist, FORMAT_PARAM, format);
  }
  
  // Create a download
  download = skippy_fragment_new (current_playlist);
  download->start_time = 0;
  download->stop_time = skippy_m3u8_client_get_total_duration (demux->client);

  // Download it
  fetch_ret = skippy_uri_downloader_fetch_fragment (demux->playlist_downloader,
    download, // Media fragment to load
    current_playlist, // Referrer
    TRUE, // Compress (good for playlists)
    TRUE, // Refresh (wipe out cached stuff)
    skippy_hls_demux_is_caching_allowed (demux), // Allow caching directive
    &err // Error
  );

  // Handle fetch result
  switch (fetch_ret) {
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_PLAYLIST, download->download_stop_time - download->download_start_time, 0);
    // Load M3U8 buffer into parser
    buf = skippy_uri_downloader_get_buffer (demux->playlist_downloader);

    g_clear_error (&err);

    load_playlist_result = skippy_m3u8_client_load_playlist (demux->client, current_playlist, buf);

    if (G_UNLIKELY(load_playlist_result != NO_ERROR)) {
      if (load_playlist_result == PLAYLIST_INCOMPLETE) {
        GST_ELEMENT_WARNING (demux, SKIPPY_HLS, PLAYLIST_INCOMPLETE_ON_REFRESH, ("While refreshing playlist: Incomplete M3U8 data."), ("%s", skippy_m3u8_client_get_current_raw_data (demux->client)));
        demux->force_secure_hls = TRUE;
      }
      else {
        GST_ELEMENT_ERROR (demux, SKIPPY_HLS, PLAYLIST_INVALID_UTF_CONTENT, ("While refreshing playlist: Invalid M3U8 data (buffer: %p)", buf), (NULL));
      }
      ret = FALSE;
    }
    else {
      ret = TRUE;
    }
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
      if (g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND)) {
        // playlist not found - if we have opus set as parm - threat this as recovery needed - try with mp3
        if (strstr(current_playlist, FORMAT_OPUS_PARAM)) {
          GST_WARNING ("Got 403 while refreshing playlist. Specified media format is not available anymore");
          GST_ELEMENT_WARNING (demux, SKIPPY_HLS, UNSUPPORTED_MEDIA_FORMAT, ("Media format not supported."), (NULL));
          gst_task_pause (demux->stream_task);
        }
      }
      ret = FALSE;
      break;
      
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
  case SKIPPY_URI_DOWNLOADER_VOID:
    if (err) {
      GST_ERROR ("Error updating playlist: %s", err->message);
    }
    ret = FALSE;
    break;
  }

  if (buf) {
    gst_buffer_unref (buf);
  }

  g_clear_error (&err);
  g_free (current_playlist);
  return ret;
}

static GstClockTime
skippy_hls_demux_get_time_until_retry_locked (SkippyHLSDemux * demux)
{
  double power;
  double retry_timer = RETRY_TIME_BASE;

  if (demux->download_failed_count >= RETRY_THRESHOLD) {
    //
    // Use this JavaScript function in a console to test the series of ouputs with a 500 ms basetime or to tune the setting here
    // backoff = function(fails, threshold) { if (fails >= threshold) { return 500 * Math.exp (fails/6) / Math.E } else { return 500; } }
    // Below is the same algorithm
    power = ((double) demux->download_failed_count) / ((double) RETRY_THRESHOLD);
    retry_timer = RETRY_TIME_BASE * (exp ( power ) / M_E);
    // Cap the value
    if (retry_timer > RETRY_MAX_TIME_UNTIL) {retry_timer = RETRY_MAX_TIME_UNTIL;}
  }

  return (GstClockTime) retry_timer;
}

// Waits for task condition with timeout - Unlocks the GST object mutex, expects it to be locked
// @param max_wait Max time this should wait in Gstreamer clock time units
static void
skippy_hls_stream_loop_wait_locked (SkippyHLSDemux * demux, GstClockTime max_wait)
{
  // Monotonic time in GLib is in useconds
  gint64 max_wait_us = ( ( (gint64) max_wait) / GST_USECOND );
  while (!demux->continuing) {
    GST_DEBUG ("Will wait for a max time of %" GST_TIME_FORMAT, GST_TIME_ARGS (max_wait));
    if (!g_cond_wait_until(&demux->wait_cond, GST_OBJECT_GET_LOCK (demux),
      g_get_monotonic_time () + max_wait_us)) {
      GST_TRACE ("Waiting timed out now");
      break;
    } else {
      GST_TRACE ("Wait got interrupted");
    }
  }
  GST_TRACE ("Continuing stream task now");
}

// Checks wether we should download another segment with respect to buffer size.
// Only runs in the streaming thread.
//
// MT-safe
static gboolean
skippy_hls_check_buffer_ahead (SkippyHLSDemux * demux)
{
  GstClockTime pos, max_buffer_duration, max_wait = 0;

  // Check if we are linked yet (did we receive a proper playlist?)
  GST_OBJECT_LOCK (demux);
  if (!demux->srcpad) {
    g_warning ("No src pad (didn't get any M3U8 data yet probably), will wait for it.");
    skippy_hls_stream_loop_wait_locked (demux, (GstClockTime) G_MAXINT64);
    demux->continuing = FALSE;
    GST_OBJECT_UNLOCK (demux);
    return FALSE;
  }


  // Check if wait condition is enabled - if not we can just continue
  if (demux->continuing) {
    GST_OBJECT_UNLOCK (demux);
    // Continue downloading
    return TRUE;
  }
  GST_OBJECT_UNLOCK (demux);

  // Check upfront position relative to stream position
  // If we branch here this means we might want to wait
  pos = skippy_hls_demux_query_position (demux);
  max_buffer_duration = demux->download_ahead;

  GST_DEBUG ("Playback position is %" GST_TIME_FORMAT " , Max buffer duration is %" GST_TIME_FORMAT ", Queued position is %" GST_TIME_FORMAT,
    GST_TIME_ARGS (pos), GST_TIME_ARGS(max_buffer_duration), GST_TIME_ARGS (demux->position_downloaded));

  // Check for wether we should limit downloading
  if (pos != GST_CLOCK_TIME_NONE && max_buffer_duration != GST_CLOCK_TIME_NONE
    && demux->position_downloaded > pos + max_buffer_duration) {
    // Diff' between current playhead and buffer-head in microseconds
    max_wait = demux->position_downloaded - pos - max_buffer_duration;
    GST_TRACE ("Waiting in task as we have preloaded enough (until %" GST_TIME_FORMAT " of media position)",
      GST_TIME_ARGS (demux->position_downloaded));
    // Timed-cond wait here
    GST_OBJECT_LOCK (demux);
    skippy_hls_stream_loop_wait_locked (demux, max_wait);
    GST_OBJECT_UNLOCK (demux);
    return FALSE;
  }

  // No waiting needed
  return TRUE;
}

static gboolean
skippy_hlsdemux_proxy_pad_remove_sticky_eos (GstPad * pad, GstEvent ** event, gpointer udata)
{
  if (GST_EVENT_TYPE (*event) == GST_EVENT_EOS) {
    gst_event_replace (event, NULL);
    return FALSE;
  }
  return TRUE;
}

static void
skippy_hlsdemux_proxy_pad_reset (SkippyHLSDemux *demux)
{
  //flush the proxy pad to reset EOS state.
  gst_pad_push_event (demux->queue_proxy_pad, gst_event_new_flush_start ());
  gst_pad_push_event (demux->queue_proxy_pad, gst_event_new_flush_stop (TRUE));
  //remove sticky EOS events
  gst_pad_sticky_events_foreach (demux->queue_proxy_pad, skippy_hlsdemux_proxy_pad_remove_sticky_eos, NULL);
  GST_OBJECT_FLAG_UNSET (demux->queue_proxy_pad, GST_PAD_FLAG_EOS);
}


static void
skippy_hlsdemux_opus_push_0_segment (SkippyHLSDemux *demux, gboolean is_discont) {
  GstMapInfo info_map;
  GstBuffer* opus_head_buffer = gst_buffer_new_and_alloc (129);
  gst_buffer_map (opus_head_buffer, &info_map, GST_MAP_READWRITE);
  memcpy (info_map.data, demux->opus_init_data, 129);
  info_map.size = 129;
  gst_buffer_unmap (opus_head_buffer, &info_map);
  GST_BUFFER_FLAG_SET (opus_head_buffer, GST_BUFFER_FLAG_DISCONT);
  GST_BUFFER_PTS (opus_head_buffer) = demux->position;
  skippy_hls_demux_proxy_pad_chain(demux->queue_proxy_pad, NULL, opus_head_buffer);
}

// Streaming task function - implements all the HLS logic.
// When this runs the streaming task mutex is/must be locked.
//
// MT-safe
static void
skippy_hls_demux_stream_loop (SkippyHLSDemux * demux)
{
  SkippyFragment *fragment = NULL, *current_opus_fragment = NULL;
  SkippyUriDownloaderFetchReturn fetch_ret = SKIPPY_URI_DOWNLOADER_VOID;
  GError *err = NULL;
  gchar* referrer_uri = NULL;
  gboolean playlist_refresh = FALSE;
  gboolean opus_need_head  = FALSE;
  GstClockTime time_until_retry;

  GST_TRACE_OBJECT (demux, "Entering stream task");

  // Check current playback position against buffer levels
  // Blocks and schedules timed-cond until next download
  // Might be interrupted by a seek event and continue
  if (!skippy_hls_check_buffer_ahead (demux)) {
    return;
  }
  GST_TRACE ("Will try to fetch next fragment ...");

  //g_usleep (1000*1000);

  // Get next fragment from M3U8 list
  referrer_uri = skippy_m3u8_client_get_uri (demux->client);
  
  
  fragment = skippy_m3u8_client_get_current_fragment (demux->client);
  
  if (demux->dataCodec == OPUS) {
    current_opus_fragment = skippy_m3u8_client_get_current_fragment (demux->client);
    // when we seek we first want to make sure that 0 segment is pushed
    if (demux->need_segment && !demux->need_stream_start) {
      if (demux->opus_0_fragment_cached) {
        // if 0 segment is already buffered push it directly
        demux->position = current_opus_fragment->start_time;
        skippy_hlsdemux_opus_push_0_segment(demux, TRUE);
      } else {
        // we are seeking but 0 segment is not buffered, so stream loop should
        // fetch it
        demux->opus_init_data_written = 0;
        opus_need_head = TRUE;
        fragment = skippy_m3u8_client_get_fragment (demux->client, 0);
      }
    } else {
      // not seeking but we did not cache the whole 0 segment
      // this may happen if download was canceled etc..
      if (demux->opus_init_data_written != 129) {
        // in this case stream loop should fetch 0 segment
        demux->opus_init_data_written = 0;
        opus_need_head = TRUE;
        fragment = skippy_m3u8_client_get_fragment (demux->client, 0);
      }
    }
  }
  
  if (fragment) {
    GST_OBJECT_LOCK (demux);
    if (opus_need_head) {
      demux->position = current_opus_fragment->start_time;
    } else {
      demux->position = fragment->start_time;
    }
    GST_OBJECT_UNLOCK (demux);
    

    GST_INFO_OBJECT (demux, "Pushing data for next fragment: %s (Byte-Range=%" G_GINT64_FORMAT " - %" G_GINT64_FORMAT ")",
      fragment->uri, fragment->range_start, fragment->range_end);
    // Tell downloader to push data
    fetch_ret = skippy_uri_downloader_fetch_fragment (demux->downloader,
      fragment, // Media fragment to load
      referrer_uri, // Referrer
      FALSE, // Compress (useless with coded media data)
      FALSE, // Refresh disabled (don't wipe out cache)
      skippy_hls_demux_is_caching_allowed (demux), // Allow caching directive
      &err
    );
    skippy_hlsdemux_proxy_pad_reset (demux);
  } else {
    GST_INFO_OBJECT (demux, "This playlist doesn't contain more fragments");
  }

  GST_TRACE ("Returning finished fragment");

  // Handle result from current attempt
  switch (fetch_ret) {
  // This case means the download did not do anything
  case SKIPPY_URI_DOWNLOADER_VOID:
    // Error & fragment should be NULL
    skippy_hls_handle_end_of_playlist (demux);
    break;
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
    GST_DEBUG ("Fragment fetch got cancelled on purpose");
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
    // When failed
    //TODO: remove this check once we make sure Error instance is initialized in all cases when download fails
    if (!err) {
      g_warning ("Error not set but download failed!");
      if (fragment) {
        g_object_unref (fragment);
      }
      g_free (referrer_uri);
      return;
    }
    GST_INFO ("Fragment fetch error: %s", err->message);
    // Actual download failure
    GST_OBJECT_LOCK (demux);
    demux->download_failed_count++;
    GST_DEBUG ("Failed to fetch fragment for %d times.", demux->download_failed_count);
    GST_OBJECT_UNLOCK (demux);
    if (g_error_matches (err, GST_STREAM_ERROR, GST_STREAM_ERROR_WRONG_TYPE)) {
      REPORT_FATAL_ERROR (demux, STREAM, WRONG_TYPE, ("%s", err->message), (NULL));
      gst_task_pause (demux->stream_task);
      goto end_stream_loop;
    }
    // We only want to refetch the playlist if we get a 403 or a 404
    if (g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)) {
      GST_OBJECT_LOCK (demux);
      demux->download_forbidden_count++;
      GST_OBJECT_UNLOCK (demux);
      GST_DEBUG_OBJECT (demux,
        "Updating the playlist because of 403 or 404 (happened %d times in a row)",
        (int) demux->download_forbidden_count);

      // This should only happen once in a row - if we detect a broken M3U8, notify the application
      if (demux->download_forbidden_count > 1) {
        REPORT_NON_FATAL_ERROR(demux,
        ("M3U8 data seems to be corrupt as it results in permanent 403s. Broken URL: %s, Failure count: %d, Error: %s",
          fragment->uri, (int) demux->download_forbidden_count, err->message),
        ("\n\n%s\n\n", skippy_m3u8_client_get_current_raw_data (demux->client)));
      }
      skippy_hls_demux_refresh_playlist (demux);
      playlist_refresh = TRUE;
    }
    break;
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    GST_DEBUG ("Fragment download completed successfully");
    // Post stats message
    skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_DOWNLOAD_FRAGMENT,
      fragment->download_stop_time - fragment->download_start_time, fragment->size);
    // Reset failure counter, position and scheduling condition
    GST_OBJECT_LOCK (demux);
    if (!opus_need_head) {
      demux->position_downloaded = fragment->stop_time;
      demux->download_failed_count = 0;
      demux->download_forbidden_count = 0;
      demux->continuing = FALSE;
      // Go to next fragment
      skippy_m3u8_client_advance_to_next_fragment (demux->client);
    }
    opus_need_head = FALSE;
    GST_OBJECT_UNLOCK (demux);
    break;
  }

  GST_TRACE_OBJECT (demux, "Exiting task now ...");

  // Handle error
  if (err && !playlist_refresh) {
    GST_OBJECT_LOCK (demux);
    time_until_retry = skippy_hls_demux_get_time_until_retry_locked (demux);
    GST_DEBUG ("Next retry scheduled in: %" GST_TIME_FORMAT, GST_TIME_ARGS (time_until_retry));
    // Waits before retrying - might be interrupted by a PAUSED -> PLAYING transition or by a seek event
    demux->continuing = FALSE;
    skippy_hls_stream_loop_wait_locked (demux, time_until_retry);
    // If there was an error we should not schedule but retry right away
    demux->continuing = TRUE;
    GST_OBJECT_UNLOCK (demux);
  }

end_stream_loop:
  // Unref current fragment
  if (fragment) {
    g_object_unref (fragment);
  }
  if (current_opus_fragment) {
    g_object_unref (current_opus_fragment);
  }
  g_free (referrer_uri);
  g_clear_error (&err);
}

static
void skippy_hls_demux_append_query_param_to_hls_url (gchar **url, const gchar* query_param_name, const gchar* query_param_value)
{
  gchar* old_url = *url;
  const gchar* delimiter = (g_strrstr(*url, "?")) ? "&": "?";
  *url = g_strconcat (*url, delimiter, query_param_name, "=", query_param_value, NULL);
  g_free (old_url);
}

static void http_replace_query_parameter(gchar **url, const gchar* query_param_name, const gchar* query_param_value)
{
  gchar **uri_parts = g_strsplit (*url, "?",2);
  gchar **parameters = NULL;
  gchar **index = uri_parts;
  GSList *params_list = NULL, *iterator = NULL;
  gboolean param_processed = FALSE;
  gchar *new_param = g_strconcat(query_param_name, "=", query_param_value, NULL);
  int i = 0;
  for (; *index != NULL; index++, i++) {
    if (i == 1) {
      parameters = g_strsplit (*index, "&", 0);
      gchar **param_index = parameters;
      for (; *param_index != NULL; param_index++) {
        if (g_str_has_prefix (*param_index, query_param_name) &&
            (*param_index)[strlen(query_param_name)] == '=') {
          if (!param_processed) {
            params_list = g_slist_prepend (params_list, new_param);
            param_processed = TRUE;
          }
        } else {
          params_list = g_slist_prepend (params_list, *param_index);
        }
      }
    }
  }
  if (!param_processed) {
    params_list = g_slist_prepend (params_list, new_param);
  }
  gchar* old_url = *url;
  *url = g_strconcat (*uri_parts, "?", NULL);
  g_free (old_url);
  for (iterator = params_list; iterator; iterator = iterator->next) {
    old_url = *url;
    if (iterator == params_list) {
      *url = g_strconcat (*url, iterator->data, NULL);
    } else {
      *url = g_strconcat (*url, "&", iterator->data, NULL);
    }
    g_free (old_url);
  }
  g_slist_free (params_list);
  g_strfreev (parameters);
  g_strfreev (uri_parts);
  g_free (new_param);
}


void skippy_hlsdemux_setup (guint hls_demux_rank)
{
  gst_element_register (NULL, "skippyhlsdemux", hls_demux_rank,
      TYPE_SKIPPY_HLS_DEMUX);
}

GQuark skippy_hls_error_quark(void) {
  static GQuark skippy_hls_quark;
  if (!skippy_hls_quark) {
    skippy_hls_quark = g_quark_from_static_string ("skippy-hls-error-quark");
  }
  return skippy_hls_quark;
}
