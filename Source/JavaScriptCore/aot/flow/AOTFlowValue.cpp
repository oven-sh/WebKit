/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowValue.h"

#if ENABLE(AOT)

#include "AOTFlowObjects.h"
#include "AOTIRPrinter.h"
#include "AOTPrimitiveKeyTexts.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlowValueTable);

BitVector findCallableClasses(const Program&);

static constexpr uint16_t plainPrimitives = Primitive::Undefined | Primitive::Null | Primitive::False | Primitive::True | Primitive::WholeNumber | Primitive::FractionalNumber | Primitive::BigInt | Primitive::Absent;
static constexpr uint16_t keyPrimitives = Primitive::String | Primitive::Symbol;
static constexpr uint16_t literalPrimitives = Primitive::LiteralString | Primitive::LiteralSymbol;

static_assert((plainPrimitives | keyPrimitives | literalPrimitives | Primitive::Integer) == Primitive::Defined);

template<typename Element>
static Vector<Element, 32> unionOf(std::span<const Element> a, std::span<const Element> b)
{
    Vector<Element, 32> united(a.size() + b.size());
    united.shrink(std::ranges::set_union(a, b, united.begin()).out - united.begin());
    return united;
}

template<typename SetID, typename Element>
SetID FlowValueTable::InternedSets<SetID, Element>::intern(std::span<const Element> elements)
{
    if (elements.empty())
        return { };
    ASSERT(std::ranges::adjacent_find(elements, std::ranges::greater_equal { }) == elements.end());

    Hasher hasher;
    for (Element element : elements)
        add(hasher, element);
    uint32_t hash = hasher.hash();

    auto existing = m_index.find(hash, [&](uint32_t position) {
        return std::ranges::equal(m_sets[position], elements);
    });
    if (existing)
        return SetID(*existing);

    SetID id(m_sets.size());
    m_sets.append(Vector<Element>(elements));
    m_index.add(hash, id.index());
    return id;
}

template<typename SetID, typename Element>
SetID FlowValueTable::InternedSets<SetID, Element>::unite(SetID a, SetID b)
{
    if (a == b || !b)
        return a;
    if (!a)
        return b;

    auto key = a < b ? std::pair { a, b } : std::pair { b, a };
    return m_unions.ensure(key, [&] {
        return intern(unionOf(at(a), at(b)).span());
    }).iterator->value;
}

template class FlowValueTable::InternedSets<FlowStringSetID, StringID>;
template class FlowValueTable::InternedSets<FlowSymbolSetID, PropertyKeyID>;

FlowValueTable::FlowValueTable(const Program& program, const FlowObjectTable& objects, TypeArena& types, BuildContext& build)
    : m_program(program)
    , m_objects(objects)
    , m_types(types)
    , m_build(build)
    , m_callableClasses(findCallableClasses(program))
    , m_maxStrings(build.limit(LimitID::MaxLiteralsInType))
    , m_maxSymbols(build.limit(LimitID::MaxSymbolLiteralsInType))
{
    m_fullRange = internRange(Range::full());
}

FlowRangeID FlowValueTable::internRange(Range range)
{
    if (range.isEmpty())
        return { };
    RELEASE_ASSERT(range.isSubsetOf(Range::full()));

    uint32_t hash = computeHash(range.minimum, range.maximum);
    auto existing = m_rangeIndex.find(hash, [&](uint32_t position) {
        return m_ranges[position] == range;
    });
    if (existing)
        return FlowRangeID(*existing);

    FlowRangeID id(m_ranges.size());
    m_ranges.append(range);
    m_rangeIndex.add(hash, id.index());
    return id;
}

void FlowValueTable::setIntegers(FlowValue& value, Range range)
{
    value.integers = internRange(range);
    if (value.integers)
        value.primitives |= Primitive::Integer;
    else
        value.primitives &= ~Primitive::Integer;
}

void FlowValueTable::setStrings(FlowValue& value, FlowStringSetID set, const Subject& subject)
{
    value.primitives &= ~Primitive::LiteralString;
    value.strings = { };
    if (!set)
        return;
    if (stringsOf(set).size() > m_maxStrings) {
        value.primitives |= Primitive::String;
        m_build.gaveUp(LimitID::MaxLiteralsInType, subject);
        return;
    }

    ASSERT(!(value.primitives & Primitive::String));
    value.primitives |= Primitive::LiteralString;
    value.strings = set;
}

void FlowValueTable::setSymbols(FlowValue& value, FlowSymbolSetID set, const Subject& subject)
{
    value.primitives &= ~Primitive::LiteralSymbol;
    value.symbols = { };
    if (!set)
        return;
    if (symbolsOf(set).size() > m_maxSymbols) {
        value.primitives |= Primitive::Symbol;
        m_build.gaveUp(LimitID::MaxSymbolLiteralsInType, subject);
        return;
    }

    ASSERT(!(value.primitives & Primitive::Symbol));
    value.primitives |= Primitive::LiteralSymbol;
    value.symbols = set;
}

FlowValue FlowValueTable::valueOfObject(FlowObjectID object) const
{
    RELEASE_ASSERT(object);
    FlowValue value;
    value.objects = FlowObjectSet(object);
    return value;
}

FlowValue FlowValueTable::valueOfPrimitives(uint16_t bits)
{
    FlowValue value;
    value.primitives = bits & (plainPrimitives | keyPrimitives);
    if (bits & Primitive::LiteralString)
        value.primitives |= Primitive::String;
    if (bits & Primitive::LiteralSymbol)
        value.primitives |= Primitive::Symbol;
    if (bits & Primitive::Integer) {
        value.primitives |= Primitive::Integer;
        value.integers = m_fullRange;
    }
    return value;
}

FlowValue FlowValueTable::valueOfIntegers(Range range)
{
    FlowValue value;
    setIntegers(value, range);
    return value;
}

FlowValue FlowValueTable::valueOfString(StringID string)
{
    RELEASE_ASSERT(string);
    FlowValue value;
    setStrings(value, m_stringSets.intern(singleElementSpan(string)), Subject { });
    return value;
}

FlowValue FlowValueTable::valueOfSymbol(PropertyKeyID symbol)
{
    RELEASE_ASSERT(symbol);
    FlowValue value;
    setSymbols(value, m_symbolSets.intern(singleElementSpan(symbol)), Subject { });
    return value;
}

FlowValue FlowValueTable::unknownValue()
{
    FlowValue value;
    value.primitives = Primitive::All;
    value.integers = m_fullRange;
    value.objects = FlowObjectSet(unknownFlowObject);
    return value;
}

FlowValue FlowValueTable::unknownObjectValue() const
{
    return valueOfObject(unknownFlowObject);
}

FlowValue FlowValueTable::valueOfType(const Type& type)
{
    FlowValue value = valueOfPrimitives(type.primitives & (plainPrimitives | keyPrimitives));
    if (type.primitives & Primitive::Integer)
        setIntegers(value, type.integers);
    if (!(type.primitives & Primitive::String))
        setStrings(value, m_stringSets.intern(type.stringLiterals.span()), Subject { });
    if (!(type.primitives & Primitive::Symbol))
        setSymbols(value, m_symbolSets.intern(type.symbolLiterals.span()), Subject { });

    for (BuiltinID builtin : type.builtins) {
        FlowObjectID object = m_objects.findBuiltin(builtin);
        value.objects.add(object ? object : unknownFlowObject);
    }
    if (type.anyObject || type.anyFunction || type.layouts || !type.classes.isEmpty() || !type.functions.isEmpty() || type.environment)
        value.objects.add(unknownFlowObject);
    return value;
}

FlowJoinOutcome FlowValueTable::join(FlowValue& destination, const FlowValue& source, const Subject& subject)
{
    FlowObjectSet ignored;
    return join(destination, source, subject, ignored);
}

FlowJoinOutcome FlowValueTable::join(FlowValue& destination, const FlowValue& source, const Subject& subject, FlowObjectSet& addedObjects)
{
    FlowJoinOutcome outcome;
    outcome.didChangePrimitives = joinPrimitives(destination, source, subject);
    outcome.didChangeObjects = destination.objects.uniteAndCollect(source.objects, addedObjects);
    return outcome;
}

FlowJoinOutcome FlowValueTable::joinExcluding(FlowValue& destination, const FlowValue& source, const FlowObjectBitmap& excludedObjects, const Subject& subject, FlowObjectSet& addedObjects)
{
    FlowJoinOutcome outcome;
    outcome.didChangePrimitives = joinPrimitives(destination, source, subject);
    outcome.didChangeObjects = destination.objects.uniteAndCollectExcluding(source.objects, excludedObjects, addedObjects);
    return outcome;
}

bool FlowValueTable::joinPrimitives(FlowValue& destination, const FlowValue& source, const Subject& subject)
{
    if (bringsNoPrimitives(source, destination))
        return false;

    uint16_t previousPrimitives = destination.primitives;
    FlowStringSetID previousStrings = destination.strings;
    FlowSymbolSetID previousSymbols = destination.symbols;
    FlowRangeID previousIntegers = destination.integers;

    destination.primitives |= source.primitives & plainPrimitives;

    Range integers = rangeOf(destination.integers);
    Range unitedIntegers = integers.unionWith(rangeOf(source.integers));
    if (unitedIntegers != integers)
        setIntegers(destination, unitedIntegers);

    if (source.primitives & Primitive::String) {
        destination.primitives |= Primitive::String;
        setStrings(destination, { }, subject);
    } else if (!(destination.primitives & Primitive::String))
        setStrings(destination, m_stringSets.unite(destination.strings, source.strings), subject);

    if (source.primitives & Primitive::Symbol) {
        destination.primitives |= Primitive::Symbol;
        setSymbols(destination, { }, subject);
    } else if (!(destination.primitives & Primitive::Symbol))
        setSymbols(destination, m_symbolSets.unite(destination.symbols, source.symbols), subject);

    return destination.primitives != previousPrimitives || destination.strings != previousStrings || destination.symbols != previousSymbols || destination.integers != previousIntegers;
}

bool FlowValueTable::isSubsetOf(const FlowValue& a, const FlowValue& b) const
{
    if (a.primitives & (plainPrimitives | keyPrimitives) & ~b.primitives)
        return false;
    if (a.integers != b.integers && !rangeOf(a.integers).isSubsetOf(rangeOf(b.integers)))
        return false;
    if (!(b.primitives & Primitive::String) && a.strings != b.strings && !std::ranges::includes(stringsOf(b.strings), stringsOf(a.strings)))
        return false;
    if (!(b.primitives & Primitive::Symbol) && a.symbols != b.symbols && !std::ranges::includes(symbolsOf(b.symbols), symbolsOf(a.symbols)))
        return false;
    return a.objects.isSubsetOf(b.objects);
}

FlowKeySet FlowValueTable::keysOf(const FlowValue& key)
{
    FlowKeySet keys;
    for (StringID string : stringsOf(key.strings))
        keys.unite(keySetOf(m_program, m_types.textKey(string)));
    for (PropertyKeyID symbol : symbolsOf(key.symbols))
        keys.names.add(symbol);
    if (key.primitives & Primitive::Symbol)
        keys.anySymbol = true;

    // The digits of a large bigint are no canonical numeric string.
    bool mayBeAnyString = key.primitives & (Primitive::String | Primitive::BigInt);
    bool mayBeAnyNumber = key.primitives & (Primitive::WholeNumber | Primitive::FractionalNumber);

    Range integers = rangeOf(key.integers);
    keys.unite(FlowKeySet::index(integers));
    if (!integers.isSubsetOf(arrayIndexRange))
        mayBeAnyNumber = true;

    for (auto [primitive, text] : primitiveKeyTexts) {
        if (!(key.primitives & primitive))
            continue;
        if (StringID string = m_program.strings.find(byteCast<uint8_t>(text.span())))
            keys.unite(keySetOf(m_program, m_types.textKey(string)));
        else
            mayBeAnyString = true;
    }

    if (mayBeAnyNumber)
        keys.unite(FlowKeySet::everyNumber());
    if (mayBeAnyString)
        keys.unite(FlowKeySet::everyString());
    if (key.mayBeObject())
        keys.unite(FlowKeySet::everyKey());
    return keys;
}

void FlowValueTable::dump(PrintStream& out, const FlowValue& value) const
{
    Type primitivePart;
    primitivePart.primitives = value.primitives;
    primitivePart.integers = rangeOf(value.integers);
    primitivePart.stringLiterals = SortedSet<StringID>(stringsOf(value.strings));
    primitivePart.symbolLiterals = SortedSet<PropertyKeyID>(symbolsOf(value.symbols));

    CommaPrinter bar("|"_s);
    out.print("{"_s);
    if (!primitivePart.isNever()) {
        out.print(bar);
        printType(out, m_program, primitivePart);
    }
    value.objects.forEach([&](FlowObjectID object) {
        out.print(bar);
        m_objects.dump(out, object);
    });
    out.print("}"_s);
}

void FlowValueTable::addTo(Statistics& statistics) const
{
    statistics.count(StatisticID::FlowLiteralSets, m_stringSets.size() + m_symbolSets.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
