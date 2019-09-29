# Makefile for node_features_nvram_generic plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = node_features_nvram_generic.la
node_features_nvram_generic_la_SOURCES = node_features_nvram_generic.c
node_features_nvram_generic_la_LDFLAGS = $(PLUGIN_FLAGS)

force:
$(node_features_nvram_generic_la_LIBADD) : force
	@cd `dirname $@` && $(MAKE) `basename $@`
