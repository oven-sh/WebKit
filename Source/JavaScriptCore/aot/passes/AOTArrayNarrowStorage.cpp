/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTIntegerRelations.h"
#include "AOTKnownValue.h"
#include "AOTRuleSupport.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NarrowArrayCalls {
    BuiltinOverloadID construct;
    BuiltinOverloadID fill;
};

struct LocalArray {
    ValueID construction;
    ValueID fill;
    NarrowStorageChoice choice;
    NarrowArrayCalls calls;
    KnownValue initialValue;
    bool hasValidLength { false };
    Vector<ValueID, 4> casts;
    Vector<ValueID, 8> reads;
    Vector<ValueID, 8> stores;
    Vector<ValueID, 4> lengths;
    Vector<ValueID, 2> tests;
    Vector<BlockID, 2> testedBranches;
    Vector<ValueID, 2> guardLists;
};

struct ArrayRewrite {
    LocalArray array;
    ProgramPoint construction;
    Vector<ProgramPoint, 8> reads;
    Vector<ProgramPoint, 8> stores;
};

struct DroppedValues {
    BitVector values;
    BitVector blocks;
};

struct ElementAccess {
    ValueID key;
    AccessSiteID site;
    bool keyIsIntegral { false };
};

struct Rewriter {
    explicit Rewriter(FunctionPassContext& context)
        : context(context)
        , function(context.function)
        , builder(context.analyses.context(), context.function, context.types, &context.analyses)
    {
    }

    ValueID didChange(ValueID id)
    {
        changedValues.append(id);
        return id;
    }

    ValueID insertBefore(ProgramPoint point, Value&& value) { return didChange(builder.insertBefore(point, WTF::move(value))); }
    ValueID append(Value&& value) { return didChange(builder.append(WTF::move(value))); }

    FunctionPassContext& context;
    Function& function;
    IRBuilder builder;
    Vector<ValueID, 16> changedValues;
};

} // anonymous namespace

static constexpr uint32_t arrayOperand = 0;
static_assert(OperandIndex::ElemGet::object == arrayOperand && OperandIndex::ElemSet::object == arrayOperand);
static_assert(OperandIndex::GetArrayElement::array == arrayOperand && OperandIndex::PutArrayElement::array == arrayOperand);
static_assert(OperandIndex::GetArrayLength::array == arrayOperand && OperandIndex::CallBuiltin::receiver == arrayOperand);
static_assert(OperandIndex::IsPristine::value == arrayOperand && OperandIndex::HasArrayStorage::array == arrayOperand && OperandIndex::IsInBounds::array == arrayOperand);
static_assert(OperandIndex::IsElementPresent::array == arrayOperand);

static constexpr uint32_t takenTargetIndex = 0;

static constexpr OpcodeSet guardedArrayAccesses { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetArrayLength };

static const Type& typeOf(const FunctionPassContext& context, ValueID id)
{
    return context.program.typeOf(context.function, id);
}

static ValueID lengthArgumentOf(const Function& function, const LocalArray& array)
{
    return function.at(array.construction).argumentsOperands()[0];
}

static ValueID keyOf(const Value& access)
{
    switch (access.opcode) {
    case Opcode::ElemGet:
    case Opcode::ElemSet:
        return access.propertyOperand();
    case Opcode::GetArrayElement:
    case Opcode::PutArrayElement:
        return access.positionOperand();
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static BuiltinCallStamps stampsOf(const Value& call)
{
    return { call.expectedResult(), call.accessSite(), call.allocationSite() };
}

static KnownValue::Kind knownKindOf(NarrowElementKind elements)
{
    switch (elements) {
    case NarrowElementKind::Boolean:
        return KnownValue::Kind::Boolean;
    case NarrowElementKind::Integer:
        return KnownValue::Kind::Integer;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A new typed array is all zero, and a flag is stored as whether it differs from the initial value.
static bool needsFillCall(const LocalArray& array)
{
    switch (array.choice.elements) {
    case NarrowElementKind::Boolean:
        return false;
    case NarrowElementKind::Integer:
        return !!array.initialValue.asInteger();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool constructsArrayWithLength(const FunctionPassContext& context)
{
    return std::ranges::any_of(context.function.values, [&](const Value& value) {
        return value.opcode == Opcode::CallBuiltin && inlineImplementationOf(context.program, value) == InlineBuiltin::ArrayWithLength;
    });
}

static bool isArrayConstruction(const FunctionPassContext& context, ValueID id)
{
    const Value& value = context.function.at(id);
    if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
        return false;
    if (inlineImplementationOf(context.program, value) != InlineBuiltin::ArrayWithLength)
        return false;
    std::span<const ValueID> arguments = value.argumentsOperands();
    return arguments.size() == 1 && typeOf(context, arguments[0]).isNumber();
}

static bool isArrayFill(const FunctionPassContext& context, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
        return false;
    return context.program.builtins().canonicalID(overloadOf(context.program, value).builtin) == Builtins::ArrayPrototypeFill;
}

static bool acceptsEveryArray(const Type& type)
{
    return type.anyObject || type.findClassPart(BuiltinClasses::Array);
}

static bool collectAliases(FunctionPassContext& context, std::span<const ValueID> aliases, LocalArray& array)
{
    const UseLists& useLists = context.analyses.useLists();
    for (ValueID alias : aliases) {
        if (alias == array.construction)
            continue;
        if (useLists.locationOf(alias).placement != ValuePlacement::InBlock)
            return false;
        const Value& value = context.function.at(alias);
        if (value.opcode == Opcode::AllGuards)
            continue;
        if (isCast(value.opcode)) {
            if (!acceptsEveryArray(typeOf(context, alias)))
                return false;
            array.casts.append(alias);
            continue;
        }
        if (array.fill || !isArrayFill(context, value))
            return false;
        array.fill = alias;
    }
    return !!array.fill;
}

static bool isFilledOncePerAllocation(FunctionPassContext& context, const LocalArray& array)
{
    const UseLists& useLists = context.analyses.useLists();
    return !context.analyses.cfg().isInCycleAvoiding(useLists.blockOf(array.fill), useLists.blockOf(array.construction));
}

static bool collectGuardUse(const Function& function, std::span<const ValueID> aliases, const Use& use, LocalArray& array)
{
    const Value& user = function.at(use.user);
    if (user.opcode == Opcode::AllGuards) {
        if (!array.guardLists.contains(use.user))
            array.guardLists.append(use.user);
        return true;
    }
    return guardedArrayAccesses.contains(user.opcode) && std::ranges::contains(aliases, user.arrayOperand());
}

static bool collectTest(FunctionPassContext& context, ValueID test, LocalArray& array)
{
    for (const Use& use : context.analyses.useLists().usesOf(test)) {
        if (use.kind != Use::Kind::TerminatorOperand || context.function.at(use.block).terminator.kind != TerminatorKind::Branch)
            return false;
        array.testedBranches.append(use.block);
    }
    array.tests.append(test);
    return true;
}

static bool collectUse(FunctionPassContext& context, const Use& use, LocalArray& array)
{
    const Value& user = context.function.at(use.user);
    switch (user.opcode) {
    case Opcode::IsPristine:
        return user.builtinClass() == BuiltinClasses::Array && collectTest(context, use.user, array);
    case Opcode::HasArrayStorage:
    case Opcode::IsInBounds:
    case Opcode::IsElementPresent:
        return collectTest(context, use.user, array);
    case Opcode::ElemGet:
    case Opcode::GetArrayElement:
        array.reads.append(use.user);
        break;
    case Opcode::ElemSet:
    case Opcode::PutArrayElement:
        array.stores.append(use.user);
        break;
    case Opcode::GetArrayLength:
        array.lengths.append(use.user);
        return true;
    case Opcode::CallBuiltin:
        if (inlineImplementationOf(context.program, user) != InlineBuiltin::ArrayLength)
            return false;
        array.lengths.append(use.user);
        return true;
    default:
        return false;
    }
    return context.analyses.dominates(array.fill, use);
}

static bool collectUses(FunctionPassContext& context, std::span<const ValueID> aliases, LocalArray& array)
{
    const UseLists& useLists = context.analyses.useLists();
    for (ValueID alias : aliases) {
        for (const Use& use : useLists.usesOf(alias)) {
            if (use.kind != Use::Kind::Operand)
                return false;
            if (context.function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard) {
                if (!collectGuardUse(context.function, aliases, use, array))
                    return false;
                continue;
            }
            if (use.operandIndex != arrayOperand)
                return false;
            if (use.user == array.fill || array.casts.contains(use.user))
                continue;
            if (!collectUse(context, use, array))
                return false;
        }
    }
    return true;
}

// Flow recorded the removable features in use before this pass ran, and a trimmed executable keeps no other.
static bool mayBeTrimmed(const FunctionPassContext& context, BuiltinID builtin)
{
    const BuiltinTable& table = context.program.builtins();
    std::span<const FeatureUseRecord> recorded = context.inputs.flow.featureUses();
    return std::ranges::any_of(table.featureUsesOf(table.canonicalID(builtin)), [&](const BuiltinFeatureUse& line) {
        return !std::ranges::binary_search(recorded, static_cast<uint32_t>(line.feature), { }, &FeatureUseRecord::feature);
    });
}

static BuiltinClassID findTypedArrayClass(const BuiltinTable& table, TypedArrayType type)
{
    for (uint32_t index = 0; index < table.classCount(); ++index) {
        BuiltinClassID id(index);
        if (table.at(id).typedArrayType == type)
            return id;
    }
    return { };
}

static BuiltinOverloadID findProvenOverload(const TypingContext& typing, BuiltinID builtin, BuiltinCallMode mode, const BuiltinOperandTypes& operands)
{
    OverloadMatch match = matchOverload(OverloadQuery { typing, builtin, mode, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return { };
    return match.overload;
}

static std::optional<NarrowArrayCalls> callsFor(FunctionPassContext& context, TypedArrayType storage)
{
    const BuiltinTable& table = context.program.builtins();
    BuiltinClassID builtinClass = findTypedArrayClass(table, storage);
    if (!builtinClass)
        return std::nullopt;

    BuiltinID constructor = table.at(builtinClass).constructor;
    BuiltinMemberLookup fill = table.findForInstance(builtinClass, BuiltinNames::fill);
    if (!constructor || fill.outcome != BuiltinMemberLookup::Outcome::Found)
        return std::nullopt;
    if (mayBeTrimmed(context, constructor) || mayBeTrimmed(context, fill.member))
        return std::nullopt;

    TypingContext typing { context.program, context.function, context.types };
    BuiltinOperandTypes constructed { { }, { TypeTable::number } };
    BuiltinOperandTypes filled { context.types.intern(Type::builtinClass(builtinClass)), { TypeTable::number } };
    NarrowArrayCalls calls {
        findProvenOverload(typing, constructor, BuiltinCallMode::Construct, constructed),
        findProvenOverload(typing, fill.member, BuiltinCallMode::Call, filled),
    };
    if (!calls.construct || !calls.fill)
        return std::nullopt;
    return calls;
}

static bool canPassStorageTests(const Function& function, const LocalArray& array, const Type& elements)
{
    return std::ranges::all_of(array.tests, [&](ValueID test) {
        const Value& value = function.at(test);
        return value.opcode != Opcode::HasArrayStorage || canStoreWithoutConversion(value.arrayStorage(), elements);
    });
}

static bool chooseStorage(FunctionPassContext& context, LocalArray& array)
{
    std::span<const ValueID> fillArguments = context.function.at(array.fill).argumentsOperands();
    if (fillArguments.size() != 1)
        return false;

    Type elements = typeOf(context, fillArguments[0]);
    for (ValueID store : array.stores)
        elements = elements.unionWith(typeOf(context, context.function.at(store).valueOperand()), context.types);
    std::optional<NarrowStorageChoice> choice = narrowStorageHolding(elements);
    if (!choice || !canPassStorageTests(context.function, array, elements))
        return false;

    array.initialValue = knownValueOf(context.program, context.function, fillArguments[0]);
    if (array.initialValue.kind() != knownKindOf(choice->elements))
        return false;

    std::optional<NarrowArrayCalls> calls = callsFor(context, choice->storage);
    if (!calls)
        return false;
    array.choice = *choice;
    array.calls = *calls;
    return true;
}

static bool isKeyInBounds(IntegerRelations& relations, ValueID key, ValueID length, BlockID at)
{
    using Answer = IntegerRelations::Answer;
    return relations.isWholeNumber(key, at) == Answer::Proven
        && relations.isAtLeast(key, 0, at) == Answer::Proven
        && relations.isAtMost(key, length, -1, at) == Answer::Proven;
}

static bool hasOnlyInBoundsAccesses(FunctionPassContext& context, const LocalArray& array)
{
    const UseLists& useLists = context.analyses.useLists();
    ValueID length = lengthArgumentOf(context.function, array);

    IntegerRelations relations(context.analyses);
    relations.assumeRange(length, Range::uint32(), useLists.blockOf(array.construction));
    for (ValueID lengthRead : array.lengths)
        relations.assumeSameValue(lengthRead, length);

    auto isAccessInBounds = [&](ValueID access) {
        return isKeyInBounds(relations, keyOf(context.function.at(access)), length, useLists.blockOf(access));
    };
    return std::ranges::all_of(array.reads, isAccessInBounds) && std::ranges::all_of(array.stores, isAccessInBounds);
}

// An access by key inside the filled length finds an own data property, so it hands the array to no other code.
static bool escapesOnlyThroughAccessesByKey(const Function& function, std::span<const ObjectUse> uses)
{
    return std::ranges::all_of(uses, [&](const ObjectUse& objectUse) {
        const Use& use = objectUse.use;
        if (!objectUse.isEscape)
            return true;
        if (use.kind != Use::Kind::Operand || use.operandIndex != arrayOperand)
            return false;
        Opcode opcode = function.at(use.user).opcode;
        return opcode == Opcode::ElemGet || opcode == Opcode::ElemSet;
    });
}

static std::optional<LocalArray> findLocalArray(FunctionPassContext& context, ObjectID object)
{
    const EscapeAnalysis& escape = context.analyses.escape();
    const TrackedObject& tracked = escape.at(object);
    if (tracked.kind != ObjectKind::Allocation || tracked.state == EscapeState::Unknown)
        return std::nullopt;
    if (!escapesOnlyThroughAccessesByKey(context.function, escape.usesOf(object)))
        return std::nullopt;

    LocalArray array;
    array.construction = tracked.definition;
    std::span<const ValueID> aliases = escape.aliasesOf(object);
    if (!isArrayConstruction(context, array.construction) || !collectAliases(context, aliases, array))
        return std::nullopt;
    if (!isFilledOncePerAllocation(context, array) || !collectUses(context, aliases, array))
        return std::nullopt;
    if (!chooseStorage(context, array) || !hasOnlyInBoundsAccesses(context, array))
        return std::nullopt;

    const Type& lengthType = typeOf(context, lengthArgumentOf(context.function, array));
    array.hasValidLength = lengthType.isInteger() && lengthType.integers.isSubsetOf(Range::uint32());
    return array;
}

static void markDropped(const UseLists& useLists, DroppedValues& dropped, ValueID id)
{
    dropped.values.set(id.index());
    dropped.blocks.set(useLists.blockOf(id).index());
}

static ArrayRewrite locateValues(const UseLists& useLists, LocalArray&& array, DroppedValues& dropped)
{
    ArrayRewrite rewrite;
    rewrite.construction = useLists.pointOf(array.construction);
    for (ValueID read : array.reads)
        rewrite.reads.append(useLists.pointOf(read));
    for (ValueID store : array.stores)
        rewrite.stores.append(useLists.pointOf(store));
    if (!needsFillCall(array))
        markDropped(useLists, dropped, array.fill);
    for (ValueID cast : array.casts)
        markDropped(useLists, dropped, cast);
    for (ValueID test : array.tests)
        markDropped(useLists, dropped, test);
    rewrite.array = WTF::move(array);
    return rewrite;
}

static ElementAccess elementAccessOf(const FunctionPassContext& context, const Value& access)
{
    ValueID key = keyOf(access);
    return { key, access.accessSite(), typeOf(context, key).isNumberWithoutFraction() };
}

static void becomeNarrowAccess(Value& value, Opcode opcode, const LocalArray& array, const ElementAccess& access)
{
    value.become(opcode);
    value.setViewOperand(array.construction);
    value.setPositionOperand(access.key);
    value.setTypedArrayType(array.choice.storage);
    value.setKeyIsIntegral(access.keyIsIntegral);
    value.setLevel(KnowledgeLevel::Proven);
    value.setAccessSite(access.site);
}

static void rewriteFlagRead(Rewriter& rewriter, const LocalArray& array, ValueID read, ProgramPoint point)
{
    Function& function = rewriter.function;
    rewriter.builder.setOrigin(function.at(read).origin());

    Value byte;
    becomeNarrowAccess(byte, Opcode::GetTypedArrayElement, array, elementAccessOf(rewriter.context, function.at(read)));
    ValueID operand = rewriter.insertBefore(point, WTF::move(byte));
    bool initialValue = array.initialValue.asBoolean();
    if (initialValue)
        operand = rewriter.insertBefore(point, IRBuilder::make(Opcode::ToBoolean, { operand }));

    Value& value = function.edit(read);
    value.become(initialValue ? Opcode::LogicalNot : Opcode::ToBoolean);
    value.setValueOperand(operand);
}

static void rewriteRead(Rewriter& rewriter, const LocalArray& array, ValueID read, ProgramPoint point)
{
    rewriter.didChange(read);
    switch (array.choice.elements) {
    case NarrowElementKind::Boolean:
        rewriteFlagRead(rewriter, array, read, point);
        return;
    case NarrowElementKind::Integer: {
        Value& value = rewriter.function.edit(read);
        becomeNarrowAccess(value, Opcode::GetTypedArrayElement, array, elementAccessOf(rewriter.context, value));
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID insertStoredByte(Rewriter& rewriter, const LocalArray& array, ValueID stored, ProgramPoint point)
{
    bool initialValue = array.initialValue.asBoolean();
    KnownValue known = knownValueOf(rewriter.context.program, rewriter.function, stored);
    if (known.kind() == KnownValue::Kind::Boolean)
        return rewriter.insertBefore(point, makeInteger(known.asBoolean() != initialValue));

    ValueID flag = stored;
    if (initialValue)
        flag = rewriter.insertBefore(point, IRBuilder::make(Opcode::LogicalNot, { stored }));
    return rewriter.insertBefore(point, IRBuilder::make(Opcode::ToNumber, { flag }));
}

static ValueID insertStoredElement(Rewriter& rewriter, const LocalArray& array, ValueID stored, ProgramPoint point)
{
    switch (array.choice.elements) {
    case NarrowElementKind::Boolean:
        return insertStoredByte(rewriter, array, stored, point);
    case NarrowElementKind::Integer:
        return stored;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void rewriteStore(Rewriter& rewriter, const LocalArray& array, ValueID store, ProgramPoint point)
{
    Function& function = rewriter.function;
    rewriter.builder.setOrigin(function.at(store).origin());
    ValueID element = insertStoredElement(rewriter, array, function.at(store).valueOperand(), point);

    Value& value = function.edit(store);
    becomeNarrowAccess(value, Opcode::PutTypedArrayElement, array, elementAccessOf(rewriter.context, value));
    value.setValueOperand(element);
    rewriter.didChange(store);
}

static void rewriteFill(Rewriter& rewriter, const LocalArray& array)
{
    Value& fill = rewriter.function.edit(array.fill);
    ValueID initialValue = fill.argumentsOperands()[0];
    becomeBuiltinCall(fill, array.calls.fill, BuiltinCallForm::Whole, { array.construction, { initialValue } }, stampsOf(fill));
    rewriter.didChange(array.fill);
}

static void removeFromGuardList(Value& list, ValueID guard)
{
    Vector<ValueID, 4> remaining;
    for (ValueID operand : list.guardsOperands()) {
        if (operand != guard)
            remaining.append(operand);
    }
    list.setGuardsOperands(remaining.span());
}

static void eraseDroppedValues(const Function& function, IRBuilder& builder, const DroppedValues& dropped)
{
    for (size_t blockIndex : dropped.blocks) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        BitVector indexes;
        for (size_t index = 0; index < values.size(); ++index) {
            if (dropped.values.get(values[index].index()))
                indexes.set(index);
        }
        builder.eraseValues(block, indexes);
    }
}

static void rewriteAccesses(Rewriter& rewriter, std::span<const ArrayRewrite> rewrites, const DroppedValues& dropped)
{
    Function& function = rewriter.function;
    ValueSubstitution substitution(function);
    for (const ArrayRewrite& rewrite : rewrites) {
        const LocalArray& array = rewrite.array;
        for (size_t index = 0; index < array.reads.size(); ++index)
            rewriteRead(rewriter, array, array.reads[index], rewrite.reads[index]);
        for (size_t index = 0; index < array.stores.size(); ++index)
            rewriteStore(rewriter, array, array.stores[index], rewrite.stores[index]);
        if (needsFillCall(array))
            rewriteFill(rewriter, array);
        substitution.replace(array.fill, array.construction);
        for (ValueID cast : array.casts)
            substitution.replace(cast, array.construction);
    }
    rewriter.builder.applyInsertions();
    substitution.apply(function, &rewriter.context.analyses);

    // Both sides of a test run the same source code. The taken side is typed for storage that holds these elements, and no access depends on a JSArray any more.
    for (const ArrayRewrite& rewrite : rewrites) {
        for (ValueID list : rewrite.array.guardLists)
            removeFromGuardList(function.edit(list), rewrite.array.construction);
        for (BlockID block : rewrite.array.testedBranches)
            rewriter.builder.convertBranchToJump(block, takenTargetIndex);
    }
    eraseDroppedValues(function, rewriter.builder, dropped);
}

static ValueID expandLengthCheck(Rewriter& rewriter, const LocalArray& array, BlockID block)
{
    Function& function = rewriter.function;
    IRBuilder& builder = rewriter.builder;
    Value original = function.at(array.construction);
    ValueID length = original.argumentsOperands()[0];
    size_t index = function.at(block).values.find(array.construction);
    RELEASE_ASSERT(index != notFound);
    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { block, static_cast<uint32_t>(index) });

    ValueID zero = rewriter.didChange(builder.appendInteger(0));
    ValueID validLength = rewriter.append(IRBuilder::make(Opcode::URShift, { length, zero }));
    ValueID isValid = rewriter.append(IRBuilder::make(Opcode::StrictEq, { validLength, length }));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(isValid, BlockFrequency::Normal, BlockFrequency::Rare);

    ValueID narrowArray = rewriter.append(makeBuiltinCall(array.calls.construct, BuiltinCallForm::Whole, { { }, { validLength } }, stampsOf(original)));
    builder.endExpansion(expansion, narrowArray);

    builder.moveToEndOf(blocks.notTaken);
    rewriter.append(WTF::move(original));
    builder.emitUnreachable();
    return validLength;
}

static void convert(Rewriter& rewriter, const LocalArray& array, BlockID block)
{
    Function& function = rewriter.function;
    ValueID length = lengthArgumentOf(function, array);
    if (array.hasValidLength) {
        Value& construction = function.edit(array.construction);
        becomeBuiltinCall(construction, array.calls.construct, BuiltinCallForm::Whole, { { }, { length } }, stampsOf(construction));
    } else
        length = expandLengthCheck(rewriter, array, block);
    rewriter.didChange(array.construction);

    for (ValueID lengthRead : array.lengths) {
        Value& value = function.edit(lengthRead);
        value.become(Opcode::ToNumeric);
        value.setValueOperand(length);
        rewriter.didChange(lengthRead);
    }
}

unsigned narrowStorageOfLocalArrays(FunctionPassContext& context)
{
    if (!constructsArrayWithLength(context))
        return 0;

    Vector<LocalArray, 2> arrays;
    unsigned numberOfObjects = context.analyses.escape().numberOfObjects();
    for (unsigned index = 0; index < numberOfObjects; ++index) {
        if (auto array = findLocalArray(context, ObjectID(index)))
            arrays.append(WTF::move(*array));
    }
    if (arrays.isEmpty())
        return 0;

    DroppedValues dropped;
    Vector<ArrayRewrite, 2> rewrites;
    for (LocalArray& array : arrays)
        rewrites.append(locateValues(context.analyses.useLists(), WTF::move(array), dropped));

    Rewriter rewriter(context);
    rewriteAccesses(rewriter, rewrites.span(), dropped);

    std::ranges::sort(rewrites, std::ranges::greater { }, [](const ArrayRewrite& rewrite) {
        return std::pair { rewrite.construction.block, rewrite.construction.index };
    });
    for (const ArrayRewrite& rewrite : rewrites)
        convert(rewriter, rewrite.array, rewrite.construction.block);

    context.analyses.didChangeValues();
    rewriter.builder.emptyUnreachableBlocks();
    narrowValueTypes(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, rewriter.changedValues.span());

    context.build.statistics().count(StatisticID::ArraysNarrowed, rewrites.size());
    return static_cast<unsigned>(rewrites.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
