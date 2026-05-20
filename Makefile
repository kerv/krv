CC = gcc
CFLAGS = -O3 -Wall -Wextra -std=c11 -fPIC
LDFLAGS = -shared
SRCDIR = src
BUILDDIR = build

SRCS = $(SRCDIR)/arithmetic.c $(SRCDIR)/predictors.c $(SRCDIR)/mixer.c $(SRCDIR)/block.c $(SRCDIR)/file.c $(SRCDIR)/lz.c $(SRCDIR)/bcj.c
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
LIB = $(BUILDDIR)/libkrv.so

.PHONY: all clean test

all: $(LIB)

$(LIB): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread -lm

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/krv.h $(SRCDIR)/lz.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

test: $(LIB)
	python3 tests/test_roundtrip.py
