CFLAGS = -Wall -Wextra -ggdb

CC = gcc
PKGCONFIG = pkg-config
RE2C = re2c

-include config.mk

BIN = \
	knit \
	knit-cat-file \
	knit-compile-plan \
	knit-hash-object

SCRIPTS = \
	$(patsubst %.sh,%,$(wildcard *.sh)) \
	$(patsubst %.pl,%,$(wildcard *.pl))

OBJS = hash.o lexer.o util.o

all: $(BIN) $(SCRIPTS)

lexer.c: lexer.re.c
	$(RE2C) -W -Werror -o $@ $<

knit-hash-object: CFLAGS += $(shell $(PKGCONFIG) --cflags libcrypto)
knit-hash-object: LDFLAGS += $(shell $(PKGCONFIG) --libs libcrypto)

knit-%: knit-%.o $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(patsubst %.c,%.o,$(wildcard *.c)): $(wildcard *.h)

knit-%: knit-%.sh
	ln -sf $< $@

knit-%: knit-%.pl
	ln -sf $< $@

clean:
	rm -f *.o
	rm -f $(BIN) $(SCRIPTS)
	rm -f lexer.c
	./clean

.PHONY: all clean
