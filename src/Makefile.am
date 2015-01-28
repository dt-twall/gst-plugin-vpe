plugin_LTLIBRARIES = libgstvpe.la

# headers we need but don't want installed
noinst_HEADERS = \
	gstvpe.h

#	gstvpebins.h 
# sources used to compile this plug-in
libgstvpe_la_SOURCES = \
	gstvpe.c \
	gstvpebuffer.c \
	gstvpebufferpool.c \
	$(noinst_HEADERS)

#	gstvpebins.c 
# compiler and linker flags used to compile this plugin, set in configure.ac
libgstvpe_la_CFLAGS = \
	$(GST_CFLAGS) \
	$(LIBDCE_CFLAGS) \
        -Wno-unused-variable \
	-DGST_USE_UNSTABLE_API

libgstvpe_la_LIBADD = \
	$(GST_LIBS) \
	$(LIBDCE_LIBS) \
	-lgstdmabuf-1.0 \
	-lgstdrm-1.0 \
	-lgstvideo-1.0

libgstvpe_la_LDFLAGS = \
	$(GST_PLUGIN_LDFLAGS) \
	$(GST_ALL_LDFLAGS) \
	--no-undefined

libgstvpe_la_LIBTOOLFLAGS = --tag=disable-static

