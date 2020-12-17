abc:
	echo -e 'a\nb\nc' > $@

tac: abc
	perl -pe 'print reverse <>' < $< > $@

left right: abc
	grep -v b $< > left
	grep b $< > right

join: left tac right
	cat $^ > $@
