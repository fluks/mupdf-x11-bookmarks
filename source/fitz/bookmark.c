#include "bookmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/file.h>
#include <stdarg.h>

/* For each document a docpath and pageno pair is saved in "docpath = pageno" format,
 * each separated by a newline.
 * TODO BM_MAX_LINE, get platform specific somehow, by pathconf?
 *      File locking, can it be implemented in a cross-platform way
 */

#define BOOKMARKS_FILE ".mupdf_bookmarks"
static char *get_config_path(void);

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
static void get_pageno(FILE *fp, const char *docpath, int *bm_pageno);

static void change_pageno(FILE *bm, FILE *tmp, const char *docpath, int bm_pageno);

static void close_fps(FILE *fps, ...);

static void free_ptrs(void *ptrs, ...);

int read_bookmark(char *docpath) {
    char *config_file          = NULL;
    int bm_pageno              = NO_BOOKMARK;
    FILE *fp                   = NULL;
    const char *mode           = "r";
    
    if (docpath == NULL)
        return NO_BOOKMARK;

    if ((config_file = get_config_path()) == NULL)
        fputs("can't get config file\n", stderr);
    else if ((fp = fopen(config_file, mode)) != NULL) {
        if (file_lock(fp, LOCK_SH)) {
            get_pageno(fp, docpath, &bm_pageno);
            file_unlock(fp);
        }
        close_fps(fp, (FILE *)(NULL));
    }
    else
        perror("fopen");

    free_ptrs(config_file, (void *)(NULL));

    return bm_pageno;
}

void save_bookmark(char *docpath, int bm_pageno) {
    char *config_file           = NULL;
    char *backup_config_file    = NULL;
    FILE *fp                    = NULL;
    const char *mode_fp         = "r";
    char *tmp_file              = NULL;
    char tmp_ext[TMP_EXT_LEN]   = { 0 }; 
    FILE *tmp                   = NULL;
    const char *mode_tmp        = "w";

    if (docpath == NULL || bm_pageno == NO_BOOKMARK)
        return;

    if ((config_file = get_config_path()) == NULL) {
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
 * @param bm_pageno
 */
static void get_pageno(FILE *fp, const char *docpath, int *bm_pageno) {
    char line[BM_MAX_LINE]   = { 0 };
    const size_t docpath_len = strlen(docpath);

    while (!feof(fp)) {
        if (fgets(line, BM_MAX_LINE, fp) == NULL)
            continue;
        if (strncmp(line, docpath, docpath_len) != 0)
            continue;
        if (strncmp(line + docpath_len, SEPARATOR, SEPARATOR_LEN) != 0)
            continue;
        /* TODO strtol()? setting errno in atoi() wasn't documented */
        *bm_pageno = atoi(line + docpath_len + SEPARATOR_LEN);
        /* XXX save old errno, set to zero and set back to old again? */
        if (errno == ERANGE) {
            perror("atoi");
            *bm_pageno = NO_BOOKMARK;
        }
        if (*bm_pageno < 1)
            *bm_pageno = NO_BOOKMARK;
        break;
    }
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

/* Get path for config file, multi-platform. Concatenates home and bookmark file.
 * If NULL is returned, there's no need to free memory, otherwise free it in
 * calling context.
 * @return pointer to path string or NULL if can't get it
 */
static char *get_config_path(void) {
    char *config_file = NULL;
    size_t bm_size    = strlen(BOOKMARKS_FILE);

    #ifdef _WIN32
        #define USERPROFILE "USERPROFILE"
        #define HOMEDRIVE "HOMEDRIVE"
        #define HOMEPATH  "HOMEPATH"
        char *userprofile = NULL;
        char *homedrive   = NULL;
        char *homepath    = NULL;
        /* 2 because '\' and '\0' */ 
        size_t size = bm_size + 2;

        if ((userprofile = getenv(USERPROFILE)) != NULL)
            size += strlen(userprofile);
        else {
            fprintf(stderr, "env %s not set\n", USERPROFILE);
            homedrive = getenv(HOMEDRIVE);
            homepath  = getenv(HOMEPATH);
            if (homedrive == NULL || homepath == NULL) {
                fprintf(stderr, "HOMEDRIVE: %s\nHOMEPATH: %s\n", homedrive, homepath);
                return NULL;
            }
            size += strlen(homedrive) + strlen(homepath);
        }
        if ((config_file = malloc(size * sizeof(*config_file))) == NULL) {
            perror("malloc");
            return NULL;
        }
        if (userprofile)
            snprintf(config_file, size, "%s\\%s", userprofile, BOOKMARKS_FILE);
        else
            snprintf(config_file, size, "%s%s\\%s", homedrive, homepath, BOOKMARKS_FILE);
    #elif defined __GNUC__ || defined __APPLE__
        #include <sys/types.h>
        #include <pwd.h>
        #define HOME "HOME"
        #define USER "USER"
        struct passwd *passwd = NULL;
        char *user = NULL;
        char *home = getenv(HOME);

        if (!home) {
            fputs("env HOME not set\n", stderr);
            if ((user = getenv(USER)) == NULL) {
                fputs("env USER not set\n", stderr);
                return NULL;
            }
            errno = 0;
            if ((passwd = getpwnam(user)) == NULL) {
                perror("getpwnam");
                return NULL;
            }
            if ((home = passwd->pw_dir) == NULL) {
                fputs("pw_dir in passwd is NULL\n", stderr);
                return NULL;
            }
        }
        /* 2 because '/' and '\0' */
        size_t size = strlen(home) + bm_size + 2;
        if ((config_file = malloc(size * sizeof(*config_file))) == NULL) {
            perror("malloc");
            return NULL;
        }
        snprintf(config_file, size, "%s/%s", home, BOOKMARKS_FILE);
    #endif

    return config_file;
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
#include <sys/file.h>
    int fd;
    if ((fd = fileno(fp)) == -1) {
        perror("fileno");
        return false;
    }
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
    int fd;
    if ((fd = fileno(fp)) == -1) {
        perror("fileno");
        return;
    }
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
