#ifndef __GST_LIVE_SOURCE_WRAPPER_H__
#define __GST_LIVE_SOURCE_WRAPPER_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_LIVE_SOURCE_WRAPPER \
  (gst_live_source_wrapper_get_type())
#define GST_LIVE_SOURCE_WRAPPER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_LIVE_SOURCE_WRAPPER,GstLiveSourceWrapper))
#define GST_LIVE_SOURCE_WRAPPER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_LIVE_SOURCE_WRAPPER,GstLiveSourceWrapperClass))
#define GST_IS_LIVE_SOURCE_WRAPPER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_LIVE_SOURCE_WRAPPER))
#define GST_IS_LIVE_SOURCE_WRAPPER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_LIVE_SOURCE_WRAPPER))

typedef struct _GstLiveSourceWrapper GstLiveSourceWrapper;
typedef struct _GstLiveSourceWrapperClass GstLiveSourceWrapperClass;

struct _GstLiveSourceWrapper
{
  GstBin parent;

  GstPad *srcpad;
  GstElement *wrapped_source;
  GstEvent *seek_event;
};

struct _GstLiveSourceWrapperClass
{
  GstBinClass parent_class;
};

GType gst_live_source_wrapper_get_type (void);

G_END_DECLS

#endif /* __GST_LIVE_SOURCE_WRAPPER_H__ */
