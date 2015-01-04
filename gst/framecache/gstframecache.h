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

typedef struct
{
  GList *events;
  GstClockTime timestamp;
} EventList;

/**
 * GstFrameCache:
 *
 * The opaque #GstFrameCache data structure.
 */
struct _GstFrameCache
{
  GstElement element;

  /* Our exposed properties, not actually used for now
   * TODO: use them
   */
  GstClockTime duration_soft_limit;
  guint64 size_soft_limit;

  GstPad *sinkpad;
  GstPad *srcpad;

  GstCaps *sinkcaps;

  GstClockTime start;
  GstClockTime stop;

  gboolean send_events;

  gboolean passthrough;

  GCond buffer_cond;
  GMutex buffer_lock;

  /* Lock of the actual frame cache, must be taken to
   * access fc->buffers, also used to protect 
   * waiting_flush_start
   */
  GMutex lock;
  /* Our frame cache, implemented as a sequence to get
   * fast random access
   */
  GSequence *buffers;

  /* Gotta be a bit clever here.
   * sequence of list of serialized events, sorted by position.
   * Fully apply to the following buffers.
   * protected by fc->lock too.
   */
  GSequence *events;
  /* Events that haven't yet been timestamped and added to events */
  EventList *pending_events;
  /* Last events sent downstream */
  EventList *sent_events;

  /* whether the src pad task should continue looping */
  gboolean running;

  /* Whether upstream finished sending us the last
   * segment we requested 
   */
  gboolean segment_done;

  /* Whether we should stop seeking further
   * FIXME: ugly, we'll want scrub backward to work too
   */
  gboolean forward_done;

  /* whether newly chained buffers should be
   * rejected, lock with fc->lock, a bit ugly
   * but does the job */
  gboolean wait_flush_start;

  /* What downstream requested from us */
  GstSegment requested_segment;

  /* What upstream currently sends us */
  GstSegment current_segment;

  gint seek_seqnum;
};

struct _GstFrameCacheClass
{
  GstElementClass element_class;
};

G_GNUC_INTERNAL GType gst_frame_cache_get_type (void);

G_END_DECLS
#endif /* __GST_FRAME_CACHE_H__ */
