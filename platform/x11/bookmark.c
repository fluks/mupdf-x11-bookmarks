#include "mupdf/bookmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/file.h>
#include <sys/types.h>
#include <pwd.h>
#include <limits.h>
#include <stddef.h>
#include <assert.h>

/* For each document an absolute docpath and pageno pair is saved in
 * "docpath = pageno" format, each separated by a newline.
 */

#define BOOKMARKS_FILE ".mupdf_bookmarks"
/* Separate filename and page number with this. */
#define SEPARATOR " = "
#define SEPARATOR_LEN 3

static char *get_bookmark_path();
static bool file_lock(FILE *fp, int operation);
static void file_unlock(FILE *fp);
static int get_pageno(FILE *fp, const char *docpath);
static void change_pageno(FILE *bm, FILE *tmp, const char *docpath, int bm_pageno);
static ssize_t jl_readline(FILE *fp, char **buffer, size_t *size);
static void copy_file(FILE *source, FILE *dest);
static FILE *open_create_if_not_exist(const char *file);

int bm_read_bookmark(const char *docpath) {
	if (docpath == NULL)
		return BM_NO_BOOKMARK;

	char *bm_file = get_bookmark_path();
	if (bm_file == NULL) {
		fputs("can't get bookmark filename\n", stderr);
		return BM_NO_BOOKMARK;
	}

	int bm_pageno = BM_NO_BOOKMARK;
	errno = 0;
	FILE *fp = fopen(bm_file, "r");
	if (fp != NULL) {
		if (file_lock(fp, LOCK_SH)) {
			bm_pageno = get_pageno(fp, docpath);
			file_unlock(fp);
		}
		if (fclose(fp))
			fprintf(stderr, "%s; fclose: %s\n", bm_file, strerror(errno));
	}
	else
		fprintf(stderr, "%s; fopen: %s\n", bm_file, strerror(errno));

	free(bm_file);

	return bm_pageno;
}

void bm_save_bookmark(const char *docpath, int bm_pageno) {
	if (docpath == NULL || bm_pageno == BM_NO_BOOKMARK)
		return;

	char *bm_file = get_bookmark_path();
	if (bm_file == NULL) {
		fputs("can't get bookmark filename\n", stderr);
		return;
	}
	FILE *fp = open_create_if_not_exist(bm_file);
	if (fp == NULL)
		goto clean1;

	char temp_file[] = "/tmp/mupdf_bookmark.XXXXXX";
	int temp_fd = mkstemp(temp_file);
	if (temp_fd == -1) {
		perror("can't create temporary file; mkstemp");
		goto clean2;
	}
	FILE *tmp = fdopen(temp_fd, "w+");
	if (tmp == NULL) {
		perror("can't get stream for temporary file; fdopen");
		goto clean3;
	}

	if (file_lock(fp, LOCK_SH)) {
		change_pageno(fp, tmp, docpath, bm_pageno);
		file_unlock(fp);
	}
	else
		goto clean4;

	rewind(fp);
	rewind(tmp);
	if (file_lock(fp, LOCK_EX)) {
		copy_file(tmp, fp);
		file_unlock(fp);
	}

	clean4: if (fclose(tmp) != 0)
			perror("can't close temporary file; fclose");
	clean3: if (remove(temp_file) != 0)
			perror("can't remove temporary file; remove");
	clean2: if (fclose(fp) != 0)
			fprintf(stderr, "%s: fclose: %s\n", bm_file, strerror(errno));
	clean1: free(bm_file);
}

/* Get page number from bookmark file.
 * @param fp
 * @param docpath
 * @return Bookmark's page number or BM_NO_BOOKMARK if not found or something fails.
 */
static int get_pageno(FILE *fp, const char *docpath) {
	char *line = NULL;
	size_t size = 128;
	size_t docpath_len = strlen(docpath);
	long bm_pageno = BM_NO_BOOKMARK;

	ssize_t read;
	while ((read = jl_readline(fp, &line, &size)) > 0) {
		/* Remove newline. */
		line[read - 1] = '\0';

		if (strncmp(line, docpath, docpath_len) != 0 ||
		    strncmp(line + docpath_len, SEPARATOR, SEPARATOR_LEN) != 0)
			continue;
		errno = 0;
		bm_pageno = strtol(line + docpath_len + SEPARATOR_LEN, NULL, 10);
		if (errno != 0) {
			bm_pageno = BM_NO_BOOKMARK;
			perror("can't read bookmark page number; strtol");
		}
		else if (bm_pageno > INT_MAX) {
			bm_pageno = BM_NO_BOOKMARK;
			fputs("bookmark page number is too big\n", stderr);
		}
		else if (bm_pageno < 1) {
			bm_pageno = BM_NO_BOOKMARK;
			fputs("bookmark page number is not positive\n", stderr);
		}
		break;
	}
	free(line);

	return bm_pageno;
}

/* Change page number for a document in bookmark file.
 * If bookmark for a document is already saved, change it, otherwise add
 * new(docpath + separator + bm_pageno).
 * @param bm fopened pointer to original bookmark file
 * @param tmp fopened pointer to temporary bookmark file
 * @param docpath
 * @param bm_pageno
 */
static void change_pageno(FILE *bm, FILE *tmp, const char *docpath, int bm_pageno) {
	const size_t docpath_len = strlen(docpath);
	char *line = NULL;
	size_t size = 128;
	bool bm_pageno_changed = false;

	ssize_t read;
	while ((read = jl_readline(bm, &line, &size)) > 0) {
		/* Remove newline. */
		line[read - 1] = '\0';

		if (strncmp(line, docpath, docpath_len) != 0 ||
		    strncmp(line + docpath_len, SEPARATOR, SEPARATOR_LEN) != 0) {
			fprintf(tmp, "%s\n", line);
			continue;
		}
		fprintf(tmp, "%s%s%d\n", docpath, SEPARATOR, bm_pageno);
		bm_pageno_changed = true;
	}
	/* Docpath not found in bookmark file, add it. */
	if (!bm_pageno_changed)
		fprintf(tmp, "%s%s%d\n", docpath, SEPARATOR, bm_pageno);

	free(line);
}

/* Get path to bookmark file. Concatenates home and bookmark file.
 * If NULL is returned, there's no need to free memory, otherwise free it in
 * calling context.
 * @return Pointer to absolute bookmark file string or NULL if can't get it.
 */
static char *get_bookmark_path() {
	char *home = getenv("HOME");

	if (!home) {
		fputs("env HOME not set\n", stderr);
		struct passwd *passwd;
		errno = 0;
		if ((passwd = getpwuid(getuid())) == NULL) {
			perror("can't get password record; getpwuid");
			return NULL;
		}
		if ((home = passwd->pw_dir) == NULL) {
			fputs("home directory not set in password file\n", stderr);
			return NULL;
		}
	}
	/* 2 because '/' and '\0' */
	size_t size = strlen(home) + strlen(BOOKMARKS_FILE) + 2;
	char *bm_file = malloc(size * sizeof(*bm_file));
	if (bm_file == NULL) {
		perror("malloc");
		return NULL;
	}
	snprintf(bm_file, size, "%s/%s", home, BOOKMARKS_FILE);

	return bm_file;
}

/* Lock a file.
 * @param fp
 * @param operation A flag for flock. In this program LOCK_SH or LOCK_EX.
 * @return Boolean indicating was flocking succesful.
 */
static bool file_lock(FILE *fp, int operation) {
	errno = 0;
	int fd = fileno(fp);
	if (fd == -1) {
		perror("fileno");
		return false;
	}
	if (flock(fd, operation) == -1) {
		perror("can't lock file; flock");
		return false;
	}
	return true;
}

/* Unlock a file previously file_lock():ed.
 * If this fails, fclose() can release the lock too.
 * @param fp
 */
static void file_unlock(FILE *fp) {
	errno = 0;
	int fd = fileno(fp);
	if (fd == -1) {
		perror("fileno");
		return;
	}
	if (flock(fd, LOCK_UN) == -1)
		perror("can't unlock file; flock");
}

/** Resize buffer.
 * @param buffer A pointer to a buffer. Can point to NULL, if size is not zero,
 *			   size is the initial buffer size.
 * @param size A pointer to a new size of the buffer.
 * @return False if couldn't allocate memory, true otherwise.
 */
#define BUFFER_DEFAULT_SIZE 16
static bool resize_buffer(char **buffer, size_t *size) {
	if (!*buffer && *size == 0)
		*size = BUFFER_DEFAULT_SIZE;
	char *temp = realloc(*buffer, *size);
	if (!temp)
		return false;
	*buffer = temp;

	return true;
}

/** Read a line.
 * Reads a line ending to '\n' or to the end of file. If '\n' is found it's
 * put to the buffer.
 * Buffer is always nul terminated.
 * @param fp A pointer to a file stream, can't be NULL.
 * @param buffer A pointer to a buffer to store the line.
 * @param size A pointer to the size of the buffer. If buffer is not NULL, the
 * size can't be zero then. If buffer is NULL, a buffer of size length is allocated.
 * After a call to jl_readline, size is set to the new size of the buffer. 
 * @return Number of chars read or -1 if eof, a file error or memory allocation error.
 * @note Buffer is dynamically allocated, caller should free its memory.
 */
static ssize_t jl_readline(FILE *fp, char **buffer, size_t *size) {
	assert(fp != NULL);
	assert( (*buffer != NULL && *size > 0) || *buffer == NULL);

	if (!*buffer) {
		if (!resize_buffer(buffer, size))
			return -1;
	}

	char *ret;
	ptrdiff_t end_of_chars = 0;
	while ((ret = fgets(*buffer + end_of_chars, *size - end_of_chars, fp)) != NULL) {
		const char *nl;
		if ((nl = memchr(ret, '\n', *size - end_of_chars)) != NULL)
			return nl - *buffer + 1;
		if (feof(fp))
			return strlen(*buffer);
		end_of_chars = *size - 1;
		*size *= 2;
		if (!resize_buffer(buffer, size))
			return -1;
	}
	return -1;
}

/* Copy a file.
 * @param source Source file.
 * @param dest Destination file.
 */
static void copy_file(FILE *source, FILE *dest) {
	const size_t size = 1024;
	char buf[size];
	size_t n;
	while ((n = fread(buf, sizeof(*buf), size, source)) > 0)
		fwrite(buf, sizeof(*buf), n, dest);

	if (ferror(source) != 0)
		perror("source file; fread");
	if (ferror(dest) != 0)
		perror("destination file; fread");

	if (fflush(dest) != 0)
		perror("destination file; fflush");
}

/* Open a file for reading and writing. If the file doesn't exist, create it.
 * @param filename
 * @return Pointer to a file or NULL if fails.
 */
static FILE *open_create_if_not_exist(const char *file) {
	errno = 0;
	FILE *fp = fopen(file, "r+");
	if (fp == NULL) {
		fprintf(stderr, "%s; fopen: %s\n", file, strerror(errno));
		if (errno == ENOENT) {
			errno = 0;
			fp = fopen(file, "w+");
			if (fp == NULL)
				fprintf(stderr, "can't create file: %s; fopen: %s\n", file, strerror(errno));
		}
	}

	return fp;
}
