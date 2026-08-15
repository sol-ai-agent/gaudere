ACLOCAL_AMFLAGS = -I m4
SUBDIRS = include src tests

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = gaudere.pc gaudere-persistence-sqlite.pc

CLEANFILES = $(pkgconfig_DATA)
EXTRA_DIST = \
	LICENSE \
	gaudere.pc.in \
	gaudere-persistence-sqlite.pc.in
