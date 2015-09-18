#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/base/base.h>

#include <curl/curl.h>

#pragma once

#define TYPE_SKIPPY_URISRC            (skippy_urisrc_get_type())
#define SKIPPY_URISRC(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),TYPE_SKIPPY_URISRC,SkippyUriSrc))
#define IS_SKIPPY_URISRC(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),TYPE_SKIPPY_URISRC))
#define SKIPPY_URISRC_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass) ,TYPE_SKIPPY_URISRC,SkippyUriSrcClass))
#define IS_SKIPPY_URISRC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass) ,TYPE_SKIPPY_URISRC))
#define SKIPPY_URISRC_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj) ,TYPE_SKIPPY_URISRC,SkippyUriSrcClass))

typedef struct _SkippyUriSrc SkippyUriSrc;
typedef struct _SkippyUriSrcClass SkippyUriSrcClass;

struct _SkippyUriSrc {
  GstPushSrc parent;

  gchar* location;
  gboolean mock_mode;

  gboolean cancel;

  CURL * curl;

  GstAdapter* adapter;

  GstTask * task;
  GRecMutex task_lock;
};

struct _SkippyUriSrcClass {
  GstPushSrcClass parent_class;
};

G_GNUC_INTERNAL
GType skippy_urisrc_get_type (void);

void
skippy_urisrc_set_mock_mode (SkippyUriSrc* self, gboolean enabled);

void
skippy_urisrc_setup (void);