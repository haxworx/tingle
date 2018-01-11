PROGRAM=tingle
SOURCES=tingle.c
CFLAGS=-O2 -Wall -pedantic -std=c99
LDFLAGS=-lm

UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
       CFLAGS += -framework AudioToolBox
endif

default:
	-mkdir $(HOME)/bin
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCES) -o $(HOME)/bin/$(PROGRAM)
	cp tmux.conf $(HOME)/.tmux.conf
	cp volctl $(HOME)/bin
	chmod +x $(HOME)/bin/*
clean:
	-rm $(PROGRAM)
