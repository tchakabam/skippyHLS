/* GStreamer
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 * Copyright (C) 2010 Andoni Morales Alastruey <ylatuya@gmail.com>
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *  Author: Youness Alaoui <youness.alaoui@collabora.co.uk>, Collabora Ltd.
 *  Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 *
 * Gsthlsdemux.c:
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

/* FIXME 0.11: suppress warnings for deprecated API such as GStaticRecMutex
 * with newer GLib versions (>= 2.31.0) */
#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include <string.h>

#include <skippyhls/skippy_hlsdemux.h>

#define GST_ELEMENT_ERROR_FROM_ERROR(el, msg, err) G_STMT_START {       \
  gchar *__dbg = g_strdup (err->message);                               \
  gchar *__msg = g_strdup (msg);                                        \
  GST_WARNING_OBJECT (el, "error: %s (%s)", __msg, __dbg);              \
  gst_element_message_full (GST_ELEMENT (el), GST_MESSAGE_ERROR,        \
    err->domain, err->code,                                             \
    __msg, __dbg, __FILE__, GST_FUNCTION, __LINE__);                    \
  g_clear_error (&err); \
} G_STMT_END

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
static void skippy_hls_demux_updates_loop (SkippyHLSDemux * demux);
static void skippy_hls_demux_stop (SkippyHLSDemux * demux);
static void skippy_hls_demux_pause_tasks (SkippyHLSDemux * demux);
static gboolean skippy_hls_demux_switch_playlist (SkippyHLSDemux * demux,
    SkippyFragment * fragment);
static SkippyFragment *skippy_hls_demux_get_next_fragment (SkippyHLSDemux * demux, GError ** err);
static gboolean skippy_hls_demux_update_playlist (SkippyHLSDemux * demux, GError ** err);
static void skippy_hls_demux_reset (SkippyHLSDemux * demux, gboolean dispose);
static gboolean skippy_hls_demux_set_location (SkippyHLSDemux * demux,
    const gchar * uri);
static gchar *skippy_hls_src_buf_to_utf8_playlist (GstBuffer * buf);
static gboolean
skippy_hls_demux_switch_bitrate (SkippyHLSDemux * demux, SkippyFragment * fragment);
static void
skippy_hls_demux_setup_playlist (SkippyHLSDemux * demux);
static gboolean
skippy_hls_demux_check_for_live_update (SkippyHLSDemux * demux);

#define skippy_hls_demux_parent_class parent_class
G_DEFINE_TYPE (SkippyHLSDemux, skippy_hls_demux, GST_TYPE_ELEMENT);

#define SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME "hlsdemux-statistics"

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

  GST_DEBUG_CATEGORY_INIT (skippy_hls_demux_debug, "hlsdemux", 0,
      "hlsdemux element");
}

// TODO: send a message here on the bus instead
static void
downloader_callback (SkippyUriDownloader* downloader,
  guint64 start_time, guint64 stop_time,
  gsize bytes_loaded, gsize bytes_total)
{
  float percentage = 100.0f * bytes_loaded / bytes_total;
  GST_DEBUG ("Loaded %ld bytes of %ld -> %f percent of media interval %f to %f seconds",
    (long int) bytes_loaded,
    (long int) bytes_total,
    percentage,
    ((float) (start_time)) / GST_SECOND,
    ((float) (stop_time)) / GST_SECOND
  );
}

static void
skippy_hls_demux_init (SkippyHLSDemux * demux)
{
  /* sink pad */
  demux->sinkpad = gst_pad_new_from_static_template (&sinktemplate, "sink");
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_chain));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_sink_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  demux->playlist = NULL;

  /* Downloader */
  demux->downloader = skippy_uri_downloader_new (downloader_callback);

  /* Properties */
  demux->bitrate_limit = DEFAULT_BITRATE_LIMIT;
  demux->connection_speed = DEFAULT_CONNECTION_SPEED;
  demux->buffer_ahead_duration_secs = DEFAULT_BUFFER_AHEAD_DURATION;
  demux->caching_enabled = DEFAULT_CACHING_ENABLED;

  // Internal flags
  demux->do_typefind = TRUE;
  demux->have_group_id = FALSE;
  demux->group_id = G_MAXUINT;

  /* Streamer task */
  g_rec_mutex_init (&demux->stream_lock);
  demux->stream_task =
      gst_task_new ((GstTaskFunction) skippy_hls_demux_stream_loop, demux, NULL);
  gst_task_set_lock (demux->stream_task, &demux->stream_lock);
}

static void
skippy_hls_demux_reset (SkippyHLSDemux * demux, gboolean dispose)
{
  GST_DEBUG ("Re-setting element: (dispose=%d)", (int) dispose);

  demux->end_of_playlist = FALSE;
  demux->do_typefind = TRUE;
  demux->download_failed_count = 0;
  demux->current_download_rate = -1;

  if (demux->playlist) {
    gst_buffer_unref (demux->playlist);
    demux->playlist = NULL;
  }

  if (demux->input_caps) {
    gst_caps_unref (demux->input_caps);
    demux->input_caps = NULL;
  }

  if (demux->client) {
    skippy_m3u8_client_free (demux->client);
    demux->client = NULL;
  }

  if (!dispose) {
    demux->client = skippy_m3u8_client_new ("");
  }

  gst_segment_init (&demux->segment, GST_FORMAT_TIME);
  demux->need_segment = TRUE;
  demux->discont = TRUE;
  demux->seeked = FALSE;

  demux->have_group_id = FALSE;
  demux->group_id = G_MAXUINT;

  demux->srcpad_counter = 0;

  if (demux->srcpad) {
    gst_element_remove_pad (GST_ELEMENT_CAST (demux), demux->srcpad);
    demux->srcpad = NULL;
  }

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

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      skippy_hls_demux_reset (demux, FALSE);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      skippy_uri_downloader_cancel (demux->downloader);
      skippy_hls_demux_stop (demux);
      skippy_hls_demux_reset (demux, FALSE);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  GST_DEBUG ("State change return: %d", ret);

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
  GList *walk;
  GstClockTime current_pos, target_pos;
  gint current_sequence;
  SkippyM3U8MediaFile *file;

  GST_INFO_OBJECT (demux, "Received GST_EVENT_SEEK");
  // Not seeking on a live stream
  if (skippy_m3u8_client_is_live (demux->client)) {
    GST_WARNING_OBJECT (demux, "Received seek event for live stream");
    gst_event_unref (event);
    return FALSE;
  }
  // Parse seek event
  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start,
      &stop_type, &stop);
  if (format != GST_FORMAT_TIME) {
    gst_event_unref (event);
    return FALSE;
  }
  GST_DEBUG_OBJECT (demux, "seek event, rate: %f start: %" GST_TIME_FORMAT
      " stop: %" GST_TIME_FORMAT, rate, GST_TIME_ARGS (start),
      GST_TIME_ARGS (stop));

  /* Walk the stream data model */
  SKIPPY_M3U8_CLIENT_LOCK (demux->client);
  file = SKIPPY_M3U8_MEDIA_FILE (demux->client->current->files->data);
  current_sequence = file->sequence;
  current_pos = 0;
  target_pos = (GstClockTime) start;
  /* FIXME: Here we need proper discont handling */
  for (walk = demux->client->current->files; walk; walk = walk->next) {
    file = walk->data;

    current_sequence = file->sequence;
    if (current_pos <= target_pos
        && target_pos < current_pos + file->duration) {
      break;
    }
    current_pos += file->duration;
  }
  SKIPPY_M3U8_CLIENT_UNLOCK (demux->client);

  // If we seeked over duration just increment the sequence
  if (walk == NULL) {
    GST_DEBUG_OBJECT (demux, "seeking further than track duration");
    current_sequence++;
  }

  // Flush start
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "sending flush start");
    gst_pad_push_event (demux->srcpad, gst_event_new_flush_start ());
  }

  //Pausing streaming task
  skippy_hls_demux_pause_tasks (demux);

  // Updating data model
  SKIPPY_M3U8_CLIENT_LOCK (demux->client);
  GST_DEBUG_OBJECT (demux, "seeking to sequence %d", current_sequence);
  demux->client->sequence = current_sequence;
  demux->client->sequence_position = current_pos;
  SKIPPY_M3U8_CLIENT_UNLOCK (demux->client);

  // Update data segment
  gst_segment_do_seek (&demux->segment, rate, format, flags, start_type,
      start, stop_type, stop, NULL);
  demux->need_segment = TRUE;
  demux->seeked = TRUE;

  // Flush stop
  if (flags & GST_SEEK_FLAG_FLUSH) {
    GST_DEBUG_OBJECT (demux, "sending flush stop");
    gst_pad_push_event (demux->srcpad, gst_event_new_flush_stop (TRUE));
  }

  /* Restart the streaming task */
  gst_task_start (demux->stream_task);

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
  GstMessage *message;
  GstStructure * structure;

  switch (metric) {
  case STAT_TIME_TO_DOWNLOAD_FRAGMENT:
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "time-to-download-fragment", GST_TYPE_CLOCK_TIME, time_val,
      "fragment-size", G_TYPE_UINT64, size,
      NULL);
    break;
  case STAT_TIME_TO_PLAYLIST:
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "time-to-playlist", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  case STAT_TIME_OF_FIRST_PLAYLIST:
    structure = gst_structure_new (SKIPPY_HLS_DEMUX_STATISTIC_MSG_NAME,
      "time-of-first-playlist", GST_TYPE_CLOCK_TIME, time_val,
      NULL);
    break;
  default:
    GST_ERROR ("Can't post unknown stats type");
    return;
  }

  message =
      gst_message_new_element (GST_OBJECT_CAST (demux), structure);
  gst_element_post_message (GST_ELEMENT_CAST (demux), message);
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
  if (!skippy_m3u8_client_load_playlist (demux->client, demux->playlist)) {
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

  if (query == NULL)
    return FALSE;

  switch (query->type) {
    case GST_QUERY_DURATION:{
      GstClockTime duration = -1;
      GstFormat fmt;

      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt == GST_FORMAT_TIME) {
        duration = skippy_m3u8_client_get_duration (hlsdemux->client);
        if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
          gst_query_set_duration (query, GST_FORMAT_TIME, duration);
          ret = TRUE;
        }
      }
      GST_TRACE_OBJECT (hlsdemux, "GST_QUERY_DURATION returns %s with duration %"
          GST_TIME_FORMAT, ret ? "TRUE" : "FALSE", GST_TIME_ARGS (duration));
      break;
    }
    case GST_QUERY_URI:
      if (hlsdemux->client) {
        /* FIXME: Do we answer with the variant playlist, with the current
         * playlist or the the uri of the downloaded fragment? */
        gst_query_set_uri (query, skippy_m3u8_client_get_uri (hlsdemux->client));
        ret = TRUE;
      }
      break;
    case GST_QUERY_SEEKING:{
      GstFormat fmt;
      gint64 stop = -1;

      gst_query_parse_seeking (query, &fmt, NULL, NULL, NULL);
      GST_INFO_OBJECT (hlsdemux, "Received GST_QUERY_SEEKING with format %d",
          fmt);
      if (fmt == GST_FORMAT_TIME) {
        GstClockTime duration;

        duration = skippy_m3u8_client_get_duration (hlsdemux->client);
        if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0)
          stop = duration;

        gst_query_set_seeking (query, fmt,
            !skippy_m3u8_client_is_live (hlsdemux->client), 0, stop);
        ret = TRUE;
        GST_INFO_OBJECT (hlsdemux, "GST_QUERY_SEEKING returning with stop : %"
            GST_TIME_FORMAT, GST_TIME_ARGS (stop));
      }
      break;
    }
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

static void
switch_pads (SkippyHLSDemux * demux, GstCaps * newcaps)
{
  GstPad *oldpad = demux->srcpad;
  GstEvent *event;
  gchar *stream_id;
  gchar *name;

  GST_DEBUG ("Switching pads (oldpad:%p) with caps: %" GST_PTR_FORMAT, oldpad,
      newcaps);

  /* First create and activate new pad */
  name = g_strdup_printf ("src_%u", demux->srcpad_counter++);
  demux->srcpad = gst_pad_new_from_static_template (&srctemplate, name);
  g_free (name);
  gst_pad_set_event_function (demux->srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_event));
  gst_pad_set_query_function (demux->srcpad,
      GST_DEBUG_FUNCPTR (skippy_hls_demux_src_query));
  gst_pad_use_fixed_caps (demux->srcpad);
  gst_pad_set_active (demux->srcpad, TRUE);

  stream_id =
      gst_pad_create_stream_id (demux->srcpad, GST_ELEMENT_CAST (demux), NULL);

  event = gst_pad_get_sticky_event (demux->sinkpad, GST_EVENT_STREAM_START, 0);
  if (event) {
    if (gst_event_parse_group_id (event, &demux->group_id))
      demux->have_group_id = TRUE;
    else
      demux->have_group_id = FALSE;
    gst_event_unref (event);
  } else if (!demux->have_group_id) {
    demux->have_group_id = TRUE;
    demux->group_id = gst_util_group_id_next ();
  }
  event = gst_event_new_stream_start (stream_id);
  if (demux->have_group_id)
    gst_event_set_group_id (event, demux->group_id);

  gst_pad_push_event (demux->srcpad, event);
  g_free (stream_id);

  if (newcaps != NULL)
    gst_pad_set_caps (demux->srcpad, newcaps);

  gst_element_add_pad (GST_ELEMENT (demux), demux->srcpad);

  gst_element_no_more_pads (GST_ELEMENT (demux));

  if (oldpad) {
    /* Push out EOS */
    gst_pad_push_event (oldpad, gst_event_new_eos ());
    gst_pad_set_active (oldpad, FALSE);
    gst_element_remove_pad (GST_ELEMENT (demux), oldpad);
  }
}

static gboolean
skippy_hls_demux_configure_src_pad (SkippyHLSDemux * demux, SkippyFragment * fragment)
{
  GstCaps *bufcaps = NULL, *srccaps = NULL;
  GstBuffer *buf = NULL;
  /* Figure out if we need to create/switch pads */
  if (G_LIKELY (demux->srcpad))
    srccaps = gst_pad_get_current_caps (demux->srcpad);
  if (fragment) {
    bufcaps = skippy_fragment_get_caps (fragment);
    if (G_UNLIKELY (!bufcaps)) {
      if (srccaps)
        gst_caps_unref (srccaps);
      return FALSE;
    }
    buf = skippy_fragment_get_buffer (fragment);
  }

  if (G_UNLIKELY (!srccaps || demux->discont || (buf
              && GST_BUFFER_IS_DISCONT (buf)))) {
    switch_pads (demux, bufcaps);
    demux->need_segment = TRUE;
    demux->discont = FALSE;
    if (buf)
      GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  if (bufcaps)
    gst_caps_unref (bufcaps);
  if (G_LIKELY (srccaps))
    gst_caps_unref (srccaps);

  if (demux->need_segment) {
    /* And send a newsegment */
    GST_DEBUG_OBJECT (demux, "Sending segment event: %"
        GST_SEGMENT_FORMAT, &demux->segment);
    gst_pad_push_event (demux->srcpad, gst_event_new_segment (&demux->segment));
    demux->need_segment = FALSE;
  }
  if (buf)
    gst_buffer_unref (buf);
  return TRUE;
}

static void
skippy_hls_handle_end_of_playlist (SkippyHLSDemux * demux)
{
  GST_DEBUG_OBJECT (demux, "Reached end of playlist, sending EOS");
  demux->end_of_playlist = TRUE;
  skippy_hls_demux_configure_src_pad (demux, NULL);
  gst_pad_push_event (demux->srcpad, gst_event_new_eos ());
  skippy_hls_demux_pause_tasks (demux);
}

static gboolean
skippy_hls_push_fragment (SkippyHLSDemux * demux, SkippyFragment* fragment)
{
  GstBuffer* buf;
  GstFlowReturn ret;
  GstCaps *caps;

  if (!skippy_hls_demux_configure_src_pad (demux, fragment)) {
    g_object_unref (fragment);
    GST_ELEMENT_ERROR (demux, STREAM, TYPE_NOT_FOUND,
        ("Could not determine type of stream"), (NULL));
    return FALSE;
  }

  buf = skippy_fragment_get_buffer (fragment);

  GST_DEBUG_OBJECT (demux, "Pushing buffer %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  // Perform typefind if necessary
  if (G_UNLIKELY (demux->do_typefind)) {

    GST_DEBUG ("Doing typefind");

    caps = skippy_fragment_get_caps (fragment);

    GST_DEBUG_OBJECT (demux, "Fragment caps: %" GST_PTR_FORMAT, caps);

    if (G_UNLIKELY (!caps)) {
      GST_ELEMENT_ERROR (demux, STREAM, TYPE_NOT_FOUND,
          ("Could not determine type of stream"), (NULL));
      gst_buffer_unref (buf);
      g_object_unref (fragment);
      return FALSE;
    }

    if (!demux->input_caps || !gst_caps_is_equal (caps, demux->input_caps)) {
      gst_caps_replace (&demux->input_caps, caps);
      /* gst_pad_set_caps (demux->srcpad, demux->input_caps); */
      GST_INFO_OBJECT (demux, "Input source caps: %" GST_PTR_FORMAT,
          demux->input_caps);
      demux->do_typefind = FALSE;
    }
    gst_caps_unref (caps);
  } else {
    skippy_fragment_set_caps (fragment, demux->input_caps);
  }

  /* Flagging buffer as discontinuous */
  if (fragment->discontinuous) {
    GST_DEBUG_OBJECT (demux, "Marking fragment as discontinuous");
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
  } else {
    GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DISCONT);
  }

  // Updating current data segment properties
  demux->segment.position = GST_BUFFER_TIMESTAMP (buf);
  demux->seeked = FALSE;
  if (demux->segment.rate > 0)
    demux->segment.position += GST_BUFFER_DURATION (buf);

  /* Push buffer */
  ret = gst_pad_push (demux->srcpad, buf);
  if (ret != GST_FLOW_OK) {
    g_object_unref (fragment);
    if (ret == GST_FLOW_NOT_LINKED || ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (demux, STREAM, FAILED, (NULL),
          ("Error pushing buffer (will send EOS): %s", gst_flow_get_name (ret)));
      gst_pad_push_event (demux->srcpad, gst_event_new_eos ());
    } else {
      GST_DEBUG_OBJECT (demux, "Can't push buffer: %s", gst_flow_get_name (ret));
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
      GST_LOG ("Current position query result: %lld ms", (long long int) pos / 1000000);
    }
    gst_query_unref (query);
    if (!query_ret) {
      GST_WARNING ("Position query did not give proper result!");
      // We assume this can happen at the very beginning of the streaming session
      // when pipeline position has some undefined state (as observed)
      pos = 0;
    }
    GST_DEBUG ("Buffer ahead duration is %d seconds", (int) demux->buffer_ahead_duration_secs);
    if (!demux->seeked && demux->segment.position >= pos + demux->buffer_ahead_duration_secs * GST_SECOND) {
      GST_LOG ("Blocking task as we have buffered enough until now (up to %f seconds of media position)",
        ((float) demux->segment.position) / GST_SECOND);
      g_usleep (1000000);
      return FALSE;
    }
  }
  return TRUE;
}

static void
skippy_hls_demux_stream_loop (SkippyHLSDemux * demux)
{
  SkippyFragment *fragment;
  GError *err = NULL;

  GST_DEBUG_OBJECT (demux, "Entering stream loop");

  if (!skippy_hls_check_buffer_ahead (demux)) {
    return;
  }

  GST_DEBUG ("Will fetch next fragment ...");

  /* Handle failure to get the next fragment */
  if ((fragment =
          skippy_hls_demux_get_next_fragment (demux, &err)) == NULL) {

    GST_DEBUG ("Got NULL as next fragment");

    if (fragment->cancelled) {
      GST_DEBUG ("Fragment fetch got cancelled");
      return;
    }

    // Is it just end of playlist?
    if (!err) {
      if (skippy_m3u8_client_is_live (demux->client)) {
        GST_DEBUG_OBJECT (demux, "No fragment left but live playlist, retrying later");
      } else {
        skippy_hls_handle_end_of_playlist (demux);
      }
      return;
    }

    GST_DEBUG ("Failed to fetch fragment for %d times.", demux->download_failed_count);

    // Actual download failure
    demux->download_failed_count++;

    // We only want to refetch the playlist if we get a 403 or a 404
    if ((g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)
        // FIXME: This error comes also on connection failures. We'd want to differentiate between a 404 and a connection failure.
        // || g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND)
        )) {
        g_clear_error (&err);
      if (skippy_hls_demux_update_playlist (demux, &err)) {
        // Success means err === NULL
        return;
      }
      /* Retry immediately, the playlist actually has changed */
      GST_DEBUG_OBJECT (demux, "Updated the playlist because of 403 or 404");
    }

    // Silently ignore any other error
    if (err) {
      g_clear_error (&err);
    }

    GST_DEBUG_OBJECT (demux, "Retrying now");
    return;
  }

  // Reset failure counter
  demux->download_failed_count = 0;

  GST_DEBUG ("Got next fragment");

  // Go to next fragment
  skippy_m3u8_client_advance_fragment (demux->client);

  // Push fragment onto pipeline
  skippy_hls_push_fragment (demux, fragment);

  // Check for live update of playlist
  skippy_hls_demux_check_for_live_update (demux);

  g_object_unref (fragment);

  return;
}

/* Interacts with M3U8 client and URI downloader to provide next fragment to streaming loop,
measures download speed, posts stat message and decrypt fragment buffer, triggers bitrate switching */
static SkippyFragment *
skippy_hls_demux_get_next_fragment (SkippyHLSDemux * demux, GError ** err)
{
  GstStructure *stat_msg;
  guint64 size;
  gboolean allow_cache = demux->caching_enabled;
  SkippyFragment* fragment;

  g_return_val_if_fail (*err == NULL, NULL);

  fragment = skippy_m3u8_client_get_next_fragment (demux->client);
  if (!fragment) {
    GST_INFO_OBJECT (demux, "This playlist doesn't contain more fragments");
    return NULL;
  }

  GST_INFO_OBJECT (demux,
      "Fetching next fragment %s (range=%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT
      ")", fragment->uri, fragment->range_start, fragment->range_end);

  skippy_uri_downloader_fetch_fragment (demux->downloader,
    fragment, // Media fragment to load
    demux->client->main ? demux->client->main->uri : NULL, // Referrer
    FALSE, // Compress (useless with coded media data)
    FALSE, // Refresh (don't wipe out cache)
    demux->client->current ? demux->client->current->allowcache && allow_cache : allow_cache, // Allow caching directive
    err // Error
  );

  if (!fragment->completed || !fragment->decrypted) {
    return NULL;
  }

  // Size of transferred payload (may be encrypted)
  size = skippy_fragment_get_buffer_size (fragment);
  skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_DOWNLOAD_FRAGMENT,
    fragment->download_stop_time - fragment->download_start_time,
    size);

  /* try to switch to another bitrate if needed */
#if 0
  skippy_hls_demux_switch_bitrate (demux, fragment);
#endif

  GST_DEBUG ("Returning finished fragment");

  return fragment;
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
  GError* err = NULL;

  /* Updating playlist only needed for live playlists */
  if (skippy_m3u8_client_is_live (demux->client)) {
    /* Wait here until we should do the next update or we're cancelled */
    GST_DEBUG_OBJECT (demux, "Checking for next playlist update ...");
    if (g_get_monotonic_time () < demux->next_update) {
      GST_DEBUG ("Too early, waiting more");
      return FALSE;
    }

    GST_DEBUG_OBJECT (demux, "Updating playlist ...");

    if (!skippy_hls_demux_update_playlist (demux, &err)) { // When failing
      if (err) {
        GST_ERROR ("Got error updating live playlist: %s", err->message);
      }
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

static void
skippy_hls_demux_setup_playlist (SkippyHLSDemux * demux)
{
  SkippyM3U8 *variant = NULL;
  GError *err = NULL;

  /* If this playlist is a variant playlist, select the first one
   * and update it */
  if (skippy_m3u8_client_has_variant_playlist (demux->client)) {
    // Select child playlist
    if (demux->connection_speed == 0) {
      variant = skippy_m3u8_client_get_current_variant (demux->client);
    } else {
      variant = skippy_m3u8_client_get_playlist_for_bitrate (demux->client,
          demux->connection_speed);
    }
    // Fetch child playlist
    skippy_m3u8_client_set_current (demux->client, variant);

    if (!skippy_hls_demux_update_playlist (demux, &err)) {
      GST_ELEMENT_ERROR_FROM_ERROR (demux, "Could not fetch child playlist", err);
      return;
    }
  }

  // Post duration message if non-live
  if (!skippy_m3u8_client_is_live (demux->client)) {
    GstClockTime duration = skippy_m3u8_client_get_duration (demux->client);

    GST_DEBUG_OBJECT (demux, "Sending duration message : %" GST_TIME_FORMAT,
        GST_TIME_ARGS (duration));
    if (duration != GST_CLOCK_TIME_NONE)
      gst_element_post_message (GST_ELEMENT (demux),
          gst_message_new_duration_changed (GST_OBJECT (demux)));
  }

  GST_DEBUG_OBJECT (demux, "Finished setting up playlist");
  return;
}

static gboolean
skippy_hls_demux_update_playlist (SkippyHLSDemux * demux, GError ** err)
{
  SkippyFragment *download;
  GstBuffer *buf;
  gboolean updated = FALSE;
  GstStructure *stat_msg;

  download = skippy_fragment_new (skippy_m3u8_client_get_current_uri (demux->client), NULL, NULL);

  skippy_uri_downloader_fetch_fragment (demux->downloader,
    download, // Media fragment to load
    demux->client->main ? demux->client->main->uri : NULL, // Referrer
    TRUE, // Compress (good for playlists)
    TRUE, // Refresh (wipe out cached stuff)
    demux->client->current ? demux->client->current->allowcache && demux->caching_enabled : demux->caching_enabled, // Allow caching directive
    err // Error
  );

  skippy_hls_demux_post_stat_msg (demux, STAT_TIME_TO_PLAYLIST, download->download_stop_time - download->download_start_time, 0);

  // Load M3U8 buffer into parser
  if (!skippy_m3u8_client_load_playlist (demux->client, skippy_fragment_get_buffer (download))) {
    GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid playlist."), (NULL));
    return FALSE;
  }

  // Sequence numbers across variants may differ, and we need to input our current position to reach
  // the correct sequence number (we might have to re-issue a segment event as well
  // if the current sequence number is too far ahead of a live stream)
  skippy_m3u8_client_update_playlist_position (demux->client, demux->segment.position, &demux->need_segment);

  g_object_unref (download);

  return updated;
}

#if 0
static gboolean
skippy_hls_demux_change_playlist (SkippyHLSDemux * demux, guint max_bitrate)
{
  GList *previous_variant, *current_variant;
  gint old_bandwidth, new_bandwidth;

  /* If user specifies a connection speed never use a playlist with a bandwidth
   * superior than it */
  if (demux->connection_speed != 0 && max_bitrate > demux->connection_speed)
    max_bitrate = demux->connection_speed;

  previous_variant = demux->client->main->current_variant;
  current_variant = skippy_m3u8_client_get_playlist_for_bitrate (demux->client,
      max_bitrate);

retry_failover_protection:
  old_bandwidth = SKIPPY_M3U8 (previous_variant->data)->bandwidth;
  new_bandwidth = SKIPPY_M3U8 (current_variant->data)->bandwidth;

  /* Don't do anything else if the playlist is the same */
  if (new_bandwidth == old_bandwidth) {
    return TRUE;
  }

  demux->client->main->current_variant = current_variant;
  SKIPPY_M3U8_CLIENT_UNLOCK (demux->client);

  skippy_m3u8_client_set_current (demux->client, current_variant->data);

  GST_INFO_OBJECT (demux, "Client was on %dbps, max allowed is %dbps, switching"
      " to bitrate %dbps", old_bandwidth, max_bitrate, new_bandwidth);
  demux->discont = TRUE;

  if (skippy_hls_demux_update_playlist (demux, FALSE, NULL)) {
    GstStructure *s;

    s = gst_structure_new ("playlist",
        "uri", G_TYPE_STRING, skippy_m3u8_client_get_current_uri (demux->client),
        "bitrate", G_TYPE_INT, new_bandwidth, NULL);
    gst_element_post_message (GST_ELEMENT_CAST (demux),
        gst_message_new_element (GST_OBJECT_CAST (demux), s));
  } else {
    GList *failover = NULL;

    GST_INFO_OBJECT (demux, "Unable to update playlist. Switching back");
    SKIPPY_M3U8_CLIENT_LOCK (demux->client);

    failover = g_list_previous (current_variant);
    if (failover && new_bandwidth == SKIPPY_M3U8 (failover->data)->bandwidth) {
      current_variant = failover;
      goto retry_failover_protection;
    }

    demux->client->main->current_variant = previous_variant;
    SKIPPY_M3U8_CLIENT_UNLOCK (demux->client);
    skippy_m3u8_client_set_current (demux->client, previous_variant->data);
    /*  Try a lower bitrate (or stop if we just tried the lowest) */
    if (new_bandwidth ==
        SKIPPY_M3U8 (g_list_first (demux->client->main->lists)->data)->bandwidth)
      return FALSE;
    else
      return skippy_hls_demux_change_playlist (demux, new_bandwidth - 1);
  }

  /* Force typefinding since we might have changed media type */
  demux->do_typefind = TRUE;

  return TRUE;
}

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
  GST_DEBUG_CATEGORY_INIT (skippy_hls_demux_debug, "skippyhlsdemux", 0,
      "Skippy HLS client");

  gst_element_register (NULL, "skippyhlsdemux", GST_RANK_PRIMARY + 100,
      TYPE_SKIPPY_HLS_DEMUX);
}

