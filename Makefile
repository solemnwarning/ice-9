CFLAGS ?= -Wall

# Cross compiler used to build Windows EXEs
CROSS_CC ?= i686-w64-mingw32-gcc
CROSS_CFLAGS ?= -Wall

.PHONY: all
all: ice9d.exe ice9r

.PHONY: clean
clean:
	rm -f ice9d.exe ice9d.o ice9r pipe9x/pipe9x.o

ice9d.exe: ice9d.o pipe9x/pipe9x.o
	$(CROSS_CC) -Wall -o $@ $^ -lws2_32

ice9d.o: ice9d.c pipe9x/pipe9x.h
	$(CROSS_CC) -Wall -c -o $@ $<

pipe9x/pipe9x.o: pipe9x/pipe9x.c pipe9x/pipe9x.h
	$(CROSS_CC) -Wall -c -o $@ $<

ice9r: ice9r.c
	$(CC) $(CFLAGS) -o $@ $^
