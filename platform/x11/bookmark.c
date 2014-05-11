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

/* For each document a docpath and pageno pair is saved in "docpath = pageno" format,
 * each separated by a newline.
 */

#define BOOKMARKS_FILE ".mupdf_bookmarks"

static char *get_bookmark_path();
static bool file_lock(FILE *fp, int operation);
static void file_unlock(FILE *fp);
/* separate filename and page number with this */
#define SEPARATOR " = "
#define SEPARATOR_LEN strlen(SEPARATOR)
static int get_pageno(FILE *fp, const char *docpath);
static void change_pageno(FILE *bm, FILE *tmp, const char *docpath, int bm_pageno);
static ssize_t jl_readline(FILE *fp, char **buffer, size_t *size);
static int copy_file(FILE *source, FILE *dest);

int bm_read_bookmark(char *docpath) {
    if (docpath == NULL)
        return BM_NO_BOOKMARK;

    char *bm_file = get_bookmark_path();
    if (bm_file == NULL) {
        fputs("can't get bookmark file\n", stderr);
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
            perror("fclose");
    }
    else
        perror("fopen");

    free(bm_file);

    return bm_pageno;
}

void bm_save_bookmark(char *docpath, int bm_pageno) {
    if (docpath == NULL || bm_pageno == BM_NO_BOOKMARK)
        return;

    char *bookmark_file = get_bookmark_path();
    if (bookmark_file == NULL) {
        fputs("can't get bookmark file\n", stderr);
        return;
    }
    errno = 0;
    FILE *fp = fopen(bookmark_file, "r+");
    if (fp == NULL) {
        perror("fopen");
        goto clean1;
    }

    char temp_filename[] = "/tmp/mupdf_bookmark.XXXXXX";
    int temp_fd = mkstemp(temp_filename);
    if (temp_fd == -1) {
        perror("mkstemp");
        goto clean2;
    }
    FILE *tmp = fdopen(temp_fd, "w+");
    if (tmp == NULL) {
        perror("fdopen");
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
                perror("fclose");
    clean3: if (remove(temp_filename) != 0)
                perror("remove");
    clean2: if (fclose(fp) != 0)
                perror("fclose");
    clean1: free(bookmark_file);
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
        
        if (strncmp(line, docpath, docpath_len) != 0)
            continue;
        if (strncmp(line + docpath_len, SEPARATOR, SEPARATOR_LEN) != 0)
            continue;
        errno = 0;
        bm_pageno = strtol(line + docpath_len + SEPARATOR_LEN, NULL, 10);
        /* TODO Should bm_pageno set to 1 if reading a number fails? */
        if (errno != 0) {
            bm_pageno = BM_NO_BOOKMARK;
            perror("strtol");
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

        if (strncmp(line, docpath, docpath_len)) {
            fprintf(tmp, "%s\n", line);
            continue;
        }
        if (strncmp(line + docpath_len, SEPARATOR, SEPARATOR_LEN)) {
            fprintf(tmp, "%s\n", line);
            continue;
        }
        fprintf(tmp, "%s%s%d\n", docpath, SEPARATOR, bm_pageno);
        bm_pageno_changed = true;
    }
    /* docpath not found in bookmark file, add it */
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
            perror("getpwnam");
            return NULL;
        }
        if ((home = passwd->pw_dir) == NULL) {
            fputs("pw_dir in passwd is NULL\n", stderr);
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
 * @param operation a flag for flock. in this program LOCK_SH or LOCK_EX
 * @return boolean indicating was flocking succesful
 */
static bool file_lock(FILE *fp, int operation) {
    errno = 0;
    int fd = fileno(fp);
    if (fd == -1) {
        perror("fileno");
        return false;
    }
    if (flock(fd, operation) == -1) {
        perror("flock");
        return false;
    }
    return true;
}

/* Unlock a file previously file_lock():ed.
 * If this fails, fclose follows and hopefully that succeeds clearing the lock.
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
        perror("flock");
}

/** Resize buffer.
 * @param buffer A pointer to a buffer. Can point to NULL, if size is not zero,
 *               size is the initial buffer size.
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
 * @param fp A pointer to a file stream, can't be NULL.
 * @param buffer A pointer to a buffer to store the line.
 * @param size A pointer to the size of the buffer. If buffer is not NULL, the
 *             size can't be zero then. If buffer is NULL, a buffer of size
 *             length is allocated. @n
 *             After a call to jl_readline, size is set to the new size of the
 *             buffer. 
 * @return Number of chars read or -1 if eof, a file error or memory allocation
 *         error.
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
    while ((ret = fgets(*buffer + end_of_chars, *size - end_of_chars, fp)) !=
           NULL) {
        const char *nl;
        if ((nl = memchr(ret, '\n', *size - end_of_chars)) != NULL)
            return nl - *buffer + 1;
        end_of_chars = *size - 1;
        *size *= 2;
        if (!resize_buffer(buffer, size))
            return -1;
    }
    return -1;
}

static int copy_file(FILE *source, FILE *dest) {
    const size_t size = 1024;
    char buf[size];
    size_t n;
    while ((n = fread(buf, sizeof(*buf), size, source)) > 0)
        fwrite(buf, sizeof(*buf), n, dest);

    if (ferror(source) != 0)
        perror("fread");
    if (ferror(dest) != 0)
        perror("fread");

    if (fflush(dest) != 0)
        perror("fflush");

    return 0;
}
