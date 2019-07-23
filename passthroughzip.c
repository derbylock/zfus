/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. Its performance is terrible.
 *
 * Compile with
 *
 *     gcc -Wall passthrough.c `pkg-config fuse3 --cflags --libs` -o passthrough
 *
 * ## Source code ##
 * \include passthrough.c
 */


#define FUSE_USE_VERSION 31

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <zlib.h>
#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "passthrough_helpers.h"

static struct options {
	const char *base;
	const char *tmp;
	int show_help;
} options;

#define OPTION(t, p) \
    { t, offsetof(struct options, p), 1 }

static const struct fuse_opt option_spec[] = {
	OPTION("--base=%s", base),
	OPTION("--tmp=%s", tmp),
	OPTION("-h", show_help),
	OPTION("--help", show_help),
	FUSE_OPT_END
};

#define MAX_MODIFIED_HANDLES 1048576
pthread_mutex_t modified_handles_lock;
static uint64_t modified_handles[MAX_MODIFIED_HANDLES]; // 1million max modified
static int modified_handles_count = 0;

void pathEscape(const char* src, char* dst) {
    while(*src != '\0') {
    	if (*src == '/') {
    		*dst = '\\';
    		dst++;
    		*dst ='_';
    		dst++;
    		src++;
    	} else if (*src =='\\') {
    		*dst='\\';
    		dst++;
    		*dst='\\';
    		dst++;
    		src++;
    	} else {
    		*dst = *src;
    		dst++;
			src++;
    	}
    }
    *dst = '\0';
}

void concatAndEscape(const char* t_s1, const char* t_s2, char* dst) {
	strcpy(dst, t_s1);
	char escaped[strlen(t_s2)*2 + 1];
	strcat(dst, "/");
	pathEscape(t_s2, escaped);
	strcat(dst, escaped);
}


#define concatLocal(t_var, t_s1, t_s2) \
   char t_var[strlen(t_s1) + strlen(t_s2) + 1]; \
   strcpy(t_var, t_s1); \
   strcat(t_var, t_s2);  \


#define concatLocalNoDirs(t_var, t_s1, t_s2) \
   char t_var[(strlen(t_s1) + strlen(t_s2)*2) + 2]; \
   concatAndEscape(t_s1, t_s2, t_var);  \


#define CHUNK_SIZE 409600
#define COMPRESSION_LEVEL 9

bool compress_file(FILE *src, FILE *dst)
{
	fseek(src, 0L, SEEK_END);
	long int srcSize = ftell(src);
	fwrite(&srcSize, sizeof(srcSize), 1, dst);
	rewind(src);

    uint8_t inbuff[CHUNK_SIZE];
    uint8_t outbuff[CHUNK_SIZE];
    z_stream stream = {0};

    if (deflateInit(&stream, COMPRESSION_LEVEL) != Z_OK)
    {
    	fprintf(stderr, "deflateInit(...) failed!\n");
        return false;
    }

	int flush;
    do {
        stream.avail_in = fread(inbuff, 1, CHUNK_SIZE, src);
        if (ferror(src))
        {
        	fprintf(stderr, "fread(...) failed!\n");
            deflateEnd(&stream);
            return false;
        }

        flush = feof(src) ? Z_FINISH : Z_NO_FLUSH;
        stream.next_in = inbuff;

        do {
            stream.avail_out = CHUNK_SIZE;
            stream.next_out = outbuff;
            deflate(&stream, flush);
            uint32_t nbytes = CHUNK_SIZE - stream.avail_out;

            if (fwrite(outbuff, 1, nbytes, dst) != nbytes || ferror(dst))
            {
            	fprintf(stderr, "fwrite(...) failed!\n");
                deflateEnd(&stream);
                return false;
            }
        } while (stream.avail_out == 0);
    } while (flush != Z_FINISH);

    deflateEnd(&stream);
    return true;
}

bool decompress_file(FILE *src, FILE *dst)
{
    uint8_t inbuff[CHUNK_SIZE];
    uint8_t outbuff[CHUNK_SIZE];
    z_stream stream = { 0 };

	long int srcSize;
	fread(&srcSize, sizeof(srcSize), 1, src);

    int result = inflateInit(&stream);
    if (result != Z_OK)
    {
    	fprintf(stderr, "inflateInit(...) failed!\n");
        return false;
    }

    do {
        stream.avail_in = fread(inbuff, 1, CHUNK_SIZE, src);
        if (ferror(src))
        {
        	fprintf(stderr, "fread(...) failed!\n");
            inflateEnd(&stream);
            return false;
        }

        if (stream.avail_in == 0)
            break;

        stream.next_in = inbuff;

        do {
            stream.avail_out = CHUNK_SIZE;
            stream.next_out = outbuff;
            result = inflate(&stream, Z_NO_FLUSH);
            if(result == Z_NEED_DICT || result == Z_DATA_ERROR ||
            	result == Z_MEM_ERROR)
            {
            	fprintf(stderr, "inflate(...) failed: %d\n", result);
                inflateEnd(&stream);
                return false;
            }

            uint32_t nbytes = CHUNK_SIZE - stream.avail_out;

            if (fwrite(outbuff, 1, nbytes, dst) != nbytes || ferror(dst))
            {
            	fprintf(stderr, "fwrite(...) failed!\n");
                inflateEnd(&stream);
                return false;
            }
        } while (stream.avail_out == 0);
    } while (result != Z_STREAM_END);

    inflateEnd(&stream);
    return result == Z_STREAM_END;
}

bool decompress_file_by_name(const char* srcFileName, const char* dstFileName) {
	FILE* srcId = fopen(srcFileName, "rb");
	if(srcId == 0)
	{
		fprintf(stderr, "open(...) failed, errno = %d\n", errno);
		return false;
	}
	FILE* dstId = fopen(dstFileName, "wb");
	if(dstId == 0)
	{
		fprintf(stderr, "open(...) failed, errno = %d\n", errno);
		fclose(srcId);
		return false;
	}

	bool res = decompress_file(srcId, dstId);

	fclose(srcId);
	fclose(dstId);

	return res;
}

bool compress_file_by_name(const char* srcFileName, const char* dstFileName) {
	FILE* srcId = fopen(srcFileName, "rb");
	if(srcId == 0)
	{
		fprintf(stderr, "open %s(...) failed, errno = %d\n", srcFileName, errno);
		return false;
	}
	FILE* dstId = fopen(dstFileName, "wb");
	if(dstId == 0)
	{
		fprintf(stderr, "open %s(...) failed, errno = %d\n", dstFileName, errno);
		fclose(srcId);
		return false;
	}

	bool res = compress_file(srcId, dstId);

	fclose(srcId);
	fclose(dstId);

	return res;
}

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	return NULL;
}

static int xmp_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	concatLocal(relative_path_base, options.base, path)
	concatLocalNoDirs(relative_path_tmp, options.tmp, path)

	res = lstat(relative_path_base, stbuf);
	if (res != -1) {
		if (S_ISDIR(stbuf->st_mode)) {
			return 0;
		}
		FILE* src = fopen(relative_path_base, "rb");
		if(src == 0)
		{
			fprintf(stderr, "IGNORE: open %s(...) failed, errno = %d\n", relative_path_base, errno);
			return 0;
		}
		long int srcSize;
		fread(&srcSize, sizeof(srcSize), 1, src);
		fclose(src);
		stbuf->st_size = srcSize;
		return 0;
	}

	res = lstat(relative_path_tmp, stbuf);

	if (res == -1) {
		return -errno;
	}

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = access(relative_path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = readlink(relative_path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;

	concatLocal(relative_path, options.base, path)
	dp = opendir(relative_path);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0, 0))
			break;
	}

	closedir(dp);
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = mknod_wrapper(AT_FDCWD, relative_path, NULL, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = mkdir(relative_path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = unlink(relative_path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = rmdir(relative_path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;

	concatLocal(relative_path_from, options.base, from)
	concatLocal(relative_path_to, options.base, to)
	res = symlink(relative_path_from, relative_path_to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
	int res;

	if (flags)
		return -EINVAL;

	concatLocal(relative_path_from, options.base, from)
	concatLocal(relative_path_to, options.base, to)
	res = rename(relative_path_from, relative_path_to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	concatLocal(relative_path_from, options.base, from)
	concatLocal(relative_path_to, options.base, to)
	res = link(relative_path_from, relative_path_to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	concatLocal(relative_path, options.base, path)
	res = chmod(relative_path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	concatLocal(relative_path, options.base, path)
	res = lchown(relative_path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size,
			struct fuse_file_info *fi)
{
	int res;

	concatLocal(relative_path, options.base, path)
	if (fi != NULL)
		res = ftruncate(fi->fh, size);
	else
		res = truncate(relative_path, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	int res;

	concatLocalNoDirs(relative_path, options.tmp, path)
	res = open(relative_path, fi->flags, mode);
	if (res == -1) {
		return -errno;
	}

	fi->fh = res;
	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	int res;

	concatLocal(relative_path_base, options.base, path)
	concatLocalNoDirs(relative_path_tmp, options.tmp, path)

	if(!decompress_file_by_name(relative_path_base, relative_path_tmp)) {
		return -1;
	}

	res = open(relative_path_tmp, fi->flags);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int fd;
	int res;

	concatLocalNoDirs(relative_path_tmp, options.tmp, path)
	if(fi == NULL)
		fd = open(relative_path_tmp, O_RDONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int fd;
	int res;

	concatLocalNoDirs(relative_path_tmp, options.tmp, path)
	(void) fi;
	if(fi == NULL)
		fd = open(relative_path_tmp, O_WRONLY);
	else {
		fd = fi->fh;
		pthread_mutex_lock(&modified_handles_lock);
		int i;
		for (i=0; i < modified_handles_count; i++) {
			if (modified_handles[i] == fd) {
				break;
			}
		}
		if (i==modified_handles_count) {
			if (i < MAX_MODIFIED_HANDLES) {
			    modified_handles[i] = fd;
			    modified_handles_count++;
			} else {
				pthread_mutex_unlock(&modified_handles_lock);
				return 24;
			}
		}
		pthread_mutex_unlock(&modified_handles_lock);
	}
	
	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL) {
		close(fd);

		concatLocal(relative_path_base, options.base, path)
		if(!compress_file_by_name(relative_path_tmp, relative_path_base)) {
			fprintf(stderr, "release compress failed\n");
			return -1;
		}

		remove(relative_path_tmp);
	}
	return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	concatLocal(relative_path, options.base, path)
	res = statvfs(relative_path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	concatLocal(relative_path_base, options.base, path)
	concatLocalNoDirs(relative_path_tmp, options.tmp, path)
	fflush(stdout);

	(void) path;
	close(fi->fh);

	pthread_mutex_lock(&modified_handles_lock);
	int i;
	for (i=0; i < modified_handles_count; i++) {
		if (modified_handles[i] == fi->fh) {
			break;
		}
	}
	if (i != modified_handles_count) {
		int j;
		for (j=i+1; j < modified_handles_count; j++) {
			modified_handles[j-1] = modified_handles[j];
		}
		modified_handles_count--;
		pthread_mutex_unlock(&modified_handles_lock);

		if(!compress_file_by_name(relative_path_tmp, relative_path_base)) {
			fprintf(stderr, "release compress failed\n");
			return -1;
		}
	} else {
		pthread_mutex_unlock(&modified_handles_lock);
	}

	remove(relative_path_tmp);

	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) isdatasync;
	(void) fi;
	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;

	if (mode)
		return -EOPNOTSUPP;

	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	if(fi == NULL)
		close(fd);
	return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_COPY_FILE_RANGE
static ssize_t xmp_copy_file_range(const char *path_in,
				   struct fuse_file_info *fi_in,
				   off_t offset_in, const char *path_out,
				   struct fuse_file_info *fi_out,
				   off_t offset_out, size_t len, int flags)
{
	int fd_in, fd_out;
	ssize_t res;

	if(fi_in == NULL)
		fd_in = open(path_in, O_RDONLY);
	else
		fd_in = fi_in->fh;

	if (fd_in == -1)
		return -errno;

	if(fi_out == NULL)
		fd_out = open(path_out, O_WRONLY);
	else
		fd_out = fi_out->fh;

	if (fd_out == -1) {
		close(fd_in);
		return -errno;
	}

	res = copy_file_range(fd_in, &offset_in, fd_out, &offset_out, len,
			      flags);
	if (res == -1)
		res = -errno;

	close(fd_in);
	close(fd_out);

	return res;
}
#endif

static struct fuse_operations xmp_oper = {
	.init           = xmp_init,
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.open		= xmp_open,
	.create 	= xmp_create,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
#ifdef HAVE_COPY_FILE_RANGE
	.copy_file_range = xmp_copy_file_range,
#endif
};

static void show_help(const char *progname)
{
	printf("usage: %s [options] <mountpoint>\n\n", progname);
	printf("File-system specific options:\n"
	   "    --base=<s>          Base folder\n"
	   "                        (default: \"/zfusbase\")\n"
	   "    --tmp=<s>      Temp base folder\n"
	   "                        (default \"/zfustmp\\n\")\n"
	   "\n");
}

int main(int argc, char *argv[])
{
	int ret;
	printf("%s\n", "Init");

    if (pthread_mutex_init(&modified_handles_lock, NULL) != 0)
    {
        printf("\n mutex init failed\n");
        return 1;
    }

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	/* Set defaults -- we have to use strdup so that
	   fuse_opt_parse can free the defaults if other
	   values are specified */
	options.base = strdup("/zfusbase");
	options.tmp = strdup("/zfustmp");

	/* Parse options */
	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) {
		return 1;
	}

	char* realBase = malloc(sizeof(char[PATH_MAX]));
	char* realTmp = malloc(sizeof(char[PATH_MAX]));
	realpath(options.base, realBase);
	realpath(options.tmp, realTmp);
	options.base = realBase;
	options.tmp = realTmp;
	printf("base = %s\n", options.base);
	printf("tmp = %s\n", options.tmp);
	concatLocal(relative_path, options.base, "/")
	printf("relative_path = %s\n", relative_path);
	concatLocalNoDirs(relative_path_tmp, options.tmp, "/")
	printf("relative_path_tmp = %s\n", relative_path_tmp);
	fflush(stdout);

	/* When --help is specified, first print our own file-system
	   specific help text, then signal fuse_main to show
	   additional help (by adding `--help` to the options again)
	   without usage: line (by setting argv[0] to the empty
	   string) */
	if (options.show_help) {
		show_help(argv[0]);
		assert(fuse_opt_add_arg(&args, "--help") == 0);
		args.argv[0][0] = '\0';
	}
	umask(0);
	ret = fuse_main(args.argc, args.argv, &xmp_oper, NULL);
	fuse_opt_free_args(&args);
	printf("%s\n", "Exiting");
	fflush(stdout);
	pthread_mutex_destroy(&modified_handles_lock);
	return ret;
}
