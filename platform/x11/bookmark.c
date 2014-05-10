#include "mupdf/bookmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/file.h>
#include <stdarg.h>
#include <sys/types.h>
#include <pwd.h>
#include <limits.h>
#include <stddef.h>
#include <assert.h>

/* For each document a docpath and pageno pair is saved in "docpath = pageno" format,
 * each separated by a newline.
 * TODO BM_MAX_LINE, get platform specific somehow, by pathconf?
 *      File locking, can it be implemented in a cross-platform way
 */

#define BOOKMARKS_FILE ".mupdf_bookmarks"

static char *get_bookmark_path();
/* temporary file's extension length */
#define TMP_EXT_LEN 10
static void random_extension(char *tmp_ext);
static bool file_lock(FILE *fp, int operation);
static void file_unlock(FILE *fp);
/* separate filename and page number with this */
#define SEPARATOR " = "
#define SEPARATOR_LEN strlen(SEPARATOR)
/* TODO stupid arbitrary number(more than 4096 which maybe is normal for unix)
 * maximum line length in bookmark file
 */
#define BM_MAX_LINE 5100
static int get_pageno(FILE *fp, const char *docpath);
static void change_pageno(FILE *bm, FILE *tmp, const char *docpath, int bm_pageno);
static void close_fps(FILE *fps, ...);
static void free_ptrs(void *ptrs, ...);
static ssize_t jl_readline(FILE *fp, char **buffer, size_t *size);

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
        errno = 0;
        if (fclose(fp))
            perror("fclose");
    }
    else
        perror("fopen");

    free(bm_file);

    return bm_pageno;
}

void bm_save_bookmark(char *docpath, int bm_pageno) {
    char *config_file           = NULL;
    char *backup_config_file    = NULL;
    FILE *fp                    = NULL;
    const char *mode_fp         = "r";
    char *tmp_file              = NULL;
    char tmp_ext[TMP_EXT_LEN]   = { 0 }; 
    FILE *tmp                   = NULL;
    const char *mode_tmp        = "w";

    if (docpath == NULL || bm_pageno == BM_NO_BOOKMARK)
        return;

    if ((config_file = get_bookmark_path()) == NULL) {
        fputs("can't get config file\n", stderr);
        return;
    }
    if ((fp = fopen(config_file, mode_fp)) == NULL) {
        perror("fopen fp");
        free_ptrs(config_file, (void *)(NULL));
        return;
    }
    if ((backup_config_file = malloc((strlen(config_file) + 1) * sizeof(*config_file))) == NULL) {
        close_fps(fp, (FILE *)(NULL));
        free_ptrs(config_file, (void *)(NULL));
        return;
    }
    strcpy(backup_config_file, config_file);
    if ((tmp_file = realloc(config_file, strlen(config_file) + TMP_EXT_LEN)) == NULL) {
        perror("realloc");
        close_fps(fp, (FILE *)(NULL));
        free_ptrs(config_file, backup_config_file, (void *)(NULL));
        return;
    }
    /* TODO It's possible that after generating a random filename, some other process decides to use
     * a file with a same name. Prevent this.
     */
    random_extension(tmp_ext);
    strcat(tmp_file, tmp_ext);
    if ((tmp = fopen(tmp_file, mode_tmp)) == NULL) {
        perror("fopen tmp");
        close_fps(fp, (FILE *)(NULL));
        free_ptrs(tmp_file, backup_config_file, (void *)(NULL));
        return;
    }
    if (file_lock(fp, LOCK_SH)) {
        change_pageno(fp, tmp, docpath, bm_pageno);
        file_unlock(fp);
    }

    close_fps(tmp, (FILE *)(NULL));

    if (file_lock(fp, LOCK_EX)) {
        if (rename(tmp_file, backup_config_file))
            perror("rename");
        file_unlock(fp);
    }
    close_fps(fp, (FILE *)(NULL));

    free_ptrs(tmp_file, backup_config_file, (void *)(NULL));
}

/* Get page number from bookmark file.
 * If bookmark for docpath is found, but it's too large for an int or if it's non-positive,
 * return no bookmark found.
 * If there are more than one docpaths(something is wrong), only first one is noticed.
 * @param fp already opened pointer to bookmark file
 * @param docpath absolute path of a file to search already saved page number for
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
    char line[BM_MAX_LINE]   = { 0 };
    bool bm_pageno_changed   = false;

    while (!feof(bm)) {
        if (fgets(line, BM_MAX_LINE, bm) == NULL)
            continue;
        if (strncmp(line, docpath, docpath_len)) {
            fprintf(tmp, "%s", line);
            continue;
        }
        if (strncmp(line + docpath_len, SEPARATOR, SEPARATOR_LEN)) {
            fprintf(tmp, "%s", line);
            continue;
        }
        fprintf(tmp, "%s%s%d\n", docpath, SEPARATOR, bm_pageno);
        /* continue to copy original file */
        bm_pageno_changed = true;
    }
    /* docpath not found in bookmark file, add it */
    if (!bm_pageno_changed)
        fprintf(tmp, "%s%s%d\n", docpath, SEPARATOR, bm_pageno);
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

/* Generate random extension. Fill buf with: [a-zA-Z0-9]{TMP_EXT_LEN - ext_len - 1}.tmp
 * @param buf needs to be able to hold at least 5 characters
 */
static void random_extension(char *tmp_ext) {
    const char *ext        = ".tmp";
    const size_t ext_len   = strlen(ext);
    const char *chars      = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t chars_len = strlen(chars); 
    size_t i, index;

    srand(time(NULL) + getpid());
    for (i = 0; i < TMP_EXT_LEN - ext_len - 1; i++) {
        index = (double)rand() / RAND_MAX * chars_len;
        tmp_ext[i] = chars[index];
    }
    tmp_ext[i] = '\0';
    strcat(tmp_ext, ext);
}

/* Lock a file.
 * For other platforms than linux and mac, this is a nop.
 * @param fp
 * @param operation a flag for flock. in this program LOCK_SH or LOCK_EX
 * @return boolean indicating was flocking succesful
 */
static bool file_lock(FILE *fp, int operation) {
/* _XOPEN_SOURCE and _POSIX_C_SOURCE for fileno() */
#if (defined __GNUC__ || defined __APPLE__) && (defined _XOPEN_SOURCE || defined _POSIX_C_SOURCE)
    errno = 0;
    int fd = fileno(fp);
    if (fd == -1) {
        perror("fileno");
        return false;
    }
    errno = 0;
    if (flock(fd, operation) == -1) {
        perror("flock");
        return false;
    }
    return true;
#else
    return true;
#endif
}

/* Unlock a file previously file_lock():ed.
 * For other platforms than linux and mac, this is a nop. If this fails, fclose follows and
 * hopefully that succeeds clearing the lock.
 * @param fp
 */
static void file_unlock(FILE *fp) {
#if (defined __GNUC__ || defined __APPLE__) && (defined _XOPEN_SOURCE || defined _POSIX_C_SOURCE)
    errno = 0;
    int fd = fileno(fp);
    if (fd == -1) {
        perror("fileno");
        return;
    }
    errno = 0;
    if (flock(fd, LOCK_UN) == -1)
        perror("flock");
#endif
}

/* Free memory.
 * @param ptrs variable amount of pointers, last one must be (void *)(NULL)
 */
static void free_ptrs(void *ptrs, ...) {
    va_list ap;
    void *ptr;

    va_start(ap, ptrs);
    ptr = ptrs;
    while (ptr != NULL) {
        free(ptr);
        ptr = NULL;
        ptr = va_arg(ap, void *);
    }
    va_end(ap);
}

/* Fclose opened file pointers.
 * @param fps variable amount of pointers to files, last one must be (FILE *)(NULL)
 */
static void close_fps(FILE *fps, ...) {
    va_list ap;
    FILE *fp;

    va_start(ap, fps);
    fp = fps;
    while (fp != NULL) {
        if (fclose(fp))
            perror("fclose");
        fp = va_arg(ap, FILE *);
    }
    va_end(ap);
}

#define BUFFER_DEFAULT_SIZE 16

/** Resize buffer.
 * @param buffer A pointer to a buffer. Can point to NULL, if size is not zero,
 *               size is the initial buffer size.
 * @param size A pointer to a new size of the buffer.
 * @return False if couldn't allocate memory, true otherwise.
 */
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
