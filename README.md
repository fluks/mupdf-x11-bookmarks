DESCRIPTION
===========

Bookmarks for mupdf on x11 platforms.

Bookmarks are saved to ~/.mupdf_bookmarks.

INSTALL
-------

```
git submodule update --init
make
sudo make prefix=/usr/local install
```

USAGE
-----

Press 'B' to save a bookmark for the document. It is only saved to a file
when the document is reloaded or the program is quit.

BUGS
----

Newline in the document's filename. See TODO.

LICENSE
-------

Same as mupdf's. See COPYING.
