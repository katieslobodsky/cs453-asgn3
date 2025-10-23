# Makefile for dine
CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -std=c11 -O2
UNAME_S := $(shell uname -s)

# Threads everywhere; no librt on macOS
LDFLAGS := -pthread

# Optional: override at build time, e.g.:
#   make dine CFLAGS+="-DNUM_PHILOSOPHERS=7"
CYCLES ?= 1

.PHONY: all clean run run2

all: dine

dine: dine.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

dine.o: dine.c
	$(CC) $(CFLAGS) -c $<

run: dine
	./dine $(CYCLES)

run2: dine
	./dine 2

clean:
	rm -f dine dine.o
