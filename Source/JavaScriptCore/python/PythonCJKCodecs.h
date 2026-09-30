/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// What the codecs for Chinese, Japanese and Korean are written with: Modules/cjkcodecs/cjkcodecs.h of CPython. Each of the files that lib/convert-cjk-codecs.py makes includes it once, having said with macros what it wants of
// it, and is compiled by itself. So there is no `#pragma once`, and what is here is in a namespace with no name.
//
// The names are as they are in CPython, since what is written with them is CPython's own.

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonImport.h"
#include "PythonMultibyteCodec.h"
#include "PythonOperations.h"

namespace JSC { namespace Python { namespace {

// A character that there is not
#define UNIINV 0xFFFE

// Codes of two bytes that no set of characters has a use for
#define NOCHAR 0xFFFF
#define MULTIC 0xFFFE
#define DBCINV 0xFFFD

// The tables are shorter for these.
#define U UNIINV
#define N NOCHAR
#define M MULTIC
#define D DBCINV

struct dbcs_index {
    const ucs2_t* map;
    unsigned char bottom, top;
};
typedef struct dbcs_index decode_map;

struct widedbcs_index {
    const Py_UCS4* map;
    unsigned char bottom, top;
};
typedef struct widedbcs_index widedecode_map;

struct unim_index {
    const DBCHAR* map;
    unsigned char bottom, top;
};
typedef struct unim_index encode_map;

struct unim_index_bytebased {
    const unsigned char* map;
    unsigned char bottom, top;
};

struct dbcs_map {
    const char* charset;
    const struct unim_index* encmap;
    const struct dbcs_index* decmap;
};

struct pair_encodemap {
    Py_UCS4 uniseq;
    DBCHAR code;
};

#ifndef CJK_MOD_SPECIFIC_STATE
#define CJK_MOD_SPECIFIC_STATE
#endif

struct cjkcodecs_module_state final : NativeState {
    PYTHON_NATIVE_STATE(cjkcodecs_module_state);
    Vector<dbcs_map> mapping_list;
    Vector<MultibyteCodec> codec_list;
    CJK_MOD_SPECIFIC_STATE
};

template<typename Visitor> void cjkcodecs_module_state::visit(Visitor&) { }

#define CODEC_INIT(encoding) \
    static int encoding##_codec_init([[maybe_unused]] JSGlobalObject* globalObject, [[maybe_unused]] const MultibyteCodec* codec)

#define ENCODER_INIT(encoding) \
    static int encoding##_encode_init([[maybe_unused]] MultibyteCodec_State* state, [[maybe_unused]] const MultibyteCodec* codec)

#define ENCODER(encoding) \
    static Py_ssize_t encoding##_encode([[maybe_unused]] MultibyteCodec_State* state, [[maybe_unused]] const MultibyteCodec* codec, const CodePoints* data, Py_ssize_t* inpos, Py_ssize_t inlen, unsigned char** outbuf, Py_ssize_t outleft, [[maybe_unused]] int flags)

#define ENCODER_RESET(encoding) \
    static Py_ssize_t encoding##_encode_reset([[maybe_unused]] MultibyteCodec_State* state, [[maybe_unused]] const MultibyteCodec* codec, [[maybe_unused]] unsigned char** outbuf, [[maybe_unused]] Py_ssize_t outleft)

#define DECODER_INIT(encoding) \
    static int encoding##_decode_init([[maybe_unused]] MultibyteCodec_State* state, [[maybe_unused]] const MultibyteCodec* codec)

#define DECODER(encoding) \
    static Py_ssize_t encoding##_decode([[maybe_unused]] MultibyteCodec_State* state, [[maybe_unused]] const MultibyteCodec* codec, const unsigned char** inbuf, Py_ssize_t inleft, TextWriter* writer)

#define DECODER_RESET(encoding) \
    static Py_ssize_t encoding##_decode_reset([[maybe_unused]] MultibyteCodec_State* state, [[maybe_unused]] const MultibyteCodec* codec)

#define NEXT_IN(i) \
    do { \
        (*inbuf) += (i); \
        (inleft) -= (i); \
    } while (0)
#define NEXT_INCHAR(i) \
    do { \
        (*inpos) += (i); \
    } while (0)
#define NEXT_OUT(o) \
    do { \
        (*outbuf) += (o); \
        (outleft) -= (o); \
    } while (0)
#define NEXT(i, o) \
    do { \
        NEXT_INCHAR(i); \
        NEXT_OUT(o); \
    } while (0)

#define REQUIRE_INBUF(n) \
    do { \
        if (inleft < (n)) \
            return MBERR_TOOFEW; \
    } while (0)

#define REQUIRE_OUTBUF(n) \
    do { \
        if (outleft < (n)) \
            return MBERR_TOOSMALL; \
    } while (0)

#define INBYTE1 ((*inbuf)[0])
#define INBYTE2 ((*inbuf)[1])
#define INBYTE3 ((*inbuf)[2])
#define INBYTE4 ((*inbuf)[3])

#define INCHAR1 ((*data)[*inpos])
#define INCHAR2 ((*data)[*inpos + 1])

// If there is no room, that is found when it is finished.
#define OUTCHAR(c) writer->append(static_cast<Py_UCS4>(c))
#define OUTCHAR2(c1, c2) \
    do { \
        writer->append(static_cast<Py_UCS4>(c1)); \
        writer->append(static_cast<Py_UCS4>(c2)); \
    } while (0)

#define OUTBYTEI(c, i) \
    do { \
        ASSERT(static_cast<unsigned char>(c) == (c)); \
        ((*outbuf)[i]) = (c); \
    } while (0)

#define OUTBYTE1(c) OUTBYTEI(c, 0)
#define OUTBYTE2(c) OUTBYTEI(c, 1)
#define OUTBYTE3(c) OUTBYTEI(c, 2)
#define OUTBYTE4(c) OUTBYTEI(c, 3)

#define WRITEBYTE1(c1) \
    do { \
        REQUIRE_OUTBUF(1); \
        OUTBYTE1(c1); \
    } while (0)
#define WRITEBYTE2(c1, c2) \
    do { \
        REQUIRE_OUTBUF(2); \
        OUTBYTE1(c1); \
        OUTBYTE2(c2); \
    } while (0)
#define WRITEBYTE3(c1, c2, c3) \
    do { \
        REQUIRE_OUTBUF(3); \
        OUTBYTE1(c1); \
        OUTBYTE2(c2); \
        OUTBYTE3(c3); \
    } while (0)
#define WRITEBYTE4(c1, c2, c3, c4) \
    do { \
        REQUIRE_OUTBUF(4); \
        OUTBYTE1(c1); \
        OUTBYTE2(c2); \
        OUTBYTE3(c3); \
        OUTBYTE4(c4); \
    } while (0)

#define MODSTATE(codec) static_cast<cjkcodecs_module_state*>((codec)->modstate)

#define _TRYMAP_ENC(m, assi, val) \
    ((m)->map != NULL && (val) >= (m)->bottom && (val) <= (m)->top && ((assi) = (m)->map[(val) - (m)->bottom]) != NOCHAR)
#define TRYMAP_ENC(charset, assi, uni) \
    _TRYMAP_ENC(&charset##_encmap[(uni) >> 8], assi, (uni) & 0xff)
#define TRYMAP_ENC_ST(charset, assi, uni) \
    _TRYMAP_ENC(&(MODSTATE(codec)->charset##_encmap)[(uni) >> 8], assi, (uni) & 0xff)

#define _TRYMAP_DEC(m, assi, val) \
    ((m)->map != NULL && (val) >= (m)->bottom && (val) <= (m)->top && ((assi) = (m)->map[(val) - (m)->bottom]) != UNIINV)
#define TRYMAP_DEC(charset, assi, c1, c2) \
    _TRYMAP_DEC(&charset##_decmap[c1], assi, c2)
#define TRYMAP_DEC_ST(charset, assi, c1, c2) \
    _TRYMAP_DEC(&(MODSTATE(codec)->charset##_decmap)[c1], assi, c2)

#define BEGIN_MAPPINGS_LIST(NUM) \
    static void add_mappings([[maybe_unused]] cjkcodecs_module_state* st) \
    {
#define MAPPING_ENCONLY(enc) \
        st->mapping_list.append(dbcs_map { #enc, reinterpret_cast<const unim_index*>(enc##_encmap), nullptr });
#define MAPPING_DECONLY(enc) \
        st->mapping_list.append(dbcs_map { #enc, nullptr, reinterpret_cast<const dbcs_index*>(enc##_decmap) });
#define MAPPING_ENCDEC(enc) \
        st->mapping_list.append(dbcs_map { #enc, reinterpret_cast<const unim_index*>(enc##_encmap), reinterpret_cast<const dbcs_index*>(enc##_decmap) });
#define END_MAPPINGS_LIST \
    }

#define BEGIN_CODECS_LIST(NUM) \
    static void add_codecs(cjkcodecs_module_state* st) \
    {

#define _STATEFUL_METHODS(enc) \
    enc##_encode, enc##_encode_init, enc##_encode_reset, enc##_decode, enc##_decode_init, enc##_decode_reset,
#define _STATELESS_METHODS(enc) \
    enc##_encode, NULL, NULL, enc##_decode, NULL, NULL,

#define NEXT_CODEC(...) st->codec_list.append(MultibyteCodec { __VA_ARGS__ st })

#define CODEC_STATEFUL(enc) \
        NEXT_CODEC(#enc, NULL, NULL, _STATEFUL_METHODS(enc));
#define CODEC_STATELESS(enc) \
        NEXT_CODEC(#enc, NULL, NULL, _STATELESS_METHODS(enc));
#define CODEC_STATELESS_WINIT(enc) \
        NEXT_CODEC(#enc, NULL, enc##_codec_init, _STATELESS_METHODS(enc));

#define END_CODECS_LIST \
    }

static void add_mappings(cjkcodecs_module_state*);
static void add_codecs(cjkcodecs_module_state*);

// getcodec() and _getcodec()
PYTHON_NATIVE(getCodec)
{
    NATIVE_PROLOGUE();
    JSString* encoding = stringIn(args[0]);
    if (!encoding)
        return JSValue::encode(raiseTypeError(globalObject, scope, "encoding name must be a string."_s));
    // PyUnicode_AsUTF8(), of which as much is looked at as comes before a null
    auto encoded = encodeUTF8(globalObject, args[0], { });
    RETURN_IF_EXCEPTION(scope, { });
    auto name = encoded->span();
    if (size_t end = find(name, static_cast<uint8_t>(0)); end != notFound)
        name = name.first(end);
    for (auto& codec : realm->moduleState<cjkcodecs_module_state>().codec_list) {
        if (!equalSpans(name, byteCast<uint8_t>(unsafeSpan(codec.encoding))))
            continue;
        JSValue create = importModuleAttribute(globalObject, "_multibytecodec"_s, "__create_codec"_s);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, create, newCapsule(globalObject, codecCapsuleName, &codec))));
    }
    return JSValue::encode(raise(globalObject, scope, BuiltinType::LookupError, "no such codec is supported."_s));
}

#ifdef USING_BINARY_PAIR_SEARCH
static DBCHAR find_pairencmap(ucs2_t body, ucs2_t modifier, const struct pair_encodemap* haystack, int haystacksize)
{
    int pos, min, max;
    Py_UCS4 value = body << 16 | modifier;

    min = 0;
    max = haystacksize;

    for (pos = haystacksize >> 1; min != max; pos = (min + max) >> 1) {
        if (value < haystack[pos].uniseq) {
            if (max != pos) {
                max = pos;
                continue;
            }
        } else if (value > haystack[pos].uniseq) {
            if (min != pos) {
                min = pos;
                continue;
            }
        }
        break;
    }

    if (value == haystack[pos].uniseq)
        return haystack[pos].code;
    return DBCINV;
}
#endif

#ifdef USING_IMPORTED_MAPS
#define IMPORT_MAP(locale, charset, encmap, decmap) \
    importmap(globalObject, "_codecs_" #locale ""_s, "__map_" #charset ""_s, reinterpret_cast<const void**>(encmap), reinterpret_cast<const void**>(decmap))

// A table that another of these modules has. It is come by as a program would come by it, so the module is imported, and what is there by that name is what is taken. -1 if it raised.
static int importmap(JSGlobalObject* globalObject, ASCIILiteral moduleName, ASCIILiteral symbol, const void** encmap, const void** decmap)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue capsule = importModuleAttribute(globalObject, moduleName, symbol);
    RETURN_IF_EXCEPTION(scope, -1);
    auto* map = static_cast<const dbcs_map*>(capsulePointer(capsule, mapCapsuleName));
    if (!map) {
        raiseValueError(globalObject, scope, "map data must be a Capsule."_s);
        return -1;
    }
    if (encmap)
        *encmap = map->encmap;
    if (decmap)
        *decmap = map->decmap;
    return 0;
}
#endif

// register_maps(), and what PyModuleDef_Init() does with what I_AM_A_MODULE_FOR() says
static JSObject* createModule(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, name);
    auto& state = globalObject->pyRealm()->moduleState<cjkcodecs_module_state>();
    if (state.codec_list.isEmpty()) {
        add_mappings(&state);
        add_codecs(&state);
    }
    for (auto& map : state.mapping_list)
        module->putDirect(vm, Identifier::fromString(vm, makeString("__map_"_s, String::fromLatin1(map.charset))), newCapsule(globalObject, mapCapsuleName, &map));
    addFunction(globalObject, module, "getcodec"_s, getCodec);
    return module;
}

#define I_AM_A_MODULE_FOR(loc, Name) \
    } \
    JSObject* createCodecs##Name##Module(JSGlobalObject* globalObject) { return createModule(globalObject, "_codecs_" #loc ""_s); } \
    namespace {
