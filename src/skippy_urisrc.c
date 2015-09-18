#include "skippyHLS/skippy_urisrc.h"

#define GST_CAT_DEFAULT skippy_urisrc_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

static void
skippy_urisrc_uri_handler_init (gpointer g_iface, gpointer iface_data);

#define skippy_urisrc_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (SkippyUriSrc, skippy_urisrc, GST_TYPE_PUSH_SRC,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        skippy_urisrc_uri_handler_init));

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static const gchar *protocols[] = { "http", "https", NULL };

// URI handler

static guint
skippy_urisrc_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
skippy_urisrc_uri_get_protocols (GType type)
{
  return protocols;
}

static gchar *
skippy_urisrc_uri_get_uri (GstURIHandler * handler)
{
  SkippyUriSrc *self = SKIPPY_URISRC (handler);
  gchar *uri = NULL;
  GST_OBJECT_LOCK (self);
  if (self->location)
    uri = g_strdup (self->location);
  GST_OBJECT_UNLOCK (self);
  return uri;
}

static gboolean
skippy_urisrc_uri_set_uri (GstURIHandler * handler, const gchar * uri, GError ** error)
{
  SkippyUriSrc *self = SKIPPY_URISRC (handler);
  GST_OBJECT_LOCK (self);
  g_free (self->location);
  self->location = g_strdup (uri);
  GST_OBJECT_UNLOCK (self);
  return TRUE;
}

static void
skippy_urisrc_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = skippy_urisrc_uri_get_type;
  iface->get_protocols = skippy_urisrc_uri_get_protocols;
  iface->get_uri = skippy_urisrc_uri_get_uri;
  iface->set_uri = skippy_urisrc_uri_set_uri;
}

// CURL

size_t skippy_urisrc_curl_write_callback (char *ptr, size_t size, size_t nmemb, void *userdata) {
  SkippyUriSrc* self = SKIPPY_URISRC (userdata);

  GST_TRACE ("CURL writes %d times %d bytes", (int) size, (int) nmemb);

  GstBuffer* buf;
  size_t buf_size = nmemb*size;
  void* data = g_malloc (buf_size);

  memcpy (data, ptr, buf_size);
  buf = gst_buffer_new_wrapped (data, buf_size);

  GST_OBJECT_LOCK (self);
  gst_adapter_push (self->adapter, buf);
  GST_OBJECT_UNLOCK (self);

  // TODO: free CURL bytes here

  return buf_size;
}

static int
skippy_urisrc_curl_progress_callback (void *clientp,   double dltotal,   double dlnow,   double ultotal, double ulnow) {

  SkippyUriSrc *self = SKIPPY_URISRC(clientp);
  gboolean cancel;

  GST_OBJECT_LOCK (self);
  cancel = self->cancel;
  GST_OBJECT_UNLOCK (self);

  if (cancel) {
    GST_TRACE ("Cancel: %d", (int) cancel);
  }

  return cancel;
}

// Helpers

static void
skippy_urisrc_close_curl (SkippyUriSrc* self) {

  GST_TRACE ("Close CURL");;

  gst_task_join (self->task);

  if (self->curl) {
    GST_TRACE ("Cleaning up CURL");
    curl_easy_cleanup(self->curl);
    self->curl = NULL;
  }
}

// Virtual methods overloads

static GstFlowReturn skippy_urisrc_create (GstPushSrc *src, GstBuffer **buf)
{
  SkippyUriSrc *self = SKIPPY_URISRC(src);

  GST_OBJECT_LOCK (self);

  GstBuffer *newbuf;
  gsize available = gst_adapter_available (self->adapter);

  if (!available) {
    *buf = newbuf = gst_buffer_new();
  } else {
    GST_TRACE ("Got %d bytes in adapter", (int) available);
    *buf = newbuf = gst_adapter_take_buffer (self->adapter, available);
    GST_TRACE ("Got buffer (size=%d): %" GST_PTR_FORMAT, (int) gst_buffer_get_size(newbuf), buf);
  }

  GST_OBJECT_UNLOCK (self);
	return GST_FLOW_OK;
}

gboolean skippy_urisrc_start (GstBaseSrc *src) {
  SkippyUriSrc *self = SKIPPY_URISRC(src);

  GST_TRACE ("Start");

  GST_OBJECT_LOCK (self);
  self->cancel = FALSE;
  GST_OBJECT_UNLOCK (self);

  gst_task_start (self->task);
  return TRUE;
}

gboolean skippy_urisrc_stop (GstBaseSrc *src) {
  SkippyUriSrc *self = SKIPPY_URISRC(src);

  GST_TRACE ("Stop");

  GST_OBJECT_LOCK (self);
  self->cancel = TRUE;
  GST_OBJECT_UNLOCK (self);

  skippy_urisrc_close_curl (self);

  GST_OBJECT_LOCK (self);
  gst_adapter_flush (self->adapter, gst_adapter_available (self->adapter));
  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

// Task func

static void
skippy_urisrc_task_loop (SkippyUriSrc* self) {
  CURLcode res;
  CURL* curl = self->curl = curl_easy_init ();
  GST_TRACE ("Task entered");
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, skippy_urisrc_curl_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, self);
  curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, skippy_urisrc_curl_progress_callback);
  curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, self);
  curl_easy_setopt(curl, CURLOPT_URL, self->location);
  res = curl_easy_perform(curl);
  // don't run the function again before we are restarted
  gst_task_stop (self->task);
  GST_TRACE ("Task exiting");
}

// Class init

static void
skippy_urisrc_class_init (SkippyUriSrcClass *klass)
{
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);

  gst_element_class_add_pad_template (gstelement_class,
    gst_static_pad_template_get (&srctemplate));

  gst_element_class_set_static_metadata (gstelement_class,
     "Skippy URI Source",
     "SkippyUriSrc",
     "A versatile URI source",
     "Stephan Hesse <stephan@soundcloud.com>");

  gstpushsrc_class->create = skippy_urisrc_create;
  gstbasesrc_class->start = skippy_urisrc_start;
  gstbasesrc_class->stop = skippy_urisrc_stop;
}

// Constructor

static void
skippy_urisrc_init (SkippyUriSrc * self)
{
  g_rec_mutex_init (&self->task_lock);
  self->task = gst_task_new ((GstTaskFunction) skippy_urisrc_task_loop, self, NULL);
  gst_task_set_lock (self->task, &self->task_lock);

  self->adapter = gst_adapter_new ();
}

// TODO: hook this up in class init
static void
skippy_urisrc_finalize (SkippyUriSrc * self)
{
  // etc ...
}

// Public functions

void
skippy_urisrc_set_mock_mode (SkippyUriSrc* self, gboolean enabled) {
  self->mock_mode = enabled;
}

void
skippy_urisrc_setup (void)
{
  GST_DEBUG_CATEGORY_INIT (skippy_urisrc_debug, "skippyurisrc", 0,
      "Skippy URI Source");

  gst_element_register (NULL, "skippyurisrc", GST_RANK_PRIMARY, TYPE_SKIPPY_URISRC);
}