#include <glib-object.h>

#include <gst/gst.h>

#include <skippyHLS/skippy_urisrc.h>

int
main (int argc, char **argv)
{
	GError *err = NULL;
	GstElement* src, *sink;
	GMainLoop* mainloop = g_main_loop_new(NULL, FALSE);

	gst_init (&argc, &argv);
	skippy_urisrc_setup ();

	src = gst_element_factory_make("skippyurisrc", NULL);
	sink = gst_element_factory_make("fakesink", NULL);

	gst_uri_handler_set_uri (GST_URI_HANDLER(src), "http://www.example.com", &err);

	gst_element_link (src, sink);

	gst_element_set_state (sink, GST_STATE_PLAYING);
	gst_element_set_state (src, GST_STATE_PLAYING);

	//gst_element_set_state (src, GST_STATE_NULL);

	g_main_loop_run (mainloop);
}