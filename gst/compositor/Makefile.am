plugin_LTLIBRARIES = libgstcompositor.la

ORC_SOURCE=compositororc

include $(top_srcdir)/common/orc.mak

libgstcompositor_la_SOURCES = \
	blend.c \
	compositor.c


nodist_libgstcompositor_la_SOURCES = $(ORC_NODIST_SOURCES)
libgstcompositor_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) \
	-I$(top_srcdir)/gst-libs \
	-I$(top_builddir)/gst-libs \
	$(GST_BASE_CFLAGS) $(GST_CFLAGS) $(ORC_CFLAGS)
libgstcompositor_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) \
	-lgstvideo-@GST_API_VERSION@ \
	$(top_builddir)/gst-libs/gst/base/libgstbadbase-$(GST_API_VERSION).la \
	$(top_builddir)/gst-libs/gst/video/libgstbadvideo-$(GST_API_VERSION).la \
	$(GST_BASE_LIBS) $(GST_LIBS) $(ORC_LIBS) $(LIBM)
libgstcompositor_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstcompositor_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

# headers we need but don't want installed
noinst_HEADERS = \
	blend.h \
	compositor.h \
	compositorpad.h

Android.mk: Makefile.am $(BUILT_SOURCES)
	androgenizer \
	-:PROJECT libgstcompositor -:SHARED libgstcompositor \
	 -:TAGS eng debug \
         -:REL_TOP $(top_srcdir) -:ABS_TOP $(abs_top_srcdir) \
	 -:SOURCES $(libgstcompositor_la_SOURCES) \
	 	   $(nodist_libgstcompositor_la_SOURCES) \
	 -:CFLAGS $(DEFS) $(DEFAULT_INCLUDES) $(libgstcompositor_la_CFLAGS) \
	 -:LDFLAGS $(libgstcompositor_la_LDFLAGS) \
	           $(libgstcompositor_la_LIBADD) \
	           -ldl \
	 -:PASSTHROUGH LOCAL_ARM_MODE:=arm \
		       LOCAL_MODULE_PATH:='$$(TARGET_OUT)/lib/gstreamer-0.10' \
	> $@
