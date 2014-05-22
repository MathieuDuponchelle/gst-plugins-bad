plugin_LTLIBRARIES = libgstvideomixer2.la

ORC_SOURCE=videomixerorc

include $(top_srcdir)/common/orc.mak

libgstvideomixer2_la_SOURCES = \
  basemixer/gstbasemixer.c \
  basemixer/videoconvert.c \
	basemixer/gstcms.c \
	blend.c \
	videomixer2.c


nodist_libgstvideomixer2_la_SOURCES = $(ORC_NODIST_SOURCES)
libgstvideomixer2_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) \
	-I$(top_srcdir)/gst-libs \
	-I$(top_builddir)/gst-libs \
	$(GST_BASE_CFLAGS) $(GST_CFLAGS) $(ORC_CFLAGS)
libgstvideomixer2_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) \
	-lgstvideo-@GST_API_VERSION@ \
	$(top_builddir)/gst-libs/gst/base/libgstbadbase-$(GST_API_VERSION).la \
	$(GST_BASE_LIBS) $(GST_LIBS) $(ORC_LIBS) $(LIBM)
libgstvideomixer2_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstvideomixer2_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

# headers we need but don't want installed
noinst_HEADERS = \
	blend.h \
	videomixer2.h \
	videomixer2pad.h \
	basemixer/gstbasemixer.h \
	basemixer/gstvideoconvertorc.h \
	basemixer/gstbasemixerpad.h \
	basemixer/gstcms.h \
	basemixer/videoconvert.h

Android.mk: Makefile.am $(BUILT_SOURCES)
	androgenizer \
	-:PROJECT libgstvideomixer2 -:SHARED libgstvideomixer2 \
	 -:TAGS eng debug \
         -:REL_TOP $(top_srcdir) -:ABS_TOP $(abs_top_srcdir) \
	 -:SOURCES $(libgstvideomixer2_la_SOURCES) \
	 	   $(nodist_libgstvideomixer2_la_SOURCES) \
	 -:CFLAGS $(DEFS) $(DEFAULT_INCLUDES) $(libgstvideomixer2_la_CFLAGS) \
	 -:LDFLAGS $(libgstvideomixer2_la_LDFLAGS) \
	           $(libgstvideomixer2_la_LIBADD) \
	           -ldl \
	 -:PASSTHROUGH LOCAL_ARM_MODE:=arm \
		       LOCAL_MODULE_PATH:='$$(TARGET_OUT)/lib/gstreamer-0.10' \
	> $@
