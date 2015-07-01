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
static void skippy_hls_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void skippy_hls_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void skippy_hls_demux_dispose (GObject * obj);

/* GstElement */
static GstStateChangeReturn
skippy_hls_demux_change_state (GstElement * element, GstStateChange transition);

/* SkippyHLSDemux */
static GstFlowReturn skippy_hls_demux_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean skippy_hls_demux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean skippy_hls_demux_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean skippy_hls_demux_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static void skippy_hls_demux_stream_loop (SkippyHLSDemux * demux);
static void skippy_hls_demux_stop (SkippyHLSDemux * demux);
static void skippy_hls_demux_pause_tasks (SkippyHLSDemux * demux);
static SkippyFragment *skippy_hls_demux_get_next_fragment (SkippyHLSDemux * demux, SkippyUriDownloaderFetchReturn* fetch_ret, GError ** err);
static gboolean skippy_hls_demux_update_playlist (SkippyHLSDemux * demux);
static void skippy_hls_demux_reset (SkippyHLSDemux * demux, gboolean dispose);
static gboolean skippy_hls_demux_set_location (SkippyHLSDemux * demux,
    const gchar * uri);
static gboolean
skippy_hls_demux_switch_bitrate (SkippyHLSDemux * demux, SkippyFragment * fragment);
static void
skippy_hls_demux_setup_playlist (SkippyHLSDemux * demux);
static gboolean
skippy_hls_demux_check_for_live_update (SkippyHLSDemux * demux);

#define skippy_hls_demux_parent_class parent_class
G_DEFINE_TYPE (SkippyHLSDemux, skippy_hls_demux, GST_TYPE_BIN);

static void
skippy_hls_demux_dispose (GObject * obj)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (obj);

  skippy_hls_demux_reset (demux, TRUE);
  skippy_hls_demux_stop (demux);

  if (demux->stream_task) {
    gst_object_unref (demux->stream_task);
    g_rec_mutex_clear (&demux->stream_lock);
    demux->stream_task = NULL;
  }

  if (demux->downloader) {
    g_object_unref (demux->downloader);
    demux->downloader = NULL;
  }

  if (demux->queue_sinkpad) {
    gst_object_unref (demux->queue_sinkpad);
  }

  G_OBJECT_CLASS (parent_class)->dispose (obj);
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

  skippy_m3u8_client_init ();
}

// TODO: send a message here on the bus instead
static void
downloader_callback (SkippyUriDownloader* downloader,
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
  parent = skippy_uri_downloader_get_parent (downloader);
  if (parent) {
    s = gst_structure_new (SKIPPY_HLS_DEMUX_DOWNLOADING_MSG_NAME,
      "fragment-start-time", G_TYPE_UINT64, start_time,
      "fragment-stop-time", G_TYPE_UINT64, stop_time,
      "loaded-bytes", G_TYPE_UINT64, (guint64) bytes_loaded,
      "total-bytes", G_TYPE_UINT64, (guint64) bytes_total,
      NULL
    );
    gst_element_post_message (parent, gst_message_new_element (GST_OBJECT(parent), s));
  }

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
  demux->downloader = skippy_uri_downloader_new (downloader_callback, GST_ELEMENT(demux));

  // Add bin elements
  gst_bin_add (GST_BIN (demux), demux->queue);

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
  demux->current_download_rate = -1;
  demux->need_segment = TRUE;
  demux->seeked = FALSE;
  demux->have_group_id = FALSE;
  demux->group_id = G_MAXUINT;
  demux->next_update = -1;
  demux->linked = FALSE;

  gst_segment_init (&demux->segment, GST_FORMAT_TIME);

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

  // Reset downloader (we re-use the object)
  skippy_uri_downloader_reset (demux->downloader);
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

  GST_DEBUG ("Performing transition: %s -> %s", gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT(transition)),
    gst_element_state_get_name (GST_STATE_TRANSITION_NEXT(transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      skippy_hls_demux_reset (demux, FALSE);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      skippy_hls_demux_pause_tasks (demux);
      skippy_hls_demux_reset (demux, FALSE);
      break;
    default:
      break;
  }

  GST_DEBUG ("Calling parent class state change handler ...");
  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  GST_DEBUG ("State transition result: %s", gst_element_state_change_return_get_name (ret));

  return ret;
}

static gboolean
skippy_hls_demux_seek (SkippyHLSDemux *demux, GstEvent * event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  GstClockTime target_pos;

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

  GST_DEBUG_OBJECT (demux, "seek event, rate: %f start: %" GST_TIME_FORMAT
      " stop: %" GST_TIME_FORMAT, rate, GST_TIME_ARGS (start),
      GST_TIME_ARGS (stop));

  // Set target position
  target_pos = (GstClockTime) start;

  // Flush start
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "sending flush start");
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_start ());
  }

  // Pausing streaming task
  skippy_hls_demux_pause_tasks (demux);

  // Update M3U8 model
  if (!skippy_m3u8_client_seek_to (demux->client, target_pos))
  {
    GST_WARNING ("Failed to find seek target position in M3U8 playlist");
    gst_event_unref (event);
    return FALSE;
  }

  // Update current  data segment
  gst_segment_do_seek (&demux->segment, rate, format, flags, start_type,
      start, stop_type, stop, NULL);

  // Set flags
  demux->need_segment = TRUE;
  demux->seeked = TRUE;

  // Flush stop
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "sending flush stop");
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_flush_stop (TRUE));
  }

  // Restart the streaming task
  gst_task_start (demux->stream_task);

  GST_DEBUG ("Restarted streaming task");

  // Swallow event
  gst_event_unref (event);
  return TRUE;
}

static gboolean
skippy_hls_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  SkippyHLSDemux *demux = SKIPPY_HLS_DEMUX (parent);

  GST_DEBUG_OBJECT (pad, "Got %" GST_PTR_FORMAT, event);

  switch (event->type) {
    case GST_EVENT_SEEK:
      return skippy_hls_demux_seek (demux, event);
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
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
    gst_message_new_element (GST_OBJECT_CAST (demux), structure));
}


static gboolean
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
    skippy_hls_demux_set_location (demux, uri);
    g_free (uri);
  }
  gst_query_unref (query);
  return ret;
}

static gboolean
skippy_hls_demux_load_initial_playlist (SkippyHLSDemux* demux)
{
  gboolean ret;

  // No playlist - can't do anything
  if (demux->playlist == NULL) {
    GST_WARNING_OBJECT (demux, "Received EOS without a playlist.");
    return FALSE;
  }

  GST_DEBUG_OBJECT (demux, "Got EOS on the sink pad: main playlist fetched");

  // Sending stats message about first playlist fetch
  skippy_hls_demux_post_stat_msg (demux, STAT_TIME_OF_FIRST_PLAYLIST, gst_util_get_timestamp (), 0);

  // Query the playlist URI
  ret = skippy_hls_demux_query_location (demux);

  // Parse playlist
  if (!skippy_m3u8_client_load_playlist (demux->client, NULL, demux->playlist)) {
    GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid playlist."), (NULL));
    return FALSE;
  }

  // Playlist is live but failed to get source URI
  if (!ret && skippy_m3u8_client_is_live (demux->client)) {
    GST_ELEMENT_ERROR (demux, RESOURCE, NOT_FOUND,
        ("Failed querying the playlist uri, "
            "required for live sources."), (NULL));
    return FALSE;
  }

  // Sets up the initial playlist (for when using a variant / sub-playlist)
  skippy_hls_demux_setup_playlist (demux);

  // Start the main task
  gst_task_start (demux->stream_task);

  return TRUE;
}

static void
skippy_hls_demux_setup_playlist (SkippyHLSDemux * demux)
{
  gchar* variant;

  /* If this playlist is a variant playlist, select the first one
   * and update it */
  if (skippy_m3u8_client_has_variant_playlist (demux->client)) {
    // Select child playlist
    if (demux->connection_speed == 0) {
      variant = skippy_m3u8_client_get_current_playlist (demux->client);
    } else {
      variant = skippy_m3u8_client_get_playlist_for_bitrate (demux->client, demux->connection_speed);
    }
    // Fetch the playlist
    if (!skippy_hls_demux_update_playlist (demux)) {
      g_free (variant);
      return;
    }
    // Set the variant as current playlist to be used
    skippy_m3u8_client_set_current_playlist (demux->client, variant);
    g_free (variant);
  }

  // Post duration message if non-live
  if (!skippy_m3u8_client_is_live (demux->client)) {
    demux->duration = skippy_m3u8_client_get_total_duration (demux->client);
    GST_DEBUG_OBJECT (demux, "Playlist duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (demux->duration));
    if (demux->duration == GST_CLOCK_TIME_NONE) {
      GST_ERROR ("Duration has invalid value, not posting message to pipeline");
      return;
    }
    gst_element_post_message (GST_ELEMENT (demux),
        gst_message_new_duration_changed (GST_OBJECT (demux)));
  }

  GST_DEBUG_OBJECT (demux, "Finished setting up playlist");
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
      gst_event_unref (event); // we don't want to forward the EOS
      return skippy_hls_demux_load_initial_playlist (demux);
    case GST_EVENT_SEGMENT:
      /* Swallow new segments, we'll push our own */
      gst_event_unref (event);
      return TRUE;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static gboolean
skippy_hls_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  SkippyHLSDemux *hlsdemux = SKIPPY_HLS_DEMUX (parent);
  gboolean ret = FALSE;
  GstFormat fmt;
  gint64 stop = -1;
  gchar* uri;

  if (query == NULL)
    return FALSE;
  switch (query->type) {
    case GST_QUERY_DURATION:
      gst_query_parse_duration (query, &fmt, NULL);
      GST_TRACE_OBJECT (hlsdemux, "Received GST_QUERY_DURATION with format %d", fmt);
      if (fmt != GST_FORMAT_TIME) {
        GST_WARNING ("Can't process duration query that is not in time format");
        break;
      }
      if (GST_CLOCK_TIME_IS_VALID (hlsdemux->duration) && hlsdemux->duration > 0) {
        gst_query_set_duration (query, GST_FORMAT_TIME, hlsdemux->duration);
        GST_TRACE_OBJECT (hlsdemux, "GST_QUERY_DURATION returns with duration %" GST_TIME_FORMAT, GST_TIME_ARGS (hlsdemux->duration));
        ret = TRUE;
      } else {
        GST_WARNING ("Bad duration: %" GST_TIME_FORMAT, GST_TIME_ARGS (hlsdemux->duration));
      }
      break;
    case GST_QUERY_URI:
      if (hlsdemux->client) {
        uri = skippy_m3u8_client_get_uri (hlsdemux->client);
        gst_query_set_uri (query, uri);
        g_free (uri);
        ret = TRUE;
      }
      break;
    case GST_QUERY_SEEKING:
      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      GST_DEBUG_OBJECT (hlsdemux, "Received GST_QUERY_SEEKING with format %d", fmt);
      if (fmt != GST_FORMAT_TIME) {
        GST_WARNING ("Can't process seeking query that is not in time format");
        break;
      }
      if (GST_CLOCK_TIME_IS_VALID (hlsdemux->duration) && hlsdemux->duration > 0) {
        stop = hlsdemux->duration;
        gst_query_set_seeking (query, fmt, !skippy_m3u8_client_is_live (hlsdemux->client), 0, stop);
        ret = TRUE;
        GST_INFO_OBJECT (hlsdemux, "GST_QUERY_SEEKING returning with stop : %" GST_TIME_FORMAT, GST_TIME_ARGS (stop));
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

  if (demux->playlist == NULL)
    demux->playlist = buf;
  else
    demux->playlist = gst_buffer_append (demux->playlist, buf);

  return GST_FLOW_OK;
}

static void
skippy_hls_demux_pause_tasks (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Pausing task ...");
  gst_task_pause (demux->stream_task);
  skippy_uri_downloader_cancel (demux->downloader);
  g_rec_mutex_lock (&demux->stream_lock);
  g_rec_mutex_unlock (&demux->stream_lock);
  GST_DEBUG ("Paused streaming task");
}

static void
skippy_hls_demux_stop (SkippyHLSDemux * demux)
{
  GST_DEBUG ("Stopping task ...");
  if (GST_TASK_STATE (demux->stream_task) != GST_TASK_STOPPED) {
    gst_task_join (demux->stream_task);
  }
  GST_DEBUG ("Stopped streaming task");
}

// FIXME: Refactor this
static void
skippy_hls_demux_link_pads (SkippyHLSDemux * demux, GstCaps * newcaps)
{
  GstEvent *event;
  gchar *stream_id;
  GstPad *queue_srcpad;
  GstPadTemplate* templ;

  GST_DEBUG ("Linking pads with caps: %" GST_PTR_FORMAT, newcaps);

  // Create and activate new source pad (linked as a ghost pad to our queue source pad)
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

  // Set caps on our internal input (queue will forward caps to output)
  // NOTE: We only can do this after sending the stream-start event (triggers caps event)
  gst_pad_use_fixed_caps (demux->queue_sinkpad);
  gst_pad_set_caps (demux->queue_sinkpad, newcaps);
}

// FIXME: Refactor this
static gboolean
skippy_hls_demux_configure_src_pad (SkippyHLSDemux * demux, SkippyFragment * fragment)
{
  GstCaps *bufcaps = NULL;
  GstBuffer *buf = NULL;
  gboolean ret = FALSE;

  // If we have not created the source pad yet, do it now
  if (G_UNLIKELY(!demux->linked)) {
    // Get buffer and caps from fragment
    buf = skippy_fragment_get_buffer (fragment);
    if (G_UNLIKELY (!buf)) {
      goto error;
    }
    bufcaps = skippy_fragment_get_caps (fragment);
    if (G_UNLIKELY (!bufcaps)) {
      goto error;
    }
    // Link pads with these caps
    skippy_hls_demux_link_pads (demux, bufcaps);
    // Flag the buffer as discont' if we just initialized the pad
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    // Set flags
    demux->linked = TRUE;
    demux->need_segment = TRUE;
  }

  // Send a segment event if needed
  if (demux->need_segment) {
    GST_DEBUG_OBJECT (demux, "Sending segment event: %" GST_SEGMENT_FORMAT,
      &demux->segment);
    gst_pad_send_event (demux->queue_sinkpad, gst_event_new_segment (&demux->segment));
    demux->need_segment = FALSE;
  }

  ret = TRUE;

error:
  if (bufcaps) {
    gst_caps_unref (bufcaps);
  }
  if (buf) {
    gst_buffer_unref (buf);
  }
  return ret;
}

static void
skippy_hls_handle_end_of_playlist (SkippyHLSDemux * demux)
{
  GST_DEBUG_OBJECT (demux, "Reached end of playlist, sending EOS");
  skippy_hls_demux_configure_src_pad (demux, NULL);
  gst_pad_send_event (demux->queue_sinkpad, gst_event_new_eos ());
  skippy_hls_demux_pause_tasks (demux);
}

static gboolean
skippy_hls_push_fragment (SkippyHLSDemux * demux, SkippyFragment* fragment)
{
  GstBuffer* buf;
  GstFlowReturn ret;
  GstCaps *caps;
  guint queue_level;

  buf = skippy_fragment_get_buffer (fragment);

  GST_DEBUG_OBJECT (demux, "Pushing buffer %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  // Flagging buffer as discontinuous if necessary
  if (fragment->discontinuous) {
    GST_DEBUG_OBJECT (demux, "Marking fragment as discontinuous");
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
  } else {
    GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  // Re-configure source pad if necessary (check for available caps)
  if (!skippy_hls_demux_configure_src_pad (demux, fragment)) {
    GST_ELEMENT_ERROR (demux, STREAM, TYPE_NOT_FOUND, ("Could not determine type of stream"), (NULL));
    gst_buffer_unref (buf);
    return FALSE;
  }

  // Updating current data segment properties
  demux->segment.position = GST_BUFFER_TIMESTAMP (buf);
  demux->seeked = FALSE;
  if (demux->segment.rate > 0) {
    demux->segment.position += GST_BUFFER_DURATION (buf);
  }

  // Push the buffer onto the queue
  g_object_get (demux->queue, "current-level-buffers", &queue_level, NULL);
  GST_DEBUG ("Enter pad chain function, current internal queue level: %d buffers", (int) queue_level);
  ret = gst_pad_chain (demux->queue_sinkpad, buf);
  GST_DEBUG ("Exit pad chain function");
  if (ret != GST_FLOW_OK) {
    if (ret == GST_FLOW_NOT_LINKED || ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (demux, STREAM, FAILED, (NULL),
          ("Error pushing buffer (will send EOS): %s", gst_flow_get_name (ret)));
      gst_pad_send_event (demux->queue_sinkpad, gst_event_new_eos ());
    } else {
      GST_WARNING_OBJECT (demux, "Can't push buffer because: %s", gst_flow_get_name (ret));
    }
    return FALSE;
  }
  GST_DEBUG_OBJECT (demux, "Pushed buffer on src pad");
  return TRUE;
}

static gboolean
skippy_hls_check_buffer_ahead (SkippyHLSDemux * demux)
{
  GstFormat format;
  gint64 pos;
  GstQuery* query;
  gboolean query_ret;

  // Check upfront position relative to stream position
  if (demux->srcpad) {
    query = gst_query_new_position (GST_FORMAT_TIME);
    query_ret = gst_pad_peer_query (demux->srcpad, query);
    if (query_ret) {
      gst_query_parse_position (query, &format, &pos);
      if (format != GST_FORMAT_TIME) {
        GST_ERROR ("Position query result is not in TIME format");
        query_ret = FALSE;
      }
      GST_TRACE ("Current position query result: %lld ms", (long long int) pos / 1000000);
    }
    gst_query_unref (query);
    if (!query_ret) {
      GST_TRACE ("Position query did not give proper result");
      // We assume this can happen at the very beginning of the streaming session
      // when pipeline position has some undefined state (as observed)
      // In this case we just continue downloading ...
      return TRUE;
    }
    GST_DEBUG ("Buffer ahead duration is %d seconds", (int) demux->buffer_ahead_duration_secs);
    if (!demux->seeked && demux->segment.position >= pos + demux->buffer_ahead_duration_secs * GST_SECOND) {
      GST_DEBUG ("Blocking task as we have buffered enough until now (up to %f seconds of media position)",
        ((float) demux->segment.position) / GST_SECOND);
      g_usleep (POLL_PERIOD);
      return FALSE;
    }
  }
  return TRUE;
}

static void
skippy_hls_demux_stream_loop (SkippyHLSDemux * demux)
{
  SkippyFragment *fragment;
  SkippyUriDownloaderFetchReturn fetch_ret = SKIPPY_URI_DOWNLOADER_VOID;
  GError *err = NULL;

  GST_DEBUG_OBJECT (demux, "Entering stream task, polling ...");

  if (!skippy_hls_check_buffer_ahead (demux)) {
    return;
  }

  GST_DEBUG ("Will fetch next fragment ...");

  /* Handle failure to get the next fragment */
  fragment = skippy_hls_demux_get_next_fragment (demux, &fetch_ret, &err);

  switch (fetch_ret) {
  case SKIPPY_URI_DOWNLOADER_VOID:
    // Error & fragment should be NULL
    g_return_if_fail (!err);
    GST_DEBUG ("Got NULL as next fragment");
    if (skippy_m3u8_client_is_live (demux->client)) {
      GST_DEBUG_OBJECT (demux, "No fragment left but live playlist, retrying later");
    } else {
      skippy_hls_handle_end_of_playlist (demux);
    }
    // We can exit just like that (err == NULL)
    break;
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
    g_return_if_fail (!err);
    GST_DEBUG ("Fragment fetch got cancelled on purpose");
    break;
  case SKIPPY_URI_DOWNLOADER_FAILED:
    g_return_if_fail (err);
    // When failed
    //gst_element_set_state (GST_ELEMENT (demux), GST_STATE_READY);
    GST_ERROR ("Fragment fetch error: %s", err->message);
    // Actual download failure
    demux->download_failed_count++;
    GST_DEBUG ("Failed to fetch fragment for %d times.", demux->download_failed_count);
    // We only want to refetch the playlist if we get a 403 or a 404
    if ((g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)
        // FIXME: This error comes also on connection failures. We'd want to differentiate between a 404 and a connection failure.
        // || g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND)
        )) {
      GST_DEBUG_OBJECT (demux, "Updating the playlist because of 403 or 404");
      skippy_hls_demux_update_playlist (demux);
    }
    break;
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    g_return_if_fail (!err);
    g_return_if_fail (fragment);
    GST_DEBUG ("Got next fragment");
    //
    //gst_element_set_state (GST_ELEMENT (demux), GST_STATE_PLAYING);
    // Reset failure counter
    demux->download_failed_count = 0;
    // Push fragment onto pipeline
    skippy_hls_push_fragment (demux, fragment);
    // Drop current fragment in any case
    g_object_unref (fragment);
    // Check for live update of playlist
    skippy_hls_demux_check_for_live_update (demux);
    // Go to next fragment
    skippy_m3u8_client_advance_to_next_fragment (demux->client);
    break;
  }

  GST_DEBUG_OBJECT (demux, "Exiting task now ...");

  // Clear any error
  if (err) {
    g_clear_error (&err);
    g_usleep (POLL_PERIOD);
  }
}

/* Interacts with M3U8 client and URI downloader to provide next fragment to streaming loop,
measures download speed, posts stat message and decrypt fragment buffer, triggers bitrate switching */
static SkippyFragment *
skippy_hls_demux_get_next_fragment (SkippyHLSDemux * demux, SkippyUriDownloaderFetchReturn *fetch_ret, GError ** err)
{
  GstStructure *stat_msg;
  guint64 size;
  SkippyFragment* fragment;
  gchar* uri;

  g_return_val_if_fail (*err == NULL, NULL);

  fragment = skippy_m3u8_client_get_current_fragment (demux->client);
  if (!fragment) {
    GST_INFO_OBJECT (demux, "This playlist doesn't contain more fragments");
    *fetch_ret = SKIPPY_URI_DOWNLOADER_VOID;
    return NULL;
  }

  GST_INFO_OBJECT (demux,
      "Fetching next fragment %s (range=%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT
      ")", fragment->uri, fragment->range_start, fragment->range_end);

  uri = skippy_m3u8_client_get_uri (demux->client);
  *fetch_ret = skippy_uri_downloader_fetch_fragment (demux->downloader,
    fragment, // Media fragment to load
    uri, // Referrer
    FALSE, // Compress (useless with coded media data)
    FALSE, // Refresh (don't wipe out cache)
    skippy_m3u8_client_allow_cache (demux->client) && demux->caching_enabled, // Allow caching directive
    err // Error
  );
  g_free (uri);

  // NOTE: We don't want to handle the error here, we just pass it in and let it be handled by our caller

  // If it's not useable just return NULL
  if (!fragment->completed) {
    GST_INFO ("Fragment incomplete");
    g_object_unref (fragment);
    return NULL;
  }
  if (!fragment->decrypted) {
    GST_ERROR ("Fragment not decrypted");
    g_object_unref (fragment);
    return NULL;
  }

  // Size of transferred payload (may be encrypted)
  size = skippy_fragment_get_buffer_size (fragment);
  skippy_hls_demux_post_stat_msg (demux,
    STAT_TIME_TO_DOWNLOAD_FRAGMENT,
    fragment->download_stop_time - fragment->download_start_time,
    size);

#if 0
  /* try to switch to another bitrate if needed */
  skippy_hls_demux_switch_bitrate (demux, fragment);
#endif

  GST_DEBUG ("Returning finished fragment");

  return fragment;
}

static gboolean
skippy_hls_demux_update_playlist (SkippyHLSDemux * demux)
{
  SkippyFragment *download;
  GstBuffer *buf;
  GstStructure *stat_msg;
  GError* err;
  SkippyUriDownloaderFetchReturn fetch_ret;
  gchar* playlist_uri;
  gchar* current_playlist = skippy_m3u8_client_get_current_playlist (demux->client);

  if (!current_playlist) {
    return FALSE;
  }

  // Create a download
  download = skippy_fragment_new (current_playlist, NULL, NULL);
  // Download it
  playlist_uri = skippy_m3u8_client_get_uri (demux->client);
  fetch_ret = skippy_uri_downloader_fetch_fragment (demux->downloader,
    download, // Media fragment to load
    playlist_uri, // Referrer
    TRUE, // Compress (good for playlists)
    TRUE, // Refresh (wipe out cached stuff)
    skippy_m3u8_client_allow_cache (demux->client) && demux->caching_enabled, // Allow caching directive
    &err // Error
  );
  // Handle fetch result
  switch (fetch_ret) {
  case SKIPPY_URI_DOWNLOADER_COMPLETED:
    skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_PLAYLIST, download->download_stop_time - download->download_start_time, 0);
    // Load M3U8 buffer into parser
    if (!skippy_m3u8_client_load_playlist (demux->client, current_playlist, skippy_fragment_get_buffer (download))) {
      GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid playlist."), (NULL));
      goto error;
    }
    goto success;
  case SKIPPY_URI_DOWNLOADER_FAILED:
  case SKIPPY_URI_DOWNLOADER_CANCELLED:
  case SKIPPY_URI_DOWNLOADER_VOID:
    if (err) {
      GST_ERROR ("Error updating playlist: %s", err->message);
      g_clear_error (&err);
    }
    goto error;
  }
success:
    g_object_unref (download);
    g_free (playlist_uri);
    g_free (current_playlist);
    return TRUE;
error:
    g_object_unref (download);
    g_free (playlist_uri);
    g_free (current_playlist);
    return FALSE;
}

static gboolean
skippy_hls_demux_set_location (SkippyHLSDemux * demux, const gchar * uri)
{
  if (demux->client)
    skippy_m3u8_client_free (demux->client);
  demux->client = skippy_m3u8_client_new (uri);
  GST_INFO_OBJECT (demux, "Set location: %s", uri);
  return TRUE;
}

static void
skippy_hls_demux_set_next_playlist_update (SkippyHLSDemux* demux, guint64 seconds_from_now)
{
  demux->next_update =
      g_get_monotonic_time () +
      gst_util_uint64_scale (seconds_from_now, G_USEC_PER_SEC, GST_SECOND);
}

static gboolean
skippy_hls_demux_check_for_live_update (SkippyHLSDemux * demux)
{
  /* Updating playlist only needed for live playlists */
  if (skippy_m3u8_client_is_live (demux->client)) {
    /* Wait here until we should do the next update or we're cancelled */
    GST_DEBUG_OBJECT (demux, "Checking for next playlist update ...");
    if (g_get_monotonic_time () < demux->next_update) {
      GST_DEBUG ("Too early, waiting more");
      return FALSE;
    }

    GST_DEBUG_OBJECT (demux, "Updating playlist ...");

    if (!skippy_hls_demux_update_playlist (demux)) { // When failing
      GST_WARNING_OBJECT (demux, "Could not update the live playlist");
      /* Try again after 1 second */
      skippy_hls_demux_set_next_playlist_update (demux, 1);
      return FALSE;
    }

    GST_DEBUG_OBJECT (demux, "Updated playlist successfully, rescheduling update");
    skippy_hls_demux_set_next_playlist_update (demux, skippy_m3u8_client_get_target_duration (demux->client));
  }
  return TRUE;
}

#if 0
static gboolean
skippy_hls_demux_change_playlist (SkippyHLSDemux * demux, guint max_bitrate)
{
  SkippyM3U8 *previous_variant, *current_variant;
  gint old_bandwidth, new_bandwidth;

  /* If user specifies a connection speed never use a playlist with a bandwidth
   * superior than it */
  if (demux->connection_speed != 0 && max_bitrate > demux->connection_speed)
    max_bitrate = demux->connection_speed;

  previous_variant = skippy_m3u8_client_get_current_variant (demux->client);
  current_variant = skippy_m3u8_client_get_playlist_for_bitrate (demux->client, max_bitrate);

  old_bandwidth = previous_variant->bandwidth;
  new_bandwidth = current_variant->bandwidth;

  /* Don't do anything else if the playlist is the same */
  if (new_bandwidth == old_bandwidth) {
    return TRUE;
  }

  // OK let's switch
  GST_INFO_OBJECT (demux, "Client was on %dbps, max allowed is %dbps, switching"
      " to bitrate %dbps", old_bandwidth, max_bitrate, new_bandwidth);

  // Update current playlist
  skippy_m3u8_client_set_current (demux->client, current_variant);

  // Perform the update
  if (skippy_hls_demux_update_playlist (demux)) {
    // SUCCESS ! Post a "playlist" message (WTF is that for?)
    GstStructure *s = gst_structure_new ("playlist",
        "uri", G_TYPE_STRING, skippy_m3u8_client_get_current_uri (demux->client),
        "bitrate", G_TYPE_INT, new_bandwidth, NULL);
    gst_element_post_message (GST_ELEMENT_CAST (demux),
        gst_message_new_element (GST_OBJECT_CAST (demux), s));
  } else {
    GST_WARNING_OBJECT (demux, "Unable to update playlist. Switching back to previous one.");
    skippy_m3u8_client_set_current (demux->client, previous_variant);
    return FALSE;
  }

  return TRUE;
}
#endif

#if 0
static gboolean
skippy_hls_demux_switch_bitrate (SkippyHLSDemux * demux, SkippyFragment * fragment)
{
  GstClockTime diff;
  gsize size;
  gint64 bitrate;
  GstBuffer *buffer;

  if (!fragment)
    return TRUE;

  /* compare the time when the fragment was downloaded with the time when it was
   * scheduled */
  diff = fragment->download_stop_time - fragment->download_start_time;
  buffer = skippy_fragment_get_buffer (fragment);
  size = gst_buffer_get_size (buffer);
  bitrate = (size * 8) / ((double) diff / GST_SECOND);

  GST_DEBUG_OBJECT (demux,
      "Downloaded %d bytes in %" GST_TIME_FORMAT ". Bitrate is : %d",
      (guint) size, GST_TIME_ARGS (diff), (gint) bitrate);

  /* Take old rate into account too */
  if (demux->current_download_rate != -1)
    bitrate = (demux->current_download_rate + bitrate * 3) / 4;
  if (bitrate > G_MAXINT)
    bitrate = G_MAXINT;
  demux->current_download_rate = bitrate;

  GST_DEBUG_OBJECT (demux, "Using current download rate: %d", (gint) bitrate);

  gst_buffer_unref (buffer);

  SKIPPY_M3U8_CLIENT_LOCK (demux->client);
  if (!demux->client->main->lists) {
    SKIPPY_M3U8_CLIENT_UNLOCK (demux->client);
    return TRUE;
  }
  SKIPPY_M3U8_CLIENT_UNLOCK (demux->client);

  return skippy_hls_demux_change_playlist (demux, bitrate * demux->bitrate_limit);
}
#endif

G_GNUC_INTERNAL
void skippy_hlsdemux_setup (void)
{
  gst_element_register (NULL, "skippyhlsdemux", GST_RANK_PRIMARY + 100,
      TYPE_SKIPPY_HLS_DEMUX);
}

