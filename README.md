Zlib fuse filesystem which compress/decompress file on opening/closing (after modification).

Most existing filesystems compress/decompress files when filesystem unmounts which in most cases is not good for filesystems with many files.

## usage
```bash
make
./passthroughzip --base="../zfusbase" --tmp="../zfustmp" ../tt
```

here:
  - zfusbase is the direcotry where compressed files will be stored,
  - zfustmp is the directory for uncompressed currently opened files
  - tt is the mountpoint
  
