all:
	clang -Wall -O2 -target bpf -I ../../inc/ -I ../../../../bess-nm/experiments/MICA_LOCAL/mica -c compose_post.c -o compose_post.o

clean:
	rm -f compose_post.o

.PHONY: all clean
