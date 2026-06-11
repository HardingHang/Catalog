# Makefile for openGauss extension

MODULE_big = gv_catalog
OBJS = src/gv_catalog.o

EXTENSION = gv_catalog
DATA = gv_catalog--1.0.0.sql
PG_CPPFLAGS += -I$(srcdir)/src/include
CXXFLAGS += -Wall -Wextra -Werror

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
