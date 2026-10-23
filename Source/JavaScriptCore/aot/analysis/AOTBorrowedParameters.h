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
// Looks again only at a function that is new, that has changed, or that hands a parameter to a function whose record looks different. The lists go by function and end where the kept bodies end.
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

// A function borrows an entry parameter if nobody holds the pointer any more when the call returns.
class BorrowedParameters {
public:
    BorrowedParameters() = default;

    bool isBorrowed(FunctionID, uint32_t entryParameter) const;
    // The same of every reference that is read through the parameter.
    bool borrowsContents(FunctionID, uint32_t entryParameter) const;
    // Whether the function, or one it hands the parameter to, may call the parameter or a reference that is read through it.
    bool mayBeCalled(FunctionID, uint32_t entryParameter) const;
    OperandSummary summaryOf(FunctionID, uint32_t entryParameter) const;

    // All that the table says of the function.
    uint64_t wordOf(FunctionID) const;
    // What the body says where a plan puts these closures into the frame of the function, less what the callees lose by this table. Whatever it says beyond wordOf() holds as well.
    uint64_t wordOfBody(const Program&, BuildContext&, FunctionID, const BitVector& closuresPlannedInFrame) const;
    void add(FunctionID, uint64_t word);
    unsigned size() const { return m_words.size(); }

    friend bool operator==(const BorrowedParameters&, const BorrowedParameters&) = default;

private:
    friend BorrowedParameters computeBorrowedParameters(const Program&, BuildContext&, BorrowingBodies&, std::span<const uint8_t>, std::span<const uint8_t>);

    Vector<uint64_t> m_words;
};

// The references are handed to an entry parameter of a callee. They keep what the callee keeps of that parameter.
struct HandedOnReferences {
    FunctionID callee;
    uint32_t entryParameter { 0 };
    // Beside the references, the bits that go if the callee may call the parameter.
    uint64_t bits { 0 };

    friend bool operator==(const HandedOnReferences&, const HandedOnReferences&) = default;
};

struct BorrowingBody {
    uint64_t word { 0 };
    Vector<HandedOnReferences> handedOn;
    Vector<FunctionID> calleesWhoseRecordsWereRead;

    friend bool operator==(const BorrowingBody&, const BorrowingBody&) = default;
};

// What each body says by itself, kept from one table to the next.
class BorrowingBodies {
    WTF_MAKE_NONCOPYABLE(BorrowingBodies);
public:
    BorrowingBodies() = default;

    unsigned size() const { return m_bodies.size(); }
    // A function that was lowered does with its parameters what it did before.
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
