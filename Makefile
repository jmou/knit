CFLAGS = -Wall -Wextra -ggdb

CC = gcc
INSTALL = install
PKGCONFIG = pkg-config
RE2C = re2c

prefix = /usr
bindir = $(prefix)/bin

LIBCRYPTO_CFLAGS = $(shell $(PKGCONFIG) --cflags libcrypto)
LIBCRYPTO_LIBS = $(shell $(PKGCONFIG) --libs libcrypto)

-include config.mk

CFLAGS += $(LIBCRYPTO_CFLAGS)
LDFLAGS += $(LIBCRYPTO_LIBS)

BIN = knit $(patsubst %.c,%,$(wildcard knit-*.c))

SCRIPTS = \
	$(patsubst %.sh,%,$(wildcard *.sh)) \
	$(patsubst %.pl,%,$(wildcard *.pl))

OBJS = alloc.o cache.o hash.o invocation.o job.o lexer.o object.o production.o resource.o session.o spec.o util.o

all: $(BIN) $(SCRIPTS)

lexer.c: lexer.re.c
	$(RE2C) -W -Werror -o $@ $<

knit-%: knit-%.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(patsubst %.c,%.o,$(wildcard *.c)): $(wildcard *.h)

knit-%: knit-%.sh
	ln -sf $< $@

knit-%: knit-%.pl
	ln -sf $< $@

knit: knit.o util.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test: all
	$(MAKE) -C tests

clean:
	rm -f *.o
	rm -f $(BIN) $(SCRIPTS)
	rm -f lexer.c
	$(MAKE) -C tests clean

install: all
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) $(BIN) $(SCRIPTS) $(DESTDIR)$(bindir)

.SECONDARY: lexer.o

.PHONY: all clean install test
