/*
 * Secret Labs' Regular Expression Engine
 *
 * regular expression matching engine
 *
 * Copyright (c) 1997-2001 by Secret Labs AB.  All rights reserved.
 *
 * This version of the SRE library can be redistributed under CNRI's
 * Python 1.6 license.  For any other use, please contact Secret Labs
 * AB (info@pythonware.com).
 *
 * Portions of this engine have been developed in cooperation with
 * CNRI.  Hewlett-Packard provided funding for 1.6 integration and
 * other compatibility work.
 */

// lib/convert-sre-engine.py made this from Modules/_sre of CPython 3.14: sre_constants.h, sre_lib.h, and as much of sre.h and sre.c as sre_lib.h makes use of. It is not to be changed by hand. It is written as
// CPython's is, names and all, so that the two can be laid side by side. It is for PythonSRE.cpp and nothing else to include.

#pragma once

#include "PythonUnicodeType.h"
#include <wtf/ASCIICType.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

#if OS(DARWIN)
#include <wctype.h>
#include <xlocale.h>
#elif !OS(WINDOWS)
#include <ctype.h>
#include <locale.h>
#endif

namespace JSC {

class JSGlobalObject;

namespace Python { namespace SRE {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// ---- Modules/_sre/sre.h

// size of a code word (must be unsigned short or larger, and large enough to hold a UCS4 character)
using SRE_CODE = uint32_t;
static constexpr SRE_CODE SRE_MAXREPEAT = ~static_cast<SRE_CODE>(0);
static constexpr SRE_CODE SRE_MAXGROUPS = static_cast<SRE_CODE>(INT32_MAX) / 2;
static constexpr unsigned SRE_CODE_BITS = 8 * sizeof(SRE_CODE);

struct SRE_REPEAT {
    ptrdiff_t count;
    const SRE_CODE* pattern; /* points to REPEAT operator arguments */
    const void* last_ptr; /* helper to check for infinite loops */
    SRE_REPEAT* prev; /* points to previous repeat context */
    /* for SRE_REPEAT pool */
    SRE_REPEAT* pool_prev;
    SRE_REPEAT* pool_next;
};

#if OS(WINDOWS)
using SRE_LOCALE = void*;
#else
using SRE_LOCALE = locale_t;
#endif

struct SRE_STATE {
    /* string pointers */
    const void* ptr { nullptr }; /* current position (also end of current slice) */
    const void* beginning { nullptr }; /* start of original string */
    const void* start { nullptr }; /* start of current slice */
    const void* end { nullptr }; /* end of original string */
    /* attributes for the match object */
    ptrdiff_t pos { 0 };
    ptrdiff_t endpos { 0 };
    int isbytes { 0 };
    int charsize { 0 }; /* character size */
    int match_all { 0 };
    int must_advance { 0 };
    /* marks */
    int lastmark { -1 };
    int lastindex { -1 };
    const void** mark { nullptr };
    /* dynamically allocated stuff */
    char* data_stack { nullptr };
    size_t data_stack_size { 0 };
    size_t data_stack_base { 0 };
    /* current repeat context */
    SRE_REPEAT* repeat { nullptr };
    /* SRE_REPEAT pool */
    SRE_REPEAT* repeat_pool_used { nullptr };
    SRE_REPEAT* repeat_pool_unused { nullptr };
    unsigned sigcount { 0 };

    // What CPython has no need of. Both are in PythonSRE.cpp.
    // PyErr_CheckSignals(), and whether the bytes that are being looked through are still where they were once that is done. Not 0 if it raised.
    int checkSignals();
    // What the C library goes by for CPython: LC_CTYPE, as it is now.
    SRE_LOCALE currentLocale();
    JSGlobalObject* globalObject { nullptr };
    SRE_LOCALE locale { nullptr };
};

// ---- Modules/_sre/sre_constants.h

static constexpr SRE_CODE SRE_MAGIC = 20230612;
static constexpr SRE_CODE SRE_OP_FAILURE = 0;
static constexpr SRE_CODE SRE_OP_SUCCESS = 1;
static constexpr SRE_CODE SRE_OP_ANY = 2;
static constexpr SRE_CODE SRE_OP_ANY_ALL = 3;
static constexpr SRE_CODE SRE_OP_ASSERT = 4;
static constexpr SRE_CODE SRE_OP_ASSERT_NOT = 5;
static constexpr SRE_CODE SRE_OP_AT = 6;
static constexpr SRE_CODE SRE_OP_BRANCH = 7;
static constexpr SRE_CODE SRE_OP_CATEGORY = 8;
static constexpr SRE_CODE SRE_OP_CHARSET = 9;
static constexpr SRE_CODE SRE_OP_BIGCHARSET = 10;
static constexpr SRE_CODE SRE_OP_GROUPREF = 11;
static constexpr SRE_CODE SRE_OP_GROUPREF_EXISTS = 12;
static constexpr SRE_CODE SRE_OP_IN = 13;
static constexpr SRE_CODE SRE_OP_INFO = 14;
static constexpr SRE_CODE SRE_OP_JUMP = 15;
static constexpr SRE_CODE SRE_OP_LITERAL = 16;
static constexpr SRE_CODE SRE_OP_MARK = 17;
static constexpr SRE_CODE SRE_OP_MAX_UNTIL = 18;
static constexpr SRE_CODE SRE_OP_MIN_UNTIL = 19;
static constexpr SRE_CODE SRE_OP_NOT_LITERAL = 20;
static constexpr SRE_CODE SRE_OP_NEGATE = 21;
static constexpr SRE_CODE SRE_OP_RANGE = 22;
static constexpr SRE_CODE SRE_OP_REPEAT = 23;
static constexpr SRE_CODE SRE_OP_REPEAT_ONE = 24;
static constexpr SRE_CODE SRE_OP_SUBPATTERN = 25;
static constexpr SRE_CODE SRE_OP_MIN_REPEAT_ONE = 26;
static constexpr SRE_CODE SRE_OP_ATOMIC_GROUP = 27;
static constexpr SRE_CODE SRE_OP_POSSESSIVE_REPEAT = 28;
static constexpr SRE_CODE SRE_OP_POSSESSIVE_REPEAT_ONE = 29;
static constexpr SRE_CODE SRE_OP_GROUPREF_IGNORE = 30;
static constexpr SRE_CODE SRE_OP_IN_IGNORE = 31;
static constexpr SRE_CODE SRE_OP_LITERAL_IGNORE = 32;
static constexpr SRE_CODE SRE_OP_NOT_LITERAL_IGNORE = 33;
static constexpr SRE_CODE SRE_OP_GROUPREF_LOC_IGNORE = 34;
static constexpr SRE_CODE SRE_OP_IN_LOC_IGNORE = 35;
static constexpr SRE_CODE SRE_OP_LITERAL_LOC_IGNORE = 36;
static constexpr SRE_CODE SRE_OP_NOT_LITERAL_LOC_IGNORE = 37;
static constexpr SRE_CODE SRE_OP_GROUPREF_UNI_IGNORE = 38;
static constexpr SRE_CODE SRE_OP_IN_UNI_IGNORE = 39;
static constexpr SRE_CODE SRE_OP_LITERAL_UNI_IGNORE = 40;
static constexpr SRE_CODE SRE_OP_NOT_LITERAL_UNI_IGNORE = 41;
static constexpr SRE_CODE SRE_OP_RANGE_UNI_IGNORE = 42;
static constexpr SRE_CODE SRE_AT_BEGINNING = 0;
static constexpr SRE_CODE SRE_AT_BEGINNING_LINE = 1;
static constexpr SRE_CODE SRE_AT_BEGINNING_STRING = 2;
static constexpr SRE_CODE SRE_AT_BOUNDARY = 3;
static constexpr SRE_CODE SRE_AT_NON_BOUNDARY = 4;
static constexpr SRE_CODE SRE_AT_END = 5;
static constexpr SRE_CODE SRE_AT_END_LINE = 6;
static constexpr SRE_CODE SRE_AT_END_STRING = 7;
static constexpr SRE_CODE SRE_AT_LOC_BOUNDARY = 8;
static constexpr SRE_CODE SRE_AT_LOC_NON_BOUNDARY = 9;
static constexpr SRE_CODE SRE_AT_UNI_BOUNDARY = 10;
static constexpr SRE_CODE SRE_AT_UNI_NON_BOUNDARY = 11;
static constexpr SRE_CODE SRE_CATEGORY_DIGIT = 0;
static constexpr SRE_CODE SRE_CATEGORY_NOT_DIGIT = 1;
static constexpr SRE_CODE SRE_CATEGORY_SPACE = 2;
static constexpr SRE_CODE SRE_CATEGORY_NOT_SPACE = 3;
static constexpr SRE_CODE SRE_CATEGORY_WORD = 4;
static constexpr SRE_CODE SRE_CATEGORY_NOT_WORD = 5;
static constexpr SRE_CODE SRE_CATEGORY_LINEBREAK = 6;
static constexpr SRE_CODE SRE_CATEGORY_NOT_LINEBREAK = 7;
static constexpr SRE_CODE SRE_CATEGORY_LOC_WORD = 8;
static constexpr SRE_CODE SRE_CATEGORY_LOC_NOT_WORD = 9;
static constexpr SRE_CODE SRE_CATEGORY_UNI_DIGIT = 10;
static constexpr SRE_CODE SRE_CATEGORY_UNI_NOT_DIGIT = 11;
static constexpr SRE_CODE SRE_CATEGORY_UNI_SPACE = 12;
static constexpr SRE_CODE SRE_CATEGORY_UNI_NOT_SPACE = 13;
static constexpr SRE_CODE SRE_CATEGORY_UNI_WORD = 14;
static constexpr SRE_CODE SRE_CATEGORY_UNI_NOT_WORD = 15;
static constexpr SRE_CODE SRE_CATEGORY_UNI_LINEBREAK = 16;
static constexpr SRE_CODE SRE_CATEGORY_UNI_NOT_LINEBREAK = 17;
static constexpr SRE_CODE SRE_FLAG_IGNORECASE = 2;
static constexpr SRE_CODE SRE_FLAG_LOCALE = 4;
static constexpr SRE_CODE SRE_FLAG_MULTILINE = 8;
static constexpr SRE_CODE SRE_FLAG_DOTALL = 16;
static constexpr SRE_CODE SRE_FLAG_UNICODE = 32;
static constexpr SRE_CODE SRE_FLAG_VERBOSE = 64;
static constexpr SRE_CODE SRE_FLAG_DEBUG = 128;
static constexpr SRE_CODE SRE_FLAG_ASCII = 256;
static constexpr SRE_CODE SRE_INFO_PREFIX = 1;
static constexpr SRE_CODE SRE_INFO_LITERAL = 2;
static constexpr SRE_CODE SRE_INFO_CHARSET = 4;

// ---- Modules/_sre/sre.c, as far as the three copies of sre_lib.h

inline SRE_LOCALE sre_locale(SRE_STATE* state)
{
    if (!state->locale)
        state->locale = state->currentLocale();
    return state->locale;
}

// On macOS, use the wide character ctype API using btowc()
inline unsigned sre_isalnum(SRE_STATE* state, unsigned ch)
{
#if OS(DARWIN)
    return static_cast<unsigned>(iswalnum_l(btowc_l(static_cast<int>(ch), sre_locale(state)), sre_locale(state)));
#elif OS(WINDOWS)
    UNUSED_PARAM(state);
    return static_cast<unsigned>(isalnum(static_cast<int>(ch)));
#else
    return static_cast<unsigned>(isalnum_l(static_cast<int>(ch), sre_locale(state)));
#endif
}

inline unsigned sre_tolower(SRE_STATE* state, unsigned ch)
{
#if OS(DARWIN)
    return static_cast<unsigned>(towlower_l(btowc_l(static_cast<int>(ch), sre_locale(state)), sre_locale(state)));
#elif OS(WINDOWS)
    UNUSED_PARAM(state);
    return static_cast<unsigned>(tolower(static_cast<int>(ch)));
#else
    return static_cast<unsigned>(tolower_l(static_cast<int>(ch), sre_locale(state)));
#endif
}

inline unsigned sre_toupper(SRE_STATE* state, unsigned ch)
{
#if OS(DARWIN)
    return static_cast<unsigned>(towupper_l(btowc_l(static_cast<int>(ch), sre_locale(state)), sre_locale(state)));
#elif OS(WINDOWS)
    UNUSED_PARAM(state);
    return static_cast<unsigned>(toupper(static_cast<int>(ch)));
#else
    return static_cast<unsigned>(toupper_l(static_cast<int>(ch), sre_locale(state)));
#endif
}

/* error codes */
static constexpr int SRE_ERROR_ILLEGAL = -1; /* illegal opcode */
static constexpr int SRE_ERROR_STATE = -2; /* illegal state */
static constexpr int SRE_ERROR_RECURSION_LIMIT = -3; /* runaway recursion */
static constexpr int SRE_ERROR_MEMORY = -9; /* out of memory */
static constexpr int SRE_ERROR_INTERRUPTED = -10; /* signal handler raised exception */

/* search engine state */

#define SRE_IS_DIGIT(ch) ((ch) <= '9' && isASCIIDigit(ch))
#define SRE_IS_SPACE(ch) ((ch) <= ' ' && ((ch) == ' ' || ((ch) >= '\t' && (ch) <= '\r')))
#define SRE_IS_LINEBREAK(ch) ((ch) == '\n')
#define SRE_IS_WORD(ch) ((ch) <= 'z' && (isASCIIAlphanumeric(ch) || (ch) == '_'))

inline unsigned sre_lower_ascii(unsigned ch)
{
    return ch < 128 ? toASCIILower(ch) : ch;
}

/* locale-specific character predicates */
/* !(c & ~N) == (c < N+1) for any unsigned c, this avoids
 * warnings when c's type supports only numbers < N+1 */
#define SRE_LOC_IS_ALNUM(state, ch) (!((ch) & ~255) ? sre_isalnum((state), (ch)) : 0)
#define SRE_LOC_IS_WORD(state, ch) (SRE_LOC_IS_ALNUM((state), (ch)) || (ch) == '_')

inline unsigned sre_lower_locale(SRE_STATE* state, unsigned ch)
{
    return ch < 256 ? sre_tolower(state, ch) : ch;
}

inline unsigned sre_upper_locale(SRE_STATE* state, unsigned ch)
{
    return ch < 256 ? sre_toupper(state, ch) : ch;
}

/* unicode-specific character predicates */

#define SRE_UNI_IS_DIGIT(ch) Unicode::isDecimalDigit(ch)
#define SRE_UNI_IS_SPACE(ch) Unicode::isWhitespace(ch)
#define SRE_UNI_IS_LINEBREAK(ch) Unicode::isLineBreak(ch)
#define SRE_UNI_IS_ALNUM(ch) Unicode::isAlphanumeric(ch)
#define SRE_UNI_IS_WORD(ch) (SRE_UNI_IS_ALNUM(ch) || (ch) == '_')

inline unsigned sre_lower_unicode(unsigned ch)
{
    return Unicode::toLowercase(ch);
}

inline unsigned sre_upper_unicode(unsigned ch)
{
    return Unicode::toUppercase(ch);
}

inline int sre_category(SRE_STATE* state, SRE_CODE category, unsigned ch)
{
    switch (category) {
    case SRE_CATEGORY_DIGIT:
        return SRE_IS_DIGIT(ch);
    case SRE_CATEGORY_NOT_DIGIT:
        return !SRE_IS_DIGIT(ch);
    case SRE_CATEGORY_SPACE:
        return SRE_IS_SPACE(ch);
    case SRE_CATEGORY_NOT_SPACE:
        return !SRE_IS_SPACE(ch);
    case SRE_CATEGORY_WORD:
        return SRE_IS_WORD(ch);
    case SRE_CATEGORY_NOT_WORD:
        return !SRE_IS_WORD(ch);
    case SRE_CATEGORY_LINEBREAK:
        return SRE_IS_LINEBREAK(ch);
    case SRE_CATEGORY_NOT_LINEBREAK:
        return !SRE_IS_LINEBREAK(ch);
    case SRE_CATEGORY_LOC_WORD:
        return SRE_LOC_IS_WORD(state, ch);
    case SRE_CATEGORY_LOC_NOT_WORD:
        return !SRE_LOC_IS_WORD(state, ch);
    case SRE_CATEGORY_UNI_DIGIT:
        return SRE_UNI_IS_DIGIT(ch);
    case SRE_CATEGORY_UNI_NOT_DIGIT:
        return !SRE_UNI_IS_DIGIT(ch);
    case SRE_CATEGORY_UNI_SPACE:
        return SRE_UNI_IS_SPACE(ch);
    case SRE_CATEGORY_UNI_NOT_SPACE:
        return !SRE_UNI_IS_SPACE(ch);
    case SRE_CATEGORY_UNI_WORD:
        return SRE_UNI_IS_WORD(ch);
    case SRE_CATEGORY_UNI_NOT_WORD:
        return !SRE_UNI_IS_WORD(ch);
    case SRE_CATEGORY_UNI_LINEBREAK:
        return SRE_UNI_IS_LINEBREAK(ch);
    case SRE_CATEGORY_UNI_NOT_LINEBREAK:
        return !SRE_UNI_IS_LINEBREAK(ch);
    }
    return 0;
}

inline int char_loc_ignore(SRE_STATE* state, SRE_CODE pattern, SRE_CODE ch)
{
    return ch == pattern
        || (SRE_CODE) sre_lower_locale(state, ch) == pattern
        || (SRE_CODE) sre_upper_locale(state, ch) == pattern;
}

/* helpers */

inline void data_stack_dealloc(SRE_STATE* state)
{
    if (state->data_stack) {
        fastFree(state->data_stack);
        state->data_stack = nullptr;
    }
    state->data_stack_size = state->data_stack_base = 0;
}

inline int data_stack_grow(SRE_STATE* state, ptrdiff_t size)
{
    ptrdiff_t minsize, cursize;
    minsize = state->data_stack_base+size;
    cursize = state->data_stack_size;
    if (cursize < minsize) {
        void* stack;
        cursize = minsize+minsize/4+1024;
        if (!WTF::tryFastRealloc(state->data_stack, cursize).getValue(stack)) {
            data_stack_dealloc(state);
            return SRE_ERROR_MEMORY;
        }
        state->data_stack = (char *)stack;
        state->data_stack_size = cursize;
    }
    return 0;
}

/* memory pool functions for SRE_REPEAT, this can avoid memory
   leak when SRE(match) function terminates abruptly.
   state->repeat_pool_used is a doubly-linked list, so that we
   can remove a SRE_REPEAT node from it.
   state->repeat_pool_unused is a singly-linked list, we put/get
   node at the head. */
inline SRE_REPEAT* repeat_pool_malloc(SRE_STATE* state)
{
    SRE_REPEAT *repeat;

    if (state->repeat_pool_unused) {
        /* remove from unused pool (singly-linked list) */
        repeat = state->repeat_pool_unused;
        state->repeat_pool_unused = repeat->pool_next;
    }
    else {
        void* memory;
        if (!tryFastMalloc(sizeof(SRE_REPEAT)).getValue(memory))
            return nullptr;
        repeat = (SRE_REPEAT *)memory;
    }

    /* add to used pool (doubly-linked list) */
    SRE_REPEAT *temp = state->repeat_pool_used;
    if (temp) {
        temp->pool_prev = repeat;
    }
    repeat->pool_prev = nullptr;
    repeat->pool_next = temp;
    state->repeat_pool_used = repeat;

    return repeat;
}

inline void repeat_pool_free(SRE_STATE* state, SRE_REPEAT* repeat)
{
    SRE_REPEAT *prev = repeat->pool_prev;
    SRE_REPEAT *next = repeat->pool_next;

    /* remove from used pool (doubly-linked list) */
    if (prev) {
        prev->pool_next = next;
    }
    else {
        state->repeat_pool_used = next;
    }
    if (next) {
        next->pool_prev = prev;
    }

    /* add to unused pool (singly-linked list) */
    repeat->pool_next = state->repeat_pool_unused;
    state->repeat_pool_unused = repeat;
}

inline void repeat_pool_clear(SRE_STATE* state)
{
    /* clear used pool */
    SRE_REPEAT *next = state->repeat_pool_used;
    state->repeat_pool_used = nullptr;
    while (next) {
        SRE_REPEAT *temp = next;
        next = temp->pool_next;
        fastFree(temp);
    }

    /* clear unused pool */
    next = state->repeat_pool_unused;
    state->repeat_pool_unused = nullptr;
    while (next) {
        SRE_REPEAT *temp = next;
        next = temp->pool_next;
        fastFree(temp);
    }
}

// ---- Modules/_sre/sre_lib.h

/* String matching engine */

/* This file is included three times, with different character settings */

template<typename SRE_CHAR>
inline int sre_at(SRE_STATE* state, const SRE_CHAR* ptr, SRE_CODE at)
{
    /* check if pointer is at given position */

    ptrdiff_t thisp, thatp;

    switch (at) {

    case SRE_AT_BEGINNING:
    case SRE_AT_BEGINNING_STRING:
        return ((void*) ptr == state->beginning);

    case SRE_AT_BEGINNING_LINE:
        return ((void*) ptr == state->beginning ||
                SRE_IS_LINEBREAK((int) ptr[-1]));

    case SRE_AT_END:
        return (((SRE_CHAR *)state->end - ptr == 1 &&
                 SRE_IS_LINEBREAK((int) ptr[0])) ||
                ((void*) ptr == state->end));

    case SRE_AT_END_LINE:
        return ((void*) ptr == state->end ||
                SRE_IS_LINEBREAK((int) ptr[0]));

    case SRE_AT_END_STRING:
        return ((void*) ptr == state->end);

    case SRE_AT_BOUNDARY:
        thatp = ((void*) ptr > state->beginning) ?
            SRE_IS_WORD((int) ptr[-1]) : 0;
        thisp = ((void*) ptr < state->end) ?
            SRE_IS_WORD((int) ptr[0]) : 0;
        return thisp != thatp;

    case SRE_AT_NON_BOUNDARY:
        thatp = ((void*) ptr > state->beginning) ?
            SRE_IS_WORD((int) ptr[-1]) : 0;
        thisp = ((void*) ptr < state->end) ?
            SRE_IS_WORD((int) ptr[0]) : 0;
        return thisp == thatp;

    case SRE_AT_LOC_BOUNDARY:
        thatp = ((void*) ptr > state->beginning) ?
            SRE_LOC_IS_WORD(state, (int) ptr[-1]) : 0;
        thisp = ((void*) ptr < state->end) ?
            SRE_LOC_IS_WORD(state, (int) ptr[0]) : 0;
        return thisp != thatp;

    case SRE_AT_LOC_NON_BOUNDARY:
        thatp = ((void*) ptr > state->beginning) ?
            SRE_LOC_IS_WORD(state, (int) ptr[-1]) : 0;
        thisp = ((void*) ptr < state->end) ?
            SRE_LOC_IS_WORD(state, (int) ptr[0]) : 0;
        return thisp == thatp;

    case SRE_AT_UNI_BOUNDARY:
        thatp = ((void*) ptr > state->beginning) ?
            SRE_UNI_IS_WORD((int) ptr[-1]) : 0;
        thisp = ((void*) ptr < state->end) ?
            SRE_UNI_IS_WORD((int) ptr[0]) : 0;
        return thisp != thatp;

    case SRE_AT_UNI_NON_BOUNDARY:
        thatp = ((void*) ptr > state->beginning) ?
            SRE_UNI_IS_WORD((int) ptr[-1]) : 0;
        thisp = ((void*) ptr < state->end) ?
            SRE_UNI_IS_WORD((int) ptr[0]) : 0;
        return thisp == thatp;

    }

    return 0;
}

template<typename SRE_CHAR>
inline int sre_charset(SRE_STATE* state, const SRE_CODE* set, SRE_CODE ch)
{
    /* check if character is a member of the given set */

    int ok = 1;

    for (;;) {
        switch (*set++) {

        case SRE_OP_FAILURE:
            return !ok;

        case SRE_OP_LITERAL:
            /* <LITERAL> <code> */
            if (ch == set[0])
                return ok;
            set++;
            break;

        case SRE_OP_CATEGORY:
            /* <CATEGORY> <code> */
            if (sre_category(state, set[0], (int) ch))
                return ok;
            set++;
            break;

        case SRE_OP_CHARSET:
            /* <CHARSET> <bitmap> */
            if (ch < 256 &&
                (set[ch/SRE_CODE_BITS] & (1u << (ch & (SRE_CODE_BITS-1)))))
                return ok;
            set += 256/SRE_CODE_BITS;
            break;

        case SRE_OP_RANGE:
            /* <RANGE> <lower> <upper> */
            if (set[0] <= ch && ch <= set[1])
                return ok;
            set += 2;
            break;

        case SRE_OP_RANGE_UNI_IGNORE:
            /* <RANGE_UNI_IGNORE> <lower> <upper> */
        {
            SRE_CODE uch;
            /* ch is already lower cased */
            if (set[0] <= ch && ch <= set[1])
                return ok;
            uch = sre_upper_unicode(ch);
            if (set[0] <= uch && uch <= set[1])
                return ok;
            set += 2;
            break;
        }

        case SRE_OP_NEGATE:
            ok = !ok;
            break;

        case SRE_OP_BIGCHARSET:
            /* <BIGCHARSET> <blockcount> <256 blockindices> <blocks> */
        {
            ptrdiff_t count, block;
            count = *(set++);

            if (ch < 0x10000u)
                block = ((unsigned char*)set)[ch >> 8];
            else
                block = -1;
            set += 256/sizeof(SRE_CODE);
            if (block >=0 &&
                (set[(block * 256 + (ch & 255))/SRE_CODE_BITS] &
                    (1u << (ch & (SRE_CODE_BITS-1)))))
                return ok;
            set += count * (256/SRE_CODE_BITS);
            break;
        }

        default:
            /* internal error -- there's not much we can do about it
               here, so let's just pretend it didn't match... */
            return 0;
        }
    }
}

template<typename SRE_CHAR>
inline int sre_charset_loc_ignore(SRE_STATE* state, const SRE_CODE* set, SRE_CODE ch)
{
    SRE_CODE lo, up;
    lo = sre_lower_locale(state, ch);
    if (sre_charset<SRE_CHAR>(state, set, lo))
       return 1;

    up = sre_upper_locale(state, ch);
    return up != lo && sre_charset<SRE_CHAR>(state, set, up);
}

template<typename SRE_CHAR>
inline ptrdiff_t sre_match(SRE_STATE* state, const SRE_CODE* pattern, int toplevel);

template<typename SRE_CHAR>
inline ptrdiff_t sre_count(SRE_STATE* state, const SRE_CODE* pattern, ptrdiff_t maxcount)
{
    SRE_CODE chr;
    SRE_CHAR c;
    const SRE_CHAR* ptr = (const SRE_CHAR *)state->ptr;
    const SRE_CHAR* end = (const SRE_CHAR *)state->end;
    ptrdiff_t i;

    /* adjust end */
    if (maxcount < end - ptr && maxcount != SRE_MAXREPEAT)
        end = ptr + maxcount;

    switch (pattern[0]) {

    case SRE_OP_IN:
        /* repeated set */
        while (ptr < end && sre_charset<SRE_CHAR>(state, pattern + 2, *ptr))
            ptr++;
        break;

    case SRE_OP_ANY:
        /* repeated dot wildcard. */
        while (ptr < end && !SRE_IS_LINEBREAK(*ptr))
            ptr++;
        break;

    case SRE_OP_ANY_ALL:
        /* repeated dot wildcard.  skip to the end of the target
           string, and backtrack from there */
        ptr = end;
        break;

    case SRE_OP_LITERAL:
        /* repeated literal */
        chr = pattern[1];
        c = (SRE_CHAR) chr;
        if ((SRE_CODE) c != chr)
            ; /* literal can't match: doesn't fit in char width */
        else
        while (ptr < end && *ptr == c)
            ptr++;
        break;

    case SRE_OP_LITERAL_IGNORE:
        /* repeated literal */
        chr = pattern[1];
        while (ptr < end && (SRE_CODE) sre_lower_ascii(*ptr) == chr)
            ptr++;
        break;

    case SRE_OP_LITERAL_UNI_IGNORE:
        /* repeated literal */
        chr = pattern[1];
        while (ptr < end && (SRE_CODE) sre_lower_unicode(*ptr) == chr)
            ptr++;
        break;

    case SRE_OP_LITERAL_LOC_IGNORE:
        /* repeated literal */
        chr = pattern[1];
        while (ptr < end && char_loc_ignore(state, chr, *ptr))
            ptr++;
        break;

    case SRE_OP_NOT_LITERAL:
        /* repeated non-literal */
        chr = pattern[1];
        c = (SRE_CHAR) chr;
        if ((SRE_CODE) c != chr)
            ptr = end; /* literal can't match: doesn't fit in char width */
        else
        while (ptr < end && *ptr != c)
            ptr++;
        break;

    case SRE_OP_NOT_LITERAL_IGNORE:
        /* repeated non-literal */
        chr = pattern[1];
        while (ptr < end && (SRE_CODE) sre_lower_ascii(*ptr) != chr)
            ptr++;
        break;

    case SRE_OP_NOT_LITERAL_UNI_IGNORE:
        /* repeated non-literal */
        chr = pattern[1];
        while (ptr < end && (SRE_CODE) sre_lower_unicode(*ptr) != chr)
            ptr++;
        break;

    case SRE_OP_NOT_LITERAL_LOC_IGNORE:
        /* repeated non-literal */
        chr = pattern[1];
        while (ptr < end && !char_loc_ignore(state, chr, *ptr))
            ptr++;
        break;

    default:
        /* repeated single character pattern */
        while ((SRE_CHAR*) state->ptr < end) {
            i = sre_match<SRE_CHAR>(state, pattern, 0);
            if (i < 0)
                return i;
            if (!i)
                break;
        }
        return (SRE_CHAR*) state->ptr - ptr;
    }

    return ptr - (SRE_CHAR*) state->ptr;
}

/* The macros below should be used to protect recursive sre_match<SRE_CHAR>()
 * calls that *failed* and do *not* return immediately (IOW, those
 * that will backtrack). Explaining:
 *
 * - Recursive sre_match<SRE_CHAR>() returned true: that's usually a success
 *   (besides atypical cases like ASSERT_NOT), therefore there's no
 *   reason to restore lastmark;
 *
 * - Recursive sre_match<SRE_CHAR>() returned false but the current sre_match<SRE_CHAR>()
 *   is returning to the caller: If the current sre_match<SRE_CHAR>() is the
 *   top function of the recursion, returning false will be a matching
 *   failure, and it doesn't matter where lastmark is pointing to.
 *   If it's *not* the top function, it will be a recursive sre_match<SRE_CHAR>()
 *   failure by itself, and the calling sre_match<SRE_CHAR>() will have to deal
 *   with the failure by the same rules explained here (it will restore
 *   lastmark by itself if necessary);
 *
 * - Recursive sre_match<SRE_CHAR>() returned false, and will continue the
 *   outside 'for' loop: must be protected when breaking, since the next
 *   OP could potentially depend on lastmark;
 *
 * - Recursive sre_match<SRE_CHAR>() returned false, and will be called again
 *   inside a local for/while loop: must be protected between each
 *   loop iteration, since the recursive sre_match<SRE_CHAR>() could do anything,
 *   and could potentially depend on lastmark.
 *
 * For more information, check the discussion at SF patch #712900.
 */
#define LASTMARK_SAVE()     \
    do { \
        ctx->lastmark = state->lastmark; \
        ctx->lastindex = state->lastindex; \
    } while (0)
#define LASTMARK_RESTORE()  \
    do { \
        state->lastmark = ctx->lastmark; \
        state->lastindex = ctx->lastindex; \
    } while (0)

#define LAST_PTR_PUSH()     \
    do { \
        DATA_PUSH(&ctx->u.rep->last_ptr); \
    } while (0)
#define LAST_PTR_POP()  \
    do { \
        DATA_POP(&ctx->u.rep->last_ptr); \
    } while (0)

#define RETURN_ERROR(i) do { return i; } while(0)
#define RETURN_FAILURE do { ret = 0; goto exit; } while(0)
#define RETURN_SUCCESS do { ret = 1; goto exit; } while(0)

#define RETURN_ON_ERROR(i) \
    do { if (i < 0) RETURN_ERROR(i); } while (0)
#define RETURN_ON_SUCCESS(i) \
    do { RETURN_ON_ERROR(i); if (i > 0) RETURN_SUCCESS; } while (0)
#define RETURN_ON_FAILURE(i) \
    do { RETURN_ON_ERROR(i); if (i == 0) RETURN_FAILURE; } while (0)

#define DATA_STACK_ALLOC(state, type, ptr) \
do { \
    alloc_pos = state->data_stack_base; \
    if (sizeof(type) > state->data_stack_size - alloc_pos) { \
        int j = data_stack_grow(state, sizeof(type)); \
        if (j < 0) return j; \
        if (ctx_pos != -1) \
            DATA_STACK_LOOKUP_AT(state, sre_match_context<SRE_CHAR>, ctx, ctx_pos); \
    } \
    ptr = (type*)(state->data_stack+alloc_pos); \
    state->data_stack_base += sizeof(type); \
} while (0)

#define DATA_STACK_LOOKUP_AT(state, type, ptr, pos) \
do { \
    ptr = (type*)(state->data_stack+pos); \
} while (0)

#define DATA_STACK_PUSH(state, data, size) \
do { \
    if (size > state->data_stack_size - state->data_stack_base) { \
        int j = data_stack_grow(state, size); \
        if (j < 0) return j; \
        if (ctx_pos != -1) \
            DATA_STACK_LOOKUP_AT(state, sre_match_context<SRE_CHAR>, ctx, ctx_pos); \
    } \
    memcpy(state->data_stack+state->data_stack_base, data, size); \
    state->data_stack_base += size; \
} while (0)

/* We add an explicit cast to memcpy here because MSVC has a bug when
   compiling C code where it believes that `const void**` cannot be
   safely casted to `void*`, see bpo-39943 for details. */
#define DATA_STACK_POP(state, data, size, discard) \
do { \
    memcpy((void*) data, state->data_stack+state->data_stack_base-size, size); \
    if (discard) \
        state->data_stack_base -= size; \
} while (0)

#define DATA_STACK_POP_DISCARD(state, size) \
do { \
    state->data_stack_base -= size; \
} while(0)

#define DATA_PUSH(x) \
    DATA_STACK_PUSH(state, (x), sizeof(*(x)))
#define DATA_POP(x) \
    DATA_STACK_POP(state, (x), sizeof(*(x)), 1)
#define DATA_POP_DISCARD(x) \
    DATA_STACK_POP_DISCARD(state, sizeof(*(x)))
#define DATA_ALLOC(t,p) \
    DATA_STACK_ALLOC(state, t, p)
#define DATA_LOOKUP_AT(t,p,pos) \
    DATA_STACK_LOOKUP_AT(state,t,p,pos)

#define PTR_TO_INDEX(ptr) \
    ((ptr) ? ((char*)(ptr) - (char*)state->beginning) / state->charsize : -1)

#define MARK_PUSH(lastmark) \
    do if (lastmark >= 0) { \
        size_t _marks_size = (lastmark+1) * sizeof(void*); \
        DATA_STACK_PUSH(state, state->mark, _marks_size); \
    } while (0)
#define MARK_POP(lastmark) \
    do if (lastmark >= 0) { \
        size_t _marks_size = (lastmark+1) * sizeof(void*); \
        DATA_STACK_POP(state, state->mark, _marks_size, 1); \
    } while (0)
#define MARK_POP_KEEP(lastmark) \
    do if (lastmark >= 0) { \
        size_t _marks_size = (lastmark+1) * sizeof(void*); \
        DATA_STACK_POP(state, state->mark, _marks_size, 0); \
    } while (0)
#define MARK_POP_DISCARD(lastmark) \
    do if (lastmark >= 0) { \
        size_t _marks_size = (lastmark+1) * sizeof(void*); \
        DATA_STACK_POP_DISCARD(state, _marks_size); \
    } while (0)

#define JUMP_NONE            0
#define JUMP_MAX_UNTIL_1     1
#define JUMP_MAX_UNTIL_2     2
#define JUMP_MAX_UNTIL_3     3
#define JUMP_MIN_UNTIL_1     4
#define JUMP_MIN_UNTIL_2     5
#define JUMP_MIN_UNTIL_3     6
#define JUMP_REPEAT          7
#define JUMP_REPEAT_ONE_1    8
#define JUMP_REPEAT_ONE_2    9
#define JUMP_MIN_REPEAT_ONE  10
#define JUMP_BRANCH          11
#define JUMP_ASSERT          12
#define JUMP_ASSERT_NOT      13
#define JUMP_POSS_REPEAT_1   14
#define JUMP_POSS_REPEAT_2   15
#define JUMP_ATOMIC_GROUP    16

#define DO_JUMPX(jumpvalue, jumplabel, nextpattern, toplevel_) \
    ctx->pattern = pattern; \
    ctx->ptr = ptr; \
    DATA_ALLOC(sre_match_context<SRE_CHAR>, nextctx); \
    nextctx->pattern = nextpattern; \
    nextctx->toplevel = toplevel_; \
    nextctx->jump = jumpvalue; \
    nextctx->last_ctx_pos = ctx_pos; \
    pattern = nextpattern; \
    ctx_pos = alloc_pos; \
    ctx = nextctx; \
    goto entrance; \
    jumplabel: \
    pattern = ctx->pattern; \
    ptr = ctx->ptr;

#define DO_JUMP(jumpvalue, jumplabel, nextpattern) \
    DO_JUMPX(jumpvalue, jumplabel, nextpattern, ctx->toplevel)

#define DO_JUMP0(jumpvalue, jumplabel, nextpattern) \
    DO_JUMPX(jumpvalue, jumplabel, nextpattern, 0)

template<typename SRE_CHAR>
struct sre_match_context {
    ptrdiff_t count;
    union {
        SRE_CODE chr;
        SRE_REPEAT* rep;
    } u;
    int lastmark;
    int lastindex;
    const SRE_CODE* pattern;
    const SRE_CHAR* ptr;
    int toplevel;
    int jump;
    ptrdiff_t last_ctx_pos;
};

#define _MAYBE_CHECK_SIGNALS                                       \
    do {                                                           \
        if ((0 == (++sigcount & 0xfff)) && state->checkSignals()) { \
            RETURN_ERROR(SRE_ERROR_INTERRUPTED);                   \
        }                                                          \
    } while (0)

# define MAYBE_CHECK_SIGNALS _MAYBE_CHECK_SIGNALS

    #define TARGET(OP) case OP
    #define DISPATCH goto dispatch

/* check if string matches the given pattern.  returns <0 for
   error, 0 for failure, and 1 for success */
template<typename SRE_CHAR>
inline ptrdiff_t sre_match(SRE_STATE* state, const SRE_CODE* pattern, int toplevel)
{
    const SRE_CHAR* end = (const SRE_CHAR *)state->end;
    ptrdiff_t alloc_pos, ctx_pos = -1;
    ptrdiff_t ret = 0;
    int jump;
    const SRE_CHAR* ptr;
    unsigned int sigcount = state->sigcount;

    sre_match_context<SRE_CHAR>* ctx;
    sre_match_context<SRE_CHAR>* nextctx;
    SRE_REPEAT* repeat_of_tail;


    DATA_ALLOC(sre_match_context<SRE_CHAR>, ctx);
    ctx->last_ctx_pos = -1;
    ctx->jump = JUMP_NONE;
    ctx->toplevel = toplevel;
    ctx_pos = alloc_pos;


entrance:

    ptr = (const SRE_CHAR *)state->ptr;

    if (pattern[0] == SRE_OP_INFO) {
        /* optimization info block */
        /* <INFO> <1=skip> <2=flags> <3=min> ... */
        if (pattern[3] && (uintptr_t)(end - ptr) < pattern[3]) {
            RETURN_FAILURE;
        }
        pattern += pattern[1] + 1;
    }

dispatch:
    MAYBE_CHECK_SIGNALS;
    switch (*pattern++)
    {

        TARGET(SRE_OP_MARK):
            /* set mark */
            /* <MARK> <gid> */
            {
                int i = pattern[0];
                if (i & 1)
                    state->lastindex = i/2 + 1;
                if (i > state->lastmark) {
                    /* state->lastmark is the highest valid index in the
                       state->mark array.  If it is increased by more than 1,
                       the intervening marks must be set to nullptr to signal
                       that these marks have not been encountered. */
                    int j = state->lastmark + 1;
                    while (j < i)
                        state->mark[j++] = nullptr;
                    state->lastmark = i;
                }
                state->mark[i] = ptr;
            }
            pattern++;
            DISPATCH;

        TARGET(SRE_OP_LITERAL):
            /* match literal string */
            /* <LITERAL> <code> */
            if (ptr >= end || (SRE_CODE) ptr[0] != pattern[0])
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_NOT_LITERAL):
            /* match anything that is not literal character */
            /* <NOT_LITERAL> <code> */
            if (ptr >= end || (SRE_CODE) ptr[0] == pattern[0])
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_SUCCESS):
            /* end of pattern */
            if (ctx->toplevel &&
                ((state->match_all && ptr != state->end) ||
                 (state->must_advance && ptr == state->start)))
            {
                RETURN_FAILURE;
            }
            state->ptr = ptr;
            RETURN_SUCCESS;

        TARGET(SRE_OP_AT):
            /* match at given position */
            /* <AT> <code> */
            if (!sre_at<SRE_CHAR>(state, ptr, *pattern))
                RETURN_FAILURE;
            pattern++;
            DISPATCH;

        TARGET(SRE_OP_CATEGORY):
            /* match at given category */
            /* <CATEGORY> <code> */
            if (ptr >= end || !sre_category(state, pattern[0], ptr[0]))
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_ANY):
            /* match anything (except a newline) */
            /* <ANY> */
            if (ptr >= end || SRE_IS_LINEBREAK(ptr[0]))
                RETURN_FAILURE;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_ANY_ALL):
            /* match anything */
            /* <ANY_ALL> */
            if (ptr >= end)
                RETURN_FAILURE;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_IN):
            /* match set member (or non_member) */
            /* <IN> <skip> <set> */
            if (ptr >= end ||
                !sre_charset<SRE_CHAR>(state, pattern + 1, *ptr))
                RETURN_FAILURE;
            pattern += pattern[0];
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_LITERAL_IGNORE):
            if (ptr >= end ||
                sre_lower_ascii(*ptr) != *pattern)
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_LITERAL_UNI_IGNORE):
            if (ptr >= end ||
                sre_lower_unicode(*ptr) != *pattern)
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_LITERAL_LOC_IGNORE):
            if (ptr >= end
                || !char_loc_ignore(state, *pattern, *ptr))
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_NOT_LITERAL_IGNORE):
            if (ptr >= end ||
                sre_lower_ascii(*ptr) == *pattern)
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_NOT_LITERAL_UNI_IGNORE):
            if (ptr >= end ||
                sre_lower_unicode(*ptr) == *pattern)
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_NOT_LITERAL_LOC_IGNORE):
            if (ptr >= end
                || char_loc_ignore(state, *pattern, *ptr))
                RETURN_FAILURE;
            pattern++;
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_IN_IGNORE):
            if (ptr >= end
                || !sre_charset<SRE_CHAR>(state, pattern+1,
                                 (SRE_CODE)sre_lower_ascii(*ptr)))
                RETURN_FAILURE;
            pattern += pattern[0];
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_IN_UNI_IGNORE):
            if (ptr >= end
                || !sre_charset<SRE_CHAR>(state, pattern+1,
                                 (SRE_CODE)sre_lower_unicode(*ptr)))
                RETURN_FAILURE;
            pattern += pattern[0];
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_IN_LOC_IGNORE):
            if (ptr >= end
                || !sre_charset_loc_ignore<SRE_CHAR>(state, pattern+1, *ptr))
                RETURN_FAILURE;
            pattern += pattern[0];
            ptr++;
            DISPATCH;

        TARGET(SRE_OP_JUMP):
        TARGET(SRE_OP_INFO):
            /* jump forward */
            /* <JUMP> <offset> */
            pattern += pattern[0];
            DISPATCH;

        TARGET(SRE_OP_BRANCH):
            /* alternation */
            /* <BRANCH> <0=skip> code <JUMP> ... <nullptr> */
            LASTMARK_SAVE();
            if (state->repeat)
                MARK_PUSH(ctx->lastmark);
            for (; pattern[0]; pattern += pattern[0]) {
                if (pattern[1] == SRE_OP_LITERAL &&
                    (ptr >= end ||
                     (SRE_CODE) *ptr != pattern[2]))
                    continue;
                if (pattern[1] == SRE_OP_IN &&
                    (ptr >= end ||
                     !sre_charset<SRE_CHAR>(state, pattern + 3,
                                   (SRE_CODE) *ptr)))
                    continue;
                state->ptr = ptr;
                DO_JUMP(JUMP_BRANCH, jump_branch, pattern+1);
                if (ret) {
                    if (state->repeat)
                        MARK_POP_DISCARD(ctx->lastmark);
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                if (state->repeat)
                    MARK_POP_KEEP(ctx->lastmark);
                LASTMARK_RESTORE();
            }
            if (state->repeat)
                MARK_POP_DISCARD(ctx->lastmark);
            RETURN_FAILURE;

        TARGET(SRE_OP_REPEAT_ONE):
            /* match repeated sequence (maximizing regexp) */

            /* this operator only works if the repeated item is
               exactly one character wide, and we're not already
               collecting backtracking points.  for other cases,
               use the MAX_REPEAT operator */

            /* <REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail */


            if ((ptrdiff_t) pattern[1] > end - ptr)
                RETURN_FAILURE; /* cannot match */

            state->ptr = ptr;

            ret = sre_count<SRE_CHAR>(state, pattern+3, pattern[2]);
            RETURN_ON_ERROR(ret);
            DATA_LOOKUP_AT(sre_match_context<SRE_CHAR>, ctx, ctx_pos);
            ctx->count = ret;
            ptr += ctx->count;

            /* when we arrive here, count contains the number of
               matches, and ptr points to the tail of the target
               string.  check if the rest of the pattern matches,
               and backtrack if not. */

            if (ctx->count < (ptrdiff_t) pattern[1])
                RETURN_FAILURE;

            if (pattern[pattern[0]] == SRE_OP_SUCCESS &&
                ptr == state->end &&
                !(ctx->toplevel && state->must_advance && ptr == state->start))
            {
                /* tail is empty.  we're finished */
                state->ptr = ptr;
                RETURN_SUCCESS;
            }

            LASTMARK_SAVE();
            if (state->repeat)
                MARK_PUSH(ctx->lastmark);

            if (pattern[pattern[0]] == SRE_OP_LITERAL) {
                /* tail starts with a literal. skip positions where
                   the rest of the pattern cannot possibly match */
                ctx->u.chr = pattern[pattern[0]+1];
                for (;;) {
                    while (ctx->count >= (ptrdiff_t) pattern[1] &&
                           (ptr >= end || *ptr != ctx->u.chr)) {
                        ptr--;
                        ctx->count--;
                    }
                    if (ctx->count < (ptrdiff_t) pattern[1])
                        break;
                    state->ptr = ptr;
                    DO_JUMP(JUMP_REPEAT_ONE_1, jump_repeat_one_1,
                            pattern+pattern[0]);
                    if (ret) {
                        if (state->repeat)
                            MARK_POP_DISCARD(ctx->lastmark);
                        RETURN_ON_ERROR(ret);
                        RETURN_SUCCESS;
                    }
                    if (state->repeat)
                        MARK_POP_KEEP(ctx->lastmark);
                    LASTMARK_RESTORE();

                    ptr--;
                    ctx->count--;
                }
                if (state->repeat)
                    MARK_POP_DISCARD(ctx->lastmark);
            } else {
                /* general case */
                while (ctx->count >= (ptrdiff_t) pattern[1]) {
                    state->ptr = ptr;
                    DO_JUMP(JUMP_REPEAT_ONE_2, jump_repeat_one_2,
                            pattern+pattern[0]);
                    if (ret) {
                        if (state->repeat)
                            MARK_POP_DISCARD(ctx->lastmark);
                        RETURN_ON_ERROR(ret);
                        RETURN_SUCCESS;
                    }
                    if (state->repeat)
                        MARK_POP_KEEP(ctx->lastmark);
                    LASTMARK_RESTORE();

                    ptr--;
                    ctx->count--;
                }
                if (state->repeat)
                    MARK_POP_DISCARD(ctx->lastmark);
            }
            RETURN_FAILURE;

        TARGET(SRE_OP_MIN_REPEAT_ONE):
            /* match repeated sequence (minimizing regexp) */

            /* this operator only works if the repeated item is
               exactly one character wide, and we're not already
               collecting backtracking points.  for other cases,
               use the MIN_REPEAT operator */

            /* <MIN_REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail */


            if ((ptrdiff_t) pattern[1] > end - ptr)
                RETURN_FAILURE; /* cannot match */

            state->ptr = ptr;

            if (pattern[1] == 0)
                ctx->count = 0;
            else {
                /* count using pattern min as the maximum */
                ret = sre_count<SRE_CHAR>(state, pattern+3, pattern[1]);
                RETURN_ON_ERROR(ret);
                DATA_LOOKUP_AT(sre_match_context<SRE_CHAR>, ctx, ctx_pos);
                if (ret < (ptrdiff_t) pattern[1])
                    /* didn't match minimum number of times */
                    RETURN_FAILURE;
                /* advance past minimum matches of repeat */
                ctx->count = ret;
                ptr += ctx->count;
            }

            if (pattern[pattern[0]] == SRE_OP_SUCCESS &&
                !(ctx->toplevel &&
                  ((state->match_all && ptr != state->end) ||
                   (state->must_advance && ptr == state->start))))
            {
                /* tail is empty.  we're finished */
                state->ptr = ptr;
                RETURN_SUCCESS;

            } else {
                /* general case */
                LASTMARK_SAVE();
                if (state->repeat)
                    MARK_PUSH(ctx->lastmark);

                while ((ptrdiff_t)pattern[2] == SRE_MAXREPEAT
                       || ctx->count <= (ptrdiff_t)pattern[2]) {
                    state->ptr = ptr;
                    DO_JUMP(JUMP_MIN_REPEAT_ONE,jump_min_repeat_one,
                            pattern+pattern[0]);
                    if (ret) {
                        if (state->repeat)
                            MARK_POP_DISCARD(ctx->lastmark);
                        RETURN_ON_ERROR(ret);
                        RETURN_SUCCESS;
                    }
                    if (state->repeat)
                        MARK_POP_KEEP(ctx->lastmark);
                    LASTMARK_RESTORE();

                    state->ptr = ptr;
                    ret = sre_count<SRE_CHAR>(state, pattern+3, 1);
                    RETURN_ON_ERROR(ret);
                    DATA_LOOKUP_AT(sre_match_context<SRE_CHAR>, ctx, ctx_pos);
                    if (ret == 0)
                        break;
                    ASSERT(ret == 1);
                    ptr++;
                    ctx->count++;
                }
                if (state->repeat)
                    MARK_POP_DISCARD(ctx->lastmark);
            }
            RETURN_FAILURE;

        TARGET(SRE_OP_POSSESSIVE_REPEAT_ONE):
            /* match repeated sequence (maximizing regexp) without
               backtracking */

            /* this operator only works if the repeated item is
               exactly one character wide, and we're not already
               collecting backtracking points.  for other cases,
               use the MAX_REPEAT operator */

            /* <POSSESSIVE_REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS>
               tail */


            if (ptr + pattern[1] > end) {
                RETURN_FAILURE; /* cannot match */
            }

            state->ptr = ptr;

            ret = sre_count<SRE_CHAR>(state, pattern + 3, pattern[2]);
            RETURN_ON_ERROR(ret);
            DATA_LOOKUP_AT(sre_match_context<SRE_CHAR>, ctx, ctx_pos);
            ctx->count = ret;
            ptr += ctx->count;

            /* when we arrive here, count contains the number of
               matches, and ptr points to the tail of the target
               string.  check if the rest of the pattern matches,
               and fail if not. */

            /* Test for not enough repetitions in match */
            if (ctx->count < (ptrdiff_t) pattern[1]) {
                RETURN_FAILURE;
            }

            /* Update the pattern to point to the next op code */
            pattern += pattern[0];

            /* Let the tail be evaluated separately and consider this
               match successful. */
            if (*pattern == SRE_OP_SUCCESS &&
                ptr == state->end &&
                !(ctx->toplevel && state->must_advance && ptr == state->start))
            {
                /* tail is empty.  we're finished */
                state->ptr = ptr;
                RETURN_SUCCESS;
            }

            /* Attempt to match the rest of the string */
            DISPATCH;

        TARGET(SRE_OP_REPEAT):
            /* create repeat context.  all the hard work is done
               by the UNTIL operator (MAX_UNTIL, MIN_UNTIL) */
            /* <REPEAT> <skip> <1=min> <2=max>
               <3=repeat_index> item <UNTIL> tail */

            /* install new repeat context */
            ctx->u.rep = repeat_pool_malloc(state);
            if (!ctx->u.rep) {
                RETURN_ERROR(SRE_ERROR_MEMORY);
            }
            ctx->u.rep->count = -1;
            ctx->u.rep->pattern = pattern;
            ctx->u.rep->prev = state->repeat;
            ctx->u.rep->last_ptr = nullptr;
            state->repeat = ctx->u.rep;

            state->ptr = ptr;
            DO_JUMP(JUMP_REPEAT, jump_repeat, pattern+pattern[0]);
            state->repeat = ctx->u.rep->prev;
            repeat_pool_free(state, ctx->u.rep);

            if (ret) {
                RETURN_ON_ERROR(ret);
                RETURN_SUCCESS;
            }
            RETURN_FAILURE;

        TARGET(SRE_OP_MAX_UNTIL):
            /* maximizing repeat */
            /* <REPEAT> <skip> <1=min> <2=max> item <MAX_UNTIL> tail */

            /* FIXME: we probably need to deal with zero-width
               matches in here... */

            ctx->u.rep = state->repeat;
            if (!ctx->u.rep)
                RETURN_ERROR(SRE_ERROR_STATE);

            state->ptr = ptr;

            ctx->count = ctx->u.rep->count+1;


            if (ctx->count < (ptrdiff_t) ctx->u.rep->pattern[1]) {
                /* not enough matches */
                ctx->u.rep->count = ctx->count;
                DO_JUMP(JUMP_MAX_UNTIL_1, jump_max_until_1,
                        ctx->u.rep->pattern+3);
                if (ret) {
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                ctx->u.rep->count = ctx->count-1;
                state->ptr = ptr;
                RETURN_FAILURE;
            }

            if ((ctx->count < (ptrdiff_t) ctx->u.rep->pattern[2] ||
                ctx->u.rep->pattern[2] == SRE_MAXREPEAT) &&
                state->ptr != ctx->u.rep->last_ptr) {
                /* we may have enough matches, but if we can
                   match another item, do so */
                ctx->u.rep->count = ctx->count;
                LASTMARK_SAVE();
                MARK_PUSH(ctx->lastmark);
                /* zero-width match protection */
                LAST_PTR_PUSH();
                ctx->u.rep->last_ptr = state->ptr;
                DO_JUMP(JUMP_MAX_UNTIL_2, jump_max_until_2,
                        ctx->u.rep->pattern+3);
                LAST_PTR_POP();
                if (ret) {
                    MARK_POP_DISCARD(ctx->lastmark);
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                MARK_POP(ctx->lastmark);
                LASTMARK_RESTORE();
                ctx->u.rep->count = ctx->count-1;
                state->ptr = ptr;
            }

            /* cannot match more repeated items here.  make sure the
               tail matches */
            state->repeat = ctx->u.rep->prev;
            DO_JUMP(JUMP_MAX_UNTIL_3, jump_max_until_3, pattern);
            state->repeat = ctx->u.rep; // restore repeat before return

            RETURN_ON_SUCCESS(ret);
            state->ptr = ptr;
            RETURN_FAILURE;

        TARGET(SRE_OP_MIN_UNTIL):
            /* minimizing repeat */
            /* <REPEAT> <skip> <1=min> <2=max> item <MIN_UNTIL> tail */

            ctx->u.rep = state->repeat;
            if (!ctx->u.rep)
                RETURN_ERROR(SRE_ERROR_STATE);

            state->ptr = ptr;

            ctx->count = ctx->u.rep->count+1;


            if (ctx->count < (ptrdiff_t) ctx->u.rep->pattern[1]) {
                /* not enough matches */
                ctx->u.rep->count = ctx->count;
                DO_JUMP(JUMP_MIN_UNTIL_1, jump_min_until_1,
                        ctx->u.rep->pattern+3);
                if (ret) {
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                ctx->u.rep->count = ctx->count-1;
                state->ptr = ptr;
                RETURN_FAILURE;
            }

            /* see if the tail matches */
            state->repeat = ctx->u.rep->prev;

            LASTMARK_SAVE();
            if (state->repeat)
                MARK_PUSH(ctx->lastmark);

            DO_JUMP(JUMP_MIN_UNTIL_2, jump_min_until_2, pattern);
            repeat_of_tail = state->repeat;
            state->repeat = ctx->u.rep; // restore repeat before return

            if (ret) {
                if (repeat_of_tail)
                    MARK_POP_DISCARD(ctx->lastmark);
                RETURN_ON_ERROR(ret);
                RETURN_SUCCESS;
            }
            if (repeat_of_tail)
                MARK_POP(ctx->lastmark);
            LASTMARK_RESTORE();

            state->ptr = ptr;

            if ((ctx->count >= (ptrdiff_t) ctx->u.rep->pattern[2]
                && ctx->u.rep->pattern[2] != SRE_MAXREPEAT) ||
                state->ptr == ctx->u.rep->last_ptr)
                RETURN_FAILURE;

            ctx->u.rep->count = ctx->count;
            /* zero-width match protection */
            LAST_PTR_PUSH();
            ctx->u.rep->last_ptr = state->ptr;
            DO_JUMP(JUMP_MIN_UNTIL_3,jump_min_until_3,
                    ctx->u.rep->pattern+3);
            LAST_PTR_POP();
            if (ret) {
                RETURN_ON_ERROR(ret);
                RETURN_SUCCESS;
            }
            ctx->u.rep->count = ctx->count-1;
            state->ptr = ptr;
            RETURN_FAILURE;

        TARGET(SRE_OP_POSSESSIVE_REPEAT):
            /* create possessive repeat contexts. */
            /* <POSSESSIVE_REPEAT> <skip> <1=min> <2=max> pattern
               <SUCCESS> tail */

            /* Set the global Input pointer to this context's Input
               pointer */
            state->ptr = ptr;

            /* Set state->repeat to non-nullptr */
            ctx->u.rep = repeat_pool_malloc(state);
            if (!ctx->u.rep) {
                RETURN_ERROR(SRE_ERROR_MEMORY);
            }
            ctx->u.rep->count = -1;
            ctx->u.rep->pattern = nullptr;
            ctx->u.rep->prev = state->repeat;
            ctx->u.rep->last_ptr = nullptr;
            state->repeat = ctx->u.rep;

            /* Initialize Count to 0 */
            ctx->count = 0;

            /* Check for minimum required matches. */
            while (ctx->count < (ptrdiff_t)pattern[1]) {
                /* not enough matches */
                DO_JUMP0(JUMP_POSS_REPEAT_1, jump_poss_repeat_1,
                         &pattern[3]);
                if (ret) {
                    RETURN_ON_ERROR(ret);
                    ctx->count++;
                }
                else {
                    state->ptr = ptr;
                    /* Restore state->repeat */
                    state->repeat = ctx->u.rep->prev;
                    repeat_pool_free(state, ctx->u.rep);
                    RETURN_FAILURE;
                }
            }

            /* Clear the context's Input stream pointer so that it
               doesn't match the global state so that the while loop can
               be entered. */
            ptr = nullptr;

            /* Keep trying to parse the <pattern> sub-pattern until the
               end is reached, creating a new context each time. */
            while ((ctx->count < (ptrdiff_t)pattern[2] ||
                    (ptrdiff_t)pattern[2] == SRE_MAXREPEAT) &&
                   state->ptr != ptr) {
                /* Save the Capture Group Marker state into the current
                   Context and back up the current highest number
                   Capture Group marker. */
                LASTMARK_SAVE();
                MARK_PUSH(ctx->lastmark);

                /* zero-width match protection */
                /* Set the context's Input Stream pointer to be the
                   current Input Stream pointer from the global
                   state.  When the loop reaches the next iteration,
                   the context will then store the last known good
                   position with the global state holding the Input
                   Input Stream position that has been updated with
                   the most recent match.  Thus, if state's Input
                   stream remains the same as the one stored in the
                   current Context, we know we have successfully
                   matched an empty string and that all subsequent
                   matches will also be the empty string until the
                   maximum number of matches are counted, and because
                   of this, we could immediately stop at that point and
                   consider this match successful. */
                ptr = (const SRE_CHAR *)state->ptr;

                /* We have not reached the maximin matches, so try to
                   match once more. */
                DO_JUMP0(JUMP_POSS_REPEAT_2, jump_poss_repeat_2,
                         &pattern[3]);

                /* Check to see if the last attempted match
                   succeeded. */
                if (ret) {
                    /* Drop the saved highest number Capture Group
                       marker saved above and use the newly updated
                       value. */
                    MARK_POP_DISCARD(ctx->lastmark);
                    RETURN_ON_ERROR(ret);

                    /* Success, increment the count. */
                    ctx->count++;
                }
                /* Last attempted match failed. */
                else {
                    /* Restore the previously saved highest number
                       Capture Group marker since the last iteration
                       did not match, then restore that to the global
                       state. */
                    MARK_POP(ctx->lastmark);
                    LASTMARK_RESTORE();

                    /* Restore the global Input Stream pointer
                       since it can change after jumps. */
                    state->ptr = ptr;

                    /* We have sufficient matches, so exit loop. */
                    break;
                }
            }

            /* Restore state->repeat */
            state->repeat = ctx->u.rep->prev;
            repeat_pool_free(state, ctx->u.rep);

            /* Evaluate Tail */
            /* Jump to end of pattern indicated by skip, and then skip
               the SUCCESS op code that follows it. */
            pattern += pattern[0] + 1;
            ptr = (const SRE_CHAR *)state->ptr;
            DISPATCH;

        TARGET(SRE_OP_ATOMIC_GROUP):
            /* Atomic Group Sub Pattern */
            /* <ATOMIC_GROUP> <skip> pattern <SUCCESS> tail */

            /* Set the global Input pointer to this context's Input
               pointer */
            state->ptr = ptr;

            /* Evaluate the Atomic Group in a new context, terminating
               when the end of the group, represented by a SUCCESS op
               code, is reached. */
            /* Group Pattern begins at an offset of 1 code. */
            DO_JUMP0(JUMP_ATOMIC_GROUP, jump_atomic_group,
                     &pattern[1]);

            /* Test Exit Condition */
            RETURN_ON_ERROR(ret);

            if (ret == 0) {
                /* Atomic Group failed to Match. */
                state->ptr = ptr;
                RETURN_FAILURE;
            }

            /* Evaluate Tail */
            /* Jump to end of pattern indicated by skip, and then skip
               the SUCCESS op code that follows it. */
            pattern += pattern[0];
            ptr = (const SRE_CHAR *)state->ptr;
            DISPATCH;

        TARGET(SRE_OP_GROUPREF):
            /* match backreference */
            {
                int groupref = pattern[0] * 2;
                if (groupref >= state->lastmark) {
                    RETURN_FAILURE;
                } else {
                    SRE_CHAR* p = (SRE_CHAR*) state->mark[groupref];
                    SRE_CHAR* e = (SRE_CHAR*) state->mark[groupref+1];
                    if (!p || !e || e < p)
                        RETURN_FAILURE;
                    while (p < e) {
                        if (ptr >= end || *ptr != *p)
                            RETURN_FAILURE;
                        p++;
                        ptr++;
                    }
                }
            }
            pattern++;
            DISPATCH;

        TARGET(SRE_OP_GROUPREF_IGNORE):
            /* match backreference */
            {
                int groupref = pattern[0] * 2;
                if (groupref >= state->lastmark) {
                    RETURN_FAILURE;
                } else {
                    SRE_CHAR* p = (SRE_CHAR*) state->mark[groupref];
                    SRE_CHAR* e = (SRE_CHAR*) state->mark[groupref+1];
                    if (!p || !e || e < p)
                        RETURN_FAILURE;
                    while (p < e) {
                        if (ptr >= end ||
                            sre_lower_ascii(*ptr) != sre_lower_ascii(*p))
                            RETURN_FAILURE;
                        p++;
                        ptr++;
                    }
                }
            }
            pattern++;
            DISPATCH;

        TARGET(SRE_OP_GROUPREF_UNI_IGNORE):
            /* match backreference */
            {
                int groupref = pattern[0] * 2;
                if (groupref >= state->lastmark) {
                    RETURN_FAILURE;
                } else {
                    SRE_CHAR* p = (SRE_CHAR*) state->mark[groupref];
                    SRE_CHAR* e = (SRE_CHAR*) state->mark[groupref+1];
                    if (!p || !e || e < p)
                        RETURN_FAILURE;
                    while (p < e) {
                        if (ptr >= end ||
                            sre_lower_unicode(*ptr) != sre_lower_unicode(*p))
                            RETURN_FAILURE;
                        p++;
                        ptr++;
                    }
                }
            }
            pattern++;
            DISPATCH;

        TARGET(SRE_OP_GROUPREF_LOC_IGNORE):
            /* match backreference */
            {
                int groupref = pattern[0] * 2;
                if (groupref >= state->lastmark) {
                    RETURN_FAILURE;
                } else {
                    SRE_CHAR* p = (SRE_CHAR*) state->mark[groupref];
                    SRE_CHAR* e = (SRE_CHAR*) state->mark[groupref+1];
                    if (!p || !e || e < p)
                        RETURN_FAILURE;
                    while (p < e) {
                        if (ptr >= end ||
                            sre_lower_locale(state, *ptr) != sre_lower_locale(state, *p))
                            RETURN_FAILURE;
                        p++;
                        ptr++;
                    }
                }
            }
            pattern++;
            DISPATCH;

        TARGET(SRE_OP_GROUPREF_EXISTS):
            /* <GROUPREF_EXISTS> <group> <skip> codeyes <JUMP> codeno ... */
            {
                int groupref = pattern[0] * 2;
                if (groupref >= state->lastmark) {
                    pattern += pattern[1];
                    DISPATCH;
                } else {
                    SRE_CHAR* p = (SRE_CHAR*) state->mark[groupref];
                    SRE_CHAR* e = (SRE_CHAR*) state->mark[groupref+1];
                    if (!p || !e || e < p) {
                        pattern += pattern[1];
                        DISPATCH;
                    }
                }
            }
            pattern += 2;
            DISPATCH;

        TARGET(SRE_OP_ASSERT):
            /* assert subpattern */
            /* <ASSERT> <skip> <back> <pattern> */
            if ((uintptr_t)(ptr - (SRE_CHAR *)state->beginning) < pattern[1])
                RETURN_FAILURE;
            state->ptr = ptr - pattern[1];
            DO_JUMP0(JUMP_ASSERT, jump_assert, pattern+2);
            RETURN_ON_FAILURE(ret);
            pattern += pattern[0];
            DISPATCH;

        TARGET(SRE_OP_ASSERT_NOT):
            /* assert not subpattern */
            /* <ASSERT_NOT> <skip> <back> <pattern> */
            if ((uintptr_t)(ptr - (SRE_CHAR *)state->beginning) >= pattern[1]) {
                state->ptr = ptr - pattern[1];
                LASTMARK_SAVE();
                if (state->repeat)
                    MARK_PUSH(ctx->lastmark);

                DO_JUMP0(JUMP_ASSERT_NOT, jump_assert_not, pattern+2);
                if (ret) {
                    if (state->repeat)
                        MARK_POP_DISCARD(ctx->lastmark);
                    RETURN_ON_ERROR(ret);
                    RETURN_FAILURE;
                }
                if (state->repeat)
                    MARK_POP(ctx->lastmark);
                LASTMARK_RESTORE();
            }
            pattern += pattern[0];
            DISPATCH;

        TARGET(SRE_OP_FAILURE):
            /* immediate failure */
            RETURN_FAILURE;

        default:
        // Also any unused opcodes:
        TARGET(SRE_OP_RANGE_UNI_IGNORE):
        TARGET(SRE_OP_SUBPATTERN):
        TARGET(SRE_OP_RANGE):
        TARGET(SRE_OP_NEGATE):
        TARGET(SRE_OP_BIGCHARSET):
        TARGET(SRE_OP_CHARSET):
            RETURN_ERROR(SRE_ERROR_ILLEGAL);

    }

exit:
    ctx_pos = ctx->last_ctx_pos;
    jump = ctx->jump;
    DATA_POP_DISCARD(ctx);
    if (ctx_pos == -1) {
        state->sigcount = sigcount;
        return ret;
    }
    DATA_LOOKUP_AT(sre_match_context<SRE_CHAR>, ctx, ctx_pos);

    switch (jump) {
        case JUMP_MAX_UNTIL_2:
            goto jump_max_until_2;
        case JUMP_MAX_UNTIL_3:
            goto jump_max_until_3;
        case JUMP_MIN_UNTIL_2:
            goto jump_min_until_2;
        case JUMP_MIN_UNTIL_3:
            goto jump_min_until_3;
        case JUMP_BRANCH:
            goto jump_branch;
        case JUMP_MAX_UNTIL_1:
            goto jump_max_until_1;
        case JUMP_MIN_UNTIL_1:
            goto jump_min_until_1;
        case JUMP_POSS_REPEAT_1:
            goto jump_poss_repeat_1;
        case JUMP_POSS_REPEAT_2:
            goto jump_poss_repeat_2;
        case JUMP_REPEAT:
            goto jump_repeat;
        case JUMP_REPEAT_ONE_1:
            goto jump_repeat_one_1;
        case JUMP_REPEAT_ONE_2:
            goto jump_repeat_one_2;
        case JUMP_MIN_REPEAT_ONE:
            goto jump_min_repeat_one;
        case JUMP_ATOMIC_GROUP:
            goto jump_atomic_group;
        case JUMP_ASSERT:
            goto jump_assert;
        case JUMP_ASSERT_NOT:
            goto jump_assert_not;
        case JUMP_NONE:
            break;
    }

    return ret; /* should never get here */
}

/* need to reset capturing groups between two sre_match<SRE_CHAR> callings in loops */
#define RESET_CAPTURE_GROUP() \
    do { state->lastmark = state->lastindex = -1; } while (0)

template<typename SRE_CHAR>
inline ptrdiff_t sre_search(SRE_STATE* state, SRE_CODE* pattern)
{
    SRE_CHAR* ptr = (SRE_CHAR *)state->start;
    SRE_CHAR* end = (SRE_CHAR *)state->end;
    ptrdiff_t status = 0;
    ptrdiff_t prefix_len = 0;
    ptrdiff_t prefix_skip = 0;
    SRE_CODE* prefix = nullptr;
    SRE_CODE* charset = nullptr;
    SRE_CODE* overlap = nullptr;
    int flags = 0;

    if (ptr > end)
        return 0;

    if (pattern[0] == SRE_OP_INFO) {
        /* optimization info block */
        /* <INFO> <1=skip> <2=flags> <3=min> <4=max> <5=prefix info>  */

        flags = pattern[2];

        if (pattern[3] && (uintptr_t)(end - ptr) < pattern[3]) {
            return 0;
        }
        if (pattern[3] > 1) {
            /* adjust end point (but make sure we leave at least one
               character in there, so literal search will work) */
            end -= pattern[3] - 1;
            if (end <= ptr)
                end = ptr;
        }

        if (flags & SRE_INFO_PREFIX) {
            /* pattern starts with a known prefix */
            /* <length> <skip> <prefix data> <overlap data> */
            prefix_len = pattern[5];
            prefix_skip = pattern[6];
            prefix = pattern + 7;
            overlap = prefix + prefix_len - 1;
        } else if (flags & SRE_INFO_CHARSET)
            /* pattern starts with a character from a known set */
            /* <charset> */
            charset = pattern + 5;

        pattern += 1 + pattern[1];
    }


    if (prefix_len == 1) {
        /* pattern starts with a literal character */
        SRE_CHAR c = (SRE_CHAR) prefix[0];
        if ((SRE_CODE) c != prefix[0])
            return 0; /* literal can't match: doesn't fit in char width */
        end = (SRE_CHAR *)state->end;
        state->must_advance = 0;
        while (ptr < end) {
            while (*ptr != c) {
                if (++ptr >= end)
                    return 0;
            }
            state->start = ptr;
            state->ptr = ptr + prefix_skip;
            if (flags & SRE_INFO_LITERAL)
                return 1; /* we got all of it */
            status = sre_match<SRE_CHAR>(state, pattern + 2*prefix_skip, 0);
            if (status != 0)
                return status;
            ++ptr;
            RESET_CAPTURE_GROUP();
        }
        return 0;
    }

    if (prefix_len > 1) {
        /* pattern starts with a known prefix.  use the overlap
           table to skip forward as fast as we possibly can */
        ptrdiff_t i = 0;

        end = (SRE_CHAR *)state->end;
        if (prefix_len > end - ptr)
            return 0;
        for (i = 0; i < prefix_len; i++)
            if ((SRE_CODE)(SRE_CHAR) prefix[i] != prefix[i])
                return 0; /* literal can't match: doesn't fit in char width */
        while (ptr < end) {
            SRE_CHAR c = (SRE_CHAR) prefix[0];
            while (*ptr++ != c) {
                if (ptr >= end)
                    return 0;
            }
            if (ptr >= end)
                return 0;

            i = 1;
            state->must_advance = 0;
            do {
                if (*ptr == (SRE_CHAR) prefix[i]) {
                    if (++i != prefix_len) {
                        if (++ptr >= end)
                            return 0;
                        continue;
                    }
                    /* found a potential match */
                    state->start = ptr - (prefix_len - 1);
                    state->ptr = ptr - (prefix_len - prefix_skip - 1);
                    if (flags & SRE_INFO_LITERAL)
                        return 1; /* we got all of it */
                    status = sre_match<SRE_CHAR>(state, pattern + 2*prefix_skip, 0);
                    if (status != 0)
                        return status;
                    /* close but no cigar -- try again */
                    if (++ptr >= end)
                        return 0;
                    RESET_CAPTURE_GROUP();
                }
                i = overlap[i];
            } while (i != 0);
        }
        return 0;
    }

    if (charset) {
        /* pattern starts with a character from a known set */
        end = (SRE_CHAR *)state->end;
        state->must_advance = 0;
        for (;;) {
            while (ptr < end && !sre_charset<SRE_CHAR>(state, charset, *ptr))
                ptr++;
            if (ptr >= end)
                return 0;
            state->start = ptr;
            state->ptr = ptr;
            status = sre_match<SRE_CHAR>(state, pattern, 0);
            if (status != 0)
                break;
            ptr++;
            RESET_CAPTURE_GROUP();
        }
    } else {
        /* general case */
        ASSERT(ptr <= end);
        state->start = state->ptr = ptr;
        status = sre_match<SRE_CHAR>(state, pattern, 1);
        state->must_advance = 0;
        if (status == 0 && pattern[0] == SRE_OP_AT &&
            (pattern[1] == SRE_AT_BEGINNING ||
             pattern[1] == SRE_AT_BEGINNING_STRING))
        {
            state->start = state->ptr = ptr = end;
            return 0;
        }
        while (status == 0 && ptr < end) {
            ptr++;
            RESET_CAPTURE_GROUP();
            state->start = state->ptr = ptr;
            status = sre_match<SRE_CHAR>(state, pattern, 0);
        }
    }

    return status;
}

#undef SRE_IS_DIGIT
#undef SRE_IS_SPACE
#undef SRE_IS_LINEBREAK
#undef SRE_IS_WORD
#undef SRE_LOC_IS_ALNUM
#undef SRE_LOC_IS_WORD
#undef SRE_UNI_IS_DIGIT
#undef SRE_UNI_IS_SPACE
#undef SRE_UNI_IS_LINEBREAK
#undef SRE_UNI_IS_ALNUM
#undef SRE_UNI_IS_WORD
#undef LASTMARK_SAVE
#undef LASTMARK_RESTORE
#undef LAST_PTR_PUSH
#undef LAST_PTR_POP
#undef RETURN_ERROR
#undef RETURN_FAILURE
#undef RETURN_SUCCESS
#undef RETURN_ON_ERROR
#undef RETURN_ON_SUCCESS
#undef RETURN_ON_FAILURE
#undef DATA_STACK_ALLOC
#undef DATA_STACK_LOOKUP_AT
#undef DATA_STACK_PUSH
#undef DATA_STACK_POP
#undef DATA_STACK_POP_DISCARD
#undef DATA_PUSH
#undef DATA_POP
#undef DATA_POP_DISCARD
#undef DATA_ALLOC
#undef DATA_LOOKUP_AT
#undef PTR_TO_INDEX
#undef MARK_PUSH
#undef MARK_POP
#undef MARK_POP_KEEP
#undef MARK_POP_DISCARD
#undef JUMP_NONE
#undef JUMP_MAX_UNTIL_1
#undef JUMP_MAX_UNTIL_2
#undef JUMP_MAX_UNTIL_3
#undef JUMP_MIN_UNTIL_1
#undef JUMP_MIN_UNTIL_2
#undef JUMP_MIN_UNTIL_3
#undef JUMP_REPEAT
#undef JUMP_REPEAT_ONE_1
#undef JUMP_REPEAT_ONE_2
#undef JUMP_MIN_REPEAT_ONE
#undef JUMP_BRANCH
#undef JUMP_ASSERT
#undef JUMP_ASSERT_NOT
#undef JUMP_POSS_REPEAT_1
#undef JUMP_POSS_REPEAT_2
#undef JUMP_ATOMIC_GROUP
#undef DO_JUMPX
#undef DO_JUMP
#undef DO_JUMP0
#undef _MAYBE_CHECK_SIGNALS
#undef MAYBE_CHECK_SIGNALS
#undef TARGET
#undef DISPATCH
#undef RESET_CAPTURE_GROUP

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } } // namespace JSC::Python::SRE
