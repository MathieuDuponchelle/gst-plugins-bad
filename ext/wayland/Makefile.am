plugin_LTLIBRARIES = libgstwaylandsink.la

libgstwaylandsink_la_SOURCES =  gstwaylandsink.c
libgstwaylandsink_la_CFLAGS = $(GST_CFLAGS) $(GST_PLUGINS_BASE_CFLAGS) \
                               $(WAYLAND_CFLAGS)
libgstwaylandsink_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) \
                               -lgstvideo-$(GST_API_VERSION) \
                               $(WAYLAND_LIBS) $(LIBOIL_LIBS)
libgstwaylandsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstwaylandsink_la_LIBTOOLFLAGS = --tag=disable-static

noinst_HEADERS = gstwaylandsink.h
