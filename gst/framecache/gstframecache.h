#ifndef __GST_FRAME_CACHE_H__
#define __GST_FRAME_CACHE_H__

#include <gst/gst.h>

G_BEGIN_DECLS
#define GST_TYPE_FRAME_CACHE \
  (gst_frame_cache_get_type())
#define GST_FRAME_CACHE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FRAME_CACHE,GstFrameCache))
#define GST_FRAME_CACHE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FRAME_CACHE,GstFrameCacheClass))
#define GST_IS_FRAME_CACHE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FRAME_CACHE))
#define GST_IS_FRAME_CACHE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FRAME_CACHE))
typedef struct _GstFrameCache GstFrameCache;
typedef struct _GstFrameCacheClass GstFrameCacheClass;

/**
 * GstFrameCache:
 *
 * The opaque #GstFrameCache data structure.
 */
struct _GstFrameCache
{
  GstElement element;

  GstClockTime duration_soft_limit;
  guint64 size_soft_limit;
  GstPad *sinkpad;
  GstPad *srcpad;

  GstCaps *sinkcaps;
  GstClockTime start;
  GstClockTime current_position;
  GstClockTime stop;
  gboolean send_events;
  gboolean passthrough;
  GCond buffer_cond;
  GMutex buffer_lock;
  GMutex lock;
  GSequence *buffers;
  gboolean running;
  gboolean segment_done;
  gboolean forward_done;
  gboolean wait_flush_start;
  GstSegment requested_segment;
  GstSegment current_segment;
};

struct _GstFrameCacheClass
{
  GstElementClass element_class;
};

G_GNUC_INTERNAL GType gst_frame_cache_get_type (void);

G_END_DECLS
#endif /* __GST_FRAME_CACHE_H__ */
