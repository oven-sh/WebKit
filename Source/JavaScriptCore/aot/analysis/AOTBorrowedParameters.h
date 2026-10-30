/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactList.h"
#include "AOTEffectsOfOperands.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
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

struct HandedOnTarget {
    FunctionID callee;
    uint32_t entryParameter { 0 };

    friend bool operator==(const HandedOnTarget&, const HandedOnTarget&) = default;
};

// One operand of a call with many callees. The callees, each with the parameter that takes the operand, are a list that all such calls share:
// n calls that each may enter n functions keep n lists' worth of entries otherwise.
struct HandedOnToManyCallees {
    uint32_t targetList { 0 };
    uint64_t bits { 0 };

    friend bool operator==(const HandedOnToManyCallees&, const HandedOnToManyCallees&) = default;
};

class HandedOnTargetLists {
    WTF_MAKE_NONCOPYABLE(HandedOnTargetLists);
public:
    HandedOnTargetLists() = default;

    struct ListOfOperand {
        uint32_t list { 0 };
        bool hasCalleeWithoutParameter { false };
    };

    uint32_t intern(Vector<HandedOnTarget>&&);
    // The list for one operand of a call that enters the functions of a set. Calls of the same form share the answer, so the set is walked once for each form.
    ListOfOperand listOfOperand(const Program&, CalleeSetID, const Value& call, uint32_t operandIndex);
    // The answers hold while the records of the callees stay as they are.
    void forgetOperands() { m_listsOfOperands.clear(); }
    unsigned size() const { return m_lists.size(); }
    std::span<const HandedOnTarget> at(uint32_t list) const LIFETIME_BOUND { return m_lists[list]->span(); }

private:
    Lock m_lock;
    Vector<std::unique_ptr<Vector<HandedOnTarget>>> m_lists;
    UncheckedKeyHashMap<unsigned, Vector<uint32_t, 1>, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_listsByHash;
    UncheckedKeyHashMap<uint64_t, ListOfOperand, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_listsOfOperands;
};

struct BorrowingBody {
    uint64_t word { 0 };
    CompactList<HandedOnReferences> handedOn;
    CompactList<HandedOnToManyCallees> handedOnToManyCallees;
    CompactList<FunctionID> calleesWhoseRecordsWereRead;
    CompactList<uint32_t> targetListsRead;

    friend bool operator==(const BorrowingBody&, const BorrowingBody&) = default;
};
static_assert(sizeof(BorrowingBody) <= 40);

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
    HandedOnTargetLists m_targetLists;
    Vector<uint8_t> m_wasLoweredSinceScanned;
    Vector<uint64_t> m_wordsOfLatestTable;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
