/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <span>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Python {

// What the lexer, the parser and the compiler ask of the runtime while they work.
class ParserHost {
public:
    virtual ~ParserHost() = default;

    enum class WarningCategory : uint8_t { SyntaxWarning, DeprecationWarning };
    enum class WarningResult : uint8_t {
        Reported,
        BecameException, // The filters made the warning an exception of its category. A SyntaxError is raised in its place, and the host drops its own.
        OtherException, // The host has a pending exception.
    };
    struct Warning {
        WarningCategory category;
        String message;
        int lineno;
        int colOffset { -1 }; // -1: only the line is known.
        int endLineno { -1 };
        int endColOffset { -1 };
    };
    virtual WarningResult warn(const Warning&) { return WarningResult::Reported; }

    // The source in UTF-8, for a coding cookie that names neither UTF-8 nor Latin-1. A null string: `error` says why,
    // and `unknownEncoding` whether the codec does not exist.
    virtual UTF8CString decodeSource(std::span<const uint8_t>, const ASCIICString& encoding, String& error, bool& unknownEncoding);

    // A line of the file the source is said to come from, in UTF-8 and with its line ending, if there is such a file:
    // that, and not the source, is the text a SyntaxError in a module shows. `encoding`: what the source declares. Null: nothing.
    virtual bool lineOfFile(int /* lineno */, const ASCIICString& /* encoding */, Vector<uint8_t>&) { return false; }

    // sys.get_int_max_str_digits()
    virtual int maxStrDigits() { return 4300; }

    // Asked before the first \N{...}: in CPython the names of characters come with the module unicodedata, which may fail to be imported.
    virtual bool hasCharacterNames() { return true; }
};

// A failed parse or compilation, in terms the runtime can raise.
struct SyntaxErrorInfo {
    enum class Type : uint8_t {
        None,
        SyntaxError,
        IndentationError,
        TabError,
        IncompleteInputError,
        ValueError,
        OverflowError,
        MemoryError,
        RecursionError,
        SystemError,
        HostException, // Pending in the host.
    };

    Type type { Type::None };
    String message;

    // SyntaxError(message, (filename, lineno, offset, text, end_lineno, end_offset)). Offsets count characters, from 1.
    bool hasLocation { false };
    // As the symbol table reports what it finds: SyntaxError(message), which is then told where through its attributes.
    bool locationIsNotAnArgument { false };
    int lineno { 0 };
    int offset { 0 };
    String text; // Null: None
    bool hasEnd { false };
    int endLineno { 0 };
    int endOffset { 0 };

    // SyntaxError._metadata, from which Lib/traceback.py works out that a name may be a misspelt keyword: where the last compound
    // statement that parsed starts (0, 0: there is none), and then the source, which whoever makes the exception adds.
    bool hasMetadata { false };
    int metadataLineno { 0 };
    int metadataColOffset { 0 };

    explicit operator bool() const { return type != Type::None; }
    bool isSyntaxError() const { return type >= Type::SyntaxError && type <= Type::IncompleteInputError; }
};

} // namespace Python
} // namespace JSC
