/* SPDX-FileCopyrightText: 2024 Thomas E. Dickey */
/* SPDX-FileCopyrightText: 2002 Keith Packard */
/* SPDX-License-Identifier: HPND-sell-variant */
/* Vendored subset of libXcursor 1.2.3 src/file.c (X11-free, modified). */
/*
 * Copyright © 2024 Thomas E. Dickey
 * Copyright © 2002 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * X11-free ".xcursor" file-loader subset extracted verbatim from
 * libXcursor 1.2.3 src/file.c.  Only the image-loading call chain reachable
 * from XcursorFilenameLoadImages() is retained; comment chunks, the write/save
 * path, theme/library helpers and everything touching X11/Xlib/Xrender/Xfixes
 * have been dropped.  The bounds and overflow checks are preserved exactly as
 * upstream: these files are attacker-controlled and the validation must not be
 * weakened.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xcursor.h"

/* glibc opens cursor files O_CLOEXEC; "e" is a no-op on libc's that ignore it */
#define FOPEN_CLOEXEC "e"

/*
 * ".xcursor" binary-format constants and structures (from Xcursor.h /
 * xcursorint.h).  Only the fields used by the loader are needed.
 */

#define XCURSOR_MAGIC		0x72756358  /* "Xcur" LSBFirst */

#define XCURSOR_FILE_MAJOR	1
#define XCURSOR_FILE_MINOR	0
#define XCURSOR_FILE_VERSION	((XCURSOR_FILE_MAJOR << 16) | (XCURSOR_FILE_MINOR))
#define XCURSOR_FILE_HEADER_LEN	(4 * 4)

#define XCURSOR_IMAGE_TYPE	0xfffd0002
#define XCURSOR_IMAGE_VERSION	1
#define XCURSOR_IMAGE_MAX_SIZE	0x7fff	/* 32767x32767 max cursor size */

typedef struct _XcursorFileToc {
    XcursorUInt	    type;	/* chunk type */
    XcursorUInt	    subtype;	/* subtype (size for images) */
    XcursorUInt	    position;	/* absolute position in file */
} XcursorFileToc;

typedef struct _XcursorFileHeader {
    XcursorUInt	    magic;	/* magic number */
    XcursorUInt	    header;	/* byte length of header */
    XcursorUInt	    version;	/* file version number */
    XcursorUInt	    ntoc;	/* number of toc entries */
    XcursorFileToc  *tocs;	/* table of contents */
} XcursorFileHeader;

typedef struct _XcursorChunkHeader {
    XcursorUInt	    header;	/* bytes in chunk header */
    XcursorUInt	    type;	/* chunk type */
    XcursorUInt	    subtype;	/* chunk subtype (size for images) */
    XcursorUInt	    version;	/* version of this type */
} XcursorChunkHeader;

static XcursorImage *
XcursorImageCreate (int width, int height)
{
    XcursorImage    *image = NULL;

    if (width < 0 || height < 0) {
       /* EMPTY */;
    } else if (width > XCURSOR_IMAGE_MAX_SIZE
    	    || height > XCURSOR_IMAGE_MAX_SIZE) {
       /* EMPTY */;
    } else {
	image = malloc (sizeof (XcursorImage) +
			(size_t) (width * height) * sizeof (XcursorPixel));
	if (image) {
	    image->version = XCURSOR_IMAGE_VERSION;
	    image->pixels  = (XcursorPixel *) (image + 1);
	    image->size	   = (XcursorDim) (width > height ? width : height);
	    image->width   = (XcursorDim) width;
	    image->height  = (XcursorDim) height;
	    image->delay   = 0;
	}
    }
    return image;
}

static void
XcursorImageDestroy (XcursorImage *image)
{
    free (image);
}

static XcursorImages *
XcursorImagesCreate (int size)
{
    XcursorImages   *images;

    images = malloc (sizeof (XcursorImages) +
		     (size_t) size * sizeof (XcursorImage *));
    if (images) {
	images->nimage = 0;
	images->images = (XcursorImage **) (images + 1);
	images->name = NULL;
    }
    return images;
}

void
XcursorImagesDestroy (XcursorImages *images)
{
    if (images) {
	int	n;

	for (n = 0; n < images->nimage; n++)
	    XcursorImageDestroy (images->images[n]);
	if (images->name)
	    free (images->name);
	free (images);
    }
}

static XcursorBool
_XcursorReadUInt (XcursorFile *file, XcursorUInt *u)
{
    unsigned char   bytes[4];

    if (!file || !u)
	return XcursorFalse;

    if ((*file->read) (file, bytes, 4) != 4)
	return XcursorFalse;

    *u = ((XcursorUInt)(bytes[0]) << 0) |
	 ((XcursorUInt)(bytes[1]) << 8) |
         ((XcursorUInt)(bytes[2]) << 16) |
         ((XcursorUInt)(bytes[3]) << 24);
    return XcursorTrue;
}

/* _XcursorReadBytes was only reachable from libXcursor's comment-loading
 * path, which is not part of this image-loading subset — dropped here. */

static void
_XcursorFileHeaderDestroy (XcursorFileHeader *fileHeader)
{
    free (fileHeader);
}

static XcursorFileHeader *
_XcursorFileHeaderCreate (XcursorUInt ntoc)
{
    XcursorFileHeader	*fileHeader;

    if (ntoc > 0x10000)
	return NULL;
    fileHeader = malloc (sizeof (XcursorFileHeader) +
			 ntoc * sizeof (XcursorFileToc));
    if (!fileHeader)
	return NULL;
    fileHeader->magic = XCURSOR_MAGIC;
    fileHeader->header = XCURSOR_FILE_HEADER_LEN;
    fileHeader->version = XCURSOR_FILE_VERSION;
    fileHeader->ntoc = ntoc;
    fileHeader->tocs = (XcursorFileToc *) (fileHeader + 1);
    return fileHeader;
}

static XcursorFileHeader *
_XcursorReadFileHeader (XcursorFile *file)
{
    XcursorFileHeader	head, *fileHeader;
    XcursorUInt		skip;
    XcursorUInt		n;

    if (!file)
        return NULL;

    if (!_XcursorReadUInt (file, &head.magic))
	return NULL;
    if (head.magic != XCURSOR_MAGIC)
	return NULL;
    if (!_XcursorReadUInt (file, &head.header))
	return NULL;
    if (head.header < XCURSOR_FILE_HEADER_LEN)
	return NULL;
    if (!_XcursorReadUInt (file, &head.version))
	return NULL;
    if (!_XcursorReadUInt (file, &head.ntoc))
	return NULL;
    skip = head.header - XCURSOR_FILE_HEADER_LEN;
    if (skip)
	if ((*file->seek) (file, skip, SEEK_CUR) == EOF)
	    return NULL;
    fileHeader = _XcursorFileHeaderCreate (head.ntoc);
    if (!fileHeader)
	return NULL;
    fileHeader->magic = head.magic;
    fileHeader->header = head.header;
    fileHeader->version = head.version;
    fileHeader->ntoc = head.ntoc;
    for (n = 0; n < fileHeader->ntoc; n++)
    {
	if (!_XcursorReadUInt (file, &fileHeader->tocs[n].type))
	    break;
	if (!_XcursorReadUInt (file, &fileHeader->tocs[n].subtype))
	    break;
	if (!_XcursorReadUInt (file, &fileHeader->tocs[n].position))
	    break;
    }
    if (n != fileHeader->ntoc)
    {
	_XcursorFileHeaderDestroy (fileHeader);
	return NULL;
    }
    return fileHeader;
}

static XcursorBool
_XcursorSeekToToc (XcursorFile		*file,
		   XcursorFileHeader	*fileHeader,
		   int			toc)
{
    if (!file || !fileHeader || \
        (*file->seek) (file, fileHeader->tocs[toc].position, SEEK_SET) == EOF)
	return XcursorFalse;
    return XcursorTrue;
}

static XcursorBool
_XcursorFileReadChunkHeader (XcursorFile	*file,
			     XcursorFileHeader	*fileHeader,
			     int		toc,
			     XcursorChunkHeader	*chunkHeader)
{
    if (!file || !fileHeader || !chunkHeader)
        return XcursorFalse;
    if (!_XcursorSeekToToc (file, fileHeader, toc))
	return XcursorFalse;
    if (!_XcursorReadUInt (file, &chunkHeader->header))
	return XcursorFalse;
    if (!_XcursorReadUInt (file, &chunkHeader->type))
	return XcursorFalse;
    if (!_XcursorReadUInt (file, &chunkHeader->subtype))
	return XcursorFalse;
    if (!_XcursorReadUInt (file, &chunkHeader->version))
	return XcursorFalse;
    /* sanity check */
    if (chunkHeader->type != fileHeader->tocs[toc].type ||
	chunkHeader->subtype != fileHeader->tocs[toc].subtype)
	return XcursorFalse;
    return XcursorTrue;
}

#define dist(a,b)   ((a) > (b) ? (a) - (b) : (b) - (a))

static XcursorDim
_XcursorFindBestSize (XcursorFileHeader *fileHeader,
		      XcursorDim	size,
		      int		*nsizesp)
{
    XcursorUInt	n;
    int		nsizes = 0;
    XcursorDim	bestSize = 0;
    XcursorDim	thisSize;

    if (!fileHeader || !nsizesp)
        return 0;

    for (n = 0; n < fileHeader->ntoc; n++)
    {
	if (fileHeader->tocs[n].type != XCURSOR_IMAGE_TYPE)
	    continue;
	thisSize = fileHeader->tocs[n].subtype;
	if (!bestSize || dist (thisSize, size) < dist (bestSize, size))
	{
	    bestSize = thisSize;
	    nsizes = 1;
	}
	else if (thisSize == bestSize)
	    nsizes++;
    }
    *nsizesp = nsizes;
    return bestSize;
}

static int
_XcursorFindImageToc (XcursorFileHeader	*fileHeader,
		      XcursorDim	size,
		      int		count)
{
    XcursorUInt		toc;
    XcursorDim		thisSize;

    if (!fileHeader)
        return 0;

    for (toc = 0; toc < fileHeader->ntoc; toc++)
    {
	if (fileHeader->tocs[toc].type != XCURSOR_IMAGE_TYPE)
	    continue;
	thisSize = fileHeader->tocs[toc].subtype;
	if (thisSize != size)
	    continue;
	if (!count)
	    break;
	count--;
    }
    if (toc == fileHeader->ntoc)
	return -1;
    return (int) toc;
}

static XcursorImage *
_XcursorReadImage (XcursorFile		*file,
		   XcursorFileHeader	*fileHeader,
		   int			toc)
{
    XcursorChunkHeader	chunkHeader;
    XcursorImage	head;
    XcursorImage	*image;
    int			n;
    XcursorPixel	*p;

    if (!file || !fileHeader)
        return NULL;

    if (!_XcursorFileReadChunkHeader (file, fileHeader, toc, &chunkHeader))
	return NULL;
    if (!_XcursorReadUInt (file, &head.width))
	return NULL;
    if (!_XcursorReadUInt (file, &head.height))
	return NULL;
    if (!_XcursorReadUInt (file, &head.xhot))
	return NULL;
    if (!_XcursorReadUInt (file, &head.yhot))
	return NULL;
    if (!_XcursorReadUInt (file, &head.delay))
	return NULL;
    /* sanity check data */
    if (head.width > XCURSOR_IMAGE_MAX_SIZE  ||
	head.height > XCURSOR_IMAGE_MAX_SIZE)
	return NULL;
    if (head.width == 0 || head.height == 0)
	return NULL;
    if (head.xhot > head.width || head.yhot > head.height)
	return NULL;

    /* Create the image and initialize it */
    image = XcursorImageCreate ((int) head.width, (int) head.height);
    if (image == NULL)
	return NULL;
    if (chunkHeader.version < image->version)
	image->version = chunkHeader.version;
    image->size = chunkHeader.subtype;
    image->xhot = head.xhot;
    image->yhot = head.yhot;
    image->delay = head.delay;
    n = (int) (image->width * image->height);
    p = image->pixels;
    while (n--)
    {
	if (!_XcursorReadUInt (file, p))
	{
	    XcursorImageDestroy (image);
	    return NULL;
	}
	p++;
    }
    return image;
}

static XcursorImage *
_XcursorResizeImage (XcursorImage *src, int size)
{
    XcursorDim dest_y, dest_x;
    double scale = (double) size / src->size;
    XcursorImage *dest;

    dest = XcursorImageCreate ((int) (src->width * scale),
			       (int) (src->height * scale));
    if (!dest)
	return NULL;

    dest->size = (XcursorDim) size;
    dest->xhot = (XcursorDim) (src->xhot * scale);
    dest->yhot = (XcursorDim) (src->yhot * scale);
    dest->delay = src->delay;

    for (dest_y = 0; dest_y < dest->height; dest_y++)
    {
	XcursorDim src_y = (XcursorDim) (dest_y / scale);
	XcursorPixel *src_row = src->pixels + (src_y * src->width);
	XcursorPixel *dest_row = dest->pixels + (dest_y * dest->width);
	for (dest_x = 0; dest_x < dest->width; dest_x++)
	{
	    XcursorDim src_x = (XcursorDim) (dest_x / scale);
	    dest_row[dest_x] = src_row[src_x];
	}
    }

    return dest;
}

static XcursorImages *
_XcursorXcFileLoadImages (XcursorFile *file, int size, XcursorBool resize)
{
    XcursorFileHeader	*fileHeader;
    XcursorDim		bestSize;
    int			nsize;
    XcursorImages	*images;
    int			n;
    XcursorImage        *image;

    if (!file || size < 0)
	return NULL;
    fileHeader = _XcursorReadFileHeader (file);
    if (!fileHeader)
	return NULL;
    bestSize = _XcursorFindBestSize (fileHeader, (XcursorDim) size, &nsize);
    if (!bestSize)
    {
        _XcursorFileHeaderDestroy (fileHeader);
	return NULL;
    }
    images = XcursorImagesCreate (nsize);
    if (!images)
    {
        _XcursorFileHeaderDestroy (fileHeader);
	return NULL;
    }
    for (n = 0; n < nsize; n++)
    {
	int toc = _XcursorFindImageToc (fileHeader, bestSize, n);
	if (toc < 0)
	    break;
	image = _XcursorReadImage (file, fileHeader, toc);
	if (!image)
	    break;
	if (resize && (image->size != (XcursorDim) size))
	{
	    XcursorImage *resized_image = _XcursorResizeImage (image, size);
	    XcursorImageDestroy (image);
	    image = resized_image;
	    if (image == NULL)
		break;
	}
	images->images[images->nimage] = image;
	images->nimage++;
    }
    _XcursorFileHeaderDestroy (fileHeader);
    if (images != NULL && images->nimage != nsize)
    {
	XcursorImagesDestroy (images);
	images = NULL;
    }
    return images;
}

static XcursorImages *
XcursorXcFileLoadImages (XcursorFile *file, int size)
{
    return _XcursorXcFileLoadImages (file, size, XcursorFalse);
}

static int
_XcursorStdioFileRead (XcursorFile *file, unsigned char *buf, int len)
{
    FILE    *f = file->closure;
    return (int) fread (buf, 1, (size_t) len, f);
}

static int
_XcursorStdioFileWrite (XcursorFile *file, unsigned char *buf, int len)
{
    FILE    *f = file->closure;
    return (int) fwrite (buf, 1, (size_t) len, f);
}

static int
_XcursorStdioFileSeek (XcursorFile *file, long offset, int whence)
{
    FILE    *f = file->closure;
    return fseek (f, offset, whence);
}

static void
_XcursorStdioFileInitialize (FILE *stdfile, XcursorFile *file)
{
    file->closure = stdfile;
    file->read = _XcursorStdioFileRead;
    file->write = _XcursorStdioFileWrite;
    file->seek = _XcursorStdioFileSeek;
}

static XcursorImages *
XcursorFileLoadImages (FILE *file, int size)
{
    XcursorFile	f;

    if (!file)
	return NULL;

    _XcursorStdioFileInitialize (file, &f);
    return XcursorXcFileLoadImages (&f, size);
}

XcursorImages *
XcursorFilenameLoadImages (const char *file, int size)
{
    FILE	    *f;
    XcursorImages   *images;

    if (!file || size < 0)
	return NULL;

    f = fopen (file, "r" FOPEN_CLOEXEC);
    if (!f)
	return NULL;
    images = XcursorFileLoadImages (f, size);
    fclose (f);
    return images;
}
