/* SPDX-FileCopyrightText: 2024 Thomas E. Dickey */
/* SPDX-FileCopyrightText: 2002 Keith Packard */
/* SPDX-License-Identifier: HPND-sell-variant */
/* Vendored subset of libXcursor 1.2.3 (X11-free, modified). */
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
 * xcursor-mini: an X11-free subset of libXcursor 1.2.3's ".xcursor" file
 * loader.  Only the public typedefs/structs and the two file-loading entry
 * points that drm-cxx consumes are exposed here.  See README.md.
 */

#ifndef XCURSOR_MINI_XCURSOR_H
#define XCURSOR_MINI_XCURSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int		XcursorBool;
typedef uint32_t	XcursorUInt;

typedef XcursorUInt	XcursorDim;
typedef XcursorUInt	XcursorPixel;

#define XcursorTrue	1
#define XcursorFalse	0

/*
 * A single cursor image (one size / one animation frame).  Field layout and
 * types match libXcursor's struct _XcursorImage exactly.
 */
typedef struct _XcursorImage {
    XcursorUInt	    version;	/* version of the image data */
    XcursorDim	    size;	/* nominal size for matching */
    XcursorDim	    width;	/* actual width */
    XcursorDim	    height;	/* actual height */
    XcursorDim	    xhot;	/* hot spot x (must be inside image) */
    XcursorDim	    yhot;	/* hot spot y (must be inside image) */
    XcursorUInt	    delay;	/* animation delay to next frame (ms) */
    XcursorPixel    *pixels;	/* pointer to pixels */
} XcursorImage;

/*
 * A set of cursor images.  Field layout and types match libXcursor's
 * struct _XcursorImages exactly.
 */
typedef struct _XcursorImages {
    int		    nimage;	/* number of images */
    XcursorImage    **images;	/* array of XcursorImage pointers */
    char	    *name;	/* name used to load images */
} XcursorImages;

/*
 * Abstract stream used by the loader.  Only read/seek are exercised by the
 * file-loading subset, but the full callback set is retained so the struct is
 * binary-compatible with upstream.
 */
typedef struct _XcursorFile XcursorFile;

struct _XcursorFile {
    void    *closure;
    int	    (*read)  (XcursorFile *file, unsigned char *buf, int len);
    int	    (*write) (XcursorFile *file, unsigned char *buf, int len);
    int	    (*seek)  (XcursorFile *file, long offset, int whence);
};

/*
 * Load all images of the best-matching size from a ".xcursor" file.
 * Returns NULL on any error; the result is owned by the caller and must be
 * freed with XcursorImagesDestroy().
 */
XcursorImages *XcursorFilenameLoadImages(const char *file, int size);

void XcursorImagesDestroy(XcursorImages *images);

#ifdef __cplusplus
}
#endif

#endif /* XCURSOR_MINI_XCURSOR_H */
