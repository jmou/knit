all: abc.reverse def.reverse
	cat $^ > $@

%.reverse: %.txt
	perl -e 'print reverse <>' $< > $@

abc.txt:
	echo -e 'a\nb\nc' > $@

def.txt:
	echo -e 'd\ne\nf' > $@
