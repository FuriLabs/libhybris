if HAS_VULKAN_HEADERS

SUBDIRS = platforms

lib_LTLIBRARIES = \
	libvulkan_libhybris.la

libvulkan_libhybris_la_SOURCES = \
	vulkan.c \
	ws.c

if WANT_VKICD
vk_icddir = $(datadir)/vulkan/icd.d
vk_icd_DATA = libhybris_icd.json

libhybris_icd.json: libhybris_icd.json.in Makefile
	$(AM_V_GEN)cp $< $@

CLEANFILES = libhybris_icd.json
endif
libvulkan_libhybris_la_CFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/common $(ANDROID_HEADERS_CFLAGS) -DPKGLIBDIR="\"$(pkglibdir)/\""
if WANT_TRACE
libvulkan_libhybris_la_CFLAGS += -DDEBUG
endif
if WANT_DEBUG
libvulkan_libhybris_la_CFLAGS += -ggdb -O0
endif
if WANT_VKICD
libvulkan_libhybris_la_CFLAGS += -DLIBHYBRIS_WANTS_VKICD
endif
libvulkan_libhybris_la_CXXFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/common $(ANDROID_HEADERS_CFLAGS) -DPKGLIBDIR="\"$(pkglibdir)/\""
if WANT_TRACE
libvulkan_libhybris_la_CXXFLAGS += -DDEBUG
endif
if WANT_DEBUG
libvulkan_libhybris_la_CXXFLAGS += -ggdb -O0
endif
if WANT_VKICD
libvulkan_libhybris_la_CXXFLAGS += -DLIBHYBRIS_WANTS_VKICD
endif
libvulkan_libhybris_la_LDFLAGS = \
	-ldl \
	$(top_builddir)/common/libhybris-common.la \
	$(top_builddir)/hardware/libhardware.la \
	-version-info 1:0:0

if WANT_VULKAN_X11_STUBS
libvulkan_libhybris_la_CFLAGS += -DWANT_VULKAN_X11_STUBS
endif

EXTRA_DIST = libhybris_icd.json.in
endif
