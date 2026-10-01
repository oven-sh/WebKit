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

#include "config.h"
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include <sys/stat.h>
#include <sys/types.h>

// The module _stat: Modules/_stat.c of CPython. stat has all of it in Python besides, with the numbers that are usual. These are the system's.

// What the system has no name for is what CPython takes it to be.
#ifndef S_IFDOOR
#define S_IFDOOR 0
#endif
#ifndef S_IFPORT
#define S_IFPORT 0
#endif
#ifndef S_IFWHT
#define S_IFWHT 0
#endif
#ifndef S_ISDOOR
#define S_ISDOOR(mode) 0
#endif
#ifndef S_ISPORT
#define S_ISPORT(mode) 0
#endif
#ifndef S_ISWHT
#define S_ISWHT(mode) 0
#endif
#ifndef S_ENFMT
#define S_ENFMT S_ISGID
#endif
#ifndef S_IREAD
#define S_IREAD 00400
#endif
#ifndef S_IWRITE
#define S_IWRITE 00200
#endif
#ifndef S_IEXEC
#define S_IEXEC 00100
#endif
#ifndef UF_SETTABLE
#define UF_SETTABLE 0x0000ffff
#endif
#ifndef UF_NODUMP
#define UF_NODUMP 0x00000001
#endif
#ifndef UF_IMMUTABLE
#define UF_IMMUTABLE 0x00000002
#endif
#ifndef UF_APPEND
#define UF_APPEND 0x00000004
#endif
#ifndef UF_OPAQUE
#define UF_OPAQUE 0x00000008
#endif
#ifndef UF_NOUNLINK
#define UF_NOUNLINK 0x00000010
#endif
#ifndef UF_COMPRESSED
#define UF_COMPRESSED 0x00000020
#endif
#ifndef UF_TRACKED
#define UF_TRACKED 0x00000040
#endif
#ifndef UF_DATAVAULT
#define UF_DATAVAULT 0x00000080
#endif
#ifndef UF_HIDDEN
#define UF_HIDDEN 0x00008000
#endif
#ifndef SF_SETTABLE
#define SF_SETTABLE 0xffff0000
#endif
#ifndef SF_ARCHIVED
#define SF_ARCHIVED 0x00010000
#endif
#ifndef SF_IMMUTABLE
#define SF_IMMUTABLE 0x00020000
#endif
#ifndef SF_APPEND
#define SF_APPEND 0x00040000
#endif
#ifndef SF_NOUNLINK
#define SF_NOUNLINK 0x00100000
#endif
#ifndef SF_SNAPSHOT
#define SF_SNAPSHOT 0x00200000
#endif
#ifndef SF_FIRMLINK
#define SF_FIRMLINK 0x00800000
#endif
#ifndef SF_DATALESS
#define SF_DATALESS 0x40000000
#endif
#if OS(DARWIN) && !defined(SF_SUPPORTED)
#undef SF_SETTABLE
#define SF_SUPPORTED 0x009f0000
#define SF_SETTABLE 0x3fff0000
#define SF_SYNTHETIC 0xc0000000
#endif

namespace JSC { namespace Python {

namespace {

// _PyLong_AsMode_t(). Nothing if it raised.
std::optional<mode_t> toMode(JSGlobalObject* globalObject, JSValue given)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toInt(globalObject, given);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (compareInts(integer, jsNumber(0)) < 0) {
        raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative value to unsigned int"_s);
        return std::nullopt;
    }
    uint64_t value = lowBitsOfInt(integer);
    if (compareInts(integer, intFromUInt64(globalObject, value))) {
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C unsigned long"_s);
        return std::nullopt;
    }
    auto mode = static_cast<mode_t>(value);
    if (mode != value) {
        raise(globalObject, scope, BuiltinType::OverflowError, "mode out of range"_s);
        return std::nullopt;
    }
    return mode;
}

enum class Kind : uint8_t { Directory, Character, Block, Regular, FIFO, Link, Socket, Door, Port, Whiteout };

bool isOfKind(mode_t mode, Kind kind)
{
    switch (kind) {
    case Kind::Directory:
        return S_ISDIR(mode);
    case Kind::Character:
        return S_ISCHR(mode);
    case Kind::Block:
        return S_ISBLK(mode);
    case Kind::Regular:
        return S_ISREG(mode);
    case Kind::FIFO:
        return S_ISFIFO(mode);
    case Kind::Link:
        return S_ISLNK(mode);
    case Kind::Socket:
        return S_ISSOCK(mode);
    case Kind::Door:
        return S_ISDOOR(mode);
    case Kind::Port:
        return S_ISPORT(mode);
    case Kind::Whiteout:
        return S_ISWHT(mode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// S_ISDIR(mode) and its like
PYTHON_NATIVE(statIsOfKind)
{
    auto kind = unpack<Kind>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto mode = toMode(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(isOfKind(*mode, kind)));
}

// S_IMODE(mode) and S_IFMT(mode)
PYTHON_NATIVE(statMasked)
{
    bool isForKind = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto mode = toMode(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromUInt64(globalObject, *mode & (isForKind ? S_IFMT : 07777)));
}

// filetype()
char letterOfKind(mode_t mode)
{
    constexpr std::pair<Kind, char> letters[] = { { Kind::Regular, '-' }, { Kind::Directory, 'd' }, { Kind::Link, 'l' }, { Kind::Block, 'b' }, { Kind::Character, 'c' }, { Kind::FIFO, 'p' }, { Kind::Socket, 's' },
        { Kind::Door, 'D' }, { Kind::Port, 'P' }, { Kind::Whiteout, 'w' } };
    for (auto [kind, letter] : letters) {
        if (isOfKind(mode, kind))
            return letter;
    }
    return '?';
}

// filemode(mode)
PYTHON_NATIVE(statFileMode)
{
    NATIVE_PROLOGUE();
    auto given = toMode(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    mode_t mode = *given;
    auto execute = [&] (mode_t special, mode_t bit, char both, char alone) -> Latin1Character { return mode & special ? (mode & bit ? both : alone) : (mode & bit ? 'x' : '-'); };
    // fileperm()
    std::array<Latin1Character, 10> text {
        static_cast<Latin1Character>(letterOfKind(mode)),
        static_cast<Latin1Character>(mode & S_IRUSR ? 'r' : '-'), static_cast<Latin1Character>(mode & S_IWUSR ? 'w' : '-'), execute(S_ISUID, S_IXUSR, 's', 'S'),
        static_cast<Latin1Character>(mode & S_IRGRP ? 'r' : '-'), static_cast<Latin1Character>(mode & S_IWGRP ? 'w' : '-'), execute(S_ISGID, S_IXGRP, 's', 'S'),
        static_cast<Latin1Character>(mode & S_IROTH ? 'r' : '-'), static_cast<Latin1Character>(mode & S_IWOTH ? 'w' : '-'), execute(S_ISVTX, S_IXOTH, 't', 'T'),
    };
    return JSValue::encode(jsString(vm, String(std::span<const Latin1Character>(text))));
}

} // namespace

JSObject* createStatModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_stat"_s);
    constexpr std::pair<ASCIILiteral, Kind> kinds[] = { { "S_ISDIR"_s, Kind::Directory }, { "S_ISCHR"_s, Kind::Character }, { "S_ISBLK"_s, Kind::Block }, { "S_ISREG"_s, Kind::Regular }, { "S_ISFIFO"_s, Kind::FIFO },
        { "S_ISLNK"_s, Kind::Link }, { "S_ISSOCK"_s, Kind::Socket }, { "S_ISDOOR"_s, Kind::Door }, { "S_ISPORT"_s, Kind::Port }, { "S_ISWHT"_s, Kind::Whiteout } };
    for (auto [name, kind] : kinds)
        addFunction(globalObject, module, name, statIsOfKind, pack(kind));
    addFunction(globalObject, module, "S_IMODE"_s, statMasked, pack(false));
    addFunction(globalObject, module, "S_IFMT"_s, statMasked, pack(true));
    addFunction(globalObject, module, "filemode"_s, statFileMode);

    auto add = [&] (ASCIILiteral name, uint64_t value) { module->putDirect(vm, Identifier::fromString(vm, name), intFromUInt64(globalObject, value)); };
#define ADD_INT_MACRO(macro) add(#macro ""_s, macro)
    ADD_INT_MACRO(S_IFDIR);
    ADD_INT_MACRO(S_IFCHR);
    ADD_INT_MACRO(S_IFBLK);
    ADD_INT_MACRO(S_IFREG);
    ADD_INT_MACRO(S_IFIFO);
    ADD_INT_MACRO(S_IFLNK);
    ADD_INT_MACRO(S_IFSOCK);
    ADD_INT_MACRO(S_IFDOOR);
    ADD_INT_MACRO(S_IFPORT);
    ADD_INT_MACRO(S_IFWHT);

    ADD_INT_MACRO(S_ISUID);
    ADD_INT_MACRO(S_ISGID);
    ADD_INT_MACRO(S_ISVTX);
    ADD_INT_MACRO(S_ENFMT);

    ADD_INT_MACRO(S_IREAD);
    ADD_INT_MACRO(S_IWRITE);
    ADD_INT_MACRO(S_IEXEC);

    ADD_INT_MACRO(S_IRWXU);
    ADD_INT_MACRO(S_IRUSR);
    ADD_INT_MACRO(S_IWUSR);
    ADD_INT_MACRO(S_IXUSR);

    ADD_INT_MACRO(S_IRWXG);
    ADD_INT_MACRO(S_IRGRP);
    ADD_INT_MACRO(S_IWGRP);
    ADD_INT_MACRO(S_IXGRP);

    ADD_INT_MACRO(S_IRWXO);
    ADD_INT_MACRO(S_IROTH);
    ADD_INT_MACRO(S_IWOTH);
    ADD_INT_MACRO(S_IXOTH);

    ADD_INT_MACRO(UF_SETTABLE);
    ADD_INT_MACRO(UF_NODUMP);
    ADD_INT_MACRO(UF_IMMUTABLE);
    ADD_INT_MACRO(UF_APPEND);
    ADD_INT_MACRO(UF_OPAQUE);
    ADD_INT_MACRO(UF_NOUNLINK);
    ADD_INT_MACRO(UF_COMPRESSED);
    ADD_INT_MACRO(UF_TRACKED);
    ADD_INT_MACRO(UF_DATAVAULT);
    ADD_INT_MACRO(UF_HIDDEN);
    ADD_INT_MACRO(SF_SETTABLE);
    ADD_INT_MACRO(SF_ARCHIVED);
    ADD_INT_MACRO(SF_IMMUTABLE);
    ADD_INT_MACRO(SF_APPEND);
    ADD_INT_MACRO(SF_NOUNLINK);
    ADD_INT_MACRO(SF_SNAPSHOT);
    ADD_INT_MACRO(SF_FIRMLINK);
    ADD_INT_MACRO(SF_DATALESS);
#ifdef SF_SUPPORTED
    ADD_INT_MACRO(SF_SUPPORTED);
#endif
#ifdef SF_SYNTHETIC
    ADD_INT_MACRO(SF_SYNTHETIC);
#endif
#undef ADD_INT_MACRO

    unsigned index = 0;
    for (auto name : { "ST_MODE"_s, "ST_INO"_s, "ST_DEV"_s, "ST_NLINK"_s, "ST_UID"_s, "ST_GID"_s, "ST_SIZE"_s, "ST_ATIME"_s, "ST_MTIME"_s, "ST_CTIME"_s })
        add(name, index++);
    return module;
}

} } // namespace JSC::Python
