TESTS = $(wildcard *.sh)

all: $(TESTS) clean

$(TESTS):
	@[ ! -x $@ ] || { echo === $@ ===; /bin/bash $@; }

clean:
	rm -rf tmp

.PHONY: $(TESTS) clean
