/* skippyHLS
 *
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
/**
 * SECTION:element-hlsdemux
 *
 * HTTP Live Streaming demuxer element.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch souphttpsrc location=http://devimages.apple.com/iphone/samples/bipbop/gear4/prog_index.m3u8 ! hlsdemux ! decodebin2 ! videoconvert ! videoscale ! autovideosink
 * ]|
 * </refsect2>
 *
 * Last reviewed on 2010-10-07
 */

#include <string.h>

#include <skippyHLS/skippy_hlsdemux.h>

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-hls"));

GST_DEBUG_CATEGORY_STATIC (skippy_hls_demux_debug);
#define GST_CAT_DEFAULT skippy_hls_demux_debug

enum
{
  PROP_0,
  PROP_BUFFER_AHEAD_DURATION,
  PROP_BITRATE_LIMIT,
  PROP_CONNECTION_SPEED,
  PROP_CACHING_ENABLED,
  PROP_LAST
};

typedef enum
{
  STAT_TIME_OF_FIRST_PLAYLIST,
  STAT_TIME_TO_PLAYLIST,
  STAT_TIME_TO_DOWNLOAD_FRAGMENT,
} SkippyHLSDemuxStats;

#define DEFAULT_BITRATE_LIMIT 0.8
#define DEFAULT_CONNECTION_SPEED 0
#define DEFAULT_BUFFER_AHEAD_DURATION 30
#define DEFAULT_CACHING_ENABLED TRUE

#define POLL_PERIOD 1000000 // microseconds

/* GObject */
static void skippy_hls_demux_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void skippy_hls_demux_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);
static void skippy_hls_demux_dispose (GObject * obj);

/* GstElement */
static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition);

/* SkippyHLSDemux */
static GstFlowReturn skippy_hls_demux_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query);
static gboolean skippy_hls_demux_handle_seek (SkippyHLSDemux *demux, GstEvent * event);
static void skippy_hls_demux_stream_loop (SkippyHLSDemux * demux);
static void skippy_hls_demux_stop (SkippyHLSDemux * demux);
static void skippy_hls_demux_pause (SkippyHLSDemux * demux);
static gboolean skippy_hls_demux_refresh_playlist (SkippyHLSDemux * demux);
static void skippy_hls_demux_reset (SkippyHLSDemux * demux, gboolean dispose);
static void skippy_hls_demux_link_pads (SkippyHLSDemux * demux);

#define skippy_hls_demux_parent_class parent_class
G_DEFINE_TYPE (SkippyHLSDemux, skippy_hls_demux, GST_TYPE_BIN);

static void
skippy_hls_demux_dispose (GObject * obj)
{
  GST_DEBUG ("Disposing ...");

  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);

  skippy_hls_demux_stop (demux);

  if (demux->stream_task) {
    gst_object_unref (demux->stream_task);
    demux->stream_task = NULL;
  }

  if (demux->downloader) {
    g_object_unref (demux->downloader);
    demux->downloader = NULL;
  }

  if (demux->queue_sinkpad) {
    g_object_unref (demux->queue_sinkpad);
    demux->queue_sinkpad = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (obj);

  GST_DEBUG ("Done cleaning up.");
}

static void
skippy_hls_demux_finalize (GObject * obj)
{
  GST_DEBUG ("Finalizing ...");

  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);

  g_rec_mutex_clear (&demux->stream_lock);

  G_OBJECT_CLASS (parent_class)->finalize (obj);

  GST_DEBUG ("Finalized.");
}

static void
skippy_hls_demux_class_init (SkippyHLSDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  gobject_class->set_property = skippy_hls_demux_set_property;
  gobject_class->get_property = skippy_hls_demux_get_property;
  gobject_class->dispose = skippy_hls_demux_dispose;
  gobject_class->finalize = skippy_hls_demux_finalize;

  g_object_class_install_property (gobject_class, PROP_BUFFER_AHEAD_DURATION,
      g_param_spec_uint ("buffer-ahead-duration",
          "Buffer duration ahead of playback in seconds",
          "Determines maximum of media segments downloaded upfront.",
          1, G_MAXUINT, DEFAULT_BUFFER_AHEAD_DURATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BITRATE_LIMIT,
      g_param_spec_float ("bitrate-limit",
          "Bitrate limit in %",
          "Limit of the available bitrate to use when switching to alternates.",
          0, 1, DEFAULT_BITRATE_LIMIT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CONNECTION_SPEED,
      g_param_spec_uint ("connection-speed", "Connection Speed",
          "Network connection speed in kbps (0 = unknown)",
          0, G_MAXUINT / 1000, DEFAULT_CONNECTION_SPEED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CACHING_ENABLED,
   	  g_param_spec_boolean ("caching-enabled", "Caching enabled",
  		  "Enable or disable response caching.",
  		  TRUE, G_PARAM_READWRITE));

  element_class->change_state = GST_DEBUG_FUNCPTR (skippy_hls_demux_change_state);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sinktemplate));

  gst_element_class_set_static_metadata (element_class,
      "HLS Demuxer",
      "Codec/Demuxer/Adaptive",
      "HTTP Live Streaming demuxer",
      "Marc-Andre Lureau <marcandre.lureau@gmail.com>\n"
      "Andoni Morales Alastruey <ylatuya@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (skippy_hls_demux_debug, "skippyhlsdemux", 0,
      "Skippy HLS client");
}

static void
skippy_hls_demux_init (SkippyHLSDemux * demux)
{
  // Pads
  demux->srcpad = NULL;
  demux->sinkpad = gst_pad_new_from_static_template (&sinktemplate, "sink");

  // Configure sink pad
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_chain));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_sink_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  // Objects
  demux->playlist = NULL;
  demux->client = NULL;
  demux->queue = gst_element_factory_make ("queue2", NULL);
  demux->queue_sinkpad = gst_element_get_static_pad (demux->queue, "sink");
  demux->downloader = skippy_uri_downloader_new ();
  demux->playlist_downloader = skippy_uri_downloader_new ();

  // Add bin elements
  gst_bin_add (GST_BIN (demux), demux->queue);
  gst_bin_add (GST_BIN (demux), GST_ELEMENT(demux->downloader));

  // Props
  demux->bitrate_limit = DEFAULT_BITRATE_LIMIT;
  demux->connection_speed = DEFAULT_CONNECTION_SPEED;
  demux->buffer_ahead_duration_secs = DEFAULT_BUFFER_AHEAD_DURATION;
  demux->caching_enabled = DEFAULT_CACHING_ENABLED;

  // Internal state
  skippy_hls_demux_reset (demux, FALSE);

  // Thread
  g_rec_mutex_init (&demux->stream_lock);
  demux->stream_task =
      gst_task_new ((GstTaskFunction) skippy_hls_demux_stream_loop, demux, NULL);
  gst_task_set_lock (demux->stream_task, &demux->stream_lock);
}

static void
skippy_hls_demux_reset (SkippyHLSDemux * demux, gboolean dispose)
{
  GST_DEBUG ("Re-setting element: (dispose=%d)", (int) dispose);

  demux->duration = GST_CLOCK_TIME_NONE;
  demux->download_failed_count = 0;
  demux->seeked = FALSE;
  demux->have_group_id = FALSE;
  demux->group_id = G_MAXUINT;

  if (demux->playlist) {
    gst_buffer_unref (demux->playlist);
    demux->playlist = NULL;
  }

  if (demux->client) {
    skippy_m3u8_client_free (demux->client);
    demux->client = NULL;
  }

  if (!dispose) {
    demux->client = skippy_m3u8_client_new ("");
  }

  if (demux->srcpad) {
    gst_element_remove_pad (GST_ELEMENT_CAST (demux), demux->srcpad);
    demux->srcpad = NULL;
  }

  if (demux->queue) {
    // Configure internal queue: get rid of all size limitations, don't emit buffering messages
    g_object_set (demux->queue,
      "max-size-buffers", 1024,
      "max-size-bytes", 128 * 1000000,
      "max-size-time", 3600 * GST_SECOND,
      "use-buffering", FALSE,
      NULL);
  }
}

static void
skippy_hls_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (object);

  switch (prop_id) {
    case PROP_BUFFER_AHEAD_DURATION:
      demux->buffer_ahead_duration_secs = g_value_get_uint (value);
      GST_DEBUG ("Set buffer ahead duration to %u seconds", demux->buffer_ahead_duration_secs);
      break;
    case PROP_BITRATE_LIMIT:
      demux->bitrate_limit = g_value_get_float (value);
      break;
    case PROP_CONNECTION_SPEED:
      demux->connection_speed = g_value_get_uint (value) * 1000;
      break;
    case PROP_CACHING_ENABLED:
      demux->caching_enabled = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
skippy_hls_demux_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (object);

  switch (prop_id) {
    case PROP_BUFFER_AHEAD_DURATION:
      g_value_set_uint (value, demux->buffer_ahead_duration_secs);
      break;
    case PROP_BITRATE_LIMIT:
      g_value_set_float (value, demux->bitrate_limit);
      break;
    case PROP_CONNECTION_SPEED:
      g_value_set_uint (value, demux->connection_speed / 1000);
      break;
    case PROP_CACHING_ENABLED:
      g_value_set_boolean (value, demux->caching_enabled);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (element);

  GST_TRACE ("Performing transition: %s -> %s", gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT(transition)),
    gst_element_state_get_name (GST_STATE_TRANSITION_NEXT(transition)));

  switch (transition) {
    // Bring the element back into initial state
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      skippy_hls_demux_stop (demux);
      break;
    default:
      break;
  }

  GST_TRACE ("Calling parent class state change handler ...");
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  GST_TRACE ("State transition result: %s", gst_element_state_change_return_get_name (ret));

  return ret;
}

static void
skippy_hls_demux_post_stat_msg (SkippyHLSDemux * demux, SkippyHLSDemuxStats metric, guint64 time_val, gsize size)
{
  GstStructure * structure = NULL;

  // Create message data
  switch (metric) {
  case STAT_TIME_TO_DOWNLOAD_FRAGMENT:
    GST_DEBUG ("Statistic: STAT_TIME_TO_DOWNLOAD_FRAGMENT");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "fragment-download-time", G_TYPE_UINT64, time_val,
      "fragment-size", G_TYPE_UINT64, (guint64) size,
      NULL);
    break;
  case STAT_TIME_TO_PLAYLIST:
    GST_DEBUG ("Statistic: STAT_TIME_TO_PLAYLIST");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "time-to-playlist", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  case STAT_TIME_OF_FIRST_PLAYLIST:
    GST_DEBUG ("Statistic: STAT_TIME_OF_FIRST_PLAYLIST");
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "manifest-download-start", GST_TYPE_CLOCK_TIME, GST_CLOCK_TIME_NONE,
      "manifest-download-stop", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  default:
    GST_ERROR ("Can't post unknown stats type");
    return;
  }

  // Post the message on the bus
  gst_element_post_message (GST_ELEMENT_CAST (demux),
    gst_message_ref (gst_message_new_element (GST_OBJECT_CAST (demux), structure))
  );
}

static gchar*
skippy_hls_demux_query_location (SkippyHLSDemux * demux)
{
  GstQuery* query = gst_query_new_uri ();
  gboolean ret = gst_pad_peer_query (demux->sinkpad, query);
  gboolean permanent;
  gchar *uri;

  if (ret) {
    gst_query_parse_uri_redirection (query, &uri);
    gst_query_parse_uri_redirection_permanent (query, &permanent);

    /* Only use the redirect target for permanent redirects */
    if (!permanent || uri == NULL) {
      g_free (uri);
      gst_query_parse_uri (query, &uri);
    }
    return uri;
  }
  gst_query_unref (query);
  return NULL;
}

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
    GST_LOG ("Position query result: %lld ms", (long long int) pos / GST_MSECOND);
  }
  gst_query_unref (query);

  if (!query_ret) {
    GST_DEBUG ("Assuming from position query result that playback did not start yet");
    // We assume this can happen at the very beginning of the streaming session
    // when pipeline position has some undefined state (as observed)
    pos = 0;
  }
  return pos;
}

static void
skippy_hls_demux_update_duration (SkippyHLSDemux * demux)
{
  // Post duration message if non-live
  if (!skippy_m3u8_client_is_live (demux->client)) {
    demux->duration = skippy_m3u8_client_get_duration (demux->client);
    GST_DEBUG_OBJECT (demux, "Playlist duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (demux->duration));
    if (demux->duration == GST_CLOCK_TIME_NONE) {
      GST_ERROR ("Duration has invalid value, not posting message to pipeline");
      return;
    }
    gst_element_post_message (GST_ELEMENT (demux), gst_message_new_duration_changed (GST_OBJECT (demux)));
  }
}

static void
skippy_hls_demux_handle_first_playlist (SkippyHLSDemux* demux)
{
  gchar* uri;

  // No playlist - can't do anything
  if (demux->playlist == NULL) {
    GST_WARNING_OBJECT (demux, "Received EOS without a playlist.");
    return;
  }

  GST_DEBUG_OBJECT (demux, "Got EOS on the sink pad: main playlist fetched");

  // Sending stats message about first playlist fetch
  skippy_hls_demux_post_stat_msg (demux, STAT_TIME_OF_FIRST_PLAYLIST, gst_util_get_timestamp (), 0);

  // Query the playlist URI
  uri = skippy_hls_demux_query_location (demux);
  if (!uri) {
    GST_ELEMENT_ERROR (demux, RESOURCE, NOT_FOUND, ("Failed querying the playlist URI"), (NULL));
    return;
  }

  GST_INFO_OBJECT (demux, "M3U8 location: %s", uri);

  // Create M3U8 client with main playlist URI
  demux->client = skippy_m3u8_client_new (uri);

  // Parse main playlist
  if (!skippy_m3u8_client_load_playlist (demux->client, demux->playlist)) {
    GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid M3U8 playlist"), (NULL));
    g_free (uri);
    return;
  }

  // Sets up the initial playlist (for when using a variant / sub-playlist)
  // Returns FALSE if we need to load a variant playlist (and selects a variant from main playlist)
  if (!skippy_m3u8_client_init_playlist (demux->client, demux->connection_speed)) {
    // Fetch the playlist
    if (!skippy_hls_demux_refresh_playlist (demux)) {
      GST_ERROR ("Failed to load variant playlist");
      return;
    }
  }

  // Updates duration field and posts message to bus
  skippy_hls_demux_update_duration (demux);

  GST_DEBUG_OBJECT (demux, "Finished setting up playlist");

  // Make sure URI downloaders are ready asap
  skippy_uri_downloader_prepare (demux->downloader, uri);
  skippy_uri_downloader_prepare (demux->playlist_downloader, uri);

  // Link pads
  skippy_hls_demux_link_pads (demux);

  // If we're not at least in PAUSED state now go to paused
  if (GST_STATE (demux) < GST_STATE_PAUSED) {
    gst_element_set_state (GST_ELEMENT(demux), GST_STATE_PAUSED);
  }

  // Start the streaming task
  gst_task_start (demux->stream_task);

  g_free (uri);
  return;
}

static gboolean
skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux;

  demux = SKIPPY_HLS_DEMUX (parent);

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_EOS:
      if (gst_task_get_state (demux->stream_task) == GST_TASK_STOPPED) {
        skippy_hls_demux_handle_first_playlist (demux);
      } else {
        GST_WARNING ("EOS on sink pad while stream loop running");
      }
      gst_event_unref (event); // we don't want to forward the EOS
      return TRUE;
    case GST_EVENT_SEGMENT:
      // Swallow new segments, we'll push our own
      gst_event_unref (event);
      return TRUE;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static gboolean
skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_SEEK:
      return skippy_hls_demux_handle_seek (demux, event);
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static gboolean
skippy_hls_demux_handle_seek (SkippyHLSDemux *demux, GstEvent * event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  GstSegment segment;

  GST_INFO ("Received GST_EVENT_SEEK");

  // Not seeking on a live stream
  if (skippy_m3u8_client_is_live (demux->client)) {
    GST_WARNING_OBJECT (demux, "Received seek event for live stream");
    gst_event_unref (event);
    return FALSE;
  }

  // Parse seek event
  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
  if (format != GST_FORMAT_TIME) {
    GST_WARNING ("Received seek event not in time format");
    gst_event_unref (event);
    return FALSE;
  }

  GST_DEBUG_OBJECT (demux, "Seek event, rate: %f start: %" GST_TIME_FORMAT " stop: %" GST_TIME_FORMAT,
    rate, GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

  // Pausing streaming task (blocking)
  skippy_hls_demux_pause (demux);
  // At this point we can be sure the stream loop is paused

  // Seek on M3U8 data model
  skippy_m3u8_client_seek (demux->client, (GstClockTime) start);

  // Update downloader segment after seek
  segment = skippy_uri_downloader_get_segment (demux->downloader);
  gst_segment_do_seek (&segment, rate, format, flags, start_type, start, stop_type, stop, NULL);
  skippy_uri_downloader_set_segment (demux->downloader, segment);

  // Set seeked flag
  demux->seeked = TRUE;

  // Flush start
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "Sending flush start");
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_start ());
  }

  // Flush stop
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "Sending flush stop");
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_stop (TRUE));
  }

  // Restart the streaming task
  GST_DEBUG ("Restarting streaming task");
  gst_task_start (demux->stream_task);

  // Handle and swallow event
  gst_event_unref (event);
  return TRUE;
}

static gboolean
skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);
  gboolean ret = FALSE;
  GstFormat fmt;
  gint64 stop = -1;

  GST_DEBUG ("Got %" GST_PTR_FORMAT, query);

  if (query == NULL)
    return FALSE;
  switch (query->type) {
    case GST_QUERY_DURATION:
      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt != GST_FORMAT_TIME) {
        GST_WARNING ("Can't process duration query that is not in time format");
        break;
      }
      if (GST_CLOCK_TIME_IS_VALID (demux->duration) && demux->duration > 0) {
        gst_query_set_duration (query, GST_FORMAT_TIME, demux->duration);
        GST_TRACE_OBJECT (demux, "Duration query: %" GST_TIME_FORMAT, GST_TIME_ARGS (demux->duration));
        ret = TRUE;
      } else {
        GST_WARNING ("Bad duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (demux->duration));
      }
      break;
    case GST_QUERY_URI:
      if (demux->client) {
        gst_query_set_uri (query, skippy_m3u8_client_get_uri (demux->client));
        ret = TRUE;
      }
      break;
    case GST_QUERY_SEEKING:
      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      if (fmt != GST_FORMAT_TIME) {
        GST_DEBUG ("Can't process seeking query that is not in time format");
        break;
      }
      if (GST_CLOCK_TIME_IS_VALID (demux->duration) && demux->duration > 0) {
        stop = demux->duration;
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

static GstFlowReturn
skippy_hls_demux_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  if (demux->playlist == NULL) {
    demux->playlist = buf;
  } else {
    demux->playlist = gst_buffer_append (demux->playlist, buf);
  }
  return GST_FLOW_OK;
}

static void
skippy_hls_demux_pause (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Pausing task ...");
  gst_task_pause (demux->stream_task);
  GST_DEBUG ("Checking for ongoing downloads to cancel ...");
  skippy_uri_downloader_cancel (demux->downloader);
  skippy_uri_downloader_cancel (demux->playlist_downloader);
  // Block until we're done cancelling
  g_rec_mutex_lock (&demux->stream_lock);
  g_rec_mutex_unlock (&demux->stream_lock);
  GST_DEBUG ("Paused streaming task");
}

static void
skippy_hls_demux_stop (SkippyHLSDemux *demux)
{
  // First go to paused (blocking)
  skippy_hls_demux_pause (demux);
  // Now let's join the streaming thread
  GST_DEBUG ("Stopping task ...");
  if (gst_task_get_state (demux->stream_task) != GST_TASK_STOPPED) {
    gst_task_join (demux->stream_task);
  }
  GST_DEBUG ("Stopped streaming task");
  // Finally reset everything
  skippy_hls_demux_reset (demux, FALSE);
}

static void
skippy_hls_demux_link_pads (SkippyHLSDemux * demux)
{
  GstEvent *event;
  gchar *stream_id;
  GstPad *queue_srcpad, *downloader_srcpad;
  GstPadTemplate* templ;

  GST_DEBUG ("Linking pads...");

  // Link downloader -> queue
  downloader_srcpad = gst_element_get_static_pad (GST_ELEMENT(demux->downloader), "src");
  if (!downloader_srcpad) {
    GST_DEBUG ("No src pad on downloader found yet");
    return;
  }

  gst_pad_link (downloader_srcpad, demux->queue_sinkpad);
  gst_object_unref (downloader_srcpad);

  // Link queue -> ext src (ghost pad)
  templ = gst_static_pad_template_get (&srctemplate);
  queue_srcpad = gst_element_get_static_pad (demux->queue, "src");
  demux->srcpad = gst_ghost_pad_new_from_template ("src", queue_srcpad, templ);
  gst_object_unref (templ);
  gst_object_unref (queue_srcpad);

  // Configure external source pad
  gst_pad_set_active (demux->srcpad, TRUE);
  gst_pad_set_event_function (demux->srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_event));
  gst_pad_set_query_function (demux->srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_query));
  gst_element_add_pad (GST_ELEMENT (demux), demux->srcpad);
  gst_element_no_more_pads (GST_ELEMENT (demux));

  // Sending stream start event that we got on sink pad (sticky event)
  stream_id = gst_pad_create_stream_id (demux->srcpad, GST_ELEMENT_CAST (demux), NULL);
  event = gst_pad_get_sticky_event (demux->sinkpad, GST_EVENT_STREAM_START, 0);
  if (event) {
    // Do we have a group ID from the sink pad sticky event?
    if (gst_event_parse_group_id (event, &demux->group_id))
      demux->have_group_id = TRUE;
    else
      demux->have_group_id = FALSE;
    // Swallow event
    gst_event_unref (event);
  } else if (!demux->have_group_id) {
    demux->have_group_id = TRUE;
    demux->group_id = gst_util_group_id_next ();
  }
  // Create stream start event from stream ID that we parsed from sink pad
  event = gst_event_new_stream_start (stream_id);
  // Eventually set group ID
  if (demux->have_group_id) {
    gst_event_set_group_id (event, demux->group_id);
  }
  // Send the stream start event
  gst_pad_send_event (demux->queue_sinkpad, event);
  g_free (stream_id);
}

static void
skippy_hls_handle_end_of_playlist (SkippyHLSDemux * demux)
{
  GST_DEBUG_OBJECT (demux, "Reached end of playlist, sending EOS");
  gst_pad_send_event (demux->queue_sinkpad, gst_event_new_eos ());
  skippy_hls_demux_pause (demux);
}

static gboolean
skippy_hls_check_buffer_ahead (SkippyHLSDemux * demux)
{
  GstClockTime pos, buffered_ahead;

  // Check upfront position relative to stream position
  if (demux->srcpad) {
    GST_DEBUG ("Buffer ahead duration is %d seconds", (int) demux->buffer_ahead_duration_secs);
    // Get current playback position from downstream
    pos = (GstClockTime) skippy_hls_demux_query_position (demux);
    buffered_ahead = (GstClockTime) (demux->buffer_ahead_duration_secs * GST_SECOND);
    if (!demux->seeked && demux->position >= pos + buffered_ahead) {
      GST_LOG ("Blocking task as we have buffered enough until now (up to %.2f seconds of media position)",
        ((float) demux->position) / GST_SECOND);
      g_usleep (POLL_PERIOD);
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean
skippy_hls_demux_is_caching_allowed (SkippyHLSDemux * demux)
{
  gboolean ret;
  ret = skippy_m3u8_client_is_caching_allowed (demux->client) && demux->caching_enabled;
  return ret;
}

static void
skippy_hls_demux_stream_loop (SkippyHLSDemux * demux)
{
  SkippyFragment *fragment;
  SkippyUriDownloaderFetchReturn fetch_ret = SKIPPY_URI_DOWNLOADER_VOID;
  GError *err = NULL;
  GstState current_state, pending_state;
  guint queue_level;

  GST_DEBUG_OBJECT (demux, "Entering stream task, polling ...");

  gst_element_get_state (GST_ELEMENT(demux), &current_state, &pending_state, GST_CLOCK_TIME_NONE);

  GST_TRACE ("Current state: %s, Pending state: %s",
    gst_element_state_get_name (current_state),
    gst_element_state_get_name (pending_state));

  // Monitor queue levels
  g_object_get (demux->queue, "current-level-buffers", &queue_level, NULL);
  GST_DEBUG ("Current internal queue level: %d buffers", (int) queue_level);

  // Check current playback position against buffer levels
  if (!skippy_hls_check_buffer_ahead (demux)) {
    return;
  }
  GST_DEBUG ("Will fetch next fragment ...");

  // Get next fragment from M3U8 list
  fragment = skippy_m3u8_client_get_next_fragment (demux->client);
  if (fragment) {
    GST_INFO_OBJECT (demux, "Will try to push data for next fragment URI: %s (Byte-Range=%" G_GINT64_FORMAT " - %" G_GINT64_FORMAT ")",
      fragment->uri, fragment->range_start, fragment->range_end);
    // Tell downloader to push data
    fetch_ret = skippy_uri_downloader_fetch_fragment (demux->downloader,
      fragment, // Media fragment to load
      skippy_m3u8_client_get_uri (demux->client), // Referrer
      FALSE, // Compress (useless with coded media data)
      FALSE, // Refresh disabled (don't wipe out cache)
      skippy_hls_demux_is_caching_allowed (demux), // Allow caching directive
      &err
    );
  } else {
    GST_INFO_OBJECT (demux, "This playlist doesn't contain more fragments");
  }

  GST_DEBUG ("Returning finished fragment");

  // Handle result from current attempt
  switch (fetch_ret) {
  // This case means the download did not do anything
  case SKIPPY_URI_DOWNLOADER_VOID:
    // Error & fragment should be NULL
    g_return_if_fail (!err);
    g_return_if_fail (!fragment);
    GST_DEBUG ("Did get NULL as next fragment");
    if (skippy_m3u8_client_is_live (demux->client)) {
      GST_DEBUG_OBJECT (demux, "No fragment left but live playlist, retrying later");
    } else {
      skippy_hls_handle_end_of_playlist (demux);
    }
    break;
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
    g_return_if_fail (!err);
    fragment = NULL;
    GST_DEBUG ("Fragment fetch got cancelled on purpose");
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
    g_return_if_fail (err);
    fragment = NULL;
    // When failed
    GST_INFO ("Fragment fetch error: %s", err->message);
    // Actual download failure
    demux->download_failed_count++;
    GST_DEBUG ("Failed to fetch fragment for %d times.", demux->download_failed_count);
    // We only want to refetch the playlist if we get a 403 or a 404
    if ((g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)
        // FIXME: This error comes also on connection failures. We'd want to differentiate between a 404 and a connection failure.
        // || g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND)
        )) {
      GST_DEBUG_OBJECT (demux, "Updating the playlist because of 403 or 404");
      skippy_hls_demux_refresh_playlist (demux);
    }
    break;
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    g_return_if_fail (!err);
    g_return_if_fail (fragment);
    GST_DEBUG ("Fragment download completed");
    // Post stats message
    skippy_hls_demux_post_stat_msg (demux,
      STAT_TIME_TO_DOWNLOAD_FRAGMENT,
      fragment->download_stop_time - fragment->download_start_time,
      fragment->size);
    // Reset failure counter
    demux->position = fragment->start_time;
    demux->download_failed_count = 0;
    // Go to next fragment
    skippy_m3u8_client_advance_fragment (demux->client);
    break;
  }

  GST_DEBUG_OBJECT (demux, "Exiting task now ...");

  // Handle error
  if (err) {
    g_usleep (POLL_PERIOD);
  }
  g_clear_error (&err);

}

static gboolean
skippy_hls_demux_refresh_playlist (SkippyHLSDemux * demux)
{
  SkippyFragment *download;
  GstBuffer *buf = NULL;
  GError* err = NULL;
  SkippyUriDownloaderFetchReturn fetch_ret;
  gboolean ret = FALSE;

  // Create a download
  download = skippy_fragment_new (skippy_m3u8_client_get_current_uri (demux->client));
  download->start_time = 0;
  download->stop_time = demux->duration;
  // Download it
  fetch_ret = skippy_uri_downloader_fetch_fragment (demux->playlist_downloader,
    download, // Media fragment to load
    skippy_m3u8_client_get_uri (demux->client), // Referrer
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
    if (!skippy_m3u8_client_load_playlist (demux->client, buf)) {
      GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid playlist."), (NULL));
      ret = FALSE;
      break;
    }
    ret = TRUE;
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
  case SKIPPY_URI_DOWNLOADER_VOID:
    if (err) {
      GST_ERROR ("Error updating playlist: %s", err->message);
      g_clear_error (&err);
    }
    ret = FALSE;
    break;
  }

  if (buf) {
    gst_buffer_unref (buf);
  }
  return ret;
}

G_GNUC_INTERNAL
void skippy_hlsdemux_setup (void)
{
  gst_element_register (NULL, "skippyhlsdemux", GST_RANK_PRIMARY + 100,
      TYPE_SKIPPY_HLS_DEMUX);
}

