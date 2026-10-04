/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <atomic>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class UnlinkedCodeBlock;

namespace AOT {

struct GuessedPlace {
    uint16_t nameID;
    uint8_t slot;
    uint8_t numberOfShapes;
};

class PropertyPlaces {
    WTF_MAKE_TZONE_ALLOCATED(PropertyPlaces);
    WTF_MAKE_NONCOPYABLE(PropertyPlaces);
public:
    using Names = Vector<UniquedStringImpl*, 8>;
    using NumberOfSitesByName = UncheckedKeyHashMap<UniquedStringImpl*, unsigned>;
    enum class Decision : uint8_t { Guessed, NoShape, Disagree, SlotTooHigh, NoNameID };
    static constexpr unsigned numberOfDecisions = 5;

    PropertyPlaces() = default;

    JS_EXPORT_PRIVATE void note(Names&& namesInSlotOrder);
    JS_EXPORT_PRIVATE void noteSites(const NumberOfSitesByName&);
    JS_EXPORT_PRIVATE void noteConstruction(UnlinkedCodeBlock* constructor, UnlinkedCodeBlock* parentConstructor, Names&& ownNamesInSlotOrder, bool ownNamesAreAll);
    JS_EXPORT_PRIVATE void finalize();
    void setFirstNameID(uint32_t firstNameID) { m_firstNameID = firstNameID; }

    const Vector<UniquedStringImpl*>& namesInIDOrder() const { return m_namesInIDOrder; }
    JS_EXPORT_PRIVATE uint16_t nameID(UniquedStringImpl*) const;
    bool isHeld(UniquedStringImpl* name) const { return m_holders.contains(name); }
    Decision decide(UniquedStringImpl* name, std::span<UniquedStringImpl* const> namesAccessed, GuessedPlace&) const;
    void countNameOnlyCalled() const { m_namesOnlyCalled.fetch_add(1, std::memory_order_relaxed); }
    void countGuardsOverWholeFunction(unsigned guards, unsigned bytecodeSize, unsigned codeSize) const
    {
        m_functionsWithGuards.fetch_add(1, std::memory_order_relaxed);
        m_guardsOverWholeFunctions.fetch_add(guards, std::memory_order_relaxed);
        m_bytecodeSizeWithGuards.fetch_add(bytecodeSize, std::memory_order_relaxed);
        m_codeSizeWithGuards.fetch_add(codeSize, std::memory_order_relaxed);
    }
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    struct Holders {
        Vector<uint32_t> shapes;
    };
    struct ListedName {
        unsigned numberOfSites { 0 };
        unsigned indexInIDOrder { 0 };
    };
    struct Construction {
        UnlinkedCodeBlock* parentConstructor { nullptr };
        Names ownNames;
        bool ownNamesAreAll { true };
    };
    static constexpr unsigned maxNumberOfAncestors = 16;
    bool appendNamesOfInstances(UnlinkedCodeBlock* constructor, Names&, bool& areAll, unsigned numberOfDescendants = 0) const;

    Lock m_lock;
    Vector<Names> m_shapes;
    size_t m_numberOfBirths { 0 };
    UncheckedKeyHashMap<UnlinkedCodeBlock*, Construction> m_constructions;
    UncheckedKeyHashMap<UniquedStringImpl*, Holders> m_holders;
    UncheckedKeyHashMap<UniquedStringImpl*, ListedName> m_listedNames;
    Vector<UniquedStringImpl*> m_namesInIDOrder;
    uint32_t m_firstNameID { 0 };
    mutable std::atomic<unsigned> m_decisions[numberOfDecisions] { };
    mutable std::atomic<unsigned> m_namesOnlyCalled { 0 };
    mutable std::atomic<unsigned> m_functionsWithGuards { 0 };
    mutable std::atomic<unsigned> m_guardsOverWholeFunctions { 0 };
    mutable std::atomic<unsigned> m_bytecodeSizeWithGuards { 0 };
    mutable std::atomic<unsigned> m_codeSizeWithGuards { 0 };
    mutable std::atomic<unsigned> m_guessesFromOneShape { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
