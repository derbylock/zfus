all: build

clean:
	rm zfus
build:
	gcc -Wall -D_FILE_OFFSET_BITS=64 zfus.c `pkg-config fuse3 zlib --cflags --libs` -o zfus