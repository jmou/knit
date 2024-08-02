CC = gcc
CFLAGS = -Wall -Wextra -ggdb

RE2C = re2c

all: compile-plan

lexer.c: lexer.re.c
	$(RE2C) -W -Werror -o $@ $<

# TODO https://www.gnu.org/software/make/manual/html_node/Automatic-Prerequisites.html
lexer.o: lexer.h

compile-plan: lexer.o

.PHONY: all
