/*
 * GStreamer
 * Copyright (C) 2008 Julien Isorce <julien.isorce@gmail.com>
 * Copyright (C) 2012 Matthew Waters <ystreet00@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#define GLIB_DISABLE_DEPRECATION_WARNINGS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include <gstglfeature.h>
#include "gstglwindow_x11_glx.h"

#define GST_CAT_DEFAULT gst_gl_window_debug

#define gst_gl_window_x11_glx_parent_class parent_class
G_DEFINE_TYPE (GstGLWindowX11GLX, gst_gl_window_x11_glx,
    GST_GL_TYPE_WINDOW_X11);

#define GST_GL_WINDOW_X11_GLX_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE((o), GST_GL_TYPE_WINDOW_X11_GLX, GstGLWindowX11GLXPrivate))

static guintptr gst_gl_window_x11_glx_get_gl_context (GstGLWindowX11 *
    window_x11);
static void gst_gl_window_x11_glx_swap_buffers (GstGLWindowX11 * window_x11);
static gboolean gst_gl_window_x11_glx_activate (GstGLWindowX11 * window_x11,
    gboolean activate);
static gboolean gst_gl_window_x11_glx_create_context (GstGLWindowX11 *
    window_x11, GstGLAPI gl_api, guintptr external_gl_context, GError ** error);
static void gst_gl_window_x11_glx_destroy_context (GstGLWindowX11 * window_x11);
static gboolean gst_gl_window_x11_glx_choose_format (GstGLWindowX11 *
    window_x11, GError ** error);
GstGLAPI gst_gl_window_x11_glx_get_gl_api (GstGLWindow * window);
static gpointer gst_gl_window_x11_glx_get_proc_address (GstGLWindow * window,
    const gchar * name);

struct _GstGLWindowX11GLXPrivate
{
  int glx_major;
  int glx_minor;

  GstGLAPI context_api;

  GLXFBConfig *fbconfigs;
    GLXContext (*glXCreateContextAttribsARB) (Display *, GLXFBConfig,
      GLXContext, Bool, const int *);
};

static void
gst_gl_window_x11_glx_class_init (GstGLWindowX11GLXClass * klass)
{
  GstGLWindowClass *window_class = (GstGLWindowClass *) klass;
  GstGLWindowX11Class *window_x11_class = (GstGLWindowX11Class *) klass;

  g_type_class_add_private (klass, sizeof (GstGLWindowX11GLXPrivate));

  window_x11_class->get_gl_context =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_get_gl_context);
  window_x11_class->activate =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_activate);
  window_x11_class->create_context =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_create_context);
  window_x11_class->destroy_context =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_destroy_context);
  window_x11_class->choose_format =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_choose_format);
  window_x11_class->swap_buffers =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_swap_buffers);

  window_class->get_gl_api =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_get_gl_api);
  window_class->get_proc_address =
      GST_DEBUG_FUNCPTR (gst_gl_window_x11_glx_get_proc_address);
}

static void
gst_gl_window_x11_glx_init (GstGLWindowX11GLX * window)
{
  window->priv = GST_GL_WINDOW_X11_GLX_GET_PRIVATE (window);
}

/* Must be called in the gl thread */
GstGLWindowX11GLX *
gst_gl_window_x11_glx_new (void)
{
  GstGLWindowX11GLX *window = g_object_new (GST_GL_TYPE_WINDOW_X11_GLX, NULL);

  return window;
}

static inline void
_describe_fbconfig (Display * display, GLXFBConfig config)
{
  int val;

  glXGetFBConfigAttrib (display, config, GLX_FBCONFIG_ID, &val);
  GST_DEBUG ("ID: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_DOUBLEBUFFER, &val);
  GST_DEBUG ("double buffering: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_RED_SIZE, &val);
  GST_DEBUG ("red: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_GREEN_SIZE, &val);
  GST_DEBUG ("green: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_BLUE_SIZE, &val);
  GST_DEBUG ("blue: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_ALPHA_SIZE, &val);
  GST_DEBUG ("alpha: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_DEPTH_SIZE, &val);
  GST_DEBUG ("depth: %d", val);
  glXGetFBConfigAttrib (display, config, GLX_STENCIL_SIZE, &val);
  GST_DEBUG ("stencil: %d", val);
}

static gboolean
gst_gl_window_x11_glx_create_context (GstGLWindowX11 * window_x11,
    GstGLAPI gl_api, guintptr external_gl_context, GError ** error)
{
  GstGLWindowX11GLX *window_glx;
  gboolean create_context;
  const char *glx_exts;
  int x_error;

  window_glx = GST_GL_WINDOW_X11_GLX (window_x11);

  glx_exts =
      glXQueryExtensionsString (window_x11->device,
      DefaultScreen (window_x11->device));

  create_context = gst_gl_check_extension ("GLX_ARB_create_context", glx_exts);
  window_glx->priv->glXCreateContextAttribsARB =
      (gpointer) glXGetProcAddressARB ((const GLubyte *)
      "glXCreateContextAttribsARB");

  if (create_context && window_glx->priv->glXCreateContextAttribsARB) {
    int context_attribs_3[] = {
      GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
      GLX_CONTEXT_MINOR_VERSION_ARB, 0,
      //GLX_CONTEXT_FLAGS_ARB        , GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
      None
    };

    int context_attribs_pre_3[] = {
      GLX_CONTEXT_MAJOR_VERSION_ARB, 1,
      GLX_CONTEXT_MINOR_VERSION_ARB, 4,
      None
    };

    gst_gl_window_x11_trap_x_errors ();
    window_glx->glx_context =
        window_glx->priv->glXCreateContextAttribsARB (window_x11->device,
        window_glx->priv->fbconfigs[0], (GLXContext) external_gl_context, True,
        context_attribs_3);

    x_error = gst_gl_window_x11_untrap_x_errors ();
    window_glx->priv->context_api = GST_GL_API_OPENGL3 | GST_GL_API_OPENGL;

    if (!window_glx->glx_context || x_error != 0) {
      GST_DEBUG ("Failed to create an Opengl 3 context. trying a legacy one");

      gst_gl_window_x11_trap_x_errors ();
      window_glx->glx_context =
          window_glx->priv->glXCreateContextAttribsARB (window_x11->device,
          window_glx->priv->fbconfigs[0], (GLXContext) external_gl_context,
          True, context_attribs_pre_3);

      x_error = gst_gl_window_x11_untrap_x_errors ();

      if (x_error != 0)
        window_glx->glx_context = NULL;
      window_glx->priv->context_api = GST_GL_API_OPENGL;
    }

  } else {
    window_glx->glx_context =
        glXCreateContext (window_x11->device, window_x11->visual_info,
        (GLXContext) external_gl_context, TRUE);
    window_glx->priv->context_api = GST_GL_API_OPENGL;
  }

  if (window_glx->priv->fbconfigs)
    XFree (window_glx->priv->fbconfigs);

  if (!window_glx->glx_context) {
    g_set_error (error, GST_GL_WINDOW_ERROR, GST_GL_WINDOW_ERROR_CREATE_CONTEXT,
        "Failed to create opengl context");
    goto failure;
  }

  GST_LOG ("gl context id: %ld", (gulong) window_glx->glx_context);

  return TRUE;

failure:
  return FALSE;
}

static void
gst_gl_window_x11_glx_destroy_context (GstGLWindowX11 * window_x11)
{
  GstGLWindowX11GLX *window_glx;

  window_glx = GST_GL_WINDOW_X11_GLX (window_x11);

  glXDestroyContext (window_x11->device, window_glx->glx_context);

  window_glx->glx_context = 0;
}

static gboolean
gst_gl_window_x11_glx_choose_format (GstGLWindowX11 * window_x11,
    GError ** error)
{
  GstGLWindowX11GLX *window_glx;
  gint error_base;
  gint event_base;

  window_glx = GST_GL_WINDOW_X11_GLX (window_x11);

  if (!glXQueryExtension (window_x11->device, &error_base, &event_base)) {
    g_set_error (error, GST_GL_WINDOW_ERROR,
        GST_GL_WINDOW_ERROR_RESOURCE_UNAVAILABLE, "No GLX extension");
    goto failure;
  }

  if (!glXQueryVersion (window_x11->device, &window_glx->priv->glx_major,
          &window_glx->priv->glx_minor)) {
    g_set_error (error, GST_GL_WINDOW_ERROR, GST_GL_WINDOW_ERROR_CREATE_CONTEXT,
        "Failed to query GLX version (glXQueryVersion failed)");
    goto failure;
  }

  GST_INFO ("GLX Version: %d.%d", window_glx->priv->glx_major,
      window_glx->priv->glx_minor);

  /* legacy case */
  if (window_glx->priv->glx_major < 1 || (window_glx->priv->glx_major == 1
          && window_glx->priv->glx_minor < 3)) {
    gint attribs[] = {
      GLX_RGBA,
      GLX_RED_SIZE, 1,
      GLX_GREEN_SIZE, 1,
      GLX_BLUE_SIZE, 1,
      GLX_DEPTH_SIZE, 16,
      GLX_DOUBLEBUFFER,
      None
    };

    window_x11->visual_info = glXChooseVisual (window_x11->device,
        window_x11->screen_num, attribs);

    if (!window_x11->visual_info) {
      g_set_error (error, GST_GL_WINDOW_ERROR, GST_GL_WINDOW_ERROR_WRONG_CONFIG,
          "Bad attributes in glXChooseVisual");
      goto failure;
    }
  } else {
    gint attribs[] = {
      GLX_RENDER_TYPE, GLX_RGBA_BIT,
      GLX_RED_SIZE, 1,
      GLX_GREEN_SIZE, 1,
      GLX_BLUE_SIZE, 1,
      GLX_DEPTH_SIZE, 16,
      GLX_DOUBLEBUFFER, True,
      None
    };
    int fbcount;

    window_glx->priv->fbconfigs = glXChooseFBConfig (window_x11->device,
        DefaultScreen (window_x11->device), attribs, &fbcount);

    if (!window_glx->priv->fbconfigs) {
      g_set_error (error, GST_GL_WINDOW_ERROR, GST_GL_WINDOW_ERROR_WRONG_CONFIG,
          "Could not find any FBConfig's to use (check attributes?)");
      goto failure;
    }

    _describe_fbconfig (window_x11->device, window_glx->priv->fbconfigs[0]);

    window_x11->visual_info = glXGetVisualFromFBConfig (window_x11->device,
        window_glx->priv->fbconfigs[0]);

    if (!window_x11->visual_info) {
      g_set_error (error, GST_GL_WINDOW_ERROR, GST_GL_WINDOW_ERROR_WRONG_CONFIG,
          "Bad attributes in FBConfig");
      goto failure;
    }
  }

  return TRUE;

failure:
  return FALSE;
}

static void
gst_gl_window_x11_glx_swap_buffers (GstGLWindowX11 * window_x11)
{
  glXSwapBuffers (window_x11->device, window_x11->internal_win_id);
}

static guintptr
gst_gl_window_x11_glx_get_gl_context (GstGLWindowX11 * window_x11)
{
  return (guintptr) GST_GL_WINDOW_X11_GLX (window_x11)->glx_context;
}

static gboolean
gst_gl_window_x11_glx_activate (GstGLWindowX11 * window_x11, gboolean activate)
{
  gboolean result;

  if (activate) {
    result = glXMakeCurrent (window_x11->device, window_x11->internal_win_id,
        GST_GL_WINDOW_X11_GLX (window_x11)->glx_context);
  } else {
    result = glXMakeCurrent (window_x11->device, None, NULL);
  }

  return result;
}

GstGLAPI
gst_gl_window_x11_glx_get_gl_api (GstGLWindow * window)
{
  GstGLWindowX11GLX *window_glx;

  window_glx = GST_GL_WINDOW_X11_GLX (window);

  return window_glx->priv->context_api;
}

static gpointer
gst_gl_window_x11_glx_get_proc_address (GstGLWindow * window,
    const gchar * name)
{
  gpointer result;

  if (!(result = glXGetProcAddressARB ((const GLubyte *) name))) {
    result = gst_gl_window_default_get_proc_address (window, name);
  }

  return result;
}