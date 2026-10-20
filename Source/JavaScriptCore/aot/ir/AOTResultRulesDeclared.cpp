/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowParts.h"
#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

TypeID TypingContext::typeIDOf(const DeclaredType& declared) const
{
    TypeID writtenOrType = declared.written ? declared.written : declared.type;
    switch (level) {
    case TypeLevel::Proven:
        return declared.type;
    case TypeLevel::Written:
        return writtenOrType;
    case TypeLevel::Predicted:
        return declared.predicted ? declared.predicted : writtenOrType;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

static std::optional<Type> tryTypeOfProvenField(const TypingContext& context, FieldReference reference)
{
    const FieldFlow& flow = flowOf(context.program, reference);
    if (!hasProvenType(flow))
        return std::nullopt;

    const Type& declared = context.typeOf(fieldAt(context.program, reference).declared);
    if (flow.occupancy == Occupancy::AlwaysFilled)
        return declared;
    if (flow.prototypeName != PrototypeNameState::AbsentOnPrototypes)
        return std::nullopt;
    return declared.withPrimitives(Primitive::Undefined);
}

static Type typeOfDeclaredField(const TypingContext& context, FieldReference reference)
{
    const Field& field = fieldAt(context.program, reference);
    const Type& declared = context.typeOf(field.declared);
    if (field.flags.contains(FieldFlags::IsRequired))
        return declared;
    return declared.withPrimitives(Primitive::Undefined);
}

static std::optional<Type> tryTypeOfField(const TypingContext& context, FieldReference reference)
{
    if (context.level == TypeLevel::Proven)
        return tryTypeOfProvenField(context, reference);
    return typeOfDeclaredField(context, reference);
}

static bool hasWrittenField(const TypingContext& written, LayoutID layout, PropertyKeyID key)
{
    std::optional<FieldReference> field = findField(written.program, layout, key, BaseChain::Predicted);
    return field && !written.typeOf(fieldAt(written.program, *field).declared).isAnything();
}

// The members of a written type include the layouts of inferred types that flowed into it. Nobody wrote the type of their fields:
// what was written about a field read through the type is in the member they flowed into.
static bool isWrittenInSupertype(const TypingContext& written, std::span<const LayoutID> receivers, LayoutID member, PropertyKeyID key)
{
    Vector<LayoutID, 8> reached { member };
    for (size_t index = 0; index < reached.size(); ++index) {
        const Layout& layout = written.program.at(reached[index]);
        auto visit = [&](LayoutID supertype) {
            if (!supertype || reached.contains(supertype))
                return false;
            reached.append(supertype);
            return std::ranges::binary_search(receivers, supertype) && hasWrittenField(written, supertype, key);
        };
        if (visit(layout.predictedBase) || std::ranges::any_of(layout.supertypes, visit))
            return true;
    }
    return false;
}

std::optional<Type> tryTypeOfFieldRead(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    bool isProof = context.level == TypeLevel::Proven;
    BaseChain chain = isProof ? BaseChain::Proven : BaseChain::Predicted;
    Type result = Type::never();
    bool isMissingInEveryReceiver = !receivers.empty();
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> field = findField(context.program, receiver, key, chain);
        if (!field) {
            if (isProof)
                return std::nullopt;
            // The members of a written type include every layout that leaves out an optional field, and a missing field reads undefined.
            result = result.withPrimitives(Primitive::Undefined);
            continue;
        }
        if (context.level == TypeLevel::Written && receivers.size() > 1 && !hasWrittenField(context, receiver, key) && isWrittenInSupertype(context, receivers, receiver, key))
            continue;
        isMissingInEveryReceiver = false;
        std::optional<Type> part = tryTypeOfField(context, *field);
        if (!part)
            return std::nullopt;
        result = result.unionWith(*part, context.types);
    }
    if (isMissingInEveryReceiver)
        return std::nullopt;
    return result;
}

static uint32_t hashOfFieldRead(LayoutSetID receivers, PropertyKeyID key, TypeLevel level)
{
    return computeHash(receivers, key, level);
}

auto FieldReadTypes::find(LayoutSetID receivers, PropertyKeyID key, TypeLevel level) const -> const Entry*
{
    std::optional<uint32_t> position = m_index.find(hashOfFieldRead(receivers, key, level), [&](uint32_t candidate) {
        const Entry& entry = m_entries[candidate];
        return entry.receivers == receivers && entry.key == key && entry.level == level;
    });
    return position ? &m_entries[*position] : nullptr;
}

void FieldReadTypes::add(const Entry& entry)
{
    m_index.add(hashOfFieldRead(entry.receivers, entry.key, entry.level), m_entries.size());
    m_entries.append(entry);
}

std::optional<Type> tryTypeOfFieldRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    std::span<const LayoutID> receivers = membersOf(context.program, receiver);
    if (!context.fieldReadTypes || receivers.size() < FieldReadTypes::fewestReceivers)
        return tryTypeOfFieldRead(context, receivers, key);

    if (const FieldReadTypes::Entry* known = context.fieldReadTypes->find(receiver.layouts, key, context.level)) {
        if (!known->type)
            return std::nullopt;
        return context.types.at(known->type);
    }
    std::optional<Type> type = tryTypeOfFieldRead(context, receivers, key);
    context.fieldReadTypes->add({ receiver.layouts, key, context.level, type ? context.types.intern(*type) : TypeID() });
    return type;
}

bool hasWrittenField(const Program& program, const Type& receiver, PropertyKeyID key)
{
    bool hasOnlyLayoutsAmongObjects = receiver.layouts && !receiver.anyObject && !receiver.anyFunction && receiver.classes.isEmpty() && receiver.functions.isEmpty() && receiver.builtins.isEmpty();
    if (!hasOnlyLayoutsAmongObjects)
        return false;
    if (!(receiver.primitives & ~Primitive::UndefinedOrNull))
        return true;
    BuiltinName name = program.builtinNameOf(key);
    return !name || program.builtins().classesWithInstanceMember(name).empty();
}

Type typeOfFieldRead(const TypingContext& context, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    return tryTypeOfFieldRead(context, receivers, key).value_or(Type::anything());
}

Type typeOfFieldRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    return tryTypeOfFieldRead(context, receiver, key).value_or(Type::anything());
}

static bool hasOnlyClassObjects(const Type& type)
{
    return !type.anyObject && !type.anyFunction && !type.layouts && type.functions.isEmpty() && type.builtins.isEmpty();
}

std::optional<Type> tryTypeOfContentsRead(const TypingContext& context, const Type& container, ContentsSlot slot)
{
    if (!container.isJavaScriptValue() || container.primitives & ~Primitive::UndefinedOrNull || !hasOnlyClassObjects(container))
        return std::nullopt;

    const BuiltinTable& builtins = context.program.builtins();
    Type result = Type::never();
    for (const ClassPart& part : container.classes) {
        BuiltinClassID typedArrayClass = builtins.typedArrayClassOf(part.builtinClass);
        if (slot == ContentsSlot::Element && typedArrayClass) {
            result = result.unionWith(elementTypeOf(builtins.at(typedArrayClass).typedArrayType), context.types);
            continue;
        }
        if (context.level == TypeLevel::Proven && !part.contentsAreProven)
            return std::nullopt;
        TypeID contents = context.predicts() ? part.predictedContentsAt(builtins, slot) : part.contentsAt(builtins, slot);
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

std::optional<Type> tryTypeOfElementRead(const TypingContext& context, const Type& container, const Type& key)
{
    if (key.isNever() || container.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return Type::never();
    if (!key.isNumber())
        return std::nullopt;
    if (container.isAnyStringOrNullish())
        return Type::fromPrimitives(Primitive::String | Primitive::Undefined);
    // The Element contents of a Set or a generator are not its indexed properties.
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
    bool hasOnlyPromiseObjects = promise && awaited.classes.size() == 1 && hasOnlyClassObjects(awaited);
    switch (context.level) {
    case TypeLevel::Proven:
        if (!hasOnlyPromiseObjects || !promise->contentsAreProven)
            return std::nullopt;
        break;
    case TypeLevel::Written:
        // A written type licenses a TypeError, and an object of any other class may be a thenable.
        if (!hasOnlyPromiseObjects)
            return std::nullopt;
        break;
    case TypeLevel::Predicted:
        if (awaited.anyObject)
            return std::nullopt;
        if (!promise)
            return awaited;
        break;
    }

    const BuiltinTable& builtins = context.program.builtins();
    TypeID contents = context.predicts() ? promise->predictedContentsAt(builtins, ContentsSlot::Value) : promise->contentsAt(builtins, ContentsSlot::Value);
    const Type& fulfillment = context.types.at(contents);
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

static TypeID declaredTypeOfField(const Program& program, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return fields[field].declared.type;
}

// The operation keeps a value that does not fit the field's type outside the slot. Only the brand holds what is no JavaScript value: the class token.
static TypeID requiredTypeOfPrivateDefinition(const Program& program, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return fields[field].flags.contains(FieldFlags::IsBrand) ? fields[field].declared.type : TypeTable::anything;
}

static TypeID declaredTypeOfInitializedField(const Program& program, const Value& allocation, uint32_t valueIndex)
{
    std::span<const uint32_t> fields = allocation.fields();
    if (valueIndex >= fields.size())
        return { };
    return declaredTypeOfField(program, allocation.layout(), fields[valueIndex]);
}

static TypeID declaredTypeOfCapture(const Function& callee, uint32_t captureIndex)
{
    if (captureIndex >= callee.captures.size())
        return { };
    return callee.captures[captureIndex].type;
}

static TypeID declaredTypeOfArgument(const TypingContext& context, FunctionID function, uint32_t entryParameterIndex)
{
    const Function& callee = context.program.at(function);
    if (entryParameterIndex >= callee.numberOfEntryParameters())
        return { };
    return declaredEntryTypeOf(context.atLevel(TypeLevel::Proven), callee, entryParameterIndex);
}

static TypeID requiredTypeOfSlotStore(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    TypeID required;
    for (LayoutID receiver : context.types.at(receivers).members) {
        std::optional<FieldReference> field = findField(context.program, receiver, key, BaseChain::Proven);
        if (!field)
            return { };
        TypeID declared = fieldAt(context.program, *field).declared.type;
        required = required ? context.types.intersectionOf(required, declared) : declared;
    }
    return required;
}

static TypeID requiredTypeOfSlotStore(const TypingContext& context, const Value& slotRun, uint32_t valueIndex)
{
    PayloadListView<PlacedKey> placedKeys = slotRun.placedKeys();
    if (valueIndex >= placedKeys.size())
        return { };
    return requiredTypeOfSlotStore(context, slotRun.receivers(), placedKeys[valueIndex].key);
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

static TypeID declaredTypeOfReturnedResult(const Function& function, const Value& results, uint32_t resultIndex)
{
    // A function with one result returns a plain value: a Results value in its body came from an inlined callee.
    if (function.results.size() == 1 || function.isUnsplitCoroutine() || resultIndex >= function.results.size())
        return { };
    if (!isReturned(function, results))
        return { };
    return function.results[resultIndex].type;
}

static TypeID declaredTypeOfIteratorResultField(const Program& program, uint32_t operandIndex)
{
    const Layout& layout = program.at(program.layoutOf(BuiltinLayouts::IteratorResult));
    BuiltinName name = operandIndex == OperandIndex::CreateIteratorResult::value ? BuiltinNames::value : BuiltinNames::done;
    std::optional<uint32_t> field = layout.findField(program.keyOf(name));
    return field ? layout.fields[*field].declared.type : TypeID();
}

TypeID requiredTypeOfOperand(const TypingContext& context, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::CreateIteratorResult)
        return declaredTypeOfIteratorResultField(context.program, operandIndex);
    if (user.operandKind(operandIndex) != OperandKind::Declared)
        return { };

    const Program& program = context.program;
    auto variadicIndex = [&] {
        return *user.variadicOperandIndex(operandIndex);
    };

    switch (user.opcode) {
    case Opcode::PutModuleVariable:
        return program.at(user.variable()).declared.type;
    case Opcode::PutEnvironmentField:
        return declaredTypeOfField(program, user.layout(), user.field());
    case Opcode::PrivateDefine:
        return requiredTypeOfPrivateDefinition(program, user.layout(), user.field());
    case Opcode::NewClosure:
        return declaredTypeOfCapture(program.at(user.function()), variadicIndex());
    case Opcode::NewObject:
    case Opcode::CloneAs:
        return declaredTypeOfInitializedField(program, user, variadicIndex());
    case Opcode::CallDirect:
    case Opcode::AwaitCallBySavingFrame:
        return declaredTypeOfArgument(context, user.function(), variadicIndex());
    case Opcode::CallGroup:
        return declaredTypeOfArgument(context, user.function(), variadicIndex() + 1);
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return requiredTypeOfSlotStore(context, user.receivers(), user.key());
    case Opcode::PutSlots:
        return requiredTypeOfSlotStore(context, user, variadicIndex());
    case Opcode::Results:
        return declaredTypeOfReturnedResult(context.function, user, variadicIndex());
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
    return function.result().type;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
