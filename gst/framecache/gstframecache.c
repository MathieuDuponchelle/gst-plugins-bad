#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include "gstframecache.h"

/* Soft limits, can and will be slightly exceeded */
#define DEFAULT_DURATION 5 * GST_SECOND
#define DEFAULT_BYTE_SIZE 1024 * 1024 * 1024

#define DEFAULT_LOOKAHEAD_DURATION 2 * GST_SECOND

GST_DEBUG_CATEGORY_STATIC (gst_frame_cache_debug);
#define GST_CAT_DEFAULT gst_frame_cache_debug

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_frame_cache_debug, "framecache", 0, \
    "frame caching element");
#define gst_frame_cache_parent_class parent_class

G_DEFINE_TYPE_WITH_CODE (GstFrameCache, gst_frame_cache, GST_TYPE_ELEMENT,
    _do_init);

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

enum
{
  PROP_0,
  PROP_DURATION_SOFT_LIMIT,
  PROP_BYTE_SIZE_SOFT_LIMIT,
  PROP_LAST,
};

static GParamSpec *properties[PROP_LAST];

/*
static GstClockTime
_get_buffer_pts (GstBuffer *buffer)
{
  return (GST_BUFFER_PTS (buffer));
}
*/

static gboolean
_buffer_covers_position (GstBuffer *buffer, GstClockTime position)
{
  return GST_BUFFER_PTS (buffer) <= position &&
         GST_BUFFER_PTS (buffer) + GST_BUFFER_DURATION (buffer) > position;
}

static gint
search_buffer (gconstpointer data, gconstpointer cmp_data, gpointer unused)
{
  GstClockTime searched = *(GstClockTime *) cmp_data;
  GstClockTime compared = GST_BUFFER_PTS (GST_BUFFER (data));

  if (compared < searched)
    return -1;
  else if (compared == searched)
    return 0;
  return 1;
}

static GstBuffer *
_get_cached_buffer (GstFrameCache *fc, GstClockTime position)
{
  GstBuffer *buffer = NULL;
  GSequenceIter *iter;

  iter = g_sequence_search (fc->buffers, &position, search_buffer, NULL);

  if (!g_sequence_iter_is_end (iter)) {
    buffer = g_sequence_get (iter);

    if (GST_BUFFER_PTS (buffer) == position)
      goto done;
  }

  if (g_sequence_iter_is_begin (iter)) {
    buffer = NULL;
    goto done;
  }

  iter = g_sequence_iter_prev (iter);
  buffer = g_sequence_get (iter);

  if (_buffer_covers_position (buffer, position))
    goto done;

  buffer = NULL;

done:
  return buffer;
}

static gboolean
_buffer_is_in_current_segment (GstFrameCache *fc, GstClockTime position)
{
  return fc->start <= position && fc->stop > position;
}

static void
_clear_buffers (GstFrameCache *fc)
{
  GST_ERROR ("clearing buffers, peace");
  g_sequence_free (fc->buffers);
  fc->buffers = g_sequence_new ((GDestroyNotify) gst_buffer_unref);
  fc->start = GST_CLOCK_TIME_NONE;
  fc->stop = GST_CLOCK_TIME_NONE;
}

static GstClockTime
_get_iter_pts (GSequenceIter *iter)
{
  return (GST_BUFFER_PTS (GST_BUFFER(g_sequence_get (iter))));
}

static void
_update_buffers (GstFrameCache *fc)
{
  GSequenceIter *first = g_sequence_get_begin_iter (fc->buffers);
  GSequenceIter *last = g_sequence_get_end_iter (fc->buffers);
  GstClockTimeDiff interval;

  last = g_sequence_iter_prev (last);

  GST_ERROR ("first buffer at %" GST_TIME_FORMAT, GST_TIME_ARGS (_get_iter_pts (first)));
  GST_ERROR ("last buffer at %" GST_TIME_FORMAT, GST_TIME_ARGS (_get_iter_pts (last)));
  interval = _get_iter_pts (last) - _get_iter_pts (last);

  if (interval > DEFAULT_DURATION)
    GST_ERROR ("Imma free shit up");
}

static void
_broadcast_src_pad (GstFrameCache *fc)
{
  gst_pad_push_event (fc->srcpad, gst_event_new_flush_start ());
  gst_pad_push_event (fc->srcpad, gst_event_new_flush_stop (TRUE));
  g_cond_broadcast (&fc->buffer_cond);
}

static void
_maybe_push_buffer (GstFrameCache *fc)
{
  GstBuffer *cached;

  cached = _get_cached_buffer (fc, fc->requested_segment.start);

  if (cached) {
    /* We already have that buffer, only need to send it */
    GST_ERROR ("we already have that buffer, that's a win !");
    _broadcast_src_pad (fc);
  }
}

static gboolean
_do_seek (GstFrameCache *fc, GstPad *pad, GstEvent *event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  gboolean ret;
  gboolean update;

  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
  gst_segment_do_seek (&fc->requested_segment, rate, format, flags, start_type, start, stop_type, stop, &update);
  gst_event_unref (event);

  if (_buffer_is_in_current_segment (fc, start)) {
    _maybe_push_buffer (fc);
    return TRUE;
  }

  _clear_buffers (fc);

  event = gst_event_new_seek (rate, format,
      GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_BEFORE | GST_SEEK_FLAG_FLUSH,
      start_type, start, stop_type, start + DEFAULT_LOOKAHEAD_DURATION);

  GST_ERROR ("I happened to be seeked, propagating %" GST_PTR_FORMAT, event);
  fc->passthrough = FALSE;

  ret = gst_pad_event_default (pad, GST_OBJECT (fc), event);
  GST_ERROR ("ret is %d", ret);
  return ret;
}

static gboolean
gst_frame_cache_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean res;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      res = _do_seek (GST_FRAME_CACHE (parent), pad, event);
      break;
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }

  return res;
}

static void
gst_frame_cache_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstFrameCache *fc = GST_FRAME_CACHE (object);

  switch (property_id) {
    case PROP_DURATION_SOFT_LIMIT:
      g_value_set_uint64 (value, fc->duration_soft_limit);
      break;
    case PROP_BYTE_SIZE_SOFT_LIMIT:
      g_value_set_uint64 (value, fc->size_soft_limit);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
gst_frame_cache_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFrameCache *fc = GST_FRAME_CACHE (object);

  switch (property_id) {
    case PROP_DURATION_SOFT_LIMIT:
      fc->duration_soft_limit = g_value_get_uint64 (value);
      break;
    case PROP_BYTE_SIZE_SOFT_LIMIT:
      fc->size_soft_limit = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
gst_frame_cache_loop (GstFrameCache *fc)
{
  GstBuffer *buffer;
  GstSegment *actual_segment;
  g_mutex_lock (&fc->buffer_lock);
  g_cond_wait (&fc->buffer_cond, &fc->buffer_lock);

  if (!fc->running) {
    gst_pad_pause_task (fc->srcpad);
    GST_ERROR ("stop that shit now ...");
    return;
  }

  buffer = _get_cached_buffer (fc, fc->requested_segment.start);
  gst_pad_push_event (fc->srcpad, gst_event_new_caps (fc->sinkcaps));
  actual_segment = gst_segment_copy (&fc->current_segment);
  actual_segment->start = fc->requested_segment.start;
  actual_segment->base = actual_segment->start;
  actual_segment->time = actual_segment->start;
  gst_pad_push_event (fc->srcpad, gst_event_new_segment (actual_segment));
  GST_DEBUG_OBJECT (fc, "pushing buffer %" GST_PTR_FORMAT, buffer);
  gst_pad_push (fc->srcpad, gst_buffer_ref(buffer));
  g_mutex_unlock (&fc->buffer_lock);
}

static GstStateChangeReturn
gst_frame_cache_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_frame_cache_class_init (GstFrameCacheClass * klass)
{
  GstElementClass *gstelement_class;
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = gst_frame_cache_get_property;
  object_class->set_property = gst_frame_cache_set_property;

  properties[PROP_DURATION_SOFT_LIMIT] = g_param_spec_uint64 ("duration-soft-limit",
      "Soft limit duration for the cache",
      "The soft maximum limit for the duration of the cache", 0, G_MAXUINT64, DEFAULT_DURATION,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
  g_object_class_install_property (object_class, PROP_DURATION_SOFT_LIMIT,
      properties[PROP_DURATION_SOFT_LIMIT]);

  properties[PROP_BYTE_SIZE_SOFT_LIMIT] = g_param_spec_uint64 ("size-soft-limit",
      "Soft limit size for the cache",
      "The soft maximum limit in bytes for the size of the cache", 0, G_MAXUINT64, DEFAULT_BYTE_SIZE,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
  g_object_class_install_property (object_class, PROP_BYTE_SIZE_SOFT_LIMIT,
      properties[PROP_BYTE_SIZE_SOFT_LIMIT]);

  gstelement_class = GST_ELEMENT_CLASS (klass);
  gst_element_class_set_static_metadata (gstelement_class,
      "FrameCache",
      "Generic",
      "Translates seeks in PAUSED",
      "Mathieu Duponchelle <mathieu.duponchelle@opencreed.com>");

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_frame_cache_change_state);
}

static gint
compare_buffers (gconstpointer new_buf, gconstpointer cmp_buf, gpointer user_data)
{
  GstClockTime new_pts = GST_BUFFER_PTS (GST_BUFFER (new_buf));
  GstClockTime cmp_pts = GST_BUFFER_PTS (GST_BUFFER (cmp_buf));

  if (new_pts < cmp_pts)
    return -1;
  else if (new_pts == cmp_pts)
    return 0;
  return 1;
}

static GstFlowReturn
gst_frame_cache_chain (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  GstFrameCache *fc = GST_FRAME_CACHE (parent);

  if (fc->send_events) {
    fc->send_events = FALSE;
  }
  GST_DEBUG ("chaining %" GST_PTR_FORMAT, buffer);
  if (!fc->passthrough) {
    GstBuffer *cached = _get_cached_buffer (fc, GST_BUFFER_PTS (buffer));
    fc->current_position = GST_BUFFER_PTS (buffer);

    if (cached != NULL) {
      GST_DEBUG ("not caching buffer");
    } else {
      g_sequence_insert_sorted (fc->buffers, buffer, compare_buffers, NULL);
      GST_DEBUG ("cached buffer");
    }
    if (GST_BUFFER_PTS (buffer) <= fc->requested_segment.start &&
        GST_BUFFER_PTS (buffer) + GST_BUFFER_DURATION (buffer) >= fc->requested_segment.start) {
      GST_DEBUG ("signaling that shit");
      _broadcast_src_pad (fc);
    }
  }
  else {
    gst_pad_push (fc->srcpad, buffer);
  }

  return GST_FLOW_OK;
}

static gboolean
gst_frame_cache_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstCaps *caps;
  GstFrameCache *fc = GST_FRAME_CACHE (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
        gst_event_parse_caps (event, &caps);
        GST_DEBUG_OBJECT (pad, "received caps %" GST_PTR_FORMAT, caps);
        GST_FRAME_CACHE (parent)->sinkcaps = gst_caps_copy (caps);
      if (fc->passthrough == FALSE) {
        gst_event_unref (event);
        event = NULL;
      }
      break;
    case GST_EVENT_SEGMENT:
      if (fc->passthrough == FALSE) {
        gst_event_copy_segment (event, &fc->current_segment);      
        GST_ERROR ("got a segment %" GST_PTR_FORMAT, event);
        if (!GST_CLOCK_TIME_IS_VALID (fc->start) || fc->start > fc->current_segment.start)
          fc->start = fc->current_segment.start;
        if (!GST_CLOCK_TIME_IS_VALID (fc->stop) || fc->stop < fc->current_segment.stop)
          fc->stop = fc->current_segment.stop;
        gst_event_unref (event);
        event = NULL;
      }
      break;
    case GST_EVENT_EOS:
      if (fc->passthrough == FALSE) {
        _update_buffers (fc);
        gst_event_unref (event);
        event = NULL;
      }
      break;
    default:
      break;
  }

  if (event)
    return gst_pad_push_event (fc->srcpad, event);
  else
    return TRUE;
}

static gboolean
gst_frame_cache_src_activate_mode (GstPad * pad, GstObject * parent, GstPadMode mode,
    gboolean active)
{
  gboolean result = TRUE;
  GstFrameCache *fc;

  fc = GST_FRAME_CACHE (parent);

  switch (mode) {
    case GST_PAD_MODE_PUSH:
      if (active) {
        fc->running = TRUE; 
        gst_pad_start_task (fc->srcpad, (GstTaskFunction) gst_frame_cache_loop,
            fc, NULL);
      } else {
        fc->running = FALSE;
        _broadcast_src_pad (fc);
        gst_pad_stop_task (fc->srcpad);
      }
      break;
    default:
      result = FALSE;
      break;
  }
  return result;
}

static void
gst_frame_cache_init (GstFrameCache * fc)
{
  fc->sinkpad = gst_pad_new_from_static_template (&sinktemplate, "sink");

  gst_pad_set_chain_function (fc->sinkpad, GST_DEBUG_FUNCPTR (gst_frame_cache_chain));
  gst_pad_set_event_function (fc->sinkpad,
      GST_DEBUG_FUNCPTR (gst_frame_cache_sink_event));
  gst_element_add_pad (GST_ELEMENT (fc), fc->sinkpad);

  fc->srcpad = gst_pad_new_from_static_template (&srctemplate, "src");
  gst_pad_set_event_function (fc->srcpad,
      GST_DEBUG_FUNCPTR (gst_frame_cache_src_event));
  gst_element_add_pad (GST_ELEMENT (fc), fc->srcpad);

  gst_pad_set_activatemode_function (fc->srcpad,
      gst_frame_cache_src_activate_mode);

  fc->passthrough = TRUE;
  fc->send_events = TRUE;
  fc->buffers = g_sequence_new ((GDestroyNotify) gst_buffer_unref);
  fc->current_position = GST_CLOCK_TIME_NONE;
  fc->start = GST_CLOCK_TIME_NONE;
  fc->stop = GST_CLOCK_TIME_NONE;
  gst_segment_init (&fc->requested_segment, GST_FORMAT_TIME);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "framecache", GST_RANK_NONE,
          gst_frame_cache_get_type ()))
    return FALSE;

  return TRUE;
}



GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, framecache,
    "Frame cache", plugin_init, VERSION, "LGPL",
    "Segments seeks in PAUSED state to efficiently cache decoded frames", "www.opencreed.com");
