h_sources = $(srcdir)/inc/frame_capture_intf.h

framecapture_includedir = $(includedir)
framecapture_include_HEADERS = $(h_sources)

AM_CFLAGS    = -Wno-missing-field-initializers -Wconversion \
               -std=c++11

cpp_sources  = src/frame_capture_intf.cpp \
               src/frame_capture_impl.cpp \
               src/Compressor.cpp

lib_LTLIBRARIES = libframecapture.la
libframecapture_la_CC = @CC@
libframecapture_la_SOURCES = $(cpp_sources)
libframecapture_la_CFLAGS = $(AM_CFLAGS)
libframecapture_la_CPPFLAGS = $(AM_CPPFLAGS)
libframecapture_la_LIBADD = ../libdebug/libdisplaydebug.la
libframecapture_la_LIBADD += ../sdm/libs/core/libsdmcore.la
libframecapture_la_LIBADD += ../sdm/libs/utils/libsdmutils.la
libframecapture_la_LDFLAGS = -shared -avoid-version -llz4
libframecapture_la_CPPFLAGS += -I$(srcdir)/../libdebug
libframecapture_la_CPPFLAGS += -I$(srcdir)/../include
libframecapture_la_CPPFLAGS += -I$(srcdir)/../sdm/include/
libframecapture_la_CPPFLAGS += -I$(srcdir)/inc
libframecapture_la_CPPFLAGS += -I$(srcdir)/src
