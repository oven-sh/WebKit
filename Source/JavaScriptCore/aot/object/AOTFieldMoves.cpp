/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldMoves.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr OptionSet<PrimitiveSet> numberPrimitives = OptionSet<PrimitiveSet>::fromRaw(Typed::Primitive::Number);
static constexpr OptionSet<PrimitiveSet> allPrimitives = OptionSet<PrimitiveSet>::fromRaw(Typed::Primitive::All);

OptionSet<ValueKind> valueKindOf(JSValue value)
{
    if (value.isInt32())
        return ValueKind::Int32;
    if (value.isString())
        return ValueKind::String;
    if (value.isTrue())
        return ValueKind::True;
    if (value.isFalse())
        return ValueKind::False;
    if (value.isUndefined())
        return ValueKind::Undefined;
    if (value.isNull())
        return ValueKind::Null;
    if (value.isDouble())
        return ValueKind::Double;
    return { };
}

static bool integerRangeCovers(const RuntimeType& type, int64_t minimum, int64_t maximum)
{
    auto range = type.integerRange();
    return !range || (range->first <= minimum && range->second >= maximum);
}

OptionSet<ValueKind> kindsAlwaysAccepted(const RuntimeType& type)
{
    OptionSet<PrimitiveSet> primitives = type.primitiveSet();
    OptionSet<ValueKind> kinds;
    if (primitives.contains(PrimitiveSet::Integer) && integerRangeCovers(type, INT32_MIN, INT32_MAX))
        kinds.add(ValueKind::Int32);
    if (primitives.containsAll(numberPrimitives) && integerRangeCovers(type, -maxSlotInteger, maxSlotInteger))
        kinds.add(ValueKind::Double);
    if (primitives.contains(PrimitiveSet::String))
        kinds.add(ValueKind::String);
    if (primitives.contains(PrimitiveSet::True))
        kinds.add(ValueKind::True);
    if (primitives.contains(PrimitiveSet::False))
        kinds.add(ValueKind::False);
    if (primitives.contains(PrimitiveSet::Undefined))
        kinds.add(ValueKind::Undefined);
    if (primitives.contains(PrimitiveSet::Null))
        kinds.add(ValueKind::Null);
    return kinds;
}

// A slot holds undefined or null wherever it has a marker for it, whatever the type of its field says.
static bool hasEveryMarkerOf(SlotRepresentation from, SlotRepresentation to)
{
    SlotTraits fromTraits = traitsOf(from);
    SlotTraits toTraits = traitsOf(to);
    return (!fromTraits.hasUndefined || toTraits.hasUndefined) && (!fromTraits.hasNull || toTraits.hasNull);
}

static bool acceptsEveryValue(const RuntimeType& type)
{
    if (!type.primitiveSet().containsAll(allPrimitives) || type.integerRange() || !type.literalStrings().empty())
        return false;
    return type.acceptsEveryObject();
}

static bool typeAlwaysFits(const RuntimeLayoutTable& layouts, const FieldData& from, const FieldData& to)
{
    if (from.type == to.type)
        return true;
    const RuntimeType& fromType = layouts.typeOf(from);
    const RuntimeType& toType = layouts.typeOf(to);
    if (acceptsEveryValue(toType))
        return true;
    if (!fromType.cellTests().empty())
        return false;
    constexpr OptionSet<PrimitiveSet> stringPrimitives { PrimitiveSet::String, PrimitiveSet::LiteralString };
    if (!(fromType.primitiveSet() - toType.primitiveSet() - PrimitiveSet::Absent - stringPrimitives).isEmpty())
        return false;
    // As stringHasType() in AOTRuntimeLayoutTable.cpp goes: a type that lists strings takes those and no other; one that lists none takes every string or none.
    if (fromType.primitiveSet().containsAny(stringPrimitives)) {
        if (toType.literalStrings().empty()) {
            if (!toType.primitiveSet().contains(PrimitiveSet::String))
                return false;
        } else {
            if (fromType.literalStrings().empty() || !toType.primitiveSet().containsAny(stringPrimitives))
                return false;
            bool takesEveryListedString = std::ranges::all_of(fromType.literalStrings(), [&](uint32_t string) {
                return std::ranges::contains(toType.literalStrings(), string);
            });
            if (!takesEveryListedString)
                return false;
        }
    }
    auto toRange = toType.integerRange();
    if (!fromType.primitiveSet().contains(PrimitiveSet::Integer) || !toRange)
        return true;
    auto fromRange = fromType.integerRange();
    return fromRange && fromRange->first >= toRange->first && fromRange->second <= toRange->second;
}

bool alwaysFits(const RuntimeLayoutTable& layouts, const FieldData& from, const FieldData& to)
{
    if (to.keepsStateOfBirth && !from.keepsStateOfBirth)
        return false;
    return hasEveryMarkerOf(from.slot.representation(), to.slot.representation()) && typeAlwaysFits(layouts, from, to);
}

std::optional<AcceptedValue> acceptInSlot(Object& target, const FieldData& to, JSValue value)
{
    if (!value)
        return std::nullopt;
    State& state = target.state();
    std::optional<AcceptedValue> accepted = state.objects->layouts.accept(to, value);
    uint64_t scratch = 0;
    if (!accepted || !tryWriteSlot(*state.strings, to.slot.representation(), &scratch, value))
        return std::nullopt;
    return accepted;
}

static uint32_t offsetOfInlineProperty(PropertyOffset offset)
{
    ASSERT(isInlineOffset(offset));
    return JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue);
}

void FieldMovePlan::appendField(const FieldData& from, PropertyOffset to)
{
    SlotRepresentation representation = from.slot.representation();
    bool liesInCell = !(from.flags & (fieldIsOutOfLine | fieldMayExistWithoutStorage));
    bool isBoxed = false;
    bool isNarrowReference = false;
    switch (representation) {
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalInt32:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        break;
    case SlotRepresentation::Cell:
    case SlotRepresentation::JSValue:
        isBoxed = true;
        break;
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::DeferredClosure:
        isNarrowReference = true;
        break;
    }
    static_assert(representsAbsentAsZero(SlotRepresentation::JSValue) && representsAbsentAsZero(SlotRepresentation::Cell));
    sourceHasDeferredClosures |= representation == SlotRepresentation::DeferredClosure;
    if (liesInCell && isBoxed && from.slot.offset <= UINT16_MAX) {
        boxedWordMoves.append({ static_cast<uint16_t>(from.slot.offset), static_cast<uint16_t>(offsetOfInlineProperty(to)) });
        return;
    }
    moves.append({ &from, from.slot.offset, offsetOfInlineProperty(to), liesInCell && isNarrowReference });
}

void FieldMovePlan::appendProperty(PropertyOffset from, PropertyOffset to)
{
    if (isInlineOffset(from))
        boxedWordMoves.append({ static_cast<uint16_t>(offsetOfInlineProperty(from)), static_cast<uint16_t>(offsetOfInlineProperty(to)) });
    else
        moves.append({ nullptr, static_cast<uint32_t>(-offsetInButterfly(from) * sizeof(EncodedJSValue)), offsetOfInlineProperty(to), false });
}

bool applyMovesToNewObject(VM& vm, const FieldMovePlan& plan, JSFinalObject* target, JSObject* source)
{
    ASSERT(plan.isUsable());
    auto* wordsOfSource = reinterpret_cast<const uint8_t*>(source);
    auto* wordsOfTarget = reinterpret_cast<uint8_t*>(target);
    for (FieldMovePlan::BoxedWordMove move : plan.boxedWordMoves) {
        EncodedJSValue word = *reinterpret_cast<const EncodedJSValue*>(wordsOfSource + move.from);
        if (!word)
            return false;
        *reinterpret_cast<EncodedJSValue*>(wordsOfTarget + move.to) = word;
    }
    for (const FieldMovePlan::Move& move : plan.moves) {
        JSValue value;
        if (!move.field)
            value = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(reinterpret_cast<const uint8_t*>(source->butterfly()) - move.from));
        else if (move.isNarrowReferenceInCell) {
            if (slotHoldsAbsent(move.field->slot.representation(), wordsOfSource + move.from))
                return false;
            uint32_t word = *reinterpret_cast<const uint32_t*>(wordsOfSource + move.from);
            value = word >= minCell32 ? JSValue(decodeCell32(source, word)) : JSValue::decode(word);
        } else {
            Object& object = *uncheckedDowncast<Object>(source);
            value = object.readInline(*move.field);
            if (!value) [[unlikely]] {
                value = object.read(*move.field);
                if (!value)
                    return false;
            }
        }
        *reinterpret_cast<EncodedJSValue*>(wordsOfTarget + move.to) = JSValue::encode(value);
    }
    vm.writeBarrier(target);
    vm.mutatorFence();
    return true;
}

FieldMovePlanCache::FieldMovePlanCache(State& state)
{
    state.caches->add(CacheKind::WeakByKey, "field move plans"_s, *this);
}

FieldMovePlanCache::~FieldMovePlanCache() = default;

static unsigned hashOfFieldMovePlanKey(const FieldMovePlanKey& key)
{
    uint64_t words = key.sourceWords * 0x9e3779b97f4a7c15ull ^ std::rotl(key.targetWords * 0xc2b2ae3d27d4eb4full, 31) ^ std::bit_cast<uintptr_t>(key.sourceStructure) ^ static_cast<uint64_t>(key.purpose) << 56;
    if (!key.replacedFields.isEmpty())
        words ^= key.replacedFields.hash();
    return static_cast<unsigned>(words >> 32 ^ words);
}

FieldMovePlanCache::Set& FieldMovePlanCache::setFor(const FieldMovePlanKey& key)
{
    ASSERT(m_sets);
    return (*m_sets)[hashOfFieldMovePlanKey(key) % numberOfSets];
}

const FieldMovePlan* FieldMovePlanCache::find(const FieldMovePlanKey& key)
{
    ASSERT(key.purpose != FieldMovePlanPurpose::Forgotten);
    const Entry*& lastFound = m_lastFound[static_cast<size_t>(key.purpose)];
    if (lastFound && lastFound->key == key)
        return lastFound->plan.get();
    if (!m_sets)
        return nullptr;
    for (auto& way : setFor(key).ways) {
        if (way && way->key == key) {
            lastFound = way.get();
            return way->plan.get();
        }
    }
    return nullptr;
}

const FieldMovePlan& FieldMovePlanCache::add(VM& vm, FieldMovePlanKey&& key, std::unique_ptr<FieldMovePlan>&& plan)
{
    ASSERT(plan);
    if (!m_sets)
        m_sets = makeUniqueWithoutFastMallocCheck<Sets>();
    m_lastFound.fill(nullptr);
    Set& set = setFor(key);
    auto way = std::ranges::find_if(set.ways, [](auto& candidate) {
        return !candidate || candidate->key.purpose == FieldMovePlanPurpose::Forgotten;
    });
    if (way == set.ways.end())
        way = set.ways.begin() + set.nextVictim++ % waysPerSet;
    *way = makeUnique<Entry>(WTF::move(key), WTF::move(plan));
    m_writes.recordAll();
    count(vm, RuntimeCounterID::FieldMovePlansMade);
    return *(*way)->plan;
}

size_t FieldMovePlanCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t removed = 0;
    auto isDead = [&](Structure* structureOrNull) {
        return structureOrNull && !vm.heap.isMarked(structureOrNull);
    };
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_lastFound.fill(nullptr);
        for (Set& set : *m_sets) {
            for (auto& way : set.ways) {
                if (!way || (!isDead(way->key.sourceStructure) && !isDead(way->plan->structure)))
                    continue;
                way->key.purpose = FieldMovePlanPurpose::Forgotten;
                way->key.sourceStructure = nullptr;
                way->plan->structure = nullptr;
                ++removed;
            }
        }
    });
    return removed;
}

static std::unique_ptr<FieldMovePlan> createCloneAsPlan(VM& vm, State& state, Structure* structure, const LayoutData& layout, const BitVector& replaced)
{
    auto plan = makeUnique<FieldMovePlan>();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    Vector<UniquedStringImpl*, 16> names;
    BitVector present;
    structure->forEachProperty(vm, [&](const auto& entry) {
        const FieldData* field = layouts.field(vm, layout, entry.key());
        uint16_t index = field ? layout.indexOf(*field) : layout.numberOfFields;
        present.set(index);
        names.append(entry.key());
        plan->clonedMembers.append({ entry.offset(), index });
        return true;
    });
    for (size_t index : replaced) {
        if (present.get(index))
            continue;
        names.append(layouts.nameOf(vm, layout.fields()[index]));
        plan->clonedMembers.append({ invalidOffset, static_cast<uint16_t>(index) });
    }
    plan->builder = ObjectBuilder::tryCreatePlan(state, layout, names.span());
    return plan;
}

Object* tryCloneAs(VM& vm, JSObject* source, const LayoutData& layout, std::span<const EncodedJSValue> replacedFields)
{
    RELEASE_ASSERT(replacedFields.size() == layout.numberOfFields);
    if (!isPlainDataObject(source))
        return nullptr;
    Structure* structure = source->structure();
    Instance* instance = Instance::of(structure->realm());
    if (!instance)
        return nullptr;
    State& state = *instance->state;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    if (!layouts.hasLayoutForShape(layout.shape) || &layouts.layoutForShape(layout.shape) != &layout)
        return nullptr;

    FieldMovePlanKey key { FieldMovePlanPurpose::CloneAs };
    key.targetWords = static_cast<uint64_t>(shapeWordOf(layout.shape)) << 32;
    key.sourceStructure = structure;
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (JSValue::decode(replacedFields[i]))
            key.replacedFields.set(i);
    }
    DeferGC deferGC(vm);
    FieldMovePlanCache& plans = state.objects->caches->movePlans;
    const FieldMovePlan* plan = plans.find(key);
    if (!plan) {
        auto created = createCloneAsPlan(vm, state, structure, layout, key.replacedFields);
        plan = &plans.add(vm, WTF::move(key), WTF::move(created));
    }
    if (!plan->builder)
        return nullptr;
    ObjectBuilder builder(vm, *plan->builder);
    unsigned index = 0;
    for (auto [from, field] : plan->clonedMembers) {
        JSValue replacement = field < layout.numberOfFields ? JSValue::decode(replacedFields[field]) : JSValue();
        if (!builder.tryStore(index++, replacement ? replacement : source->getDirect(from)))
            return nullptr;
    }
    JSObject* created = builder.finish();
    if (!created)
        return nullptr;
    Object* object = uncheckedDowncast<Object>(created);
    object->tryFinishBirth();
    return object;
}

JSFinalObject* tryCopyAsOrdinaryObject(VM& vm, JSGlobalObject* realm, JSObject* source, ExcludedRun excluded)
{
    Instance* instance = Instance::of(realm);
    if (!instance || source->structure()->realm() != realm)
        return nullptr;
    State& state = *instance->state;
    Object* compiled = compiledObjectOrNull(source);
    FieldMovePlanKey key { FieldMovePlanPurpose::CopyAsOrdinaryObject };
    key.targetWords = excluded.numberPlusOne;
    if (compiled) {
        if (compiled->hasExtraProperties() || compiled->hasOutOfLineKeyOrder() || compiled->layout().keysAreNeverEnumerated)
            return nullptr;
        key.sourceWords = compiled->orderAndShapeWords() & orderCountAndShapeMask;
    } else {
        if (source->type() != FinalObjectType)
            return nullptr;
        key.sourceStructure = source->structure();
    }

    DeferGC deferGC(vm);
    FieldMovePlanCache& plans = state.objects->caches->movePlans;
    const FieldMovePlan* plan = plans.find(key);
    if (!plan)
        plan = &plans.add(vm, WTF::move(key), state.objects->caches->assignmentPlans.createPlanForOrdinaryCopy(vm, realm, source, excluded));
    if (!plan->isUsable())
        return nullptr;
    if (plan->sourceHasDeferredClosures)
        compiled->materializeDeferredClosures(vm);
    JSFinalObject* copy = JSFinalObject::create(vm, plan->structure);
    return applyMovesToNewObject(vm, *plan, copy, source) ? copy : nullptr;
}

bool isPlainDataObject(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != FinalObjectType)
        return false;
    Structure* structure = value.asCell()->structure();
    return !structure->hasPolyProto()
        && structure->storedPrototype() == structure->realm()->objectPrototype()
        && !hasIndexedProperties(structure->indexingType())
        && !structure->isDictionary()
        && structure->isStructureExtensible()
        && !structure->hasAnyKindOfGetterSetterProperties()
        && !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto()
        && !structure->hasNonEnumerableProperties()
        && !structure->hasNonConfigurableProperties()
        && !structure->hasUnderscoreProtoPropertyExcludingOriginalProto()
        && structure->isQuickPropertyAccessAllowedForEnumeration();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
