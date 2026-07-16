# Build shared library
lib_LTLIBRARIES = libsdmclient.la
libsdmclient_la_CC = @CC@
libsdmclient_la_SOURCES = $(wildcard *.cpp)
libsdmclient_la_CFLAGS = $(COMMON_CFLAGS) -DLOG_TAG=\"SDM\" -DLSR_API
libsdmclient_la_CPPFLAGS = $(AM_CPPFLAGS) -DPP_DRM_ENABLE
libsdmclient_la_LIBADD = ../sdm/libs/utils/libsdmutils.la
libsdmclient_la_LIBADD += ../libdebug/libdisplaydebug.la
libsdmclient_la_LIBADD += ../libframecapture/libframecapture.la
libsdmclient_la_LIBADD += ../sdm/libs/core/libsdmcore.la
libsdmclient_la_LIBADD += -ltinyxml2
libsdmclient_la_LDFLAGS = -shared -avoid-version
