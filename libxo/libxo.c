/*
 * Copyright (c) 2014, Juniper Networks, Inc.
 * All rights reserved.
 * This SOFTWARE is licensed under the LICENSE provided in the
 * ../Copyright file. By downloading, installing, copying, or otherwise
 * using the SOFTWARE, you agree to be bound by the terms of that
 * LICENSE.
 * Phil Shafer, July 2014
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <alloca.h>

#include "libxo.h"

#ifndef UNUSED
#define UNUSED __attribute__ ((__unused__))
#endif /* UNUSED */

#define XO_INDENT_BY 2	/* Amount to indent when pretty printing */
#define XO_BUFSIZ	(8*1024) /* Initial buffer size */
#define XO_DEPTH	512	 /* Default stack depth */

/*
 * xo_buffer_t: a memory buffer that can be grown as needed.  We
 * use them for building format strings and output data.
 */
typedef struct xo_buffer_s {
    char *xb_bufp;		/* Buffer memory */
    char *xb_curp;		/* Current insertion point */
    int xb_size;		/* Size of buffer */
} xo_buffer_t;

/*
 * xo_stack_t: As we open and close containers and levels, we
 * create a stack of frames to track them.  This is needed for
 * XOF_WARN and XOF_XPATH.
 */
typedef struct xo_stack_s {
    unsigned xs_flags;		/* Flags for this frame */
    char *xs_name;		/* Name (for XPath value) */
} xo_stack_t;

/* Flags for xs_flags: */
#define XSF_NOT_FIRST	(1<<0)	/* Not the first element */
#define XSF_LIST	(1<<1)	/* Frame is a list */
#define XSF_INSTANCE	(1<<2)	/* Frame is an instance */

/*
 * xo_handle_t: this is the principle data structure for libxo.
 * It's used as a store for state, options, and content.
 */
struct xo_handle_s {
    unsigned short xo_style;	/* XO_STYLE_* value */
    unsigned short xo_flags;	/* Flags */
    unsigned short xo_indent;	/* Indent level (if pretty) */
    unsigned short xo_indent_by; /* Indent amount (tab stop) */
    xo_write_func_t xo_write;	/* Write callback */
    xo_close_func_t xo_close;	/* Close callback */
    xo_formatter_t xo_formatter; /* Custom formating function */
    void *xo_opaque;		/* Opaque data for write function */
    FILE *xo_fp;		/* XXX File pointer */
    xo_buffer_t xo_data;	/* Output data */
    xo_buffer_t xo_fmt;		/* Work area for building format strings */
    xo_stack_t *xo_stack;	/* Stack pointer */
    int xo_depth;		/* Depth of stack */
    int xo_stack_size;		/* Size of the stack */
    xo_info_t *xo_info;		/* Info fields for all elements */
    int xo_info_count;		/* Number of info entries */
};

/* Flags for formatting functions */
#define XFF_COLON	(1<<0)	/* Append a ":" */
#define XFF_COMMA	(1<<1)	/* Append a "," iff there's more output */
#define XFF_WS		(1<<2)	/* Append a blank */
#define XFF_HIDE	(1<<3)	/* Hide this from text output */
#define XFF_QUOTE	(1<<4)	/* Force quotes */
#define XFF_NOQUOTE	(1<<5)	/* Force no quotes */

/*
 * We keep a default handle to allow callers to avoid having to
 * allocate one.  Passing NULL to any of our functions will use
 * this default handle.
 */
static xo_handle_t xo_default_handle;
static int xo_default_inited;

/*
 * To allow libxo to be used in diverse environment, we allow the
 * caller to give callbacks for memory allocation.
 */
static xo_realloc_func_t xo_realloc = realloc;
static xo_free_func_t xo_free = free;

/*
 * Callback to write data to a FILE pointer
 */
static int
xo_write_to_file (void *opaque, const char *data)
{
    FILE *fp = (FILE *) opaque;
    return fprintf(fp, "%s", data);
}

/*
 * Callback to close a file
 */
static void
xo_close_file (void *opaque)
{
    FILE *fp = (FILE *) opaque;
    fclose(fp);
}

/*
 * Initialize the contents of an xo_buffer_t.
 */
static void
xo_buf_init (xo_buffer_t *xbp)
{
    xbp->xb_size = XO_BUFSIZ;
    xbp->xb_bufp = xo_realloc(NULL, xbp->xb_size);
    xbp->xb_curp = xbp->xb_bufp;
}

/*
 * Initialize an xo_handle_t, using both static defaults and
 * the global settings from the LIBXO_OPTIONS environment
 * variable.
 */
static void
xo_init_handle (xo_handle_t *xop)
{
    xop->xo_opaque = stdout;
    xop->xo_write = xo_write_to_file;

    xo_buf_init(&xop->xo_data);
    xo_buf_init(&xop->xo_fmt);

    xop->xo_indent_by = XO_INDENT_BY;
    xop->xo_stack_size = XO_DEPTH;
    xop->xo_stack = xo_realloc(NULL,
			 sizeof(xop->xo_stack[0]) * xop->xo_stack_size);

#if !defined(NO_LIBXO_OPTIONS)
    char *env = getenv("LIBXO_OPTIONS");
    if (env) {
	int sz;

	for ( ; *env; env++) {
	    switch (*env) {
	    case 'H':
		xop->xo_style = XO_STYLE_HTML;
		break;

	    case 'I':
		xop->xo_flags |= XOF_INFO;
		break;

	    case 'i':
		sz = strspn(env + 1, "0123456789");
		if (sz > 0) {
		    xop->xo_indent_by = atoi(env + 1);
		    env += sz - 1;	/* Skip value */
		}
		break;

	    case 'J':
		xop->xo_style = XO_STYLE_JSON;
		break;

	    case 'P':
		xop->xo_flags |= XOF_PRETTY;
		break;

	    case 'T':
		xop->xo_style = XO_STYLE_TEXT;
		break;

	    case 'W':
		xop->xo_flags |= XOF_WARN;
		break;

	    case 'X':
		xop->xo_style = XO_STYLE_XML;
		break;

	    case 'x':
		xop->xo_flags |= XOF_XPATH;
		break;
	    }
	}
    }
#endif /* NO_GETENV */
}

/*
 * Initialize the default handle.
 */
static void
xo_default_init (void)
{
    xo_handle_t *xop = &xo_default_handle;

    xo_init_handle(xop);

    xo_default_inited = 1;
}

/*
 * Does the buffer have room for the given number of bytes of data?
 * If not, realloc the buffer to make room.  If that fails, we
 * return 0 to tell the caller they are in trouble.
 */
static int
xo_buf_has_room (xo_buffer_t *xbp, int len)
{
    if (xbp->xb_curp + len >= xbp->xb_bufp + xbp->xb_size) {
	int sz = xbp->xb_size + XO_BUFSIZ;
	char *bp = xo_realloc(xbp->xb_bufp, sz);
	if (bp == NULL)
	    return 0;
	xbp->xb_bufp = bp;
	xbp->xb_size = sz;
    }

    return 1;
}

/*
 * Print some data thru the handle.
 */
static int
xo_printf (xo_handle_t *xop, const char *fmt, ...)
{
    xo_buffer_t *xbp = &xop->xo_data;
    int left = xbp->xb_size - (xbp->xb_curp - xbp->xb_bufp);
    int rc;
    va_list vap;

    va_start(vap, fmt);
    rc = vsnprintf(xbp->xb_curp, left, fmt, vap);
    if (rc > xbp->xb_size) {
	if (!xo_buf_has_room(xbp, rc))
	    return -1;
	rc = vsnprintf(xbp->xb_curp, left, fmt, vap);
    }

    xbp->xb_curp = xbp->xb_bufp;
    rc = xop->xo_write(xop->xo_opaque, xbp->xb_bufp);

    va_end(vap);

    return rc;
}

/*
 * Append the given string to the given buffer
 */
static void
xo_buf_append (xo_buffer_t *xbp, const char *str, int len)
{
    if (!xo_buf_has_room(xbp, len))
	return;

    memcpy(xbp->xb_curp, str, len);
    xbp->xb_curp += len;
}

/*
 * Cheap convenience function to return either the argument, or
 * the internal handle, after it has been initialized.  The usage
 * is:
 *    xop = xo_default(xop);
 */
static xo_handle_t *
xo_default (xo_handle_t *xop)
{
    if (xop == NULL) {
	if (xo_default_inited == 0)
	    xo_default_init();
	xop = &xo_default_handle;
    }

    return xop;
}

/*
 * Return the number of spaces we should be indenting.  If
 * we are pretty-printing, theis is indent * indent_by.
 */
static int
xo_indent (xo_handle_t *xop)
{
    xop = xo_default(xop);

    if (xop->xo_flags & XOF_PRETTY)
	return xop->xo_indent * xop->xo_indent_by;

    return 0;
}

/*
 * Generate a warning.  Normally, this is a text message written to
 * standard error.  If the XOF_WARN_XML flag is set, then we generate
 * XMLified content on standard output.
 */
static void
xo_warn (xo_handle_t *xop, const char *fmt, ...)
{
    va_list vap;
    int len = strlen(fmt);
    char *newfmt = alloca(len + 2);

    memcpy(newfmt, fmt, len);	/* Add a newline to the fmt string */
    newfmt[len] = '\n';
    newfmt[len + 1] = '\0';

    va_start(vap, fmt);
    if (xop->xo_flags & XOF_WARN_XML) {
	vfprintf(stderr, newfmt, vap);
    } else {
	vfprintf(stderr, newfmt, vap);
    }

    va_end(vap);
}

/**
 * Create a handle for use by later libxo functions.
 *
 * Note: normal use of libxo does not require a distinct handle, since
 * the default handle (used when NULL is passed) generates text on stdout.
 *
 * @style Style of output desired (XO_STYLE_* value)
 * @flags Set of XOF_* flags in use with this handle
 */
xo_handle_t *
xo_create (unsigned style, unsigned flags)
{
    xo_handle_t *xop = xo_realloc(NULL, sizeof(*xop));

    if (xop) {
	xop->xo_style  = style;
	xop->xo_flags = flags;
	xo_init_handle(xop);
    }

    return xop;
}

xo_handle_t *
xo_create_to_file (FILE *fp, unsigned style, unsigned flags)
{
    xo_handle_t *xop = xo_create(style, flags);

    if (xop) {
	xop->xo_opaque = fp;
	xop->xo_write = xo_write_to_file;
	xop->xo_close = xo_close_file;
    }

    return xop;
}

void
xo_destroy (xo_handle_t *xop)
{
    xop = xo_default(xop);

    if (xop->xo_close && (xop->xo_flags & XOF_CLOSE_FP))
	xop->xo_close(xop->xo_opaque);

    xo_free(xop->xo_stack);
    xo_free(xop->xo_data.xb_bufp);
    xo_free(xop->xo_fmt.xb_bufp);

    if (xop == &xo_default_handle) {
	bzero(&xo_default_handle, sizeof(&xo_default_handle));
	xo_default_inited = 0;
    } else
	xo_free(xop);
}

/**
 * Record a new output style to use for the given handle (or default if
 * handle is NULL).  This output style will be used for any future output.
 *
 * @xop XO handle to alter (or NULL for default handle)
 * @style new output style (XO_STYLE_*)
 */
void
xo_set_style (xo_handle_t *xop, unsigned style)
{
    xop = xo_default(xop);
    xop->xo_style = style;
}

/**
 * Set one or more flags for a given handle (or default if handle is NULL).
 * These flags will affect future output.
 *
 * @xop XO handle to alter (or NULL for default handle)
 * @flags Flags to be set (XOF_*)
 */
void
xo_set_flags (xo_handle_t *xop, unsigned flags)
{
    xop = xo_default(xop);

    xop->xo_flags |= flags;
}

/**
 * Record the info data for a set of tags
 *
 * @xop XO handle to alter (or NULL for default handle)
 * @info Info data (xo_info_t) to be recorded (or NULL) (MUST BE SORTED)
 * @count Number of entries in info (or -1 to count them ourselves)
 */
void
xo_set_info (xo_handle_t *xop, xo_info_t *infop, int count)
{
    xop = xo_default(xop);

    if (count < 0 && infop) {
	xo_info_t *xip;

	for (xip = infop, count = 0; xip->xi_name; xip++, count++)
	    continue;
    }

    xop->xo_info = infop;
    xop->xo_info_count = count;
}

/**
 * Set the formatter callback for a handle.  The callback should return
 * a newly formatting contents of a formatting instruction, meanly the
 bits inside the braces.  
 */
void
xo_set_formatter (xo_handle_t *xop, xo_formatter_t func)
{
    xop = xo_default(xop);

    xop->xo_formatter = func;
}

/**
 * Clear one or more flags for a given handle (or default if handle is NULL).
 * These flags will affect future output.
 *
 * @xop XO handle to alter (or NULL for default handle)
 * @flags Flags to be cleared (XOF_*)
 */
void
xo_clear_flags (xo_handle_t *xop, unsigned flags)
{
    xop = xo_default(xop);

    xop->xo_flags &= ~flags;
}

static void
xo_buf_indent (xo_handle_t *xop, int indent)
{
    xo_buffer_t *xbp = &xop->xo_fmt;

    if (indent <= 0)
	indent = xo_indent(xop);

    if (!xo_buf_has_room(xbp, indent))
	return;

    memset(xbp->xb_curp, ' ', indent);
    xbp->xb_curp += indent;
}

static void
xo_line_ensure_open (xo_handle_t *xop, unsigned flags UNUSED)
{
    static char div_open[] = "<div class=\"line\">";

    if (xop->xo_flags & XOF_DIV_OPEN)
	return;

    if (xop->xo_style != XO_STYLE_HTML)
	return;

    xop->xo_flags |= XOF_DIV_OPEN;
    xo_buf_append(&xop->xo_fmt, div_open, sizeof(div_open) - 1);

    if (xop->xo_flags & XOF_PRETTY)
	xo_buf_append(&xop->xo_fmt, "\n", 1);
}

static void
xo_line_close (xo_handle_t *xop)
{
    static char div_close[] = "</div>";

    switch (xop->xo_style) {
    case XO_STYLE_HTML:
	if (!(xop->xo_flags & XOF_DIV_OPEN))
	    xo_line_ensure_open(xop, 0);

	xop->xo_flags &= ~XOF_DIV_OPEN;
	xo_buf_append(&xop->xo_fmt, div_close, sizeof(div_close) - 1);

	if (xop->xo_flags & XOF_PRETTY)
	    xo_buf_append(&xop->xo_fmt, "\n", 1);
	break;

    case XO_STYLE_TEXT:
	xo_buf_append(&xop->xo_fmt, "\n", 1);
	break;
    }
}

static void
xo_buf_append_escaped (xo_buffer_t *xbp, const char *str, int len)
{
    /* XXX Complete cheat for now.... */
    xo_buf_append(xbp, str, len);
}

static int
xo_info_compare (const void *key, const void *data)
{
    const char *name = key;
    const xo_info_t *xip = data;

    return strcmp(name, xip->xi_name);
}

static xo_info_t *
xo_info_find (xo_handle_t *xop, const char *name, int nlen)
{
    xo_info_t *xip;
    char *cp = alloca(nlen + 1); /* Need local copy for NUL termination */

    memcpy(cp, name, nlen);
    cp[nlen] = '\0';

    xip = bsearch(cp, xop->xo_info, xop->xo_info_count,
		  sizeof(xop->xo_info[0]), xo_info_compare);
    return xip;
}

static void
xo_buf_append_div (xo_handle_t *xop, const char *class,
		   const char *name, int nlen,
		   const char *value, int vlen)
{
    static char div1[] = "<div class=\"";
    static char div2[] = "\" data-tag=\"";
    static char div3[] = "\" data-xpath=\"";
    static char div4[] = "\">";
    static char div5[] = "</div>";

    xo_buffer_t *xbp = &xop->xo_fmt;

    xo_line_ensure_open(xop, 0);

    if (xop->xo_flags & XOF_PRETTY)
	xo_buf_indent(xop, xop->xo_indent_by);

    xo_buf_append(xbp, div1, sizeof(div1) - 1);
    xo_buf_append(xbp, class, strlen(class));

    if (name) {
	xo_buf_append(xbp, div2, sizeof(div2) - 1);
	xo_buf_append(xbp, name, nlen);
    }

    if (name && (xop->xo_flags & XOF_XPATH)) {
	int i;
	xo_stack_t *xsp;

	xo_buf_append(xbp, div3, sizeof(div3) - 1);
	for (i = 0; i <= xop->xo_depth; i++) {
	    xsp = &xop->xo_stack[i];
	    if (xsp->xs_name == NULL)
		continue;

	    xo_buf_append(xbp, "/", 1);
	    xo_buf_append(xbp, xsp->xs_name, strlen(xsp->xs_name));
	}

	xo_buf_append(xbp, "/", 1);
	xo_buf_append(xbp, name, nlen);
    }

    if (name && (xop->xo_flags & XOF_INFO) && xop->xo_info) {
	static char in_type[] = "\" data-type=\"";
	static char in_help[] = "\" data-help=\"";

	xo_info_t *xip = xo_info_find(xop, name, nlen);
	if (xip) {
	    if (xip->xi_type) {
		xo_buf_append(xbp, in_type, sizeof(in_type) - 1);
		xo_buf_append_escaped(xbp, xip->xi_type, strlen(xip->xi_type));
	    }
	    if (xip->xi_help) {
		xo_buf_append(xbp, in_help, sizeof(in_help) - 1);
		xo_buf_append_escaped(xbp, xip->xi_help, strlen(xip->xi_help));
	    }
	}
    }

    xo_buf_append(xbp, div4, sizeof(div4) - 1);
    xo_buf_append_escaped(xbp, value, vlen);
    xo_buf_append(xbp, div5, sizeof(div5) - 1);

    if (xop->xo_flags & XOF_PRETTY)
	xo_buf_append(xbp, "\n", 1);
}

static void
xo_format_text (xo_handle_t *xop, const char *str, int len)
{
    switch (xop->xo_style) {
    case XO_STYLE_TEXT:
	xo_buf_append(&xop->xo_fmt, str, len);
	break;

    case XO_STYLE_HTML:
	xo_buf_append_div(xop, "text", NULL, 0, str, len);
	break;
    }
}

static void
xo_format_label (xo_handle_t *xop, const char *str, int len)
{
    switch (xop->xo_style) {
    case XO_STYLE_TEXT:
	xo_buf_append(&xop->xo_fmt, str, len);
	break;

    case XO_STYLE_HTML:
	xo_buf_append_div(xop, "label", NULL, 0, str, len);
	break;
    }
}

static void
xo_format_title (xo_handle_t *xop, const char *str, int len,
		 const char *fmt, int flen)
{
    static char div_open[] = "<div class=\"title\">";
    static char div_close[] = "</div>";

    if (xop->xo_style != XO_STYLE_TEXT && xop->xo_style != XO_STYLE_HTML)
	return;

    xo_buffer_t *xbp = &xop->xo_fmt;
    int left = xbp->xb_size - (xbp->xb_curp - xbp->xb_bufp);
    int rc;

    char *newstr = alloca(len + 1);
    memcpy(newstr, str, len);
    newstr[len] = '\0';

    char *newfmt = alloca(flen + 1);
    memcpy(newfmt, fmt, flen);
    newfmt[flen] = '\0';

    if (xop->xo_style == XO_STYLE_HTML) {
	xo_line_ensure_open(xop, 0);
	if (xop->xo_flags & XOF_PRETTY)
	    xo_buf_indent(xop, xop->xo_indent_by);
	xo_buf_append(xbp, div_open, sizeof(div_open) - 1);
    }
	
    rc = snprintf(xbp->xb_curp, left, newfmt, newstr);
    if (rc > left) {
	if (!xo_buf_has_room(xbp, rc))
	    return;
	rc = snprintf(xbp->xb_curp, left, newfmt, newstr);
    }
    xbp->xb_curp += rc;

    if (xop->xo_style == XO_STYLE_HTML) {
	xo_buf_append(xbp, div_close, sizeof(div_close) - 1);
	if (xop->xo_flags & XOF_PRETTY)
	    xo_buf_append(xbp, "\n", 1);
    }
}

static void
xo_format_prep (xo_handle_t *xop)
{
    if (xop->xo_stack[xop->xo_depth].xs_flags & XSF_NOT_FIRST) {
	xo_buf_append(&xop->xo_fmt, ",", 1);
	if (xop->xo_flags & XOF_PRETTY)
	    xo_buf_append(&xop->xo_fmt, "\n", 1);
    } else
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;
}

static void
xo_format_value (xo_handle_t *xop, const char *name, int nlen, 
		 const char *format, int flen,
		 const char *encoding, int elen, unsigned flags)
{
    int pretty = (xop->xo_flags & XOF_PRETTY);
    int quote;

    switch (xop->xo_style) {
    case XO_STYLE_TEXT:
	xo_buf_append(&xop->xo_fmt, format, flen);
	break;

    case XO_STYLE_HTML:
	xo_buf_append_div(xop, "data", name, nlen, format, flen);
	break;

    case XO_STYLE_XML:
	if (encoding) {
	    format = encoding;
	    flen = elen;
	}

	if (pretty)
	    xo_buf_indent(xop, -1);
	xo_buf_append(&xop->xo_fmt, "<", 1);
	xo_buf_append(&xop->xo_fmt, name, nlen);
	xo_buf_append(&xop->xo_fmt, ">", 1);
	xo_buf_append(&xop->xo_fmt, format, flen);
	xo_buf_append(&xop->xo_fmt, "</", 2);
	xo_buf_append(&xop->xo_fmt, name, nlen);
	xo_buf_append(&xop->xo_fmt, ">", 1);
	if (pretty)
	    xo_buf_append(&xop->xo_fmt, "\n", 1);
	break;

    case XO_STYLE_JSON:
	if (encoding) {
	    format = encoding;
	    flen = elen;
	}

	xo_format_prep(xop);

	if (flags & XFF_QUOTE)
	    quote = 1;
	else if (flags & XFF_NOQUOTE)
	    quote = 0;
	else if (format[flen - 1] == 's')
	    quote = 1;
	else
	    quote = 0;

	if (pretty)
	    xo_buf_indent(xop, -1);
	xo_buf_append(&xop->xo_fmt, "\"", 1);
	xo_buf_append(&xop->xo_fmt, name, nlen);
	xo_buf_append(&xop->xo_fmt, "\":", 2);

	if (pretty)
	    xo_buf_append(&xop->xo_fmt, " ", 1);
	if (quote)
	    xo_buf_append(&xop->xo_fmt, "\"", 1);

	xo_buf_append(&xop->xo_fmt, format, flen);

	if (quote)
	    xo_buf_append(&xop->xo_fmt, "\"", 1);
	break;
    }
}

static void
xo_format_decoration (xo_handle_t *xop, const char *str, int len)
{
    switch (xop->xo_style) {
    case XO_STYLE_TEXT:
	xo_buf_append(&xop->xo_fmt, str, len);
	break;

    case XO_STYLE_HTML:
	xo_buf_append_div(xop, "decoration", NULL, 0, str, len);
	break;
    }
}

static void
xo_format_padding (xo_handle_t *xop, const char *str, int len)
{
    switch (xop->xo_style) {
    case XO_STYLE_TEXT:
	xo_buf_append(&xop->xo_fmt, str, len);
	break;

    case XO_STYLE_HTML:
	xo_buf_append_div(xop, "padding", NULL, 0, str, len);
	break;
    }
}

int
xo_emit_hv (xo_handle_t *xop, const char *fmt, va_list vap)
{
    xop = xo_default(xop);

    int rc = 0;
    const char *cp, *sp, *ep, *basep;
    char *newp = NULL;

    for (cp = fmt; *cp; ) {
	if (*cp == '\n') {
	    xo_line_close(xop);
	    cp += 1;
	    continue;

	} else if (*cp == '{') {
	    if (cp[1] == '{') {	/* Start of {{escaped braces}} */

		cp += 2;	/* Skip over _both_ characters */
		for (sp = cp; *sp; sp++) {
		    if (*sp == '}' && sp[1] == '}')
			break;
		}

		xo_format_text(xop, cp, sp - cp);

		/* Move along the string, but don't run off the end */
		if (*sp == '}' && sp[1] == '}')
		    sp += 2;
		cp = *sp ? sp + 1 : sp;
		continue;
	    }
	    /* Else fall thru to the code below */

	} else {
	    /* Normal text */
	    for (sp = cp; *sp; sp++) {
		if (*sp == '{' || *sp == '\n')
		    break;
	    }
	    xo_format_text(xop, cp, sp - cp);

	    cp = sp;
	    continue;
	}

	/*
	 * A customer formatter gives the caller a pre-format
	 * hook for changing data before it gets processed.
	 */
	basep = cp + 1;
	if (xop->xo_formatter) {
	    for (ep = basep; *ep; ep++) {
		if (*ep == '}')
		    break;
	    }

	    int tlen = ep - cp;
	    char *tmp = alloca(tlen + 1);
	    memcpy(tmp, cp + 1, tlen);
	    tmp[tlen] = '\0';

	    newp = xop->xo_formatter(xop, tmp);
	    if (newp) {
		basep = newp;
	    }
	}

	/*
	 * We are looking at the start of a braces pattern.  The format is:
	 *  '{' modifiers ':' content [ '/' print-fmt [ '/' encode-fmt ]] '}'
	 * Modifiers are optional, but are:
	 *   'D': decoration; something non-text and non-data (colons, commmas)
	 *   'L': label; text surrounding data
	 *   'P': padding; whitespace
	 *   'T': Title, where 'content' is a column title
	 *   'V': value, where 'content' is the name of the field
	 *   'C': flag: emit a colon after the label
	 *   'W': emit a blank after the label
	 *   'H': field is hidden from text output
	 * The print-fmt and encode-fmt strings is the printf-style formating
	 * for this data.  JSON and XML will use the encoding-fmt, if present.
	 * If the encode-fmt is not provided, it defaults to the print-fmt.
	 * If the print-fmt is not provided, it defaults to 's'.
	 */
	unsigned style = 0, flags = 0;
	const char *content = NULL, *format = NULL, *encoding = NULL;
	int clen = 0, flen = 0, elen = 0;

	for (sp = basep; sp; sp++) {
	    if (*sp == ':' || *sp == '/' || *sp == '}')
		break;

	    switch (*sp) {
	    case 'D':
	    case 'L':
	    case 'P':
	    case 'T':
	    case 'V':
		if ((xop->xo_flags & XOF_WARN) && style != 0)
		    xo_warn(xop, "format string uses multiple styles: %s",
			    fmt);
		style = *sp;
		break;

	    case 'C':
		flags |= XFF_COLON;
		break;

	    case 'H':
		flags |= XFF_HIDE;
		break;

	    case 'N':
		flags |= XFF_NOQUOTE;
		break;

	    case 'Q':
		flags |= XFF_QUOTE;
		break;

	    case 'W':
		flags |= XFF_WS;
		break;

	    default:
		if (xop->xo_flags & XOF_WARN)
		    xo_warn(xop, "format string uses unknown modifier: %s",
			    fmt);
	    }
	}

	if (*sp == ':') {
	    for (ep = ++sp; *sp; sp++) {
		if (*sp == '}' || *sp == '/')
		    break;
	    }
	    if (ep != sp) {
		clen = sp - ep;
		content = ep;
	    }
	}

	if (*sp == '/') {
	    for (ep = ++sp; *sp; sp++) {
		if (*sp == '}' || *sp == '/')
		    break;
	    }
	    if (ep != sp) {
		flen = sp - ep;
		format = ep;
	    }
	}


	if (*sp == '/') {
	    for (ep = ++sp; *sp; sp++) {
		if (*sp == '}')
		    break;
	    }
	    if (ep != sp) {
		elen = sp - ep;
		encoding = ep;
	    }
	}

	if (*sp == '}') {
	    sp += 1;
	}

	if (format == NULL) {
	    format = "%s";
	    flen = 2;
	}

	if (style == 'T')
	    xo_format_title(xop, content, clen, format, flen);
	else if (style == 'L')
	    xo_format_label(xop, content, clen);
	else if (style == 0 || style == 'V')
	    xo_format_value(xop, content, clen, format, flen,
			    encoding, elen, flags);
	else if (style == 'D')
	    xo_format_decoration(xop, content, clen);
	else if (style == 'P')
 	    xo_format_padding(xop, content, clen);

	if (flags & XFF_COLON)
	    xo_format_decoration(xop, ":", 1);
	if (flags & XFF_WS)
	    xo_format_padding(xop, " ", 1);

	cp += sp - basep + 1;
	if (newp) {
	    xo_free(newp);
	    newp = NULL;
	}
    }

    xo_buf_append(&xop->xo_fmt, "", 1); /* Append ending NUL */

    rc = vsnprintf(xop->xo_data.xb_bufp, xop->xo_data.xb_size,
		   xop->xo_fmt.xb_bufp, vap);
    xop->xo_fmt.xb_curp = xop->xo_fmt.xb_bufp;
    if (rc > xop->xo_data.xb_size) {
	if (!xo_buf_has_room(&xop->xo_data, rc))
	    return -1;
	rc = vsnprintf(xop->xo_data.xb_bufp, xop->xo_data.xb_size,
		       xop->xo_fmt.xb_bufp, vap);
    }

    xop->xo_write(xop->xo_opaque, xop->xo_data.xb_bufp);

    return rc;
}

int
xo_emit_h (xo_handle_t *xop, const char *fmt, ...)
{
    va_list vap;
    int rc;

    va_start(vap, fmt);
    rc = xo_emit_hv(xop, fmt, vap);
    va_end(vap);

    return rc;
}

int
xo_emit (const char *fmt, ...)
{
    va_list vap;
    int rc;

    va_start(vap, fmt);
    rc = xo_emit_hv(NULL, fmt, vap);
    va_end(vap);

    return rc;
}

static void
xo_depth_change (xo_handle_t *xop, const char *name,
		 int delta, int indent, unsigned flags)
{
    xo_stack_t *xsp = &xop->xo_stack[xop->xo_depth];

    if (delta >= 0) {			/* Push operation */
	xsp += delta;
	xsp->xs_flags = flags;

	if (name && (xop->xo_flags & (XOF_XPATH | XOF_WARN))) {
	    int len = strlen(name) + 1;
	    char *cp = xo_realloc(NULL, len);
	    if (cp) {
		memcpy(cp, name, len);
		xsp->xs_name = cp;
	    }
	}

    } else {			/* Pop operation */
	if (xop->xo_depth == 0) {
	    if (xop->xo_flags & XOF_WARN)
		xo_warn(xop, "xo: close with empty stack: '%s'", name);
	    return;
	}

	if (xop->xo_flags & XOF_WARN) {
	    const char *top = xsp->xs_name;
	    if (top && strcmp(name, top) != 0)
		xo_warn(xop, "xo: incorrect close: '%s' .vs. '%s'", name, top);
	    if ((xsp->xs_flags & XSF_LIST) != (flags & XSF_LIST))
		xo_warn(xop, "xo: list close on list confict: '%s'", name);
	    if ((xsp->xs_flags & XSF_INSTANCE) != (flags & XSF_INSTANCE))
		xo_warn(xop, "xo: list close on instance confict: '%s'", name);
	}

	if (xop->xo_flags & XOF_XPATH) {
	    if (xsp->xs_name) {
		xo_free(xsp->xs_name);
		xsp->xs_name = NULL;
	    }
	}
    }

    xop->xo_depth += delta;	/* Record new depth */
    xop->xo_indent += indent;
}

int
xo_open_container_h (xo_handle_t *xop, const char *name)
{
    xop = xo_default(xop);

    int rc = 0;
    const char *ppn = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
    const char *pre_nl = "";

    switch (xop->xo_style) {
    case XO_STYLE_XML:
	rc = xo_printf(xop, "%*s<%s>%s", xo_indent(xop), "",
		     name, ppn);
	xo_depth_change(xop, name, 1, 1, 0);
	break;

    case XO_STYLE_JSON:
	if (xop->xo_stack[xop->xo_depth].xs_flags & XSF_NOT_FIRST)
	    pre_nl = (xop->xo_flags & XOF_PRETTY) ? ",\n" : ", ";
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;

	rc = xo_printf(xop, "%s%*s\"%s\": {%s",
		       pre_nl, xo_indent(xop), "", name, ppn);
	xo_depth_change(xop, name, 1, 1, 0);
	break;

    case XO_STYLE_HTML:
    case XO_STYLE_TEXT:
	xo_depth_change(xop, name, 1, 0, 0);
	break;
    }

    return rc;
}

int
xo_open_container (const char *name)
{
    return xo_open_container_h(NULL, name);
}

int
xo_close_container_h (xo_handle_t *xop, const char *name)
{
    xop = xo_default(xop);

    int rc = 0;
    const char *ppn = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
    const char *pre_nl = "";

    switch (xop->xo_style) {
    case XO_STYLE_XML:
	xo_depth_change(xop, name, -1, -1, 0);
	rc = xo_printf(xop, "%*s</%s>%s", xo_indent(xop), "", name, ppn);
	break;

    case XO_STYLE_JSON:
	pre_nl = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
	ppn = (xop->xo_depth <= 1) ? "\n" : "";

	xo_depth_change(xop, name, -1, -1, 0);
	rc = xo_printf(xop, "%s%*s}%s", pre_nl, xo_indent(xop), "", ppn);
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;
	break;

    case XO_STYLE_HTML:
    case XO_STYLE_TEXT:
	xo_depth_change(xop, name, -1, 0, 0);
	break;
    }

    return rc;
}

int
xo_close_container (const char *name)
{
    return xo_close_container_h(NULL, name);
}

int
xo_open_list_h (xo_handle_t *xop, const char *name UNUSED)
{
    xop = xo_default(xop);

    int rc = 0;
    const char *ppn = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
    const char *pre_nl = "";

    switch (xop->xo_style) {
    case XO_STYLE_JSON:
	if (xop->xo_stack[xop->xo_depth].xs_flags & XSF_NOT_FIRST)
	    pre_nl = (xop->xo_flags & XOF_PRETTY) ? ",\n" : ", ";
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;

	rc = xo_printf(xop, "%s%*s\"%s\": [%s",
		       pre_nl, xo_indent(xop), "", name, ppn);
	xo_depth_change(xop, name, 1, 1, XSF_LIST);
	break;
    }

    return rc;
}

int
xo_open_list (const char *name)
{
    return xo_open_list_h(NULL, name);
}

int
xo_close_list_h (xo_handle_t *xop, const char *name UNUSED)
{
    int rc = 0;
    const char *pre_nl = "";

    xop = xo_default(xop);

    switch (xop->xo_style) {
    case XO_STYLE_JSON:
	if (xop->xo_stack[xop->xo_depth].xs_flags & XSF_NOT_FIRST)
	    pre_nl = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;

	xo_depth_change(xop, name, -1, -1, XSF_LIST);
	rc = xo_printf(xop, "%s%*s]", pre_nl, xo_indent(xop), "");
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;
	break;
    }

    return 0;
}

int
xo_close_list (const char *name)
{
    return xo_close_list_h(NULL, name);
}

int
xo_open_instance_h (xo_handle_t *xop, const char *name)
{
    xop = xo_default(xop);

    int rc = 0;
    const char *ppn = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
    const char *pre_nl = "";

    switch (xop->xo_style) {
    case XO_STYLE_XML:
	rc = xo_printf(xop, "%*s<%s>%s", xo_indent(xop), "", name, ppn);
	xo_depth_change(xop, name, 1, 1, 0);
	break;

    case XO_STYLE_JSON:
	if (xop->xo_stack[xop->xo_depth].xs_flags & XSF_NOT_FIRST)
	    pre_nl = (xop->xo_flags & XOF_PRETTY) ? ",\n" : ", ";
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;

	rc = xo_printf(xop, "%s%*s{%s",
		       pre_nl, xo_indent(xop), "", ppn);
	xo_depth_change(xop, name, 1, 1, 0);
	break;

    case XO_STYLE_HTML:
    case XO_STYLE_TEXT:
	xo_depth_change(xop, name, 1, 0, 0);
	break;
    }

    return rc;
}

int
xo_open_instance (const char *name)
{
    return xo_open_instance_h(NULL, name);
}

int
xo_close_instance_h (xo_handle_t *xop, const char *name)
{
    xop = xo_default(xop);

    int rc = 0;
    const char *ppn = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
    const char *pre_nl = "";

    switch (xop->xo_style) {
    case XO_STYLE_XML:
	xo_depth_change(xop, name, -1, -1, 0);
	rc = xo_printf(xop, "%*s</%s>%s", xo_indent(xop), "", name, ppn);
	break;

    case XO_STYLE_JSON:
	pre_nl = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";

	xo_depth_change(xop, name, -1, -1, 0);
	rc = xo_printf(xop, "%s%*s}", pre_nl, xo_indent(xop), "");
	xop->xo_stack[xop->xo_depth].xs_flags |= XSF_NOT_FIRST;
	break;

    case XO_STYLE_HTML:
    case XO_STYLE_TEXT:
	xo_depth_change(xop, name, -1, 0, 0);
	break;
    }

    return rc;
}

int
xo_close_instance (const char *name)
{
    return xo_close_instance_h(NULL, name);
}

#if 0
int
xo_header_line_h (xo_handle_t *xop, const char *fmt, ...)
{
    static char div_start[] =
        "%*s<div class=\"line\">%s%*s%<div class=\"header-line\">";
    static char div_end[] = "%s%*s</div>%s%*s</div>%s";

    xop = xo_default(xop);
    const char *ppn = (xop->xo_flags & XOF_PRETTY) ? "\n" : "";
    int rc = 0;

    switch (xop->xo_style) {
    }

    return rc;
}
#endif

void
xo_set_writer (xo_handle_t *xop, void *opaque, xo_write_func_t write_func,
	       xo_close_func_t close_func)
{
    xop = xo_default(xop);

    xop->xo_opaque = opaque;
    xop->xo_write = write_func;
    xop->xo_close = close_func;
}

void
xo_set_allocator (xo_realloc_func_t realloc_func, xo_free_func_t free_func)
{
    xo_realloc = realloc_func;
    xo_free = free_func;
}

#ifdef UNIT_TEST
int
main (int argc, char **argv)
{
    static char base_grocery[] = "GRO";
    static char base_hardware[] = "HRD";
    struct item {
	const char *i_title;
	int i_sold;
	int i_instock;
	int i_onorder;
	const char *i_sku_base;
	int i_sku_num;
    };
    struct item list[] = {
	{ "gum", 1412, 54, 10, base_grocery, 415 },
	{ "rope", 85, 4, 2, base_hardware, 212 },
	{ "ladder", 0, 2, 1, base_hardware, 517 },
	{ "bolt", 4123, 144, 42, base_hardware, 632 },
	{ "water", 17, 14, 2, base_grocery, 2331 },
	{ NULL, 0, 0, 0, NULL, 0 }
    };
    struct item list2[] = {
	{ "fish", 1321, 45, 1, base_grocery, 533 },
    };
    struct item *ip;
    xo_info_t info[] = {
	{ "in-stock", "number", "Number of items in stock" },
	{ "name", "string", "Name of the item" },
	{ "on-order", "number", "Number of items on order" },
	{ "sku", "string", "Stock Keeping Unit" },
	{ "sold", "number", "Number of items sold" },
	{ NULL, NULL, NULL },
    };
    int info_count = (sizeof(info) / sizeof(info[0])) - 1;
    
    for (argc = 1; argv[argc]; argc++) {
	if (strcmp(argv[argc], "xml") == 0)
	    xo_set_style(NULL, XO_STYLE_XML);
	else if (strcmp(argv[argc], "json") == 0)
	    xo_set_style(NULL, XO_STYLE_JSON);
	else if (strcmp(argv[argc], "text") == 0)
	    xo_set_style(NULL, XO_STYLE_TEXT);
	else if (strcmp(argv[argc], "html") == 0)
	    xo_set_style(NULL, XO_STYLE_HTML);
	else if (strcmp(argv[argc], "pretty") == 0)
	    xo_set_flags(NULL, XOF_PRETTY);
	else if (strcmp(argv[argc], "xpath") == 0)
	    xo_set_flags(NULL, XOF_XPATH);
	else if (strcmp(argv[argc], "info") == 0)
	    xo_set_flags(NULL, XOF_INFO);
    }

    xo_set_info(NULL, info, info_count);

    xo_open_container_h(NULL, "top");

    xo_open_container("data");
    xo_open_list("item");

    xo_emit("{T:Item/%-10s}{T:Total Sold/%12s}{T:In Stock/%12s}"
	    "{T:On Order/%12s}{T:SKU/%5s}\n");

    for (ip = list; ip->i_title; ip++) {
	xo_open_instance("item");

	xo_emit("{:item/%-10s/%s}{:sold/%12u/%u}{:in-stock/%12u/%u}"
		"{:on-order/%12u/%u}{:sku/%5s-000-%u/%s-000-%u}\n",
		ip->i_title, ip->i_sold, ip->i_instock, ip->i_onorder,
		ip->i_sku_base, ip->i_sku_num);

	xo_close_instance("item");
    }

    xo_close_list("item");
    xo_close_container("data");

    xo_emit("\n\n");

    xo_open_container("data");
    xo_open_list("item");

    for (ip = list; ip->i_title; ip++) {
	xo_open_instance("item");

	xo_emit("{L:Item} '{:name/%s}':\n", ip->i_title);
	xo_emit("{P:   }{L:Total sold}: {N:sold/%u%s}\n",
		ip->i_sold, ip->i_sold ? ".0" : "");
	xo_emit("{P:   }{LWC:In stock}{:in-stock/%u}\n", ip->i_instock);
	xo_emit("{P:   }{LWC:On order}{:on-order/%u}\n", ip->i_onorder);
	xo_emit("{P:   }{L:SKU}: {Q:sku/%s-000-%u}\n",
		ip->i_sku_base, ip->i_sku_num);

	xo_close_instance("item");
    }

    xo_close_list("item");
    xo_close_container("data");

    xo_open_container("data");
    xo_open_list("item");

    for (ip = list2; ip->i_title; ip++) {
	xo_open_instance("item");

	xo_emit("{L:Item} '{:name/%s}':\n", ip->i_title);
	xo_emit("{P:   }{L:Total sold}: {N:sold/%u%s}\n",
		ip->i_sold, ip->i_sold ? ".0" : "");
	xo_emit("{P:   }{LWC:In stock}{:in-stock/%u}\n", ip->i_instock);
	xo_emit("{P:   }{LWC:On order}{:on-order/%u}\n", ip->i_onorder);
	xo_emit("{P:   }{L:SKU}: {Q:sku/%s-000-%u}\n",
		ip->i_sku_base, ip->i_sku_num);

	xo_close_instance("item");
    }

    xo_close_list("item");
    xo_close_container("data");

    xo_close_container_h(NULL, "top");

    return 0;
}
#endif /* UNIT_TEST */
