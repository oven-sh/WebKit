#!/usr/bin/env python3
#
# Copyright (C) 2026 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


# Makes PythonSREEngine.h out of Modules/_sre/sre_lib.h and Modules/_sre/sre_constants.h of CPython.
#
#     python3 convert-sre-engine.py <the directory that CPython's source is in> > ../PythonSREEngine.h
#
# sre_lib.h is what matches a regular expression. In CPython it is included three times over, once for each width of character. Here it is a template. Nothing else about it is changed but what C++ requires, so that
# it does what CPython's does, and so that this can be run again when CPython's changes.
import re
import sys

directory = sys.argv[1] + "/Modules/_sre/"
text = open(directory + "sre_lib.h").read()
constants = open(directory + "sre_constants.h").read()


def replace(old, new, count=1):
    global text
    assert text.count(old) == count, (old, text.count(old))
    text = text.replace(old, new)


def substitute(pattern, new, flags=0, at_least=1):
    global text
    text, n = re.subn(pattern, new, text, flags=flags)
    assert n >= at_least, (pattern, n)


# The comment at the top, which is written again below, and what undoes the macros at the bottom
text = text[text.index("/* String matching engine */"):text.index("#undef SRE_CHAR")]

# Tracing
substitute(r"[ \t]*TRACE\(\(.*?\)\);[ \t]*\\?\n", "", re.S)
substitute(r"[ \t]*INIT_TRACE\(state\);\n", "")
substitute(r"#if VERBOSE\n.*?#endif\n", "", re.S)
substitute(r"[ \t]*MARK_TRACE\(.*?\);[ \t]*\\\n", "")

# What is only for a debug build of CPython
substitute(r"#ifdef Py_DEBUG\n# define MAYBE_CHECK_SIGNALS.*?#else\n(# define MAYBE_CHECK_SIGNALS _MAYBE_CHECK_SIGNALS\n)#endif /\* Py_DEBUG \*/\n", r"\1", re.S)

# It is a switch, and not a table of labels.
substitute(r"#ifdef HAVE_COMPUTED_GOTOS\n.*?#if USE_COMPUTED_GOTOS\n.*?#else\n(.*?)#endif\n", r"\1", re.S)
substitute(r"#if USE_COMPUTED_GOTOS\n#include \"sre_targets.h\"\n#endif\n", "")
substitute(r"#if USE_COMPUTED_GOTOS\n    DISPATCH;\n#else\n(.*?)#endif\n", r"\1", re.S)
substitute(r"#if !USE_COMPUTED_GOTOS\n(        default:\n)#endif\n", r"\1")

# What is left out for the widest characters does no harm to them: it asks whether a character of the pattern fits in a character of the string.
substitute(r"#if SIZEOF_SRE_CHAR < 4\n(.*?)#endif\n", r"\1", re.S, 4)

# A template in place of three copies
substitute(r"typedef struct \{\n(.*?)\} SRE\(match_context\);", r"template<typename SRE_CHAR>\nstruct sre_match_context {\n\1};", re.S)
substitute(r"LOCAL\((\w+)\)\s+SRE\((\w+)\)\(", r"template<typename SRE_CHAR>\ninline \1 sre_\2(")
substitute(r"SRE\((\w+)\)", r"sre_\1<SRE_CHAR>")

# What goes by the locale is told which
for name in ("sre_lower_locale", "sre_upper_locale", "char_loc_ignore", "sre_category", "SRE_LOC_IS_WORD"):
    substitute(name + r"\(", name + "(state, ")

# C++ does not let a jump pass over a variable being given its first value, nor make a pointer to a character of a pointer to anything.
replace("    SRE_REPEAT *repeat_of_tail = state->repeat;", "    repeat_of_tail = state->repeat;")
replace("    sre_match_context<SRE_CHAR>* nextctx;\n", "    sre_match_context<SRE_CHAR>* nextctx;\n    SRE_REPEAT* repeat_of_tail;\n")
replace("    ;  // Fashion statement.\n    const SRE_CHAR *ptr = (SRE_CHAR *)state->ptr;\n", "    ptr = (const SRE_CHAR *)state->ptr;\n")
replace("    int jump;\n    unsigned int sigcount", "    int jump;\n    const SRE_CHAR* ptr;\n    unsigned int sigcount")
replace(" ptr = state->ptr;", " ptr = (const SRE_CHAR *)state->ptr;", 3)
replace("PyErr_CheckSignals()", "state->checkSignals()")
substitute(r"\bPy_ssize_t\b", "ptrdiff_t")
substitute(r"\bNULL\b", "nullptr")
substitute(r"\bassert\(", "ASSERT(")

PROLOGUE = r"""/*
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

"""

HELPERS = r"""// ---- Modules/_sre/sre.c, as far as the three copies of sre_lib.h

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

"""

print(PROLOGUE, end="")
print("// ---- Modules/_sre/sre_constants.h\n")
for line in constants.split("\n"):
    if match := re.match(r"#define (SRE_\w+) (\d+)$", line):
        print("static constexpr SRE_CODE %s = %s;" % match.groups())
print()
print(HELPERS, end="")
print(text.rstrip("\n"))
print()
for name in dict.fromkeys(re.findall(r"^\s*#\s*define (\w+)", HELPERS + text, re.M)):
    print("#undef " + name)
print("""
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } } // namespace JSC::Python::SRE""")
