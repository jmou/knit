CC = gcc
CFLAGS = -Wall -Wextra -ggdb

RE2C = re2c

BIN = \
	knit \
	knit-compile-plan

SCRIPTS = \
	$(patsubst %.sh,%,$(wildcard *.sh)) \
	$(patsubst %.pl,%,$(wildcard *.pl))

all: $(BIN) $(SCRIPTS)

lexer.c: lexer.re.c
	$(RE2C) -W -Werror -o $@ $<

# TODO https://www.gnu.org/software/make/manual/html_node/Automatic-Prerequisites.html
lexer.o: lexer.h

knit-compile-plan: lexer.o

knit-%: knit-%.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

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
