/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTTypeArena.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkStrings.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet keyedAccesses { Opcode::ElemGet, Opcode::ElemSet };
static constexpr OpcodeSet namedAccesses { Opcode::PropGet, Opcode::PropSet };
static constexpr OpcodeSet storesAtAddresses { Opcode::ElemSet, Opcode::PropSet };
static constexpr size_t maxLayoutsOfHolder = 4;

using HolderLayouts = Vector<LayoutID, maxLayoutsOfHolder>;

static HolderLayouts exactLayoutsOf(Walker& walker, TypeID holder)
{
    TypeArena& types = walker.types();
    Type objects = types.at(types.withoutNullish(holder));
    if (!objects.hasObjects() || !objects.hasOnlyLayouts() || objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return { };
    HolderLayouts layouts;
    for (LayoutID layout : membersOf(types, objects)) {
        if (layouts.size() == maxLayoutsOfHolder)
            return { };
        layouts.append(layout);
    }
    return layouts;
}

static bool hasBoxedFields(Walker& walker, LayoutID layout)
{
    for (FieldIndex field = 0; field < walker.program().at(layout).fields.size(); ++field) {
        if (walker.plan().slot(layout, field).representation != SlotRepresentation::JSValue)
            return false;
    }
    return true;
}

static bool fitsFieldTypes(Walker& walker, TypeID holder, TypeID stored)
{
    HolderLayouts layouts = exactLayoutsOf(walker, holder);
    return !layouts.isEmpty() && std::ranges::all_of(layouts, [&](LayoutID layout) {
        for (FieldIndex field = 0; field < walker.program().at(layout).fields.size(); ++field) {
            TypeID held = walker.plan().slot(layout, field).typeThatStoresAreHeldTo();
            if (held && !walker.types().isSubset(stored, TypeTable::valuesOf(held)))
                return false;
        }
        return true;
    });
}

static bool hasNamedFields(const Program& program, LayoutID layout)
{
    std::span<const Field> fields = program.at(layout).fields;
    return !fields.empty() && std::ranges::all_of(fields, [&](const Field& field) {
        return field.key && program.at(field.key).kind == PropertyKeyKind::Text;
    });
}

static bool findsOnlyFields(Walker& walker, TypeID holder)
{
    HolderLayouts layouts = holder ? exactLayoutsOf(walker, holder) : HolderLayouts();
    return !layouts.isEmpty() && std::ranges::all_of(layouts, [&](LayoutID layout) { return hasNamedFields(walker.program(), layout); });
}

static void addFieldsOf(Walker& walker, TypeID holder, LocationMasks& masks)
{
    for (LayoutID layout : exactLayoutsOf(walker, holder)) {
        for (const Field& field : walker.program().at(layout).fields)
            masks.add(walker.program(), MemoryLocation::objectField(field.key));
    }
}

static void narrowEffects(Walker& walker, Opcode opcode, TypeID holder, bool findsFields, Effects& effects)
{
    constexpr AbstractHeapSet named { AbstractHeap::NamedProperties };
    constexpr AbstractHeapSet fields { AbstractHeap::ObjectFields };
    if (opcode == Opcode::PutKeyedProperty) {
        effects.writes.remove(named);
        if (findsFields) {
            effects.writes.remove(fields);
            addFieldsOf(walker, holder, effects.writes);
        }
        return;
    }
    RELEASE_ASSERT(opcode == Opcode::GetKeyedProperty || opcode == Opcode::GetKeyedValueOrAbsent);
    if (!findsFields)
        return;
    effects.reads.remove(fields);
    addFieldsOf(walker, holder, effects.reads);
}

static void narrowToName(Walker& walker, Opcode opcode, PropertyKeyID name, Effects& effects)
{
    bool isStore = opcode == Opcode::PutKeyedProperty;
    LocationMasks& masks = isStore ? effects.writes : effects.reads;
    masks.remove(AbstractHeapSet { AbstractHeap::ObjectFields });
    masks.add(walker.program(), MemoryLocation::objectField(name));
    if (isStore)
        masks.add(walker.program(), MemoryLocation::objectField(walker.program().keyOf(BuiltinNames::__proto__)));
}

void narrowNamedLookupEffects(const Program& program, PropertyKeyID name, Effects& effects)
{
    if (!name || liesInThread(name) || program.at(name).kind != PropertyKeyKind::Text || program.isIndexLike(name) || program.nameClassOf(name) != NameClass::Ordinary)
        return;
    effects.reads.remove(AbstractHeapSet { AbstractHeap::ObjectFields });
    effects.reads.add(program, MemoryLocation::objectField(name));
}

void narrowKeyedAccessEffects(Walker& walker, const Draft& draft, Effects& effects)
{
    bool isLookup = draft.opcode == Opcode::GetKeyedValueOrAbsent;
    const Operation* address = isLookup ? nullptr : &walker.code().at(draft.addressOperand().id());
    narrowEffects(walker, draft.opcode, walker.typeOf(draft.objectOperand()), isLookup ? draft.findsOnlyFields() : address->findsOnlyFields(), effects);
    StringID text = isLookup || address->findsOnlyFields() ? StringID() : textKnownForStrings(walker, referenceToRecordForStrings(walker, address->propertyOperand()));
    PropertyKeyID name = text && !liesInThread(text) ? walker.types().textKey(text) : PropertyKeyID();
    if (name && !liesInThread(name) && !walker.program().isIndexLike(name) && walker.program().nameClassOf(name) != NameClass::Proto)
        narrowToName(walker, draft.opcode, name, effects);
}

static bool mayBeName(Walker& walker, const BodyValue& access, TypeID key, TypeID holder)
{
    constexpr uint16_t names = Primitive::AnyString | Primitive::AnySymbol;
    TypeArena& types = walker.types();
    uint16_t primitives = types.at(key).primitives;
    if (!(primitives & names))
        return false;
    if (!(primitives & Primitive::Number))
        return true;
    TypeID expected = findExpectation(walker, findUpdatedKey(walker, walker.currentBodyIndex(), access.propertyOperand())).type;
    if (expected && !types.isUninformative(expected)) {
        primitives = types.at(expected).primitives;
        return (primitives & names) && !(primitives & Primitive::Number);
    }
    return !types.isUninformative(holder);
}

static bool isGlobalThis(Walker& walker, const Type& type)
{
    BuiltinID named = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    return named && walker.program().builtins().canonicalID(named) == walker.program().builtins().canonicalID(Builtins::GlobalThis);
}

static bool mayHoldNames(Walker& walker, const BodyValue& access, TypeID holder, bool isStoredInto)
{
    TypeArena& types = walker.types();
    if (!holder || holdsOnlyPrimitives(holder))
        return false;
    auto mayGiveAddress = [&](TypeID said) {
        TypeID objects = types.withoutNullish(said);
        if (isNeverType(objects))
            return false;
        Type type = types.at(objects);
        if (isGlobalThis(walker, type))
            return true;
        if (!type.hasObjects() || type.hasOnlyClosures() || type.hasOnlyClassObjects() || type.hasOnlyBuiltins())
            return false;
        return !isStoredInto || std::ranges::all_of(membersOf(types, type), [&](LayoutID layout) { return hasBoxedFields(walker, layout); });
    };
    if (!types.isUninformative(holder))
        return mayGiveAddress(holder);
    TypeID expected = findExpectation(walker, access.objectOperand()).type;
    return !expected || types.isUninformative(expected) || mayGiveAddress(expected);
}

static bool isFoundBeforeLoop(Walker& walker, const BodyIndex& index, const BodyValue& access, ValueID operand)
{
    ValueID source = index.stripCasts(findUpdatedKey(walker, index, operand));
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    if (!loop)
        return walker.isInLoop() && walker.isInvariantInInnermostLoop(walker.referenceOf(operand));
    uint32_t firstValueOfLoop = index.block(index.loop(loop).head).firstParameter().index();
    for (unsigned depth = 2; source && depth--;) {
        if (source.index() < firstValueOfLoop)
            return true;
        if (index.isParameter(source))
            return false;
        BodyValue definition = index.value(source);
        bool isGlobalThisByName = definition.opcode() == Opcode::PropGet && definition.isFreeName() && definition.key() == walker.program().keyOf(BuiltinNames::globalThis);
        if (definition.opcode() == Opcode::GetModuleVariable || definition.opcode() == Opcode::Builtin || isGlobalThisByName)
            return true;
        if (definition.opcode() != Opcode::CheckInitialized)
            return false;
        source = index.stripCasts(definition.operand(0));
    }
    return false;
}

static bool isCallee(const BodyIndex& index, const BodyValue& access)
{
    ValueID user = storesAtAddresses.contains(access.opcode()) ? ValueID() : index.onlyUsingValue(access.id());
    return user && index.opcodeOf(user) == Opcode::Call && index.value(user).as<Opcode::Call>().calleeOperand() == access.id();
}

static bool storesUnderKeys(const BodyIndex& index, const BodyValue& access)
{
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    auto stores = [&](BlockID block) {
        return std::ranges::any_of(index.block(block).values(), [](BodyValue operation) { return operation.opcode() == Opcode::ElemSet; });
    };
    return loop ? std::ranges::any_of(index.membersOf(loop), stores) : index.holdsAnyOf(OpcodeSet { Opcode::ElemSet });
}

bool mayUseKeyedAddress(Walker& walker, const BodyValue& access, TypeID holder, TypeID key)
{
    if (!keyedAccesses.contains(access.opcode()) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !holder || !key)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    constexpr uint16_t names = Primitive::AnyString | Primitive::AnySymbol;
    if (!(walker.types().at(key).primitives & names) || !isFoundBeforeLoop(walker, index, access, access.propertyOperand()) || !isFoundBeforeLoop(walker, index, access, access.objectOperand()))
        return false;
    return !isCallee(index, access) && mayBeName(walker, access, key, holder) && mayHoldNames(walker, access, holder, storesUnderKeys(index, access));
}

static constexpr uint32_t variableHolder = 1u << 31;
static constexpr uint32_t noLoop = UINT32_MAX;
static constexpr uint32_t bodyWithoutLoops = noLoop - 1;

static uint32_t holderOf(const BodyIndex& index, ValueID operand)
{
    ValueID source = index.stripCasts(operand);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::GetModuleVariable)
        return variableHolder | index.value(source).variable().index();
    return source ? source.index() : 0;
}

static constexpr uint32_t updatesField = UINT32_MAX;

static uint32_t updatedHolderOf(const BodyIndex& index, ValueID operand)
{
    static constexpr uint32_t elementHolder = 1u << 30;
    ValueID source = index.stripCasts(operand);
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::ElemGet)
        return elementHolder | holderOf(index, index.value(source).objectOperand());
    return holderOf(index, operand);
}

static constexpr uint32_t nameOfWords = 1u << 30;

static bool isWrittenInteger(Walker& walker, const BodyIndex& index, ValueID parameter)
{
    BlockID block = index.blockOf(parameter);
    if (block == entryBlock || index.isLoopHead(block) || index.isEnteredByHandlerEdge(block))
        return false;
    uint32_t position = parameter.index() - index.block(block).parameter(0).index();
    return std::ranges::all_of(walker.incomingEdgesOf(index, block), [&](const BodyIncomingEdge& edge) {
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID argument = position < arguments.size() ? index.stripCasts(arguments[position]) : ValueID();
        return argument && !index.isParameter(argument) && index.opcodeOf(argument) == Opcode::Integer;
    });
}

static bool isSumOfWords(Walker& walker, const BodyIndex& index, PropertyKeyID key, ValueID written, unsigned& termsLeft, bool& someTermIsWord)
{
    ValueID term = index.stripCasts(written);
    if (!term || !termsLeft)
        return false;
    --termsLeft;
    if (index.isParameter(term))
        return isWrittenInteger(walker, index, term);
    BodyValue made = index.value(term);
    if (allBitwiseOperators.contains(made.opcode())) {
        someTermIsWord = true;
        return true;
    }
    if (made.opcode() == Opcode::PropGet)
        return !made.isFreeName() && made.key() == key;
    if (made.opcode() != Opcode::Add && made.opcode() != Opcode::Sub)
        return made.opcode() == Opcode::Integer;
    return isSumOfWords(walker, index, key, made.operand(0), termsLeft, someTermIsWord) && isSumOfWords(walker, index, key, made.operand(1), termsLeft, someTermIsWord);
}

static bool isWordSumStore(Walker& walker, const BodyIndex& index, const BodyValue& store)
{
    static constexpr unsigned maxTerms = 24;
    static constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub };
    ValueID stored = index.stripCasts(store.valueOperand());
    if (!stored || index.isParameter(stored) || !sums.contains(index.opcodeOf(stored)))
        return false;
    unsigned termsLeft = maxTerms;
    bool someTermIsWord = false;
    return isSumOfWords(walker, index, store.key(), stored, termsLeft, someTermIsWord) && someTermIsWord;
}

static uint64_t entryOfHolder(uint32_t loop, uint32_t holder)
{
    return static_cast<uint64_t>(loop) << 32 | holder;
}

struct FieldStoredInLoop {
    uint32_t loop;
    uint32_t holder;
    uint32_t key;
    friend auto operator<=>(const FieldStoredInLoop&, const FieldStoredInLoop&) = default;
};

static void addLoopsUpdatingFields(const BodyIndex& index, Vector<FieldStoredInLoop>& stored, Vector<uint64_t>& found)
{
    std::ranges::sort(stored);
    uint32_t loopLookedAt = noLoop;
    for (const FieldStoredInLoop& store : stored) {
        if (store.loop == loopLookedAt)
            continue;
        loopLookedAt = store.loop;
        auto readsStoredField = [&](BlockID member) {
            return std::ranges::any_of(index.block(member).values(), [&](BodyValue read) {
                return read.opcode() == Opcode::PropGet && !read.isFreeName() && std::ranges::binary_search(stored, FieldStoredInLoop { store.loop, updatedHolderOf(index, read.objectOperand()), read.key().index() });
            });
        };
        bool updates = false;
        if (store.loop == bodyWithoutLoops) {
            for (uint32_t block = 0; block < index.numberOfBlocks(); ++block)
                updates |= index.isReachable(BlockID(block)) && readsStoredField(BlockID(block));
        } else
            updates = std::ranges::any_of(index.membersOf(LoopID(store.loop)), readsStoredField);
        if (updates)
            found.append(entryOfHolder(store.loop, updatesField));
    }
}

const Vector<uint64_t>& Walker::holdersStoredByName(const BodyIndex& index)
{
    return m_holdersStoredByName.ensure(&index, [&] {
        Vector<uint64_t> found;
        Vector<FieldStoredInLoop> stored;
        if (!index.holdsAnyOf(OpcodeSet { Opcode::PropSet }))
            return found;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            if (!index.isReachable(BlockID(block)))
                continue;
            for (BodyValue operation : index.block(BlockID(block)).values()) {
                if (operation.opcode() != Opcode::PropSet)
                    continue;
                uint32_t holder = holderOf(index, operation.objectOperand());
                LoopID loop = index.loopOf(BlockID(block));
                found.append(entryOfHolder(noLoop, holder));
                if (loop && index.loop(loop).numberOfLoopsWithin == 1 && !operation.isFreeName())
                    stored.append({ loop.index(), updatedHolderOf(index, operation.objectOperand()), operation.key().index() });
                else if (!loop && !index.numberOfLoops() && !operation.isFreeName())
                    stored.append({ bodyWithoutLoops, updatedHolderOf(index, operation.objectOperand()), operation.key().index() });
                if (loop && !operation.isFreeName() && operation.key() && isWordSumStore(*this, index, operation))
                    found.append(entryOfHolder(loop.index(), nameOfWords | operation.key().index()));
                for (; loop; loop = index.loop(loop).parent)
                    found.append(entryOfHolder(loop.index(), holder));
            }
        }
        addLoopsUpdatingFields(index, stored, found);
        std::ranges::sort(found);
        return found;
    }).iterator->value;
}

bool Walker::storesByName(const BodyIndex& index, const BodyValue& access)
{
    const Vector<uint64_t>& stored = holdersStoredByName(index);
    if (stored.isEmpty())
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    return std::ranges::binary_search(stored, entryOfHolder(loop ? loop.index() : noLoop, holderOf(index, access.objectOperand())));
}

bool Walker::storesSumOfWords(const BodyIndex& index, LoopID loop, PropertyKeyID key)
{
    const Vector<uint64_t>& stored = holdersStoredByName(index);
    return !stored.isEmpty() && std::ranges::binary_search(stored, entryOfHolder(loop.index(), nameOfWords | key.index()));
}

bool Walker::updatesFieldInLoop(const BodyIndex& index, LoopID loop)
{
    return std::ranges::binary_search(holdersStoredByName(index), entryOfHolder(loop ? loop.index() : bodyWithoutLoops, updatesField));
}

static bool anyOuterLoopStoresName(Walker& walker, const BodyIndex& index, const BodyValue& access, PropertyKeyID key)
{
    LoopID outermost = index.loopOf(index.blockOf(access.id()));
    if (!outermost)
        return false;
    while (LoopID around = index.loop(outermost).parent)
        outermost = around;
    return walker.textOfOutermostLoopStoresUnderNameInCurrentBody(outermost, key);
}

static bool isFieldFoundBeforeLoop(Walker& walker, const BodyIndex& index, const BodyValue& access, ValueID operand)
{
    ValueID source = index.stripCasts(operand);
    if (!source || index.isParameter(source) || index.opcodeOf(source) != Opcode::PropGet)
        return false;
    BodyValue read = index.value(source);
    return !read.isFreeName() && isFoundBeforeLoop(walker, index, access, read.objectOperand()) && !anyOuterLoopStoresName(walker, index, access, read.key());
}

static bool isFunctionOutsideClasses(Walker& walker, TypeID holder)
{
    FunctionID function = singleFunctionOf(walker.types(), walker.types().at(holder));
    return function && !classOfClassObject(walker.program(), function);
}

static bool mayMeetObjectOfFile(Walker& walker, PropertyKeyID key)
{
    ModuleID own = walker.program().at(walker.currentFunction()).module;
    if (!walker.plan().slots().someLayoutWrittenInHasField(own, key))
        return false;
    const LayoutsByNames& layouts = walker.facts().layoutsByNames();
    LayoutSetID staying = walker.facts().tables().layoutsThatStayWithCompiledCode;
    uint32_t ownAlone[] = { own.index() };
    return std::ranges::any_of(layouts.holdersOfRarestOf(singleElementSpan(key)), [&](uint32_t holder) {
        return layouts.isWrittenInOneOf(ownAlone, holder) && layouts.hasAll(LayoutID(holder), singleElementSpan(key)) && !(staying && walker.types().layoutSets().contains(staying, LayoutID(holder)));
    });
}

static bool storesIntoIterationObject(Walker& walker, const BodyIndex& index, const BodyValue& access, TypeID holder)
{
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    bool isInIteration = loop ? index.loop(loop).numberOfLoopsWithin == 1 : !index.numberOfLoops() && walker.currentFrameIndex() && walker.isInLoop();
    if (!isInIteration || !walker.types().isUninformative(holder))
        return false;
    auto storesWhatItComputes = [&] {
        static constexpr OpcodeSet operators = allNumericOperators | allRelations;
        ValueID value = access.opcode() == Opcode::PropSet ? index.stripCasts(access.valueOperand()) : ValueID();
        return (access.opcode() != Opcode::PropSet || (value && !index.isParameter(value) && operators.contains(index.opcodeOf(value)))) && hasComputedRead(index, access.objectOperand());
    };
    if (!walker.updatesFieldInLoop(index, loop) && !storesWhatItComputes())
        return false;
    if (!walker.ignoresNameGuesses() && mayMeetObjectOfFile(walker, access.key()))
        return false;
    if (access.opcode() == Opcode::PropSet)
        return true;
    uint32_t stored = holderOf(index, access.objectOperand());
    auto storesField = [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue store) {
            return store.opcode() == Opcode::PropSet && !store.isFreeName() && store.key() == access.key() && holderOf(index, store.objectOperand()) == stored;
        });
    };
    if (loop)
        return std::ranges::any_of(index.membersOf(loop), storesField);
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        if (index.isReachable(BlockID(block)) && storesField(BlockID(block)))
            return true;
    }
    return false;
}

bool mayUseNamedAddress(Walker& walker, const BodyValue& access, TypeID holder)
{
    if (!namedAccesses.contains(access.opcode()) || !holder || access.isFreeName() || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses))
        return false;
    if (readsExpectedGetter(walker, access) || storesThroughExpectedSetter(walker, access))
        return false;
    bool isInFunction = isFunctionOutsideClasses(walker, holder);
    if (!isInFunction && !walker.types().isUninformative(holder))
        return false;
    const Program& program = walker.program();
    const BodyIndex& index = walker.currentBodyIndex();
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto || key == program.keyOf(BuiltinNames::length))
        return false;
    if (!walker.storesByName(index, access) || isCallee(index, access))
        return false;
    bool wasFoundBeforeLoop = isFoundBeforeLoop(walker, index, access, access.objectOperand()) || isFieldFoundBeforeLoop(walker, index, access, access.objectOperand());
    if (!wasFoundBeforeLoop && !storesIntoIterationObject(walker, index, access, holder))
        return false;
    if (access.opcode() == Opcode::PropGet && index.loopOf(index.blockOf(access.id())) && !anyOuterLoopStoresName(walker, index, access, key))
        return false;
    if (isInFunction)
        return key != program.keyOf(BuiltinNames::prototype) && key != program.keyOf(BuiltinNames::name);
    TypeID expected = findExpectation(walker, access.objectOperand()).type;
    return !expected || walker.types().isUninformative(expected);
}

static bool isSameOperand(const BodyIndex& index, ValueID first, ValueID second)
{
    auto sourceOf = [&](ValueID operand) {
        ValueID source = index.stripCasts(operand);
        if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
            source = index.stripCasts(index.value(source).operand(0));
        return source;
    };
    ValueID source = sourceOf(first);
    ValueID other = sourceOf(second);
    if (!source || source == other)
        return source == other;
    auto isVariable = [&](ValueID value) { return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::GetModuleVariable; };
    return isVariable(source) && isVariable(other) && index.value(source).variable() == index.value(other).variable();
}

bool sharesKeyWithOtherAccess(Walker& walker, const BodyValue& access)
{
    if (!keyedAccesses.contains(access.opcode()))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID key = index.stripCasts(access.propertyOperand());
    return std::ranges::any_of(index.block(index.blockOf(access.id())).values(), [&](BodyValue other) {
        return other.id() != access.id() && keyedAccesses.contains(other.opcode()) && index.stripCasts(other.propertyOperand()) == key && isSameOperand(index, other.objectOperand(), access.objectOperand());
    });
}

bool isKeyedByElement(const BodyIndex& index, const BodyValue& access)
{
    if (access.opcode() != Opcode::ElemGet)
        return false;
    LoopID loop = index.loopOf(index.blockOf(access.id()));
    ValueID key = index.stripCasts(access.propertyOperand());
    if (!loop || index.isParameter(key) || index.opcodeOf(key) != Opcode::ElemGet || !index.isInLoop(index.blockOf(key), loop))
        return false;
    uint32_t firstValueOfLoop = index.block(index.loop(loop).head).firstParameter().index();
    ValueID names = index.stripCasts(index.value(key).objectOperand());
    if (names.index() >= firstValueOfLoop && !index.isParameter(names) && index.opcodeOf(names) == Opcode::CheckInitialized)
        names = index.stripCasts(index.value(names).operand(0));
    return names.index() < firstValueOfLoop || (!index.isParameter(names) && index.opcodeOf(names) == Opcode::GetModuleVariable);
}

static bool hasFewNames(Walker& walker, TypeID holder)
{
    HolderLayouts layouts = exactLayoutsOf(walker, holder);
    size_t names = 0;
    for (LayoutID layout : layouts) {
        if (!hasNamedFields(walker.program(), layout))
            return false;
        names += walker.program().at(layout).fields.size();
    }
    return !layouts.isEmpty() && names <= mostTextsToldApart;
}

bool mayDispatchOverNames(Walker& walker, const BodyValue& access, TypeID holder, TypeID key)
{
    if (!keyedAccesses.contains(access.opcode()) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !holder || !key)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    constexpr uint16_t names = Primitive::AnyString | Primitive::AnySymbol;
    if (!(walker.types().at(key).primitives & names) || isCallee(index, access) || !index.loopOf(index.blockOf(access.id())) || !hasFewNames(walker, holder))
        return false;
    return (isFoundBeforeLoop(walker, index, access, access.propertyOperand()) || sharesKeyWithOtherAccess(walker, access) || isKeyedByElement(index, access)) && mayBeName(walker, access, key, holder);
}

Effects effectsOfDispatchOverNames(Walker& walker, const BodyValue& access, TypeID holder)
{
    Effects effects = Effects::none();
    addFieldsOf(walker, holder, access.opcode() == Opcode::ElemSet ? effects.writes : effects.reads);
    return effects;
}

Effects effectsOfKeyedAccess(Walker& walker, const BodyValue& access, TypeID holder)
{
    bool findsFields = findsOnlyFields(walker, holder);
    if (keyedAccesses.contains(access.opcode()) && !storesUnderKeys(walker.currentBodyIndex(), access)) {
        Effects ofLookup = declaredBoundOf(Opcode::GetKeyedValueOrAbsent);
        narrowEffects(walker, Opcode::GetKeyedValueOrAbsent, holder, findsFields, ofLookup);
        return ofLookup;
    }
    Opcode opcode = storesAtAddresses.contains(access.opcode()) ? Opcode::PutKeyedProperty : Opcode::GetKeyedProperty;
    Effects effects = declaredBoundOf(Opcode::GetKeyedPropertyAddress);
    Effects ofAccess = declaredBoundOf(opcode);
    narrowEffects(walker, opcode, holder, findsFields, ofAccess);
    if (namedAccesses.contains(access.opcode()) && !findsFields)
        narrowToName(walker, opcode, access.key(), ofAccess);
    effects.merge(ofAccess);
    return effects;
}

static Reference buildKeyedAddress(Walker& walker, Reference object, Reference key, PropertyKeyID name, bool fitsFields, bool takesFunctions, Origin origin)
{
    constexpr auto purity = Walker::IterationPurity::KnownEffectsOnly;
    Reference address;
    Reference test;
    bool isInvariant = walker.isInvariantInSpecializedVersion(object, purity);
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInvariant, purity);
        Draft lookup(Opcode::GetKeyedPropertyAddress, origin);
        lookup.setObjectOperand(object);
        lookup.setPropertyOperand(key);
        lookup.setKey(name);
        lookup.setFindsOnlyFields(findsOnlyFields(walker, walker.typeOf(object)));
        lookup.setFitsFieldTypes(fitsFields);
        lookup.setTakesFunctions(takesFunctions);
        lookup.setSkipsCompiledObjects(!isInvariant);
        address = walker.build(WTF::move(lookup));
        if (!address || walker.hasFailed())
            return { };
        Draft question(Opcode::IsPropertyFiled, origin);
        question.setAddressOperand(address);
        test = walker.build(WTF::move(question));
        if (!test || walker.hasFailed())
            return { };
    }
    walker.recordSpeculation();
    walker.count(isInvariant ? StatisticID::KeyedAddressesHoisted : StatisticID::AddressesFoundInIterations);
    if (isInvariant && walker.tryBranchInPreheader(test, true, purity))
        return address;
    BlockID leaves = walker.addBlock(BlockFrequency::Rare);
    BlockID goesOn = walker.addBlock();
    walker.branch(test, goesOn, leaves);
    walker.continueIn(leaves);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(goesOn);
    return walker.isUnreachable() || walker.hasFailed() ? Reference() : address;
}

static ValueID sourceOfKey(Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID source = index.stripCasts(findUpdatedKey(walker, index, access.propertyOperand()));
    if (source && !index.isParameter(source) && index.opcodeOf(source) == Opcode::CheckInitialized)
        source = index.stripCasts(index.value(source).operand(0));
    return source;
}

bool tryHoistKeyVariable(Walker& walker, const BodyValue& read)
{
    VariableID variable = read.variable();
    TypeArena& types = walker.types();
    TypeID held = walker.facts().held().variable(variable).proven;
    TypeID known = held ? types.withoutPrimitives(held, Primitive::Absent) : TypeID();
    if (!known || isNeverType(known))
        return false;
    TypeID expected = types.isUninformative(known) ? findExpectation(walker, read.id()).type : known;
    if (!expected || isNeverType(expected) || !types.isSubset(types.withoutNullish(expected), TypeTable::string))
        return false;
    if (!walker.opensGuesses() || !walker.canBuildIterationGuardAfterStores() || !walker.canRestartIteration() || !walker.iterationsPreserveVariable(variable))
        return false;
    if (Reference value = walker.state().variable(variable); value && walker.isInvariantInSpecializedVersion(value, Walker::IterationPurity::KnownEffectsOnly))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(index.blockOf(read.id()));
    bool namesProperty = loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            return keyedAccesses.contains(operation.opcode()) && sourceOfKey(walker, operation) == read.id();
        });
    });
    if (!namesProperty)
        return false;
    Reference key = walker.readVariableInPreheader(variable, TypeTable::string, walker.originOf(read));
    if (!key || walker.hasFailed())
        return walker.hasFailed();
    walker.state().noteVariable(variable, key);
    walker.bind(read.id(), key);
    return true;
}

static TypeID typeOfOperandAhead(Walker& walker, const BodyIndex& index, ValueID operand)
{
    if (Reference built = walker.referenceOf(operand))
        return walker.typeOf(built);
    ValueID source = index.stripCasts(operand);
    return index.isParameter(source) ? TypeID(TypeTable::anything) : widestTypeOfOperation(walker, index.value(source));
}

static bool isKeyOfUpdate(Walker& walker, ValueID operand, BlockID block)
{
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(block);
    ValueID key = index.stripCasts(operand);
    while (key && index.isParameter(key) && index.valueNarrowedInto(key))
        key = index.stripCasts(index.valueNarrowedInto(key));
    return key && loop && std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            if (!keyedAccesses.contains(operation.opcode()) || index.stripCasts(findUpdatedKey(walker, index, operation.propertyOperand())) != key)
                return false;
            return mayUseKeyedAddress(walker, operation, typeOfOperandAhead(walker, index, operation.objectOperand()), TypeTable::anything);
        });
    });
}

bool convertsInvariantKey(Walker& walker, const BodyValue& conversion)
{
    return conversion.opcode() == Opcode::ToPropertyKey && isKeyOfUpdate(walker, conversion.operand(0), walker.currentBodyIndex().blockOf(conversion.id()));
}

bool tryGuardKeyAsString(Walker& walker, const BodyValue& test)
{
    constexpr auto purity = Walker::IterationPurity::KnownEffectsOnly;
    if (test.testedType() != TypeTable::object || !walker.opensGuesses() || !walker.canBuildIterationGuardAfterStores() || !walker.canRestartIteration())
        return false;
    Reference key = walker.referenceOf(test.valueOperand());
    if (!key || !key.isValue() || !walker.types().isUninformative(walker.typeOf(key)) || !walker.isInvariantInSpecializedVersion(key, purity) || !walker.canHoistKeyedAddress(false))
        return false;
    if (!isKeyOfUpdate(walker, test.valueOperand(), walker.currentBodyIndex().blockOf(test.id())))
        return false;
    Origin origin = walker.originOf(test);
    origin.isTailCall = false;
    Reference isString;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(key);
        question.setTestedType(TypeTable::string);
        isString = walker.build(WTF::move(question));
    }
    if (!isString || walker.hasFailed() || !walker.tryBranchInPreheader(isString, true, purity))
        return walker.hasFailed();
    Draft isObject(Opcode::Boolean);
    isObject.setBoolean(false);
    walker.bind(test.id(), walker.constant(WTF::move(isObject)));
    return true;
}

static bool storedFits(Walker& walker, const BodyValue& access, Reference object)
{
    if (access.opcode() != Opcode::ElemSet)
        return findsOnlyFields(walker, walker.typeOf(object));
    Reference stored = walker.referenceOf(access.valueOperand());
    return stored && findsOnlyFields(walker, walker.typeOf(object)) && fitsFieldTypes(walker, walker.typeOf(object), walker.typeOf(stored));
}

static bool hasFewTexts(Walker& walker, const BodyValue& access)
{
    Reference key = walker.referenceOf(access.propertyOperand());
    const Type& type = walker.types().at(walker.typeOf(key));
    return type.hasOnlyPrimitives(Primitive::LiteralString) && walker.types().at(type.stringLiterals).size() <= mostTextsToldApart && findsOnlyFields(walker, walker.typeOf(walker.referenceOf(access.objectOperand())));
}

static bool canReadInPreheader(Walker& walker, const BodyValue& read, Reference object)
{
    if (read.opcode() != Opcode::ElemGet || storesUnderKeys(walker.currentBodyIndex(), read) || hasFewNames(walker, walker.typeOf(object)) || hasFewTexts(walker, read))
        return false;
    return walker.canHoistLookup(effectsOfKeyedAccess(walker, read, walker.typeOf(object)).reads);
}

bool Walker::isResultUsedAsNumber() const
{
    if (m_frames.size() < 2 || !m_frames.last().callCopied)
        return false;
    const Frame& caller = m_frames[m_frames.size() - 2];
    const BodyIndex& index = *caller.index;
    ValueID taken = m_frames.last().callCopied;
    static constexpr OpcodeSet takesNumbers = allBitwiseOperators | OpcodeSet { Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod };
    static constexpr unsigned maxSums = 4;
    for (unsigned sumsLeft = maxSums; sumsLeft--;) {
        ValueID user = index.onlyUsingValue(taken);
        if (!user)
            return false;
        BodyValue operation = index.value(user);
        if (takesNumbers.contains(operation.opcode()))
            return true;
        if (operation.opcode() != Opcode::Add)
            return false;
        ValueID beside = index.stripCasts(operation.operand(index.stripCasts(operation.operand(0)) == taken ? 1 : 0));
        Reference other = referenceBoundIn(caller, beside);
        if (other && other.isRecord())
            return m_types.at(typeOf(other)).isNumber();
        if (index.isParameter(beside) || index.opcodeOf(beside) != Opcode::Call)
            return false;
        taken = user;
    }
    return false;
}

bool canAccessAtKeyedAddress(Walker& walker, const BodyValue& access)
{
    bool isNamed = namedAccesses.contains(access.opcode());
    if ((!isNamed && !keyedAccesses.contains(access.opcode())) || !walker.opensGuesses() || walker.hasUninlinedCallInIterations() || !walker.canBuildIterationGuardAfterStores() || !walker.canRestartIteration())
        return false;
    if (storesAtAddresses.contains(access.opcode()) && !walker.canRecordUndoableStore())
        return false;
    constexpr auto purity = Walker::IterationPurity::KnownEffectsOnly;
    Reference object = walker.referenceOf(access.objectOperand());
    if (isNamed && object && object.isValue() && !walker.isInvariantInSpecializedVersion(object, purity))
        return walker.types().isUninformative(walker.typeOf(object)) && mayUseNamedAddress(walker, access, walker.typeOf(object));
    if (isNamed)
        return object && object.isRecord() && mayUseNamedAddress(walker, access, walker.typeOf(object)) && walker.isInvariantInSpecializedVersion(object, purity) && walker.canHoistKeyedAddress(isFunctionOutsideClasses(walker, walker.typeOf(object)));
    Reference key = walker.referenceOf(access.propertyOperand());
    if (!object || !object.isRecord() || !key || !key.isRecord() || !mayUseKeyedAddress(walker, access, walker.typeOf(object), walker.typeOf(key)))
        return false;
    if (!walker.isInvariantInSpecializedVersion(object, purity) || !walker.isInvariantInSpecializedVersion(key, purity))
        return false;
    if (!canReadInPreheader(walker, access, object)) {
        if (!storesUnderKeys(walker.currentBodyIndex(), access) && (hasFewNames(walker, walker.typeOf(object)) || hasFewTexts(walker, access)) && walker.currentBodyIndex().loopOf(walker.currentBodyIndex().blockOf(access.id())))
            return false;
        bool mayFit = access.opcode() != Opcode::ElemSet || !findsOnlyFields(walker, walker.typeOf(object)) || storedFits(walker, access, object);
        return mayFit && walker.canHoistKeyedAddress(false);
    }
    return true;
}

static void readInPreheader(Walker& walker, const BodyValue& read, Reference object, Reference key, Origin origin)
{
    constexpr auto purity = Walker::IterationPurity::KnownEffectsOnly;
    TypeID asked = typeRequiredOfRead(walker, read);
    Reference found;
    Reference test;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true, purity);
        Draft lookup(Opcode::GetKeyedValueOrAbsent, origin);
        lookup.setObjectOperand(object);
        lookup.setPropertyOperand(key);
        lookup.setValueType(TypeTable::anythingOrAbsent);
        lookup.setFindsOnlyFields(findsOnlyFields(walker, walker.typeOf(object)));
        found = walker.build(WTF::move(lookup));
        if (!found || walker.hasFailed())
            return;
        Draft question(Opcode::TypeTest, origin);
        question.setValueOperand(found);
        question.setTestedType(asked ? asked : TypeArena::fromPrimitives(Primitive::Absent));
        test = walker.build(WTF::move(question));
        if (!test || walker.hasFailed())
            return;
    }
    walker.recordSpeculation();
    walker.count(StatisticID::InvariantKeyReadsHoisted);
    bool isTested = walker.tryBranchInPreheader(test, !!asked, purity);
    if (!isTested) {
        BlockID leaves = walker.addBlock(BlockFrequency::Rare);
        BlockID goesOn = walker.addBlock();
        walker.branch(test, asked ? goesOn : leaves, asked ? leaves : goesOn);
        walker.continueIn(leaves);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(goesOn);
        if (walker.isUnreachable() || walker.hasFailed())
            return;
    }
    Walker::SpecializedPreheaderScope preheaderScope(walker, isTested, purity);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(found);
    proof.setTestOperand(test);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(!!asked);
    walker.bind(read.id(), walker.build(WTF::move(proof)));
}

static Reference keyOfAccess(Walker& walker, const BodyValue& access)
{
    if (!namedAccesses.contains(access.opcode()))
        return walker.referenceOf(access.propertyOperand());
    Draft name(Opcode::String);
    name.setString(walker.program().at(access.key()).text);
    return walker.constant(WTF::move(name));
}

bool tryAccessAtKeyedAddressOfItsOpcodes(Walker& walker, const BodyValue& access)
{
    if (!canAccessAtKeyedAddress(walker, access))
        return false;
    bool isStore = storesAtAddresses.contains(access.opcode());
    Reference object = walker.referenceOf(access.objectOperand());
    Reference key = keyOfAccess(walker, access);
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (isStore && !stored)
        return false;
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    if (keyedAccesses.contains(access.opcode()) && canReadInPreheader(walker, access, object)) {
        readInPreheader(walker, access, object, key, origin);
        return true;
    }
    bool isNamed = namedAccesses.contains(access.opcode());
    bool takesFunctions = isNamed && isFunctionOutsideClasses(walker, walker.typeOf(object));
    PropertyKeyID name = isNamed && !takesFunctions ? access.key() : PropertyKeyID();
    Reference address = buildKeyedAddress(walker, object, key, name, storedFits(walker, access, object), takesFunctions, origin);
    if (!address)
        return true;
    auto read = [&] {
        Draft draft(Opcode::GetKeyedProperty, origin);
        draft.setAddressOperand(address);
        draft.setObjectOperand(object);
        return draft;
    };
    if (!isStore) {
        readAtAddress(walker, access, read(), false, typeRequiredOfRead(walker, access), origin);
        return true;
    }
    Reference before = walker.build(read());
    if (!before || walker.hasFailed())
        return true;
    Draft store(Opcode::PutKeyedProperty, origin);
    walker.chooseConservativeFields(store);
    store.setAddressOperand(address);
    store.setObjectOperand(object);
    store.setValueOperand(stored);
    store.inputValueGiven = access.id();
    walker.recordUndoableKeyedStore(address, object, before, name);
    walker.build(WTF::move(store));
    walker.takePendingUndoableStore();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
