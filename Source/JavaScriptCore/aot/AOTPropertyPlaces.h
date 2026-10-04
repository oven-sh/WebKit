/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class UnlinkedCodeBlock;

namespace AOT {

class CalleeHints;

struct GuessedPlace {
    uint16_t family;
    uint8_t slot;
};

class PropertyPlaces {
    WTF_MAKE_TZONE_ALLOCATED(PropertyPlaces);
    WTF_MAKE_NONCOPYABLE(PropertyPlaces);
public:
    using Names = Vector<UniquedStringImpl*, 8>;
    using VariableKey = std::pair<const void*, unsigned>;
    struct NamesAccessed {
        Vector<UniquedStringImpl*, 4> names;
        Vector<UniquedStringImpl*, 2> namesOnlyCalled;
        unsigned numberOfSites { 0 };
        VariableKey variableReadFrom { nullptr, 0 };
    };
    enum class Decision : uint8_t { Guessed, NoShape, Disagree, SlotTooHigh, NoFamily, SameNamesBornInAnotherModule };
    static constexpr unsigned numberOfDecisions = 6;

    PropertyPlaces() = default;

    JS_EXPORT_PRIVATE void noteLiteral(const CalleeHints* module, UnlinkedCodeBlock*, unsigned bytecodeOffset, Names&& namesInSlotOrder, unsigned numberOfNamesGivenAtOnce);
    JS_EXPORT_PRIVATE void noteNamesAccessed(const CalleeHints* module, Vector<NamesAccessed>&&);
    JS_EXPORT_PRIVATE void noteConstruction(const CalleeHints* module, UnlinkedCodeBlock* constructor, UnlinkedCodeBlock* parentConstructor, Names&& ownNamesInSlotOrder, bool ownNamesAreAll, unsigned numberOfNamesGivenAtOnce);
    JS_EXPORT_PRIVATE void finalize();

    bool isHeld(UniquedStringImpl* name) const { return m_holders.contains(name); }
    Decision decide(const CalleeHints* module, UniquedStringImpl* name, const NamesAccessed&, GuessedPlace&, bool& usesNamesOnVariable) const;
    unsigned numberOfFamilies() const { return static_cast<unsigned>(m_shapeOfFamily.size()); }
    JS_EXPORT_PRIVATE uint16_t familyOfLiteral(UnlinkedCodeBlock*, unsigned bytecodeOffset) const;
    JS_EXPORT_PRIVATE uint16_t familyOfInstances(UnlinkedCodeBlock* constructor) const;
    JS_EXPORT_PRIVATE std::span<UniquedStringImpl* const> namesOfFamily(uint16_t family) const;
    void countNameOnlyCalled() const { m_namesOnlyCalled.fetch_add(1, std::memory_order_relaxed); }
    void countGuardsOverWholeFunction(unsigned guards, unsigned bytecodeSize, unsigned codeSize) const
    {
        m_functionsWithGuards.fetch_add(1, std::memory_order_relaxed);
        m_guardsOverWholeFunctions.fetch_add(guards, std::memory_order_relaxed);
        m_bytecodeSizeWithGuards.fetch_add(bytecodeSize, std::memory_order_relaxed);
        m_codeSizeWithGuards.fetch_add(codeSize, std::memory_order_relaxed);
    }
    void countGuardsThatContradictAnalysis() const { m_functionsWhoseGuardsContradictAnalysis.fetch_add(1, std::memory_order_relaxed); }
    void countContradictionWithoutGuards() const { m_functionsThatContradictAnalysisWithoutGuards.fetch_add(1, std::memory_order_relaxed); }
    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    struct Holders {
        Vector<uint32_t> shapes;
    };
    struct Construction {
        const CalleeHints* module { nullptr };
        UnlinkedCodeBlock* parentConstructor { nullptr };
        Names ownNames;
        bool ownNamesAreAll { true };
        unsigned numberOfNamesGivenAtOnce { 0 };
    };
    struct Birth {
        Names names;
        const CalleeHints* module { nullptr };
        UnlinkedCodeBlock* codeBlock { nullptr };
        std::optional<unsigned> bytecodeOffsetOfLiteral;
        unsigned numberOfNamesGivenAtOnce { 0 };
        bool isOfDerivedClass { false };
    };
    struct Shape {
        Names names;
        UncheckedKeyHashSet<const CalleeHints*> modules;
        unsigned numberOfNamesGivenAtOnce { 0 };
        unsigned numberOfSites { 0 };
        uint16_t family { 0 };
        bool isOfDerivedClass { false };
    };
    struct NamesOnVariable {
        Vector<UniquedStringImpl*> names;
        Vector<UniquedStringImpl*> namesOnlyCalled;
        bool areTooMany { false };
    };
    static constexpr unsigned maxNumberOfAncestors = 16;
    static constexpr unsigned maxNumberOfNamesOnVariable = 64;
    static constexpr unsigned maxNumberOfFamilies = std::numeric_limits<uint16_t>::max();
    bool appendNamesOfInstances(UnlinkedCodeBlock* constructor, Names&, bool& areAll, unsigned numberOfDescendants = 0) const;
    void note(Birth&&);
    Vector<uint32_t, 8> shapesWithAllOf(const CalleeHints* module, const NamesAccessed&, const NamesOnVariable*, Vector<uint32_t, 8>* bornInOtherModules) const;
    Vector<uint32_t, 8> candidateShapes(const CalleeHints* module, const NamesAccessed&, bool& usesNamesOnVariable, Vector<uint32_t, 8>* bornInOtherModules = nullptr) const;

    Lock m_lock;
    Vector<Birth> m_births;
    Vector<std::pair<const CalleeHints*, NamesAccessed>> m_namesAccessed;
    UncheckedKeyHashMap<VariableKey, NamesOnVariable> m_namesOnVariables;
    Vector<Shape> m_shapes;
    Vector<uint32_t> m_shapeOfFamily;
    UncheckedKeyHashMap<UnlinkedCodeBlock*, uint32_t> m_shapeOfInstances;
    UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<std::pair<unsigned, uint32_t>, 1>> m_shapesOfLiterals;
    size_t m_numberOfBirths { 0 };
    UncheckedKeyHashMap<UnlinkedCodeBlock*, Construction> m_constructions;
    UncheckedKeyHashMap<UniquedStringImpl*, Holders> m_holders;
    mutable std::atomic<unsigned> m_decisions[numberOfDecisions] { };
    mutable std::atomic<unsigned> m_namesOnlyCalled { 0 };
    mutable std::atomic<unsigned> m_functionsWithGuards { 0 };
    mutable std::atomic<unsigned> m_functionsWhoseGuardsContradictAnalysis { 0 };
    mutable std::atomic<unsigned> m_functionsThatContradictAnalysisWithoutGuards { 0 };
    mutable std::atomic<unsigned> m_guardsOverWholeFunctions { 0 };
    mutable std::atomic<unsigned> m_bytecodeSizeWithGuards { 0 };
    mutable std::atomic<unsigned> m_codeSizeWithGuards { 0 };
    mutable std::atomic<unsigned> m_guessesByNamesOnVariable { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
