/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowKeys.h"
#include "AOTFlowObjectSet.h"
#include "AOTIR.h"
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowObjectTable;

struct FlowValue {
    uint16_t primitives { 0 };
    FlowStringSetID strings;
    FlowSymbolSetID symbols;
    FlowRangeID integers;
    FlowObjectSet objects;

    bool isEmpty() const { return !primitives && objects.isEmpty(); }
    bool mayBeObject() const { return !objects.isEmpty(); }
    bool hasUnknownObject() const { return objects.containsUnknownObject(); }
    bool hasOnlyTrackedObjects() const { return !hasUnknownObject(); }
    bool hasOnlyObjects() const { return !primitives; }
    bool mayBeNullish() const { return primitives & Primitive::UndefinedOrNull; }

    FlowValue withoutObjects() const
    {
        FlowValue result;
        result.primitives = primitives;
        result.strings = strings;
        result.symbols = symbols;
        result.integers = integers;
        return result;
    }

    friend bool operator==(const FlowValue&, const FlowValue&) = default;
};

static_assert(sizeof(FlowValue) == 24);

// True only where a join of the primitives of the source leaves the destination as it is. A destination with a kind that has no literals holds every value of the kind.
inline bool bringsNoPrimitives(const FlowValue& source, const FlowValue& destination)
{
    constexpr uint16_t kindsWithLiterals = Primitive::LiteralString | Primitive::LiteralSymbol | Primitive::Integer;
    if (!(source.primitives & kindsWithLiterals))
        return !(source.primitives & ~destination.primitives);
    return source.primitives == destination.primitives && source.strings == destination.strings && source.symbols == destination.symbols && source.integers == destination.integers;
}

struct FlowJoinOutcome {
    bool didChangePrimitives { false };
    bool didChangeObjects { false };

    explicit operator bool() const { return didChangePrimitives || didChangeObjects; }
};

class FlowValueTable {
    WTF_MAKE_TZONE_ALLOCATED(FlowValueTable);
    WTF_MAKE_NONCOPYABLE(FlowValueTable);
public:
    FlowValueTable(const Program&, const FlowObjectTable&, TypeArena&, BuildContext&);

    const Program& program() const { return m_program; }
    const FlowObjectTable& objects() const { return m_objects; }
    TypeArena& types() { return m_types; }

    std::span<const StringID> stringsOf(FlowStringSetID set) const LIFETIME_BOUND { return m_stringSets.at(set); }
    std::span<const PropertyKeyID> symbolsOf(FlowSymbolSetID set) const LIFETIME_BOUND { return m_symbolSets.at(set); }
    Range rangeOf(FlowRangeID range) const { return range ? m_ranges[range.index()] : Range::empty(); }

    FlowValue valueOfObject(FlowObjectID) const;
    FlowValue valueOfPrimitives(uint16_t bits);
    FlowValue valueOfIntegers(Range);
    FlowValue valueOfString(StringID);
    FlowValue valueOfSymbol(PropertyKeyID);
    FlowValue unknownValue();
    FlowValue unknownObjectValue() const;
    FlowValue valueOfType(const Type&);
    FlowValue valueOfType(TypeID type) { return valueOfType(m_program.at(type)); }

    FlowJoinOutcome join(FlowValue& destination, const FlowValue& source, const Subject&);
    FlowJoinOutcome join(FlowValue& destination, const FlowValue& source, const Subject&, FlowObjectSet& addedObjects);
    // The excluded objects of the source stay out. The unknown object never does.
    FlowJoinOutcome joinExcluding(FlowValue& destination, const FlowValue& source, const FlowObjectBitmap& excludedObjects, const Subject&, FlowObjectSet& addedObjects);
    bool isSubsetOf(const FlowValue&, const FlowValue&) const;

    FlowValue filter(const FlowValue&, const Type& target);
    FlowValue filterOut(const FlowValue&, const Type& tested);
    // Whether every value is certainly of the type. Interns nothing, so the result families may ask from several threads.
    bool isInside(const FlowValue&, const Type&) const;
    bool isObjectInside(FlowObjectID, const Type&) const;
    bool isObjectOutside(FlowObjectID, const Type&) const;

    FlowKeySet keysOf(const FlowValue& key);

    void dump(PrintStream&, const FlowValue&) const;
    void addTo(Statistics&) const;

private:
    template<typename SetID, typename Element>
    class InternedSets {
    public:
        std::span<const Element> at(SetID id) const LIFETIME_BOUND { return id ? m_sets[id.index()].span() : std::span<const Element> { }; }
        unsigned size() const { return m_sets.size(); }
        SetID intern(std::span<const Element> ascendingElements);
        SetID unite(SetID, SetID);

    private:
        Vector<Vector<Element>> m_sets;
        HashedIndex m_index;
        HashMap<std::pair<SetID, SetID>, SetID> m_unions;
    };

    FlowRangeID internRange(Range);
    void setIntegers(FlowValue&, Range);
    void setStrings(FlowValue&, FlowStringSetID, const Subject&);
    void setSymbols(FlowValue&, FlowSymbolSetID, const Subject&);
    bool joinPrimitives(FlowValue& destination, const FlowValue& source, const Subject&);
    void filterLiterals(const FlowValue&, const Type& target, FlowValue& result);
    void filterOutLiterals(const FlowValue&, const Type& tested, FlowValue& result);

    const Program& m_program;
    const FlowObjectTable& m_objects;
    TypeArena& m_types;
    BuildContext& m_build;
    BitVector m_callableClasses;
    uint64_t m_maxStrings;
    uint64_t m_maxSymbols;
    InternedSets<FlowStringSetID, StringID> m_stringSets;
    InternedSets<FlowSymbolSetID, PropertyKeyID> m_symbolSets;
    Vector<Range> m_ranges;
    HashedIndex m_rangeIndex;
    FlowRangeID m_fullRange;
};

extern template class FlowValueTable::InternedSets<FlowStringSetID, StringID>;
extern template class FlowValueTable::InternedSets<FlowSymbolSetID, PropertyKeyID>;

class FlowNaturalEvaluator {
    WTF_MAKE_NONCOPYABLE(FlowNaturalEvaluator);
public:
    static constexpr uint32_t maxOperandCount = 4;

    FlowNaturalEvaluator(const Program&, FlowValueTable&, TypeArena&);

    FlowValue evaluate(const Value& operation, std::span<const FlowValue> operands);
    FlowValue narrowOperand(const Value& test, uint32_t operandIndex, bool isOnTakenEdge, std::span<const FlowValue> operands);
    TypeID typeFor(const FlowValue&);

private:
    const Value& copyWithTypesOf(const Value& operation, std::span<const FlowValue> operands);

    const Program& m_program;
    FlowValueTable& m_values;
    TypeArena& m_types;
    Function m_scratch;
    HashMap<std::tuple<uint16_t, uint32_t, bool, unsigned>, TypeID> m_typesByValue;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
