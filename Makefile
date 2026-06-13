# Makefile for openGauss extension

MODULE_big = iceberg_catalog
OBJS = src/iceberg_catalog.o src/table.o src/metadata.o src/errors.o

EXTENSION = iceberg_catalog
DATA = iceberg_catalog--1.0.0.sql
PG_CPPFLAGS += -I$(srcdir)/src/include
ifdef GAUSS_SRC
PG_CPPFLAGS += -I$(GAUSS_SRC)/src/include
endif
CXXFLAGS += -Wall -Wextra -Werror

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Some openGauss PGXS builds inject -fPIE, which is invalid for extension .so
# objects and causes relocation failures at link time.  Override to -fPIC.
override CXXFLAGS := $(filter-out -fPIE,$(CXXFLAGS)) -fPIC
override CPPFLAGS := $(filter-out -fPIE,$(CPPFLAGS))
