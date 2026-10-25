/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BorrowedParameters;
class BorrowingBodies;
class Program;

JS_EXPORT_PRIVATE BorrowedParameters computeBorrowedParameters(const Program&, BuildContext&);
BorrowedParameters computeBorrowedParameters(const Program&, BuildContext&, BorrowingBodies& kept, std::span<const uint8_t> hasChanged, std::span<const uint8_t> recordLooksDifferent);

inline constexpr unsigned maxBorrowedParameters = 21;

inline constexpr OperandUses usesOfBorrowedParameter {
    OperandUse::Inspects,
    OperandUse::Calls,
    OperandUse::CallsPerElement,
    OperandUse::CallsRepeatedly,
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
};

class BorrowedParameters {
public:
    BorrowedParameters() = default;

    bool isBorrowed(FunctionID function, uint32_t entryParameter) const { return hasBit(function, 0, entryParameter); }
    bool borrowsContents(FunctionID function, uint32_t entryParameter) const { return hasBit(function, 1, entryParameter); }
    bool mayBeCalled(FunctionID function, uint32_t entryParameter) const { return !hasBit(function, 2, entryParameter); }
    OperandSummary summaryOf(FunctionID, uint32_t entryParameter) const;

    uint64_t wordOf(FunctionID function) const { return function.index() < m_words.size() ? m_words[function.index()] : 0; }
    uint64_t wordOfBody(const Program&, BuildContext&, FunctionID, const BitVector& closuresPlannedInFrame) const;
    void add(FunctionID function, uint64_t word) { m_words[function.index()] |= word; }
    unsigned size() const { return m_words.size(); }

    friend bool operator==(const BorrowedParameters&, const BorrowedParameters&) = default;

private:
    friend BorrowedParameters computeBorrowedParameters(const Program&, BuildContext&, BorrowingBodies&, std::span<const uint8_t>, std::span<const uint8_t>);

    bool hasBit(FunctionID function, unsigned row, uint32_t entryParameter) const { return entryParameter < maxBorrowedParameters && wordOf(function) >> (row * maxBorrowedParameters + entryParameter) & 1; }

    Vector<uint64_t> m_words;
};

struct HandedOnReferences {
    FunctionID callee;
    uint32_t entryParameter { 0 };
    uint64_t bits { 0 };

    friend bool operator==(const HandedOnReferences&, const HandedOnReferences&) = default;
};

struct BorrowingBody {
    uint64_t word { 0 };
    Vector<HandedOnReferences> handedOn;
    Vector<FunctionID> calleesWhoseRecordsWereRead;

    friend bool operator==(const BorrowingBody&, const BorrowingBody&) = default;
};

class BorrowingBodies {
    WTF_MAKE_NONCOPYABLE(BorrowingBodies);
public:
    BorrowingBodies() = default;

    unsigned size() const { return m_bodies.size(); }
    void didLower(std::span<const FunctionID>);
    bool wasLoweredSinceScanned(FunctionID id) const { return id.index() < m_wasLoweredSinceScanned.size() && m_wasLoweredSinceScanned[id.index()]; }

private:
    friend BorrowedParameters computeBorrowedParameters(const Program&, BuildContext&, BorrowingBodies&, std::span<const uint8_t>, std::span<const uint8_t>);

    Vector<BorrowingBody> m_bodies;
    Vector<uint8_t> m_wasLoweredSinceScanned;
    Vector<uint64_t> m_wordsOfLatestTable;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
