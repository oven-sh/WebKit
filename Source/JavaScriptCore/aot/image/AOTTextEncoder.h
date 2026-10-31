/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageText.h"
#include "AOTVector.h"
#include <span>
#include "AOTBitVector.h"

namespace JSC { namespace AOT {

struct TextInputs {
    const Typed::Program& program;
    const StringUses& uses;
    const IdentifierNumbering& identifiers;
    const StringConstantNumbering& constants;
    const BitVector& stringsNamedByTables;
    const BitVector* stringsOnlyShownByTables { nullptr };
};

struct EncodedText {
    Vector<uint8_t> strings;
    Vector<uint8_t> longStrings;
    Vector<uint8_t> text;
    Vector<uint8_t> identifiers;
    Vector<uint8_t> stringConstants;
};

EncodedText encodeText(const TextInputs&, BuildContext&);

struct DecodedString {
    bool is16Bit() const { return !units16.isEmpty(); }
    uint32_t length() const { return is16Bit() ? units16.size() : units8.size(); }
    std::span<const uint8_t> bytes() const { return is16Bit() ? asBytes(units16.span()) : units8.span(); }

    Vector<uint8_t> units8;
    Vector<char16_t> units16;
    uint32_t hash { 0 };
};

DecodedString decodeForImage(std::span<const uint8_t> programString);

struct TextSharing {
    Vector<uint32_t> holders;
    Vector<uint32_t> gaps;
    uint32_t searchLimitHits { 0 };
};

TextSharing shareText(std::span<const DecodedString>, const BitVector& hasText, const BitVector& isName, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
