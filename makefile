PROGRAM=tingle
SOURCES=tingle.c
CFLAGS=-O2 -Wall -pedantic -std=c99 -lpthread
LDFLAGS=-lm
HAVE_ALSA := 0

ALSA_TEST := $(shell pkg-config --exists alsa 1>&2 2>/dev/null; echo $$?)
HAVE_ALSA := $(shell if [ $(ALSA_TEST) -eq 0 ]; then echo "true"; else echo "false"; fi)

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
       CFLAGS += -framework AudioToolBox
else if ($(UNAME),Linux)
       ifeq ($(HAVE_ALSA),true)
               CFLAGS += -lasound -DHAVE_ALSA=1
       endif
endif

default:
	-mkdir $(HOME)/bin
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCES) -o $(HOME)/bin/$(PROGRAM)
#	cp tmux.conf $(HOME)/.tmux.conf
	cp volctl $(HOME)/bin
	chmod +x $(HOME)/bin/*
clean:
	-rm $(PROGRAM)
