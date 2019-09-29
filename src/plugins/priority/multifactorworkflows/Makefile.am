# Makefile for priority/multifactorworkflows plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = priority_multifactorworkflows.la

# Null priority logging plugin.
priority_multifactorworkflows_la_SOURCES = priority_multifactorworkflows.c fair_treeworkflows.c fair_treeworkflows.h priority_multifactorworkflows.h


priority_multifactorworkflows_la_LDFLAGS = $(PLUGIN_FLAGS)
priority_multifactorworkflows_la_LIBADD  = -lm
