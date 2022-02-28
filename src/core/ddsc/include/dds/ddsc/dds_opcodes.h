/*
 * Copyright(c) 2021 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

/** @file
 *
 * @brief DDS C (de)serialization opcodes
 *
 * Opcodes for (de)serialization of types generated by idlc. Isolated in a
 * separate header to share with idlc without the need to pull in the entire
 * Eclipse Cyclone DDS C language binding.
 */
#ifndef DDS_OPCODES_H
#define DDS_OPCODES_H

#include "dds/ddsrt/static_assert.h"

#if defined (__cplusplus)
extern "C" {
#endif

#define DDS_OP_MASK               0xff000000
#define DDS_OP_TYPE_FLAGS_MASK    0x00800000
#define DDS_OP_TYPE_MASK          0x007f0000
#define DDS_OP_SUBTYPE_MASK       0x0000ff00
#define DDS_OP_JMP_MASK           0x0000ffff
#define DDS_OP_FLAGS_MASK         0x000000ff
#define DDS_JEQ_TYPE_FLAGS_MASK   0x00800000
#define DDS_JEQ_TYPE_MASK         0x007f0000
#define DDS_PLM_FLAGS_MASK        0x00ff0000
#define DDS_KOF_OFFSET_MASK       0x0000ffff

#define DDS_OP(o)             ((enum dds_stream_opcode) ((o) & DDS_OP_MASK))
#define DDS_OP_TYPE(o)        ((enum dds_stream_typecode) (((o) & DDS_OP_TYPE_MASK) >> 16))
#define DDS_OP_TYPE_FLAGS(o)  ((o) & DDS_OP_TYPE_FLAGS_MASK)
#define DDS_OP_SUBTYPE(o)     ((enum dds_stream_typecode) (((o) & DDS_OP_SUBTYPE_MASK) >> 8))
#define DDS_OP_FLAGS(o)       ((o) & DDS_OP_FLAGS_MASK)
#define DDS_OP_ADR_JSR(o)     ((int16_t) ((o) & DDS_OP_JMP_MASK))
#define DDS_OP_ADR_PLM(o)     ((int16_t) ((o) & DDS_OP_JMP_MASK))
#define DDS_OP_LENGTH(o)      ((uint16_t) ((o) & DDS_OP_JMP_MASK))
#define DDS_OP_JUMP(o)        ((int16_t) ((o) & DDS_OP_JMP_MASK))
#define DDS_OP_ADR_JMP(o)     ((o) >> 16)
#define DDS_JEQ_TYPE(o)       ((enum dds_stream_typecode) (((o) & DDS_JEQ_TYPE_MASK) >> 16))
#define DDS_JEQ_TYPE_FLAGS(o) ((o) & DDS_JEQ_TYPE_FLAGS_MASK)
#define DDS_PLM_FLAGS(o)      ((enum dds_stream_typecode) (((o) & DDS_PLM_FLAGS_MASK) >> 16))

/* Topic encoding instruction types */

enum dds_stream_opcode {
  /* return from subroutine, exits top-level
     [RTS,   0,   0, 0] */
  DDS_OP_RTS = 0x00 << 24,

  /* data field
     [ADR, nBY,   0, f] [offset]
     [ADR, ENU,   0, f] [offset] [max]
     [ADR, STR,   0, f] [offset]
     [ADR, BST,   0, f] [offset] [max-size]

     [ADR, SEQ, nBY, f] [offset]
     [ADR, SEQ, ENU, f] [offset] [max]
     [ADR, SEQ, STR, f] [offset]
     [ADR, SEQ, BST, f] [offset] [max-size]
     [ADR, SEQ,   s, f] [offset] [elem-size] [next-insn, elem-insn]
       where s = {SEQ,ARR,UNI,STU,BSQ}
     [ADR, SEQ, EXT, f] *** not supported

     [ADR, BSQ, nBY, f] [offset] [sbound]
     [ADR, BSQ, ENU, f] [offset] [sbound] [max]
     [ADR, BSQ, STR, f] [offset] [sbound]
     [ADR, BSQ, BST, f] [offset] [sbound] [max-size]
     [ADR, BSQ,   s, f] [offset] [sbound] [elem-size] [next-insn, elem-insn]
       where s = {SEQ,ARR,UNI,STU,BSQ}
     [ADR, BSQ, EXT, f] *** not supported

     [ADR, ARR, nBY, f] [offset] [alen]
     [ADR, ARR, ENU, f] [offset] [alen] [max]
     [ADR, ARR, STR, f] [offset] [alen]
     [ADR, ARR, BST, f] [offset] [alen] [0] [max-size]
     [ADR, ARR,   s, f] [offset] [alen] [next-insn, elem-insn] [elem-size]
         where s = {SEQ,ARR,UNI,STU,BSQ}
     [ADR, ARR, EXT, f] *** not supported

     [ADR, UNI,   d, z] [offset] [alen] [next-insn, cases]
     [ADR, UNI, ENU, z] [offset] [alen] [next-insn, cases] [max]
     [ADR, UNI, EXT, f] *** not supported
       where
         d = discriminant type of {1BY,2BY,4BY}
         z = default present/not present (DDS_OP_FLAG_DEF)
         offset = discriminant offset
         max = max enum value
       followed by alen case labels: in JEQ format

     [ADR, e | EXT,   0, f] [offset] [next-insn, elem-insn] [elem-size iff "external" flag e is set, or flag f has DDS_OP_FLAG_OPT]
     [ADR, STU,   0, f] *** not supported
   where
     s            = subtype
     e            = external: stored as external data (pointer) (DDS_OP_FLAG_EXT)
     f            = flags:
                    - key/not key (DDS_OP_FLAG_KEY)
                    - base type member, used with EXT type (DDS_OP_FLAG_BASE)
                    - optional (DDS_OP_FLAG_OPT)
                    - must-understand (DDS_OP_FLAG_MU)
                    - storage size, only for ENU (n << DDS_OP_FLAG_SZ_SHIFT)
     [offset]     = field offset from start of element in memory
     [elem-size]  = element size in memory (elem-size is only included in case 'external' flag is set)
     [max-size]   = string bound + 1
     [max]        = max enum value
     [alen]       = array length, number of cases
     [sbound]     = bounded sequence maximum number of elements
     [next-insn]  = (unsigned 16 bits) offset to instruction for next field, from start of insn
     [elem-insn]  = (unsigned 16 bits) offset to first instruction for element, from start of insn
     [cases]      = (unsigned 16 bits) offset to first case label, from start of insn
   */
  DDS_OP_ADR = 0x01 << 24,

  /* jump-to-subroutine (e.g. used for recursive types and appendable unions)
     [JSR,   0, e]
       where
         e = (signed 16 bits) offset to first instruction in subroutine, from start of insn
             instruction sequence must end in RTS, execution resumes at instruction
             following JSR */
  DDS_OP_JSR = 0x02 << 24,

  /* jump-if-equal, used for union cases:
     [JEQ, nBY, 0] [disc] [offset]
     [JEQ, STR, 0] [disc] [offset]
     [JEQ, s,   i] [disc] [offset]
     [JEQ4, e | nBY, 0] [disc] [offset] 0
     [JEQ4, e | STR, 0] [disc] [offset] 0
     [JEQ4, e | ENU, f] [disc] [offset] [max]
     [JEQ4, EXT, 0] *** not supported, use STU/UNI for external defined types
     [JEQ4, e | s, i] [disc] [offset] [elem-size iff "external" flag e is set, else 0]
       where
         e  = external: stored as external data (pointer) (DDS_OP_FLAG_EXT)
         s  = subtype other than {nBY,STR} for JEQ and {nBY,STR,ENU,EXT} for JEQ4
         i  = (unsigned 16 bits) offset to first instruction for case, from start of insn
              instruction sequence must end in RTS, at which point executes continues
              at the next field's instruction as specified by the union
         f  = size flags for ENU instruction

      Note that the JEQ instruction is deprecated and replaced by the JEQ4 instruction. The
      IDL compiler only generates JEQ4 for union cases, the JEQ instruction is included here
      for backwards compatibility (topic descriptors generated with a previous version of IDLC)
  */
  DDS_OP_JEQ = 0x03 << 24,

  /* XCDR2 delimited CDR (inserts DHEADER before type)
    [DLC, 0, 0]
  */
  DDS_OP_DLC = 0x04 << 24,

  /* XCDR2 parameter list CDR (inserts DHEADER before type and EMHEADER before each member)
     [PLC, 0, 0]
          followed by a list of JEQ instructions
  */
  DDS_OP_PLC = 0x05 << 24,

  /*
     [PLM,   f, elem-insn] [member id]
       for members of aggregated mutable types (pl-cdr):
       where
         f           = flags:
                       - jump to base type (DDS_OP_FLAG_BASE)
         [elem-insn] = (unsigned 16 bits) offset to instruction for element, from start of insn
                        when FLAG_BASE is set, this is the offset of the PLM list of the base type
         [member id] = id for this member (0 in case FLAG_BASE is set)
  */
  DDS_OP_PLM = 0x06 << 24,

  /* Key offset list
     [KOF, 0, n] [offset-1] ... [offset-n]
       where
        n      = number of key offsets in following ops
        offset = offset of the key field, relative to the previous offset
                  (repeated n times, e.g. when key in nested struct)
  */
  DDS_OP_KOF = 0x07 << 24,

  /* see comment for JEQ/JEQ4 above */
  DDS_OP_JEQ4 = 0x08 << 24
};

enum dds_stream_typecode {
  DDS_OP_VAL_1BY = 0x01, /* one byte simple type (char, octet, boolean) */
  DDS_OP_VAL_2BY = 0x02, /* two byte simple type ((unsigned) short) */
  DDS_OP_VAL_4BY = 0x03, /* four byte simple type ((unsigned) long, enums, float) */
  DDS_OP_VAL_8BY = 0x04, /* eight byte simple type ((unsigned) long long, double) */
  DDS_OP_VAL_STR = 0x05, /* string */
  DDS_OP_VAL_BST = 0x06, /* bounded string */
  DDS_OP_VAL_SEQ = 0x07, /* sequence */
  DDS_OP_VAL_ARR = 0x08, /* array */
  DDS_OP_VAL_UNI = 0x09, /* union */
  DDS_OP_VAL_STU = 0x0a, /* struct */
  DDS_OP_VAL_BSQ = 0x0b, /* bounded sequence */
  DDS_OP_VAL_ENU = 0x0c, /* enumerated value (long) */
  DDS_OP_VAL_EXT = 0x0d  /* field with external definition */
};

/* primary type code for DDS_OP_ADR, DDS_OP_JEQ */
enum dds_stream_typecode_primary {
  DDS_OP_TYPE_1BY = DDS_OP_VAL_1BY << 16,
  DDS_OP_TYPE_2BY = DDS_OP_VAL_2BY << 16,
  DDS_OP_TYPE_4BY = DDS_OP_VAL_4BY << 16,
  DDS_OP_TYPE_8BY = DDS_OP_VAL_8BY << 16,
  DDS_OP_TYPE_STR = DDS_OP_VAL_STR << 16,
  DDS_OP_TYPE_BST = DDS_OP_VAL_BST << 16,
  DDS_OP_TYPE_SEQ = DDS_OP_VAL_SEQ << 16,
  DDS_OP_TYPE_ARR = DDS_OP_VAL_ARR << 16,
  DDS_OP_TYPE_UNI = DDS_OP_VAL_UNI << 16,
  DDS_OP_TYPE_STU = DDS_OP_VAL_STU << 16,
  DDS_OP_TYPE_BSQ = DDS_OP_VAL_BSQ << 16,
  DDS_OP_TYPE_ENU = DDS_OP_VAL_ENU << 16,
  DDS_OP_TYPE_EXT = DDS_OP_VAL_EXT << 16
};
#define DDS_OP_TYPE_BOO DDS_OP_TYPE_1BY

/* This flag indicates that the type has external data (i.e. a mapped to a pointer type),
   which can be the case because of (1) the @external annotation in idl or (2) the @optional
   annotation (optional fields are also mapped to pointer types as described in the XTypes spec).
   This flag is stored in the most-significant bit of the 'type' part of the serializer instruction. */
#define DDS_OP_FLAG_EXT (1u << 23)


/* sub-type code:
   - encodes element type for DDS_OP_TYPE_{SEQ,ARR},
   - discriminant type for DDS_OP_TYPE_UNI */
enum dds_stream_typecode_subtype {
  DDS_OP_SUBTYPE_1BY = DDS_OP_VAL_1BY << 8,
  DDS_OP_SUBTYPE_2BY = DDS_OP_VAL_2BY << 8,
  DDS_OP_SUBTYPE_4BY = DDS_OP_VAL_4BY << 8,
  DDS_OP_SUBTYPE_8BY = DDS_OP_VAL_8BY << 8,
  DDS_OP_SUBTYPE_STR = DDS_OP_VAL_STR << 8,
  DDS_OP_SUBTYPE_BST = DDS_OP_VAL_BST << 8,
  DDS_OP_SUBTYPE_SEQ = DDS_OP_VAL_SEQ << 8,
  DDS_OP_SUBTYPE_ARR = DDS_OP_VAL_ARR << 8,
  DDS_OP_SUBTYPE_UNI = DDS_OP_VAL_UNI << 8,
  DDS_OP_SUBTYPE_STU = DDS_OP_VAL_STU << 8,
  DDS_OP_SUBTYPE_BSQ = DDS_OP_VAL_BSQ << 8,
  DDS_OP_SUBTYPE_ENU = DDS_OP_VAL_ENU << 8
};
#define DDS_OP_SUBTYPE_BOO DDS_OP_SUBTYPE_1BY

/* key field: applicable to {1,2,4,8}BY, STR, BST, ARR-of-{1,2,4,8}BY.
   Note that when defining keys in nested types, the key flag should be set
   on both the field(s) in the subtype and on the enclosing STU/EXT field. */
#define DDS_OP_FLAG_KEY  (1u << 0)

/* For a union: (1) the discriminator may be a key field; (2) there may be a default value;
   and (3) the discriminator can be an integral type (or enumerated - here treated as equivalent).
   What it can't be is a floating-point type. So DEF and FP need never be set at the same time.
   There are only a few flag bits, so saving one is not such a bad idea. */
#define DDS_OP_FLAG_DEF  (1u << 1) /* union has a default case (for DDS_OP_ADR | DDS_OP_TYPE_UNI) */

#define DDS_OP_FLAG_FP   (1u << 1) /* floating-point: applicable to {4,8}BY and arrays, sequences of them */
#define DDS_OP_FLAG_SGN  (1u << 2) /* signed: applicable to {1,2,4,8}BY and arrays, sequences of them */
#define DDS_OP_FLAG_MU   (1u << 3) /* must-understand flag */
#define DDS_OP_FLAG_BASE (1u << 4) /* jump to base type, used with PLM in mutable types and for
                                      the TYPE_EXT 'parent' member in final and appendable types */
#define DDS_OP_FLAG_OPT  (1u << 5) /* optional flag, used with struct members. For non-string types,
                                      an optional member also gets the FLAG_EXT, see above. */

#define DDS_OP_FLAG_SZ_SHIFT  (6)
#define DDS_OP_FLAG_SZ_MASK   (3u << DDS_OP_FLAG_SZ_SHIFT) /* Enum and bitmask storage size -- SZ2, SZ1: 00 = 1 byte, 01 = 2 bytes, 10 = 4 bytes, 11 = 8 bytes (bitmask only) */
#define DDS_OP_FLAGS_SZ(f)    (1u << (((f) & DDS_OP_FLAG_SZ_MASK) >> DDS_OP_FLAG_SZ_SHIFT))
#define DDS_OP_TYPE_SZ(o)     DDS_OP_FLAGS_SZ(DDS_OP_FLAGS(o))

/* Topic descriptor flag values */
#define DDS_TOPIC_NO_OPTIMIZE                   (1u << 0)
#define DDS_TOPIC_FIXED_KEY                     (1u << 1)   /* Set if the XCDR1 serialized key fits in 16 bytes */
#define DDS_TOPIC_CONTAINS_UNION                (1u << 2)
// (1u << 3) unused, was used for DDS_TOPIC_DISABLE_TYPECHECK
#define DDS_TOPIC_FIXED_SIZE                    (1u << 4)
#define DDS_TOPIC_FIXED_KEY_XCDR2               (1u << 5)   /* Set if the XCDR2 serialized key fits in 16 bytes */
#define DDS_TOPIC_XTYPES_METADATA               (1u << 6)   /* Set if XTypes meta-data is present for this topic */

/* Max size of fixed key */
#define DDS_FIXED_KEY_MAX_SIZE (16)

#if defined(__cplusplus)
}
#endif

#endif /* DDS_OPCODES_H */
