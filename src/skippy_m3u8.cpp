#include <string>
#include <mutex>
#include <string.h> // for memcpy

#include <skippyHLS/skippy_m3u8.h>
#include <skippyHLS/skippy_fragment.h>

#include <skippyHLS/SkippyM3UParser.hpp>

GST_DEBUG_CATEGORY_STATIC (skippy_m3u8_debug);
#define GST_CAT_DEFAULT skippy_m3u8_debug

#define NANOSECONDS_TO_GST_TIME(t) ((GstClockTime)t*GST_NSECOND)

using namespace std;

struct SkippyM3U8ClientPrivate
{
  SkippyM3U8ClientPrivate (string uri)
  :current_index(0)
  ,playlist(uri)
  {

  }

  ~SkippyM3U8ClientPrivate ()
  {

  }

  int current_index;
  SkippyM3UPlaylist playlist;
  recursive_mutex mutex;
};

void skippy_m3u8_client_init ()
{
  GST_DEBUG_CATEGORY_INIT (skippy_m3u8_debug, "skippyhls-m3u8", 0, "M3U8 client");
}

SkippyM3U8Client *skippy_m3u8_client_new (const gchar * uri)
{
  SkippyM3U8Client* client = g_slice_new0 (SkippyM3U8Client);
  client->priv = new SkippyM3U8ClientPrivate(string(uri));
  return client;
}

void skippy_m3u8_client_free (SkippyM3U8Client * client)
{
  delete client->priv;
  g_slice_free(SkippyM3U8Client, client);
}

static gchar* buf_to_utf8_playlist (GstBuffer * buf)
{
  GstMapInfo info;
  gchar *playlist;

  if (!gst_buffer_map (buf, &info, GST_MAP_READ)) {
    return NULL;
  }

  if (!g_utf8_validate ((gchar *) info.data, info.size, NULL)) {
    gst_buffer_unmap (buf, &info);
    return NULL;
  }

  /* alloc size + 1 to end with a null character */
  playlist = (gchar*) g_malloc0 (info.size + 1);
  memcpy (playlist, info.data, info.size);

  gst_buffer_unmap (buf, &info);
  return playlist;
}

// Update/set/identify variant (sub-) playlist by URIs advertised in master playlist
gboolean skippy_m3u8_client_load_playlist (SkippyM3U8Client * client, const gchar *uri, GstBuffer* playlist_buffer)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  SkippyM3UParser p;
  string loaded_playlist_uri = client->priv->playlist.uri;

  gchar* playlist = buf_to_utf8_playlist (playlist_buffer);
  if (!playlist) {
    return FALSE;
  }

  if (uri != NULL) {
    loaded_playlist_uri = string(uri);
  }

  client->priv->playlist = p.parse(loaded_playlist_uri, string(playlist));

  g_free (playlist);
  return TRUE;
}

// Called to get the next fragment
SkippyFragment* skippy_m3u8_client_get_current_fragment (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  SkippyFragment *fragment;
  SkippyM3UItem item;

  if (client->priv->current_index >= client->priv->playlist.items.size()) {
    return NULL;
  }

  item = client->priv->playlist.items.at (client->priv->current_index);

  fragment = skippy_fragment_new (item.url.c_str(), NULL, NULL);
  fragment->start_time = NANOSECONDS_TO_GST_TIME (item.start);
  fragment->stop_time = NANOSECONDS_TO_GST_TIME (item.end);
  fragment->duration = NANOSECONDS_TO_GST_TIME (item.duration);
  fragment->discontinuous = TRUE;
  return fragment;
}

void skippy_m3u8_client_advance_to_next_fragment (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  if (client->priv->current_index < client->priv->playlist.items.size()) {
    client->priv->current_index++;
  }
}

gboolean skippy_m3u8_client_seek_to (SkippyM3U8Client * client, GstClockTime target)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);

  SkippyM3UItem item;
  unsigned long target_pos = (unsigned long) GST_TIME_AS_NSECONDS(target);

  GST_LOG ("Seek to target: %ld ns", target_pos);

  for (int i=0;i<client->priv->playlist.items.size();i++) {
    item = client->priv->playlist.items.at(i);
    if (target_pos >= item.start && target_pos < item.end)
    {
      GST_LOG ("Seeked to index %d, interval %ld - %ld", i, (long) item.start, (long) item.end);
      client->priv->current_index = i;
      return TRUE;
    }
  }
  return FALSE;
}

const gchar* skippy_m3u8_client_get_uri(SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return client->priv->playlist.uri.c_str();
}

const gchar* skippy_m3u8_client_get_playlist_for_bitrate (SkippyM3U8Client * client, guint bitrate)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return NULL;
}

const gchar *skippy_m3u8_client_get_current_playlist (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return client->priv->playlist.uri.c_str();
}

void skippy_m3u8_client_set_current_playlist (SkippyM3U8Client * client, const gchar *uri)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  ;
}

GstClockTime skippy_m3u8_client_get_total_duration (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return NANOSECONDS_TO_GST_TIME (client->priv->playlist.totalDuration);
}

GstClockTime skippy_m3u8_client_get_target_duration (SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return NANOSECONDS_TO_GST_TIME (client->priv->playlist.targetDuration);
}

gboolean skippy_m3u8_client_has_variant_playlist(SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return FALSE;
}

gboolean skippy_m3u8_client_is_live(SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  if (client->priv->playlist.type == "event") {
    return TRUE;
  }
  // Defaults to "vod"
  return FALSE;
}

gboolean skippy_m3u8_client_allow_cache(SkippyM3U8Client * client)
{
  lock_guard<recursive_mutex> lock(client->priv->mutex);
  return TRUE;
}