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
_buffer_covers_position (GstBuffer * buffer, GstClockTime position)
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

static GSequenceIter *
_get_cached_iter (GstFrameCache * fc, GstClockTime position)
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
    iter = NULL;
    goto done;
  }

  iter = g_sequence_iter_prev (iter);
  buffer = g_sequence_get (iter);

  if (_buffer_covers_position (buffer, position))
    goto done;

  iter = NULL;

done:
  return iter;
}

static GstBuffer *
_get_cached_buffer (GstFrameCache * fc, GstClockTime position)
{
  GSequenceIter *iter;

  iter = _get_cached_iter (fc, position);
  if (iter)
    return g_sequence_get (iter);

  return NULL;
}

static gboolean
_buffer_is_in_current_segment (GstFrameCache * fc, GstClockTime position)
{
  return fc->start <= position && fc->stop > position;
}

static void
_clear_buffers (GstFrameCache * fc)
{
  GST_INFO_OBJECT (fc, "clearing buffers, peace");
  g_mutex_lock (&fc->lock);
  fc->wait_flush_start = TRUE;
  g_sequence_free (fc->buffers);
  fc->buffers = g_sequence_new ((GDestroyNotify) gst_buffer_unref);
  g_mutex_unlock (&fc->lock);
  fc->segment_done = TRUE;
}

static GstClockTime
_get_iter_pts (GSequenceIter * iter)
{
  return (GST_BUFFER_PTS (GST_BUFFER (g_sequence_get (iter))));
}

static void
_free_buffers (GstFrameCache *fc, GstClockTime position, gboolean forward) {
  GSequenceIter *iter = _get_cached_iter (fc, position);

  if (forward) {
    g_mutex_lock (&fc->lock);
    g_sequence_remove_range (g_sequence_get_begin_iter(fc->buffers), iter);
    g_mutex_unlock (&fc->lock);
    fc->start = _get_iter_pts (g_sequence_get_begin_iter (fc->buffers));
    GST_INFO_OBJECT (fc, "we now start at %" GST_TIME_FORMAT, GST_TIME_ARGS (fc->start));
  }
}

static gboolean
_make_room (GstFrameCache *fc)
{
  GSequenceIter *first = g_sequence_get_begin_iter (fc->buffers);
  GSequenceIter *last = g_sequence_get_end_iter (fc->buffers);
  GstClockTimeDiff interval;

  GST_DEBUG_OBJECT (fc, "making room");
  last = g_sequence_iter_prev (last);

  if (g_sequence_iter_is_end (first))
    return TRUE;

  GST_DEBUG_OBJECT (fc, "first : %" GST_TIME_FORMAT, GST_TIME_ARGS (_get_iter_pts (first)));
  GST_DEBUG_OBJECT (fc, "last : %" GST_TIME_FORMAT, GST_TIME_ARGS (_get_iter_pts (last)));
  interval = _get_iter_pts (last) - _get_iter_pts (first);

  if (fc->requested_segment.position + DEFAULT_DURATION < _get_iter_pts (last))
    return FALSE;

  if (interval > DEFAULT_DURATION) {
    _free_buffers (fc, _get_iter_pts (last) - DEFAULT_DURATION, TRUE);
  }

  return TRUE;
}

static gboolean
_update_buffers (GstFrameCache * fc)
{
  GstEvent *event;

  if (!_make_room (fc)) {
    return FALSE;
  }

  if (fc->forward_done == FALSE && fc->segment_done == TRUE) {
    GstClockTime stop = fc->stop + DEFAULT_LOOKAHEAD_DURATION;

    if (GST_CLOCK_TIME_IS_VALID (fc->requested_segment.stop))
      stop = MIN (stop, fc->requested_segment.stop);
    fc->segment_done = FALSE;
    event = gst_event_new_seek (1.0, GST_FORMAT_TIME,
        GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_BEFORE | GST_SEEK_FLAG_FLUSH,
        GST_SEEK_TYPE_SET, fc->stop, GST_SEEK_TYPE_SET, stop);
    GST_INFO_OBJECT (fc, "pushing %" GST_PTR_FORMAT, event);
    gst_pad_event_default (fc->srcpad, GST_OBJECT (fc), event);
  } else if (fc->forward_done == TRUE) {
    GST_INFO_OBJECT (fc, "I'm done filling up forward bye");
  }

  return FALSE;
}

static void
_broadcast_src_pad (GstFrameCache * fc, gboolean flush)
{
  if (flush) {
    gst_pad_push_event (fc->srcpad, gst_event_new_flush_start ());
    fc->send_events = TRUE;
    gst_pad_push_event (fc->srcpad, gst_event_new_flush_stop (TRUE));
  }

  g_cond_broadcast (&fc->buffer_cond);
}

static gboolean
_do_seek (GstFrameCache * fc, GstPad * pad, GstEvent * event)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  gboolean update;

  GST_INFO_OBJECT (fc, "doing seek %" GST_PTR_FORMAT, event);
  gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start,
      &stop_type, &stop);
  gst_segment_do_seek (&fc->requested_segment, rate, format, flags, start_type,
      start, stop_type, stop, &update);
  gst_event_unref (event);

  if (fc->passthrough == TRUE) {
    gst_pad_push_event (pad, gst_event_new_flush_start ());
    gst_pad_push_event (pad, gst_event_new_flush_stop (TRUE));
  }

  fc->passthrough = FALSE;

  if (!_buffer_is_in_current_segment (fc, start)) {
    _clear_buffers (fc);
    fc->start = start;
    fc->stop = start;
    fc->requested_segment.position = start;
  }

  _broadcast_src_pad (fc, TRUE);

  return TRUE;
}

static gboolean
gst_frame_cache_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean res;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      res = _do_seek (GST_FRAME_CACHE (parent), pad, event);
      break;
    case GST_EVENT_QOS:
      /* FIXME : we might want to respect QOS somehow */
      res = TRUE;
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
gst_frame_cache_loop (GstFrameCache * fc)
{
  GstBuffer *buffer;
  GstSegment *actual_segment;
  GstFlowReturn ret;
  g_mutex_lock (&fc->buffer_lock);
  g_cond_wait (&fc->buffer_cond, &fc->buffer_lock);
  g_mutex_unlock (&fc->buffer_lock);

  if (!fc->running) {
    gst_pad_pause_task (fc->srcpad);
    GST_INFO_OBJECT (fc, "paused task now ...");
    return;
  }

  if (fc->send_events == TRUE)
  {
    gst_pad_push_event (fc->srcpad, gst_event_new_caps (fc->sinkcaps));
    actual_segment = gst_segment_copy (&fc->requested_segment);
    actual_segment->start = fc->requested_segment.start;
    actual_segment->time = actual_segment->start;
    GST_INFO ("pushing segment %" GST_SEGMENT_FORMAT, actual_segment);
    gst_pad_push_event (fc->srcpad, gst_event_new_segment (actual_segment));
    fc->send_events = FALSE;
  }

  g_mutex_lock (&fc->lock);
  buffer = _get_cached_buffer (fc, fc->requested_segment.position);
  if (buffer)
    buffer = gst_buffer_ref (buffer);
  g_mutex_unlock (&fc->lock);

  while (buffer) {
    GstClockTime new_pos = GST_BUFFER_PTS (buffer) + GST_BUFFER_DURATION (buffer);
    GST_DEBUG_OBJECT (fc, "pushing buffer %" GST_PTR_FORMAT, buffer);
    ret = gst_pad_push (fc->srcpad, buffer);
    GST_DEBUG_OBJECT (fc, "pushed buffer, ret is %s", gst_flow_get_name (ret));

    if (ret != GST_FLOW_OK)
      break;

    fc->requested_segment.position = new_pos;
    g_mutex_lock (&fc->lock);
    buffer = _get_cached_buffer (fc, fc->requested_segment.position);
    if (buffer)
      buffer = gst_buffer_ref (buffer);
    g_mutex_unlock (&fc->lock);
  }

  g_idle_add((GSourceFunc) _update_buffers, fc);
}

static GstStateChangeReturn
gst_frame_cache_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstFrameCache *fc = GST_FRAME_CACHE (element);

  (void) fc;

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

  properties[PROP_DURATION_SOFT_LIMIT] =
      g_param_spec_uint64 ("duration-soft-limit",
      "Soft limit duration for the cache",
      "The soft maximum limit for the duration of the cache", 0, G_MAXUINT64,
      DEFAULT_DURATION, G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
  g_object_class_install_property (object_class, PROP_DURATION_SOFT_LIMIT,
      properties[PROP_DURATION_SOFT_LIMIT]);

  properties[PROP_BYTE_SIZE_SOFT_LIMIT] =
      g_param_spec_uint64 ("size-soft-limit", "Soft limit size for the cache",
      "The soft maximum limit in bytes for the size of the cache", 0,
      G_MAXUINT64, DEFAULT_BYTE_SIZE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT);
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
compare_buffers (gconstpointer new_buf, gconstpointer cmp_buf,
    gpointer user_data)
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
gst_frame_cache_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstFrameCache *fc = GST_FRAME_CACHE (parent);
  GSequenceIter *cached;

  GST_DEBUG ("chaining %" GST_PTR_FORMAT, buffer);

  if (fc->passthrough == TRUE)
    return gst_pad_push (fc->srcpad, buffer);

  g_mutex_lock (&fc->lock);
  if (fc->wait_flush_start) {
    GST_INFO ("returning flushing bye");
    g_mutex_unlock (&fc->lock);
    return GST_FLOW_FLUSHING;
  }

  cached = _get_cached_iter (fc, GST_BUFFER_PTS (buffer));
  fc->current_position = GST_BUFFER_PTS (buffer);

  if (cached != NULL) {
    g_sequence_remove (cached);
  }

  g_sequence_insert_sorted (fc->buffers, buffer, compare_buffers, NULL);
  g_mutex_unlock (&fc->lock);
  GST_DEBUG ("cached buffer");

  _broadcast_src_pad (fc, FALSE);

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
        GST_INFO_OBJECT (fc, "got a segment %" GST_PTR_FORMAT, event);
        if (!GST_CLOCK_TIME_IS_VALID (fc->start)
            || fc->start > fc->current_segment.start)
          fc->start = fc->current_segment.start;
        if (!GST_CLOCK_TIME_IS_VALID (fc->stop)
            || fc->stop < fc->current_segment.stop)
          fc->stop = fc->current_segment.stop;
        if (fc->current_segment.start + DEFAULT_LOOKAHEAD_DURATION > fc->stop)
          fc->forward_done = TRUE;
        else
          fc->forward_done = FALSE;
        gst_event_unref (event);
        event = NULL;
      }
      break;
    case GST_EVENT_EOS:
      if (fc->passthrough == FALSE) {
        fc->segment_done = TRUE;
        g_idle_add((GSourceFunc) _update_buffers, fc);
        gst_event_unref (event);
        event = NULL;
      }
      break;
    case GST_EVENT_FLUSH_START:
      fc->wait_flush_start = FALSE;
    case GST_EVENT_FLUSH_STOP:
      gst_event_unref (event);
      event = NULL;
      break;
    default:
      break;
  }

  if (event) {
    GST_INFO ("pushing %" GST_PTR_FORMAT, event);
    return gst_pad_push_event (fc->srcpad, event);
  }
  else
    return TRUE;
}

static gboolean
gst_frame_cache_src_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
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
        _broadcast_src_pad (fc, TRUE);
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

  gst_pad_set_chain_function (fc->sinkpad,
      GST_DEBUG_FUNCPTR (gst_frame_cache_chain));
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
    "Segments seeks in PAUSED state to efficiently cache decoded frames",
    "www.opencreed.com");
