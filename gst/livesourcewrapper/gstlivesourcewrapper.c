#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <string.h>

#include "gstlivesourcewrapper.h"

GST_DEBUG_CATEGORY_STATIC (gst_live_source_wrapper_debug);
#define GST_CAT_DEFAULT gst_live_source_wrapper_debug

enum
{
  PROP_0,
};

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

#define gst_live_source_wrapper_parent_class parent_class
G_DEFINE_TYPE (GstLiveSourceWrapper, gst_live_source_wrapper, GST_TYPE_BIN);

static gboolean
_add_element (GstBin * bin, GstElement * element)
{
  GstLiveSourceWrapper *self = GST_LIVE_SOURCE_WRAPPER (bin);

  if (self->wrapped_source != NULL)
    return FALSE;

  self->wrapped_source = element;
  return GST_BIN_CLASS (parent_class)->add_element (bin, element);
}

static GstFlowReturn
_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  if (GST_BUFFER_TIMESTAMP (buffer) > 2 * GST_SECOND) {
    GST_ERROR ("returning EOS");
    return GST_FLOW_EOS;
  }
  return gst_proxy_pad_chain_default (pad, parent, buffer);
}

static gboolean
_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstPad *peer = gst_pad_get_peer (pad);
  GstObject *element = gst_object_get_parent (GST_OBJECT (peer));
  GstLiveSourceWrapper *wrapper = GST_LIVE_SOURCE_WRAPPER (element);

  gst_object_unref (peer);
  gst_object_unref (element);
  GST_ERROR ("seen event : %" GST_PTR_FORMAT, event);
  switch GST_EVENT_TYPE
    (event) {
    case GST_EVENT_EOS:
      gst_event_set_seqnum (event, gst_event_get_seqnum (wrapper->seek_event));
      GST_ERROR ("that's an EOS mister");
      break;
    default:
      break;
    }

  return gst_pad_event_default (pad, parent, event);
}

static GstStateChangeReturn
_change_state (GstElement * element, GstStateChange transition)
{
  GstLiveSourceWrapper *wrapper = (GstLiveSourceWrapper *) element;
  GstPad *peer;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_ghost_pad_set_target (GST_GHOST_PAD (wrapper->srcpad),
          gst_element_get_static_pad (wrapper->wrapped_source, "src"));
      peer = gst_pad_get_peer (wrapper->srcpad);
      gst_pad_set_chain_function (peer, _chain);
      gst_pad_set_event_function (peer, _sink_event);
      gst_object_unref (peer);
      gst_pad_set_active (wrapper->srcpad, TRUE);
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_live_source_wrapper_class_init (GstLiveSourceWrapperClass * klass)
{
  GstBinClass *bin_class = GST_BIN_CLASS (klass);
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gstelement_class->change_state = _change_state;

  bin_class->add_element = _add_element;
}

static gboolean
_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstLiveSourceWrapper *self = GST_LIVE_SOURCE_WRAPPER (parent);

  switch GST_EVENT_TYPE
    (event) {
    case GST_EVENT_SEEK:
      self->seek_event = gst_event_ref (event);
      return TRUE;
    default:
      break;
    }

  return gst_pad_event_default (pad, parent, event);
}

static void
gst_live_source_wrapper_init (GstLiveSourceWrapper * wrapper)
{
  wrapper->srcpad = gst_ghost_pad_new_no_target ("src", GST_PAD_SRC);
  gst_pad_set_event_function (wrapper->srcpad, _event);

  gst_element_add_pad (GST_ELEMENT (wrapper), GST_PAD (wrapper->srcpad));
  gst_pad_set_active (wrapper->srcpad, TRUE);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_live_source_wrapper_debug, "livesourcewrapper",
      0, "livesourcewrapper");

  return gst_element_register (plugin, "livesourcewrapper",
      GST_RANK_PRIMARY + 1, GST_TYPE_LIVE_SOURCE_WRAPPER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    live_source_wrapper,
    "Live source wrapper", plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
