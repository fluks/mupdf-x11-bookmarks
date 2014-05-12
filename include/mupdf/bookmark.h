#ifndef BOOKMARK_H
    #define BOOKMARK_H
#define BM_NO_BOOKMARK -1

/* Read bookmark page number.
 * @param docpath
 * @return Previously saved bookmark page number if exists or BM_NO_BOOKMARK if
 * not or something fails.
 */
int bm_read_bookmark(const char *docpath);

/* Save bookmark.
 * @param docpath
 * @param bm_pageno New or changed page number of the bookmark or
 * BM_NO_BOOKMARK if there is no bookmark to save.
 */
void bm_save_bookmark(const char *docpath, int bm_pageno);

#endif // BOOKMARK_H
