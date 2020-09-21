/*
 * Copyright(c) 2020 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#ifndef IDL_PROCESSOR_H
#define IDL_PROCESSOR_H

/**
 * @file
 * Types and functions for the IDL compiler.
 */

#include <stdarg.h>
#include <stddef.h>

#include "idl/export.h"
#include "idl/retcode.h"
#include "idl/tree.h"
#include "idl/scope.h"

/** @private */
typedef struct idl_buffer idl_buffer_t;
struct idl_buffer {
  char *data;
  size_t size; /**< total number of bytes available */
  size_t used; /**< number of bytes used */
};

/** @private */
typedef struct idl_lexeme idl_lexeme_t;
struct idl_lexeme {
  const char *marker;
  const char *limit;
  idl_location_t location;
};

/** @private */
typedef struct idl_token idl_token_t;
struct idl_token {
  int32_t code; /**< token identifier (generated by Bison) */
  union {
    int32_t chr;
    unsigned long long ullng;
    long double ldbl;
    char *str;
  } value;
  idl_location_t location;
};


/**
 * @name IDL_processor_options
 * IDL processor options
 * @{
 */
/** Debug */
#define IDL_FLAG_DEBUG (1u<<1)
/** Preprocess */
#define IDL_PREPROCESS (1u<<0)
/** Flag used by idlc to indicate end-of-buffer */
#define IDL_WRITE (1u<<11)

#if 0
/* FIXME: introduce compatibility options
 * -e(xtension) with e.g. embedded-struct-def. The -e flags can also be used
 *  to enable/disable building blocks from IDL 4.x.
 * -s with e.g. 3.5 and 4.0 to enable everything allowed in the specific IDL
 *  specification.
 */

/* FIXME: introduce flags? can be used to enable embedded structs and arrays
          in structs, which is not allowed in IDL4, except with building block
          anonymous types (not embedded structs). */
#define IDL_FLAG_EMBEDDED_STRUCT_DEF (1u<<2)
#define IDL_FLAG_EMBEDDED_ARRAY_DEF

/* FIXME: probably better not to mix IDL 3.5 and 4.0 and just use separate
          grammars. one of the reasons being anonymous types, especially
          embedded struct definitions. one problem is that a member and a
          struct can both be annotated, if a struct is declared in a struct,
          what's being annotated? is it the member or the struct? */
#endif

#define IDL_FLAG_EXTENDED_DATA_TYPES (1u<<3)
#define IDL_FLAG_ANNOTATIONS (1u<<4)

#if 0
/* FIXME: introduce flag? would require IDL4. at least there for @hashid */
#define IDL_FLAG_XTYPES (1u<<5)
#endif
/** @} */

typedef struct idl_symbol idl_symbol_t;
struct idl_symbol {
  idl_mask_t mask;
  idl_location_t location;
};

/** @private */
typedef struct idl_processor idl_processor_t;
struct idl_processor {
  uint32_t flags; /**< processor options */
  enum {
    IDL_SCAN,
    /** scanning preprocessor directive */
    IDL_SCAN_DIRECTIVE = (1<<7),
    IDL_SCAN_DIRECTIVE_NAME,
    /** scanning #line directive */
    IDL_SCAN_LINE = (IDL_SCAN_DIRECTIVE | 1<<6),
    IDL_SCAN_FILENAME,
    IDL_SCAN_EXTRA_TOKEN,
    /** scanning #pragma directive */
    IDL_SCAN_PRAGMA = (IDL_SCAN_DIRECTIVE | 1<<5),
    IDL_SCAN_UNKNOWN_PRAGMA,
    /** scanning #pragma keylist directive */
    IDL_SCAN_KEYLIST = (IDL_SCAN_PRAGMA | 1<<4),
    IDL_SCAN_KEY,
    /** scanning IDL code */
    IDL_SCAN_CODE = (1<<9),
    /** scanning a scoped name in IDL code */
    IDL_SCAN_SCOPED_NAME = (IDL_SCAN_CODE | (1<<8)),
    /** end of input */
    IDL_EOF = (1<<10)
  } state; /**< processor state */
  idl_file_t *files; /**< list of encountered files */
  idl_symbol_t *directive;
  idl_buffer_t buffer; /**< dynamically sized input buffer */
  void *locale;
  idl_scope_t *global_scope, *scope;
  struct {
    const char *cursor;
    const char *limit;
    idl_position_t position;
  } scanner;
  struct {
    void *yypstate; /**< state of Bison generated parser */
  } parser;
};

IDL_EXPORT idl_retcode_t
idl_processor_init(idl_processor_t *proc);

IDL_EXPORT void
idl_processor_fini(idl_processor_t *proc);

IDL_EXPORT idl_retcode_t
idl_parse(idl_processor_t *proc, idl_node_t **nodeptr);

IDL_EXPORT void
idl_verror(idl_processor_t *proc, const idl_location_t *loc, const char *fmt, va_list ap);

IDL_EXPORT void
idl_error(idl_processor_t *proc, const idl_location_t *loc, const char *fmt, ...);

IDL_EXPORT void
idl_warning(idl_processor_t *proc, const idl_location_t *loc, const char *fmt, ...);

#endif /* IDL_PROCESSOR_H */
