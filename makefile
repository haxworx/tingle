
default:
	-mkdir $(HOME)/bin
	$(CC) -lm tingle.c -o $(HOME)/bin/tingle
	cp tmux.conf $(HOME)/.tmux.conf
	cp volctl $(HOME)/bin
	chmod +x $(HOME)/bin/*
