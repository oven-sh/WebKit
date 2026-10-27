/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowParts.h"
#include "AOTHeldOperands.h"
#include "AOTIRProgram.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

TypeID TypingContext::typeIDOf(const DeclaredType& declared) const
{
    if (knownPositions && level == TypeLevel::Proven) {
        if (std::optional<Type> known = knownPositions->find(declared))
            return types.intern(*known);
    }
    return level == TypeLevel::Expected ? declared.expected().predicted() : program.provenTypeOf(declared);
}

static TypeID typeIDOfField(const TypingContext& context, const Field& field)
{
    TypeID narrowed = context.level == TypeLevel::Proven ? narrowedSlotTypeOf(field) : TypeID();
    return narrowed ? narrowed : context.typeIDOf(field.declared);
}

static const Type& typeOfField(const TypingContext& context, const Field& field)
{
    return context.types.at(typeIDOfField(context, field));
}

static bool hasProvenType(const FieldFlow& flow)
{
    return flow.location.writers == WriterState::AllKnown
        && flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined, FieldStability::IsNeverStoredWithOtherType });
}

FieldTypeState fieldTypeStateOf(const Program& program, FieldReference reference)
{
    return hasProvenType(flowOf(program, reference)) ? FieldTypeState::Proven : FieldTypeState::Unknown;
}

static FunctionID findMethodBehindField(const Program& program, LayoutID receiver, PropertyKeyID key)
{
    if (program.at(receiver).kind != LayoutKind::Instance)
        return { };
    MemberAnswer above = findMemberOnPrototypes({ program }, { MemberStart::Instance, receiver, key, Access::Read, false });
    return above.kind == MemberKind::Method ? above.method : FunctionID();
}

static std::optional<Type> tryTypeOfField(const TypingContext& context, LayoutID receiver, FieldReference reference)
{
    const Field& field = fieldAt(context.program, reference);
    if (field.form.contains(FieldForm::IsAccessor)) {
        if (context.level == TypeLevel::Proven)
            return std::nullopt;
        return context.typeOf(field.declared);
    }
    const Type& declared = typeOfField(context, field);
    if (context.level == TypeLevel::Expected) {
        if (field.flags.contains(FieldFlags::IsRequired))
            return declared;
        // An object that has no property of its own under the name answers with the method of its class.
        if (FunctionID method = field.flags.contains(FieldFlags::IsFilledByCreator) ? FunctionID() : findMethodBehindField(context.program, receiver, field.key))
            return declared.unionWith(Type::forFunction(method), context.types);
        return declared.withPrimitives(Primitive::Undefined);
    }

    const FieldFlow& flow = flowOf(context.program, reference);
    if (!hasProvenType(flow))
        return std::nullopt;
    if (flow.occupancy == Occupancy::AlwaysFilled)
        return declared;
    return std::nullopt;
}

std::optional<Type> tryTypeOfFieldRead(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    bool isProof = context.level == TypeLevel::Proven;
    BaseChain chain = isProof ? BaseChain::Proven : BaseChain::Predicted;
    Type result = Type::never();
    bool isMissingInEveryReceiver = true;
    for (LayoutID receiver : receivers) {
        if (context.program.at(receiver).flow.allocation == AllocationState::NeverAllocated)
            continue;
        std::optional<FieldReference> field = findField(context.program, receiver, key, chain);
        if (!field) {
            if (isProof)
                return std::nullopt;
            result = result.withPrimitives(Primitive::Undefined);
            continue;
        }
        isMissingInEveryReceiver = false;
        std::optional<Type> part = tryTypeOfField(context, receiver, *field);
        if (!part)
            return std::nullopt;
        result = result.unionWith(*part, context.types);
    }
    if (isMissingInEveryReceiver)
        return std::nullopt;
    return result;
}

static Type typeOfSlotContents(const TypingContext& context, FieldReference reference)
{
    if (fieldAt(context.program, reference).form.contains(FieldForm::IsAccessor))
        return context.types.at(slotTypeOf(context.program, fieldAt(context.program, reference)));
    const Type& declared = typeOfField(context, fieldAt(context.program, reference));
    if (context.level == TypeLevel::Expected || !slotHoldsUndefinedAndNull(context.program, reference))
        return declared;
    return declared.withPrimitives(Primitive::UndefinedOrNull);
}

std::optional<Type> tryTypeOfSlotContents(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    Type result = Type::never();
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> field = findField(context.program, receiver, key, BaseChain::Proven);
        if (!field)
            return std::nullopt;
        result = result.unionWith(typeOfSlotContents(context, *field), context.types);
    }
    return result;
}

// The body that is typed with its checks taken out has opened the sets of the accesses behind them. An access by slot has no form for another layout, so it is typed there as where its check has passed.
static std::span<const LayoutID> layoutsReadBySlot(const TypingContext& context, LayoutSetID set)
{
    const LayoutSet& layouts = context.types.at(set);
    return layouts.listsEveryLayout() ? layouts.completeMembers().value() : layouts.seen().predicted();
}

std::optional<Type> tryTypeOfSetterInSlot(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    if (context.level == TypeLevel::Proven)
        return Type::function();
    Type result = Type::never();
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> field = findField(context.program, receiver, key, BaseChain::Proven);
        if (!field || !fieldAt(context.program, *field).setterType)
            return Type::function();
        result = result.unionWith(context.types.at(fieldAt(context.program, *field).setterType), context.types);
    }
    return result;
}

std::optional<Type> tryTypeOfSetterInSlot(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    return tryTypeOfSetterInSlot(context, layoutsReadBySlot(context, receivers), key);
}

std::optional<Type> tryTypeOfDeferringSlotContents(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    return tryTypeOfSlotContents(context, receivers, key);
}

static std::optional<Type> tryTypeAcceptedBySlots(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    std::optional<Type> accepted;
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> field = findField(context.program, receiver, key, BaseChain::Proven);
        if (!field)
            return std::nullopt;
        Type contents = typeOfSlotContents(context, *field);
        accepted = accepted ? accepted->intersectWith(contents, context.types) : WTF::move(contents);
    }
    return accepted;
}

static uint32_t hashOfFieldType(uint64_t receivers, PropertyKeyID key, TypeLevel level, FieldTypeRule rule)
{
    return computeHash(receivers, key, level, rule);
}

auto FieldReadTypes::find(LayoutSetID receivers, PropertyKeyID key, TypeLevel level, FieldTypeRule rule) const -> const Entry*
{
    std::optional<uint32_t> position = m_index.find(hashOfFieldType(receivers.index(), key, level, rule), [&](uint32_t candidate) {
        const Entry& entry = m_entries[candidate];
        return entry.receivers == receivers && entry.key == key && entry.level == level && entry.rule == rule;
    });
    return position ? &m_entries[*position] : nullptr;
}

void FieldReadTypes::add(const Entry& entry)
{
    m_index.add(hashOfFieldType(entry.receivers.index(), entry.key, entry.level, entry.rule), m_entries.size());
    m_entries.append(entry);
}

auto SharedFieldTypes::find(ListID receivers, PropertyKeyID key, TypeLevel level, FieldTypeRule rule) const -> const Entry*
{
    return m_entries.find(hashOfFieldType(receivers, key, level, rule), [&](const Entry& entry) {
        return entry.receivers == receivers && entry.key == key && entry.level == level && entry.rule == rule;
    });
}

auto SharedFieldTypes::add(Entry&& added) -> const Entry&
{
    uint32_t hash = hashOfFieldType(added.receivers, added.key, added.level, added.rule);
    ListID receivers = added.receivers;
    PropertyKeyID key = added.key;
    TypeLevel level = added.level;
    FieldTypeRule rule = added.rule;
    return m_entries.add(hash, WTF::move(added), [&](const Entry& entry) {
        return entry.receivers == receivers && entry.key == key && entry.level == level && entry.rule == rule;
    });
}

static std::optional<Type> computeFieldType(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key, FieldTypeRule rule)
{
    switch (rule) {
    case FieldTypeRule::NamedRead:
        return tryTypeOfFieldRead(context, receivers, key);
    case FieldTypeRule::SlotRead:
        return tryTypeOfSlotContents(context, receivers, key);
    case FieldTypeRule::DeferringSlotRead:
        return tryTypeOfDeferringSlotContents(context, receivers, key);
    case FieldTypeRule::SlotStore:
        return tryTypeAcceptedBySlots(context, receivers, key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Type> sharedFieldTypeOf(const TypingContext& context, LayoutSetID set, std::span<const LayoutID> receivers, PropertyKeyID key, FieldTypeRule rule)
{
    SharedFieldTypes* shared = context.program.sharedFieldTypes();
    if (!shared || TypeTable::isProvisional(key))
        return computeFieldType(context, receivers, key, rule);

    SharedFieldTypes::ListID list = shared->lists().idOf(context.types.table(), set);
    const SharedFieldTypes::Entry* known = shared->find(list, key, context.level, rule);
    if (!known) {
        ScratchTypeArena scratch(context.types.table());
        TypingContext scratchContext { context.program, context.function, scratch.arena(), context.level };
        known = &shared->add({ list, key, context.level, rule, computeFieldType(scratchContext, receivers, key, rule) });
    }
    if (!known->type)
        return std::nullopt;
    return context.types.adopt(*known->type);
}

static std::optional<Type> fieldTypeOf(const TypingContext& context, LayoutSetID set, PropertyKeyID key, FieldTypeRule rule)
{
    std::span<const LayoutID> receivers = layoutsReadBySlot(context, set);
    if (receivers.size() < FieldReadTypes::fewestReceivers)
        return computeFieldType(context, receivers, key, rule);
    if (!context.memo)
        return sharedFieldTypeOf(context, set, receivers, key, rule);

    FieldReadTypes& fieldReads = context.memo->fieldReads;
    if (const FieldReadTypes::Entry* known = fieldReads.find(set, key, context.level, rule)) {
        context.types.markCurrentAnswersUnshareable();
        if (!known->type)
            return std::nullopt;
        return context.types.at(known->type);
    }
    std::optional<Type> type = sharedFieldTypeOf(context, set, receivers, key, rule);
    fieldReads.add({ set, key, context.level, rule, type ? context.types.intern(*type) : TypeID() });
    return type;
}

std::optional<Type> tryTypeOfFieldRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    if (!receiver.layouts)
        return std::nullopt;
    return fieldTypeOf(context, receiver.layouts, key, FieldTypeRule::NamedRead);
}

std::optional<Type> tryTypeOfSlotContents(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    return fieldTypeOf(context, receivers, key, FieldTypeRule::SlotRead);
}

std::optional<Type> tryTypeOfDeferringSlotContents(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    return fieldTypeOf(context, receivers, key, FieldTypeRule::DeferringSlotRead);
}

std::optional<Type> tryTypeOfContentsRead(const TypingContext& context, const Type& container, ContentsSlot slot)
{
    if (!container.isJavaScriptValue() || container.primitives & ~Primitive::UndefinedOrNull || !container.hasOnlyClassObjects())
        return std::nullopt;

    const BuiltinTable& builtins = context.program.builtins();
    Type result = Type::never();
    for (const ClassPart& part : container.classes) {
        TypedArrayType typedArrayType = builtins.typedArrayTypeOf(part.builtinClass);
        if (slot == ContentsSlot::Element && typedArrayType != NotTypedArray) {
            result = result.unionWith(elementTypeOf(typedArrayType), context.types);
            continue;
        }
        if (context.level == TypeLevel::Proven && !part.contentsAreProven)
            return std::nullopt;
        TypeID contents = context.level == TypeLevel::Proven ? part.writtenContentsAt(builtins, slot) : part.expectedContentsAt(builtins, slot).predicted();
        result = result.unionWith(context.types.at(contents), context.types);
    }
    return result;
}

Type typeOfContentsRead(const TypingContext& context, const Type& container, ContentsSlot slot)
{
    return tryTypeOfContentsRead(context, container, slot).value_or(Type::anything());
}

static bool hasOnlyIndexedClasses(const BuiltinTable& builtins, const Type& container)
{
    return std::ranges::all_of(container.classes, [&](const ClassPart& part) {
        return !!indexedElementSlotOf(builtins, part.builtinClass);
    });
}

static bool isPositionInsideEveryText(const TypingContext& context, const Type& string, const Type& position)
{
    if ((string.primitives & Primitive::AnyString) != Primitive::LiteralString || !position.isInteger() || position.integers.minimum < 0)
        return false;
    return std::ranges::all_of(string.stringLiterals, [&](StringID literal) {
        std::span<const uint8_t> text = context.program.string(literal);
        return static_cast<uint64_t>(position.integers.maximum) < text.size() && std::ranges::all_of(text, isASCII<uint8_t>);
    });
}

std::optional<Type> tryTypeOfElementRead(const TypingContext& context, const Type& container, const Type& key)
{
    if (key.isNever() || container.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return Type::never();
    if (!key.isNumber())
        return std::nullopt;
    if (container.isAnyStringOrNullish() && isPositionInsideEveryText(context, container, key))
        return Type::fromPrimitives(Primitive::String);
    if (container.isAnyStringOrNullish())
        return Type::fromPrimitives(Primitive::String | Primitive::Undefined);
    if (!hasOnlyIndexedClasses(context.program.builtins(), container))
        return std::nullopt;

    std::optional<Type> element = tryTypeOfContentsRead(context, container, ContentsSlot::Element);
    if (!element)
        return std::nullopt;
    return element->withPrimitives(Primitive::Undefined);
}

Type typeOfElementRead(const TypingContext& context, const Type& container, const Type& key)
{
    return tryTypeOfElementRead(context, container, key).value_or(Type::anything());
}

std::optional<Type> tryTypeOfAwaited(const TypingContext& context, const Type& awaited)
{
    if (!awaited.hasObjects())
        return awaited;

    const ClassPart* promise = awaited.findClassPart(BuiltinClasses::Promise);
    bool hasOnlyPromiseObjects = promise && awaited.classes.size() == 1 && awaited.hasOnlyClassObjects();
    switch (context.level) {
    case TypeLevel::Proven:
        if (!hasOnlyPromiseObjects || !promise->contentsAreProven)
            return std::nullopt;
        break;
    case TypeLevel::Expected:
        if (awaited.anyObject)
            return std::nullopt;
        if (!promise)
            return awaited;
        break;
    }

    const BuiltinTable& builtins = context.program.builtins();
    const Type& fulfillment = context.types.at(context.level == TypeLevel::Proven ? promise->writtenContentsAt(builtins, ContentsSlot::Value) : promise->expectedContentsAt(builtins, ContentsSlot::Value).predicted());
    return awaited.without(Type::builtinClass(BuiltinClasses::Promise), context.types).unionWith(fulfillment, context.types);
}

Type typeOfAwaited(const TypingContext& context, const Type& awaited)
{
    return tryTypeOfAwaited(context, awaited).value_or(Type::anything());
}

TypeID declaredEntryTypeOf(const TypingContext& context, const Function& callee, unsigned entryParameterIndex)
{
    switch (callee.entryParameterKind(entryParameterIndex)) {
    case EntryParameter::Capture:
    case EntryParameter::This:
    case EntryParameter::Parameter:
        return context.typeIDOf(*callee.declaredEntryParameter(entryParameterIndex));
    case EntryParameter::Callee:
        return TypeTable::function;
    case EntryParameter::NewTarget:
        return TypeTable::anything;
    case EntryParameter::ArgumentCount:
        return context.types.intern(Type::integer(Range::uint32()));
    case EntryParameter::HiddenRest: {
        const TypeID contents[] { TypeTable::anything };
        return context.types.intern(Type::builtinClass(BuiltinClasses::Array, contents));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID requiredTypeOfField(const TypingContext& positions, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = positions.program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return typeIDOfField(positions, fields[field]);
}

static TypeID requiredTypeOfPrivateDefinition(const TypingContext& positions, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = positions.program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return fields[field].flags.contains(FieldFlags::IsBrand) ? positions.typeIDOf(fields[field].declared) : TypeTable::anything;
}

static TypeID requiredTypeOfInitializedField(const TypingContext& positions, const Value& allocation, uint32_t valueIndex)
{
    std::span<const uint32_t> fields = allocation.fields();
    if (valueIndex >= fields.size())
        return { };
    TypeID declared = requiredTypeOfField(positions, allocation.layout(), fields[valueIndex]);
    if (!declared || !slotHoldsUndefinedAndNull(positions.program, { allocation.layout(), fields[valueIndex] }))
        return declared;
    return positions.types.withPrimitives(declared, Primitive::UndefinedOrNull);
}

static TypeID requiredTypeOfCapture(const TypingContext& positions, FunctionID function, uint32_t captureIndex)
{
    const Function& callee = positions.program.at(function);
    if (captureIndex >= callee.captures.size())
        return { };
    return positions.typeIDOf(callee.captures[captureIndex]);
}

static TypeID requiredTypeOfArgument(const TypingContext& positions, FunctionID function, uint32_t entryParameterIndex)
{
    const Function& callee = positions.program.at(function);
    if (entryParameterIndex >= callee.numberOfEntryParameters())
        return { };
    return declaredEntryTypeOf(positions, callee, entryParameterIndex);
}

static TypeID requiredTypeOfSlotStore(const TypingContext& positions, LayoutSetID receivers, PropertyKeyID key)
{
    std::optional<Type> accepted = fieldTypeOf(positions, receivers, key, FieldTypeRule::SlotStore);
    return accepted ? positions.types.intern(*accepted) : TypeID();
}

static TypeID requiredTypeOfSlotStore(const TypingContext& positions, const Value& slotRun, uint32_t valueIndex)
{
    PayloadListView<PlacedKey> placedKeys = slotRun.placedKeys();
    if (valueIndex >= placedKeys.size())
        return { };
    return requiredTypeOfSlotStore(positions, slotRun.receivers(), placedKeys[valueIndex].key);
}

static bool isReturned(const Function& function, const Value& results)
{
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = block.terminator.valueOperand();
        if (returned && &function.at(returned) == &results)
            return true;
    }
    return false;
}

static TypeID requiredTypeOfReturnedResult(const TypingContext& positions, const Value& results, uint32_t resultIndex)
{
    const Function& function = positions.function;
    if (function.results.size() == 1 || function.isUnsplitCoroutine() || resultIndex >= function.results.size())
        return { };
    if (!isReturned(function, results))
        return { };
    return positions.typeIDOf(function.results[resultIndex]);
}

static TypeID requiredTypeOfIteratorResultField(const TypingContext& positions, uint32_t operandIndex)
{
    const Program& program = positions.program;
    const Layout& layout = program.at(program.layoutOf(BuiltinLayouts::IteratorResult));
    BuiltinName name = operandIndex == OperandIndex::CreateIteratorResult::value ? BuiltinNames::value : BuiltinNames::done;
    std::optional<uint32_t> field = layout.findField(program.keyOf(name));
    return field ? positions.typeIDOf(layout.fields[*field].declared) : TypeID();
}

// A literal that names storage for numbers alone is born with it, whatever is known of its elements where code is generated, and nobody who reads the name tests the storage.
// One that names none is born with such storage where its elements are known to fit. Storage for every value takes an element whatever is known of it.
static TypeID requiredTypeOfElement(const TypingContext& positions, const Value& literal)
{
    ArrayStorage named = literal.arrayStorage();
    bool mayHoldOnlyNumbers = holdsOnlyNumbers(named == ArrayStorage::Other ? arrayStorageAtAllocation(positions.types.at(literal.contents())) : named);
    return literal.contentsAreProven() && mayHoldOnlyNumbers ? literal.contents() : TypeID();
}

const OpcodeSet& opcodesWithRequiredOperandTypes()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result { Opcode::CreateIteratorResult };
        for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
            Opcode opcode = static_cast<Opcode>(index);
            uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
            for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
                if (operandKindOf(opcode, operandIndex) == OperandKind::Declared)
                    result.add(opcode);
            }
        }
        return result;
    }();
    return opcodes;
}

TypeID requiredTypeOfOperand(const TypingContext& context, const Value& user, uint32_t operandIndex)
{
    TypingContext positions = context.atLevel(TypeLevel::Proven);
    if (user.opcode == Opcode::CreateIteratorResult)
        return requiredTypeOfIteratorResultField(positions, operandIndex);
    if (user.operandKind(operandIndex) != OperandKind::Declared || isHeldOperand(user, operandIndex))
        return { };

    auto variadicIndex = [&] {
        return *user.variadicOperandIndex(operandIndex);
    };

    switch (user.opcode) {
    case Opcode::PutModuleVariable:
        return positions.typeIDOf(positions.program.at(user.variable()).declared);
    case Opcode::PutEnvironmentField:
        return requiredTypeOfField(positions, user.layout(), user.field());
    case Opcode::PrivateDefine:
        return requiredTypeOfPrivateDefinition(positions, user.layout(), user.field());
    case Opcode::NewClosure:
        return requiredTypeOfCapture(positions, user.function(), variadicIndex());
    case Opcode::NewObject:
        return requiredTypeOfInitializedField(positions, user, variadicIndex());
    case Opcode::NewArray:
        return requiredTypeOfElement(positions, user);
    case Opcode::CallDirect:
    case Opcode::CallTable:
    case Opcode::AwaitCallBySavingFrame:
        return requiredTypeOfArgument(positions, user.function(), variadicIndex());
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return requiredTypeOfSlotStore(positions, user.receivers(), user.key());
    case Opcode::PutSlots:
        return requiredTypeOfSlotStore(positions, user, variadicIndex());
    case Opcode::Results:
        return requiredTypeOfReturnedResult(positions, user, variadicIndex());
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID requiredTypeOfOperand(const TypingContext& context, const Terminator& user, uint32_t operandIndex)
{
    const Function& function = context.function;
    if (user.kind != TerminatorKind::Return || !user.operand(operandIndex))
        return { };
    if (function.results.size() != 1 || function.isUnsplitCoroutine())
        return { };
    return context.atLevel(TypeLevel::Proven).typeIDOf(function.result());
}

RequiredTypeHolder holderOfRequiredTypes(const Program& program, const Value& user)
{
    switch (user.opcode) {
    case Opcode::PutModuleVariable:
    case Opcode::PutEnvironmentField:
    case Opcode::NewClosure:
    case Opcode::CallDirect:
    case Opcode::CallTable:
    case Opcode::AwaitCallBySavingFrame:
    case Opcode::Results:
    case Opcode::CreateIteratorResult:
        return RequiredTypeHolder::PositionThatFlowFollows;
    case Opcode::NewObject:
        return program.at(user.layout()).isEnvironment() ? RequiredTypeHolder::PositionThatFlowFollows : RequiredTypeHolder::InitializedSlot;
    case Opcode::NewArray:
        return RequiredTypeHolder::ContentsOfLiteral;
    default:
        return RequiredTypeHolder::StoredSlot;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
