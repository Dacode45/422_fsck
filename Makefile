all: fs.h fsck.c
	clang -g -c fsck.c -Wall -Werror
	clang -g -o fsck fsck.o

debug: fs.h fsck.c
	clang -g -c fsck.c -Wall -Werror -o fsck.o -DDEBUG
	clang -g -o fsck fsck.o

clean:
	rm -f fsck.o
