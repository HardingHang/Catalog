# Makefile for openGauss extension

MODULE_big = iceberg_catalog
OBJS = src/iceberg_catalog.o src/table.o src/namespace.o

EXTENSION = iceberg_catalog
DATA = iceberg_catalog--1.0.0.sql
PG_CPPFLAGS += -I$(srcdir)/src/include -I$(GAUSS_SRC)/src/include
CXXFLAGS += -Wall -Wextra -Werror

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
