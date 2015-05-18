# make it so.

vcap: vcap.c
	gcc -g3 -o $@ $< -ljpeg

