all: build

clean:
	rm hello
build:
	gcc -Wall -D_FILE_OFFSET_BITS=64 hello.c `pkg-config fuse3 --cflags --libs` -o hello
	gcc -Wall -D_FILE_OFFSET_BITS=64 passthrough.c `pkg-config fuse3 --cflags --libs` -o passthrough
	gcc -Wall -D_FILE_OFFSET_BITS=64 passthroughzip.c `pkg-config fuse3 zlib --cflags --libs` -o passthroughzip