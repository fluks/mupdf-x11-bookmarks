#ifndef BOOKMARK_H
    #define BOOKMARK_H
#define BM_NO_BOOKMARK -1

/* Read bookmark page number if saved for docpath document.
 * @param docpath
 * @return previously saved bookmark page number if exists or -1 if not or something fails
 */
int bm_read_bookmark(const char *docpath);

/* Save bookmark. Bookmark is actually saved to a file only when closing the application.
 * @param docpath
 * @param bm_pageno should be -1 if bookmark not saved or changed, even if it exists
 */
void bm_save_bookmark(const char *docpath, int bm_pageno);

#endif // BOOKMARK_H
