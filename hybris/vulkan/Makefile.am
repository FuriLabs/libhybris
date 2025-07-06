if HAS_VULKAN_HEADERS

SUBDIRS = platforms

lib_LTLIBRARIES = \
	libvulkan_hybris.la

libvulkan_hybris_la_SOURCES = \
	vulkan.c \
	ws.c

if WANT_VKICD
vk_icddir = $(datadir)/vulkan/icd.d
vk_icd_DATA = libhybris_icd.json

libhybris_icd.json: libhybris_icd.json.in Makefile
	$(AM_V_GEN)cp $< $@

CLEANFILES = libhybris_icd.json
endif
libvulkan_hybris_la_CFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/common $(ANDROID_HEADERS_CFLAGS) -DPKGLIBDIR="\"$(pkglibdir)/\""
if WANT_TRACE
libvulkan_hybris_la_CFLAGS += -DDEBUG
endif
if WANT_DEBUG
libvulkan_hybris_la_CFLAGS += -ggdb -O0
endif
if WANT_VKICD
libvulkan_hybris_la_CFLAGS += -DLIBHYBRIS_WANTS_VKICD
endif
libvulkan_hybris_la_CXXFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/common $(ANDROID_HEADERS_CFLAGS) -DPKGLIBDIR="\"$(pkglibdir)/\""
if WANT_TRACE
libvulkan_hybris_la_CXXFLAGS += -DDEBUG
endif
if WANT_DEBUG
libvulkan_hybris_la_CXXFLAGS += -ggdb -O0
endif
if WANT_VKICD
libvulkan_hybris_la_CXXFLAGS += -DLIBHYBRIS_WANTS_VKICD
endif
libvulkan_hybris_la_LDFLAGS = \
	-ldl \
	$(top_builddir)/common/libhybris-common.la \
	$(top_builddir)/hardware/libhardware.la \
	-version-info 1:0:0

EXTRA_DIST = libhybris_icd.json.in
endif
