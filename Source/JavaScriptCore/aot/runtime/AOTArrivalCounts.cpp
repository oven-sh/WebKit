/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrivalCounts.h"

#if ENABLE(AOT)

#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ArrivalCounts);

static constexpr size_t maxNamesShown = 12;

static constexpr std::array<ASCIILiteral, 7> primitiveNames { "undefined"_s, "null"_s, "boolean"_s, "number"_s, "string"_s, "symbol"_s, "bigint"_s };
static_assert(static_cast<size_t>(PrimitiveArrival::BigInt) + 1 == primitiveNames.size());

namespace {

struct ArrivalLine {
    uint32_t site;
    uint64_t count;
    String what;
};

} // anonymous namespace

static std::optional<PrimitiveArrival> primitiveArrivalOf(JSValue value)
{
    if (value.isUndefined())
        return PrimitiveArrival::Undefined;
    if (value.isNull())
        return PrimitiveArrival::Null;
    if (value.isBoolean())
        return PrimitiveArrival::Boolean;
    if (value.isNumber())
        return PrimitiveArrival::Number;
    if (value.isString())
        return PrimitiveArrival::String;
    if (value.isSymbol())
        return PrimitiveArrival::Symbol;
    if (value.isBigInt())
        return PrimitiveArrival::BigInt;
    return std::nullopt;
}

static bool hasLeftShape(const Object& object)
{
    return object.hasFlagsInOrderWord() || object.compiledStructure().hasOtherPrototype();
}

// A property gets the next free offset, so the offsets give the order in which the properties were added. Only a property that took the place of a deleted one is out of order.
static String describeNamesOf(Structure* structure)
{
    Vector<PropertyTableEntry> properties = structure->getPropertiesConcurrently();
    std::ranges::sort(properties, { }, [](const PropertyTableEntry& property) {
        return property.offset();
    });

    StringBuilder names;
    names.append("object{"_s);
    for (size_t index = 0; index < properties.size(); ++index) {
        if (index)
            names.append(',');
        if (index == maxNamesShown) {
            names.append(".."_s);
            break;
        }
        names.append(StringView(properties[index].key()));
    }
    names.append('}');
    if (structure->isDictionary())
        names.append("dict"_s);
    return names.toString();
}

ArrivalCounts::ArrivalCounts() = default;
ArrivalCounts::~ArrivalCounts() = default;

uint32_t ArrivalCounts::numberOfText(const String& text)
{
    return m_numbersOfTexts.ensure(text, [&] {
        m_texts.append(text);
        return static_cast<uint32_t>(m_texts.size() - 1);
    }).iterator->value;
}

uint32_t ArrivalCounts::numberOfNamesOf(Structure* structure)
{
    uint64_t stateOfNames = static_cast<uint64_t>(structure->propertyHash()) << 1 | structure->isDictionary();
    return m_namesOfStructures.ensure(StructureKey { structure, stateOfNames }, [&] {
        return numberOfText(describeNamesOf(structure));
    }).iterator->value;
}

uint32_t ArrivalCounts::numberOfClass(const ClassInfo* classInfo)
{
    return m_classes.ensure(classInfo, [&] {
        return numberOfText(makeString("class:"_s, classInfo->className));
    }).iterator->value;
}

auto ArrivalCounts::classify(JSValue value) -> Arrival
{
    if (std::optional<PrimitiveArrival> primitive = primitiveArrivalOf(value))
        return { ArrivalKind::Primitive, static_cast<uint32_t>(*primitive) };

    if (Object* object = compiledObjectOrNull(value))
        return { hasLeftShape(*object) ? ArrivalKind::LayoutLeftShape : ArrivalKind::Layout, object->layout().index };

    JSCell* cell = value.asCell();
    if (isFunctionType(cell->type())) {
        std::optional<FunctionID> function = functionOf(cell);
        return { ArrivalKind::Function, function ? function->index() : ArrivalKey::nativeFunction };
    }
    if (cell->type() == FinalObjectType)
        return { ArrivalKind::OrdinaryObject, numberOfNamesOf(cell->structure()) };
    return { ArrivalKind::BuiltinClass, numberOfClass(cell->classInfo()) };
}

void ArrivalCounts::record(JSValue receiver, uint32_t site)
{
    ASSERT(receiver);
    Arrival arrival = classify(receiver);
    ++m_counts.add(ArrivalKey { site, arrival.kind, arrival.id }, 0).iterator->value;
    m_lastSite = site;
    m_hasCalledNothingSinceArrival = true;
}

void ArrivalCounts::didCallOperation(Entry entry)
{
    if (!m_hasCalledNothingSinceArrival || entry == Entry::operationAOTRecordArrival)
        return;
    m_hasCalledNothingSinceArrival = false;
    ++m_operationsBehindArrivals.add(SiteAndOperation { m_lastSite, static_cast<uint16_t>(entry) }, 0).iterator->value;
}

String ArrivalCounts::describe(const ArrivalKey& key) const
{
    switch (key.kind) {
    case ArrivalKind::Layout:
        return makeString("layout:"_s, key.id);
    case ArrivalKind::LayoutLeftShape:
        return makeString("layout:"_s, key.id, "+changed"_s);
    case ArrivalKind::OrdinaryObject:
    case ArrivalKind::BuiltinClass:
        return m_texts[key.id];
    case ArrivalKind::Function:
        if (key.id == ArrivalKey::nativeFunction)
            return "function:native"_s;
        return makeString("function:"_s, key.id);
    case ArrivalKind::Primitive:
        return primitiveNames[key.id];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void sortBySiteThenByCount(Vector<ArrivalLine>& lines)
{
    std::ranges::sort(lines, [](const ArrivalLine& a, const ArrivalLine& b) {
        if (a.site != b.site)
            return a.site < b.site;
        if (a.count != b.count)
            return a.count > b.count;
        return codePointCompareLessThan(a.what, b.what);
    });
}

void ArrivalCounts::dump(PrintStream& out) const
{
    Vector<ArrivalLine> arrivals;
    arrivals.reserveInitialCapacity(m_counts.size());
    for (auto& [key, count] : m_counts)
        arrivals.append(ArrivalLine { key.site, count, describe(key) });
    sortBySiteThenByCount(arrivals);
    for (const ArrivalLine& line : arrivals)
        out.println(line.count, "\tarrival:", line.site, "\t", line.what);

    Vector<ArrivalLine> operations;
    operations.reserveInitialCapacity(m_operationsBehindArrivals.size());
    for (auto& [key, count] : m_operationsBehindArrivals)
        operations.append(ArrivalLine { key.first, count, nameOf(static_cast<Entry>(key.second)) });
    sortBySiteThenByCount(operations);
    for (const ArrivalLine& line : operations)
        out.println(line.count, "\tleft:", line.site, "\t", line.what);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRecordArrival, void, (Instance* instance, EncodedJSValue receiver, uint32_t site))
{
    State& state = *instance->state;
    if (!state.arrivalCounts) [[unlikely]]
        state.arrivalCounts = makeUnique<ArrivalCounts>();
    state.arrivalCounts->record(JSValue::decode(receiver), site);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
