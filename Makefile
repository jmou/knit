CC = gcc
CFLAGS = -Wall -Wextra -ggdb

RE2C = re2c

BIN = \
	knit \
	knit-compile-plan

SCRIPTS = \
	$(patsubst %.sh,%,$(wildcard *.sh)) \
	$(patsubst %.pl,%,$(wildcard *.pl))

OBJS = lexer.o util.o

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

clean:
	rm -f *.o
	rm -f $(BIN) $(SCRIPTS)
	rm -f lexer.c
	./clean

.PHONY: all clean
