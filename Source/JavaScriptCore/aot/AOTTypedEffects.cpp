/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include "Options.h"

namespace JSC { namespace AOT { namespace Typed {

FunctionID constructedBy(const Program& program, const Function& function, ValueID object)
{
    return (program.allocations.isEmpty() ? function.allocations : program.allocations)[function.at(object).allocation()].constructedBy;
}

static std::optional<uint32_t> storedFieldOf(const Program& program, const Function& function, ValueID object, const Value& store, bool isForFunction)
{
    if (store.child(0) != object || store.child(1) == object)
        return std::nullopt;
    LayoutID created = function.at(object).layout();
    switch (store.opcode) {
    case Opcode::PutField:
        if (store.layout() == created)
            return store.field();
        return program.at(created).fieldNamed(program.at(store.layout()).fields[store.field()].name);
    case Opcode::PutById:
        if ((program.prototypePropertyNames.get(store.string().index()) && inheritsThroughLayoutPrototype(program, created, store.string())) || (program.at(created).kind == LayoutKind::Record && program.at(created).mayHaveOtherPrototype && isForFunction))
            return std::nullopt;
        if (program.prototypePropertyNames.get(store.string().index()) && program.at(created).kind != LayoutKind::Record)
            return std::nullopt;
        return program.at(created).fieldNamed(store.string());
    default:
        return std::nullopt;
    }
}

static thread_local ImmutableFunctionScope* currentImmutableFunctionScope;

ImmutableFunctionScope::ImmutableFunctionScope()
    : m_outerScope(std::exchange(currentImmutableFunctionScope, this))
{
}

ImmutableFunctionScope::~ImmutableFunctionScope()
{
    currentImmutableFunctionScope = m_outerScope;
}

bool isInitializingStore(const Program& program, const Function& function, const Block& block, size_t index)
{
    ValueID object = function.at(block.values[index]).child(0);
    if (!object || function.at(object).opcode != Opcode::NewObjectWithShape)
        return false;
    auto* scope = currentImmutableFunctionScope;
    bool isKnownUpToLast = scope && scope->m_initializingStoresBlock == &block && scope->m_initializingStoresObject == object;
    if (isKnownUpToLast && index <= scope->m_lastInitializingStore)
        return true;
    LayoutID created = function.at(object).layout();
    std::optional<bool> isForFunction;
    for (size_t i = index + 1; i--;) {
        ValueID id = block.values[i];
        if (id == object || (isKnownUpToLast && i == scope->m_lastInitializingStore)) {
            if (scope) {
                scope->m_initializingStoresBlock = &block;
                scope->m_initializingStoresObject = object;
                scope->m_lastInitializingStore = index;
            }
            return true;
        }
        auto& store = function.at(id);
        if (store.opcode == Opcode::PutField && store.layout() == created && store.child(0) == object && store.child(1) != object)
            continue;
        if (store.opcode == Opcode::NewClosure && i < index && program.at(created).kind == LayoutKind::Record)
            continue;
        if (!isForFunction)
            isForFunction = hasUnknownPrototypeAtCreation(program, function, object);
        if (!storedFieldOf(program, function, object, store, *isForFunction))
            return false;
    }
    return false;
}

bool isLocalAllocation(const Function& function, ValueID id)
{
    while (function.at(id).opcode == Opcode::Cast)
        id = function.at(id).child(0);
    return function.at(id).opcode == Opcode::NewObjectWithShape;
}

std::span<const ValueID> initialStoresOf(const Function& function)
{
    if (function.blocks.isEmpty())
        return { };
    auto values = function.blocks[0].values.span();
    auto isOne = [&](ValueID id) {
        auto& value = function.at(id);
        return value.opcode == Opcode::PutById && value.isInitialStore() && !value.inlineFrame;
    };
    size_t begin = 0;
    while (begin < values.size() && !isOne(values[begin]))
        ++begin;
    size_t end = begin;
    while (end < values.size() && isOne(values[end]))
        ++end;
    return values.subspan(begin, end - begin);
}

void setInitializedFields(const Layout& layout, Allocation& allocation)
{
    for (unsigned field = 0; field < layout.fields.size(); ++field) {
        if (!(layout.fields[field].flags & fieldIsAbsentAtCreation))
            allocation.order.append(static_cast<uint16_t>(field));
    }
    allocation.initialFieldCount = static_cast<uint16_t>(allocation.order.size());
}

static constexpr OpcodeSet allocationsWithShape { Opcode::NewObjectWithShape };

void assignAllocations(const Program& program, Function& function)
{
    for (auto& block : function.blocks) {
        for (size_t at = 0; at < block.values.size(); ++at) {
            ValueID object = block.values[at];
            if (!allocationsWithShape.contains(function.at(object).opcode))
                continue;
            Allocation allocation;
            allocation.layout = function.at(object).layout();
            auto& layout = program.at(allocation.layout);
            if (layout.kind == LayoutKind::Record) {
                BitVector isInOrder;
                for (size_t next = at + 1; next < block.values.size(); ++next) {
                    if (function.at(block.values[next]).opcode == Opcode::NewClosure)
                        continue;
                    auto field = storedFieldOf(program, function, object, function.at(block.values[next]), false);
                    if (!field)
                        break;
                    if (!isInOrder.set(*field))
                        allocation.order.append(static_cast<uint16_t>(*field));
                }
                allocation.initialFieldCount = static_cast<uint16_t>(allocation.order.size());
            } else
                setInitializedFields(layout, allocation);
            if (function.at(object).child(0)) {
                auto discriminant = layout.kind == LayoutKind::Record ? layout.fieldNamed(StringID(function.at(object).immediates[1])) : std::nullopt;
                if (discriminant)
                    allocation.discriminant = static_cast<uint16_t>(*discriminant);
                else
                    function.at(object).children[0] = { };
            }
            function.at(object).immediates[1] = function.allocationOf(WTF::move(allocation));
        }
    }
}

void assignAllocations(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (program.functions.mayContain(i, allocationsWithShape))
            assignAllocations(program, program.functions.atKeepingValueIndex(i));
    });
}

static bool coerces(const Type& from, const Type& to)
{
    return from.hasObjects() && !to.objectLayouts.isEmpty();
}

static void findFieldsWithAnyObjectSubtype(Program& program)
{
    Vector<Vector<StringID, 2>> matchingFieldNames(program.layouts.size());
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        for (auto& field : std::as_const(program).layouts[i].fields) {
            if (!field.type || program.at(field.type).anyObject || program.at(field.type).acceptsUntypedObjects(program))
                matchingFieldNames[i].append(field.name);
        }
    });
    Vector<uint32_t> lastLayoutWithName(FillWith { }, program.strings.size(), UINT32_MAX);
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& supertype = program.layouts[i];
        bool hasUnnamedField = false;
        for (LayoutID subtype : supertype.subtypes) {
            if (subtype.index() == i)
                continue;
            for (StringID name : matchingFieldNames[subtype.index()]) {
                if (name)
                    lastLayoutWithName[name.index()] = i;
                else
                    hasUnnamedField = true;
            }
        }
        for (auto& field : supertype.fields) {
            if (field.name ? lastLayoutWithName[field.name.index()] == i : hasUnnamedField)
                field.isAnyObjectInSomeSubtype = true;
        }
    }
}

static constexpr size_t maxSubtypesCheckedAtRead = 32;

static void findFieldsHoldingConvertibleRecords(Program& program)
{
    BitVector hasConvertibleSubtype(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (program.layouts[i].subtypes.size() <= maxSubtypesCheckedAtRead && program.layouts[i].subtypes.containsIf([&](LayoutID subtype) { return !program.at(subtype).specializations.isEmpty(); }))
            hasConvertibleSubtype.set(i);
    }
    if (hasConvertibleSubtype.isEmpty())
        return;
    Vector<Vector<std::pair<StringID, TypeID>, 1>> convertibleFields(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        for (auto& field : program.layouts[i].fields) {
            if (field.type && program.at(field.type).objectLayouts.containsIf([&](LayoutID supertype) { return hasConvertibleSubtype.get(supertype.index()); }))
                convertibleFields[i].append({ field.name, field.type });
        }
    }
    UncheckedKeyHashMap<uint64_t, bool> cache;
    auto mayHoldConvertible = [&](TypeID held, TypeID own) {
        return cache.ensure((static_cast<uint64_t>(held.index()) << 32 | own.index()) + 1, [&] {
            BitVector isSubtypeOfOwnType(program.layouts.size());
            for (LayoutID supertype : program.at(own).objectLayouts) {
                for (LayoutID subtype : program.at(supertype).subtypes)
                    isSubtypeOfOwnType.set(subtype.index());
            }
            for (LayoutID supertype : program.at(held).objectLayouts) {
                if (!hasConvertibleSubtype.get(supertype.index()))
                    continue;
                for (LayoutID layout : program.at(supertype).subtypes) {
                    if (isSubtypeOfOwnType.get(layout.index()))
                        continue;
                    if (program.at(layout).specializations.containsIf([&](LayoutID specialization) { return isSubtypeOfOwnType.get(specialization.index()); }))
                        return true;
                }
            }
            return false;
        }).iterator->value;
    };
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& supertype = program.layouts[i];
        for (LayoutID subtype : supertype.subtypes) {
            if (subtype.index() == i)
                continue;
            for (auto [name, held] : convertibleFields[subtype.index()]) {
                for (auto& field : supertype.fields) {
                    if (field.name == name && !field.isAnyObjectInSomeSubtype && field.type && field.type != held && !program.at(field.type).objectLayouts.isEmpty() && mayHoldConvertible(held, field.type))
                        field.isAnyObjectInSomeSubtype = true;
                }
            }
        }
    }
}

static void findFieldsCoercibleInSubtypes(Program& program)
{
    BitVector matchingFieldNames;
    for (auto& supertype : program.layouts) {
        for (auto& field : supertype.fields) {
            field.hasDifferentTypeInSomeSupertype = false;
            field.mayBeCoercedInSomeSubtype = false;
            if (field.type && readMayInstantiate(program, program.at(field.type)))
                matchingFieldNames.set(field.name.index());
        }
    }
    if (matchingFieldNames.isEmpty())
        return;
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        auto& supertype = program.layouts[i];
        for (auto& field : supertype.fields) {
            if (!matchingFieldNames.get(field.name.index()) || (field.type && !program.at(field.type).hasObjects()))
                continue;
            for (LayoutID id : supertype.subtypes) {
                auto& subtype = program.layouts[id.index()];
                if (&subtype == &supertype)
                    continue;
                if (auto index = subtype.fieldNamed(field.name); index && subtype.fields[*index].type != field.type)
                    WTF::atomicStore(&subtype.fields[*index].hasDifferentTypeInSomeSupertype, true, std::memory_order_relaxed);
            }
        }
    });
    forEachIndexInParallel(program.layouts.size(), [&](unsigned i) {
        auto& supertype = program.layouts[i];
        for (auto& field : supertype.fields) {
            if (!field.type || !readMayInstantiate(program, program.at(field.type)))
                continue;
            field.mayBeCoercedInSomeSubtype = std::ranges::any_of(supertype.subtypes, [&](LayoutID id) {
                auto& subtype = program.layouts[id.index()];
                auto index = subtype.fieldNamed(field.name);
                return index && (subtype.fields[*index].hasDifferentTypeInSomeSupertype || subtype.fields[*index].type != field.type);
            });
        }
    });
}

void findRetagTargets(Program& program)
{
    findFieldsWithAnyObjectSubtype(program);
    findFieldsHoldingConvertibleRecords(program);
    unsigned numberOfLayouts = program.layouts.size();
    BitVector isSpecialization(numberOfLayouts);
    BitVector isInstantiation(numberOfLayouts);
    auto hasFieldsOf = [](const Layout& layout, const Layout& other) {
        return std::ranges::all_of(other.fields, [&](const Field& field) {
            return field.flags & fieldIsNeverStored || layout.fieldNamed(field.name);
        });
    };
    for (auto& layout : program.layouts) {
        for (LayoutID specialization : layout.specializations) {
            isSpecialization.set(specialization.index());
            if (hasFieldsOf(layout, program.at(specialization)) && hasFieldsOf(program.at(specialization), layout))
                isInstantiation.set(specialization.index());
        }
    }
    auto& instantiatedTo = program.instantiationTargets;
    instantiatedTo.clearAll();
    instantiatedTo.ensureSize(numberOfLayouts);
    auto& retaggedTo = program.retagTargets;
    retaggedTo.clearAll();
    retaggedTo.ensureSize(numberOfLayouts);
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        auto& layout = program.layouts[i];
        bool hasInstantiation = layout.isAllocated ? isInstantiation.get(i) : std::ranges::any_of(layout.subtypes, [&](LayoutID subtype) {
            return isInstantiation.get(subtype.index());
        });
        if (hasInstantiation)
            instantiatedTo.set(i);
        if (layout.isAllocated) {
            if (isSpecialization.get(i))
                retaggedTo.set(i);
            continue;
        }
        for (LayoutID subtype : layout.subtypes) {
            if (isSpecialization.get(subtype.index())) {
                retaggedTo.set(i);
                break;
            }
        }
    }
    findFieldsCoercibleInSubtypes(program);
    auto& trivialCoercion = program.layoutsWithTrivialCoercion;
    trivialCoercion.clearAll();
    if (Options::auditAOTTypedAssertions()) [[unlikely]]
        return;
    trivialCoercion.ensureSize(numberOfLayouts);
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (!retaggedTo.get(i) && !isSpecialization.get(i) && !program.layouts[i].checksUntypedObjects())
            trivialCoercion.set(i);
    }
    Vector<uint8_t> hasOnlyTrivialCoercionFields(FillWith { }, numberOfLayouts, static_cast<uint8_t>(0));
    Vector<uint8_t> holds(FillWith { }, numberOfLayouts, static_cast<uint8_t>(0));
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        forEachIndexInParallel(numberOfLayouts, [&](unsigned i) {
            auto& layout = std::as_const(program).layouts[i];
            hasOnlyTrivialCoercionFields[i] = layout.kind != LayoutKind::Record || !layout.fields.containsIf([&](auto& field) {
                if (!field.type || program.at(field.type).objectLayouts.isEmpty())
                    return false;
                auto& type = program.at(field.type);
                return !type.acceptsUntypedObjects(program) || type.objectLayouts.containsIf([&](LayoutID supertype) {
                    return !trivialCoercion.get(supertype.index());
                });
            });
        });
        forEachIndexInParallel(numberOfLayouts, [&](unsigned i) {
            holds[i] = !trivialCoercion.get(i) || (hasOnlyTrivialCoercionFields[i] && std::ranges::all_of(std::as_const(program).layouts[i].subtypes, [&](LayoutID subtype) {
                return hasOnlyTrivialCoercionFields[subtype.index()];
            }));
        });
        for (unsigned i = 0; i < numberOfLayouts; ++i) {
            if (!holds[i]) {
                trivialCoercion.clear(i);
                hasChanged = true;
            }
        }
    }
}

bool coercionMayMutateObjects(const Program& program, const Type& from, const Type& to, bool isFreshData)
{
    if (!coerces(from, to))
        return false;
    if (isFreshData && (from.anyObject || from.acceptsUntypedObjects(program)))
        return true;
    return narrowingMayRetag(program, to);
}

static Writes writesOfCoercion(const Type& from, const Type& to)
{
    return coerces(from, to) ? Writes::RuntimeState : Writes::Nothing;
}

Writes writesOfElementRead(const Program& program, const Type& type)
{
    for (LayoutID supertype : type.objectLayouts) {
        if (program.at(supertype).kind == LayoutKind::Record && (!program.at(supertype).acceptsUntypedObjects || program.at(supertype).checksUntypedObjects()))
            return Writes::RuntimeState;
    }
    return Writes::Nothing;
}

static bool mayHoldRecordConvertibleTo(const Program& program, const Type& from, const Type& to)
{
    if (&from == &to || !narrowingMayRetag(program, to))
        return false;
    auto isSubtypeOfTarget = [&](LayoutID layout) {
        return to.objectLayouts.containsIf([&](LayoutID supertype) {
            return program.at(supertype).subtypes.contains(layout);
        });
    };
    for (LayoutID supertype : from.objectLayouts) {
        if (program.at(supertype).subtypes.size() > maxSubtypesCheckedAtRead)
            continue;
        for (LayoutID layout : program.at(supertype).subtypes) {
            if (!program.at(layout).specializations.isEmpty() && !isSubtypeOfTarget(layout) && program.at(layout).specializations.containsIf(isSubtypeOfTarget))
                return true;
        }
    }
    return false;
}

bool claimMayAllocate(const Program& program, const Function& function, const Value& value)
{
    if ((value.castReason() != CastReason::Claim && value.castReason() != CastReason::Flow && value.castReason() != CastReason::Store) || value.string() || value.accessedName() || Options::auditAOTTypedAssertions())
        return false;
    auto& from = program.typeOf(function, value.child(0));
    return (from.anyObject || from.acceptsUntypedObjects(program) || mayHoldRecordConvertibleTo(program, from, program.at(value.type))) && writesOfElementRead(program, program.at(value.type)) != Writes::Nothing;
}

bool fieldReadMayAllocate(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::GetById || !value.type || Options::auditAOTTypedAssertions() || writesOfElementRead(program, program.at(value.type)) == Writes::Nothing)
        return false;
    auto access = fieldOf(program, program.typeOf(function, value.child(0)), value.string());
    if (!access)
        return false;
    auto& field = program.at(access->layout).fields[access->field];
    if (field.type == value.type ? field.mayBeCoercedInSomeSubtype : readMayInstantiate(program, program.at(value.type)))
        return true;
    return !field.type || program.at(field.type).anyObject || program.at(field.type).acceptsUntypedObjects(program) || field.isAnyObjectInSomeSubtype || mayHoldRecordConvertibleTo(program, program.at(field.type), program.at(value.type));
}

Writes writesOfCast(const Program& program, const Function& function, const Value& value)
{
    auto& from = program.typeOf(function, value.child(0));
    CastReason reason = value.castReason();
    if (Options::auditAOTTypedAssertions() && (reason == CastReason::As || reason == CastReason::Flow || reason == CastReason::Coerce)) [[unlikely]]
        return Writes::Heap;
    switch (reason) {
    case CastReason::Upcast:
        return Writes::Pure;
    case CastReason::Claim:
    case CastReason::Store:
        return claimMayAllocate(program, function, value) ? Writes::RuntimeState : Writes::Nothing;
    case CastReason::NonNull:
    case CastReason::Member:
        return Writes::Nothing;
    case CastReason::Flow:
        return value.keepsUnconvertibleObjects() || narrowingMayRetag(program, program.at(value.type)) || claimMayAllocate(program, function, value) ? Writes::RuntimeState : Writes::Nothing;
    case CastReason::As:
        return writesOfCoercion(from, program.at(value.type));
    case CastReason::Coerce:
        break;
    case CastReason::CoerceFreshObject:
    case CastReason::CoerceFreshGraph:
    case CastReason::AsFreshObject:
    case CastReason::AsIfConvertible:
    case CastReason::AsFreshObjectIfConvertible:
    case CastReason::FlowIfConvertible:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto& to = program.at(value.coercedType());
    Writes writes = Writes::Nothing;
    if (!from.isSubsetOf(program.at(value.type), program))
        writes = from.hasObjects() && !to.hasObjects() ? Writes::Heap : Writes::RuntimeState;
    if (coercionMayMutateObjects(program, from, to, value.coercesFreshData()))
        writes = std::max(writes, writesOfCoercion(from, to));
    return writes;
}

bool readMayInstantiate(const Program& program, const Type& type)
{
    return type.objectLayouts.containsIf([&](LayoutID supertype) {
        return program.instantiationTargets.get(supertype.index());
    });
}

bool narrowingMayRetag(const Program& program, const Type& type)
{
    for (LayoutID supertype : type.objectLayouts) {
        if (program.retagTargets.get(supertype.index()))
            return true;
    }
    return false;
}

bool castWritesShapeWords(const Program& program, const Function& function, const Value& value)
{
    if (Options::auditAOTTypedAssertions()) [[unlikely]]
        return writesOfCast(program, function, value) == Writes::Heap;
    if (value.castReason() == CastReason::Flow)
        return value.keepsUnconvertibleObjects() || narrowingMayRetag(program, program.at(value.type)) || (program.castsMayRetag && claimMayAllocate(program, function, value));
    if (!program.castsMayRetag)
        return false;
    if (value.castReason() == CastReason::Claim || value.castReason() == CastReason::Store)
        return claimMayAllocate(program, function, value);
    auto& from = program.typeOf(function, value.child(0));
    if (value.castReason() == CastReason::As)
        return coerces(from, program.at(value.type));
    return value.castReason() == CastReason::Coerce && (coercionMayMutateObjects(program, from, program.at(value.coercedType()), value.coercesFreshData()) || writesOfCast(program, function, value) == Writes::Heap);
}

static bool convertsObjects(const Builtin& builtin, std::span<const Type> arguments)
{
    if (builtin.parameters.isEmpty())
        return false;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (!arguments[i].hasObjects())
            continue;
        auto& parameter = builtin.parameters[std::min<unsigned>(i, builtin.parameters.size() - 1)];
        if (i >= builtin.parameters.size() && parameter.mode != ParameterMode::Rest)
            continue;
        switch (parameter.conversion) {
        case ArgumentConversion::ToNumber:
        case ArgumentConversion::ToString:
        case ArgumentConversion::ToPropertyKey:
            return true;
        case ArgumentConversion::None:
        case ArgumentConversion::RequiresNumber:
        case ArgumentConversion::RequiresInteger:
        case ArgumentConversion::RequiresTypedArray:
            break;
        }
    }
    return false;
}

static bool hasObjects(std::span<const Type> types)
{
    for (auto& type : types) {
        if (type.hasObjects())
            return true;
    }
    return false;
}

static bool isArrayBufferView(const Type* receiver)
{
    auto* engineClass = receiver ? receiver->singleEngineClass() : nullptr;
    return engineClass && (engineClass->typedArrayType != NotTypedArray || engineClass->type == DataViewType);
}

static Writes writesOf(const Builtin& builtin, const Type* receiver, std::span<const Type> arguments)
{
    if (builtin.effects & (Effect::CallsUserCode | Effect::WritesArguments))
        return Writes::Heap;
    if (builtin.effects & Effect::WritesReceiver && !isArrayBufferView(receiver))
        return Writes::Heap;
    if ((builtin.effects & Effect::ReadsArguments && hasObjects(arguments)) || convertsObjects(builtin, arguments))
        return Writes::Heap;
    if (builtin.effects & Effect::ReadsReceiver && receiver && receiver->hasObjects() && !receiver->singleEngineClass())
        return Writes::Heap;
    if (builtin.effects & Effect::WritesReceiver)
        return Writes::TypedArrayProperties;
    if (builtin.effects & (Effect::WritesRuntimeState | Effect::Allocates))
        return Writes::RuntimeState;
    if (builtin.effects || (receiver && receiver->mayBeUndefinedOrNull()))
        return Writes::Nothing;
    return Writes::Pure;
}

Writes writesOfBuiltinCall(const Program& program, const Function& function, const Value& value)
{
    return hasGenericFallback(program, function, value) ? Writes::Heap : writesOfResolvedBuiltinCall(program, function, value);
}

Writes writesOfResolvedBuiltinCall(const Program& program, const Function& function, const Value& value)
{
    auto& builtin = Builtins::singleton().at(value.builtin());
    const Type* receiver = builtin.hasReceiver() ? &program.typeOf(function, value.child(0)) : nullptr;
    Vector<Type, 4> arguments;
    for (ValueID argument : value.arguments)
        arguments.append(program.typeOf(function, argument));
    Writes implementationEffects = Writes::RuntimeState;
    if (builtin.implementation.kind == BuiltinImplementation::Inline) {
        if (auto inlinePath = writesOfInlineBuiltin(builtin.implementation.inlined, arguments.span()))
            implementationEffects = *inlinePath;
    }
    return std::max(writesOf(builtin, receiver, arguments.span()), implementationEffects);
}

bool isNumericOperand(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
}

Writes writesOfArithmetic(const Type& a, const Type* b)
{
    if (isNumericOperand(a) && (!b || isNumericOperand(*b)))
        return Writes::Nothing;
    return a.hasObjects() || (b && b->hasObjects()) ? Writes::Heap : Writes::RuntimeState;
}

namespace {

class EffectSummaries {
public:
    explicit EffectSummaries(Program& program, bool reportsFieldStores = true)
        : m_program(program)
        , m_reportsFieldStores(reportsFieldStores)
    {
    }

    void run()
    {
        computeCastEffects();
        if (Options::auditAOTTypedAssertions()) [[unlikely]]
            return;
        unsigned numberOfFunctions = m_program.functions.size();
        m_effects.fill({ }, 2 * numberOfFunctions);
        auto parts = collectInParallel<std::pair<unsigned, unsigned>>(numberOfFunctions, [&](unsigned i, Vector<std::pair<unsigned, unsigned>>& collected) {
            FunctionID id(i);
            EffectSummaries(m_program, m_reportsFieldStores).summarizeBody(id, m_effects[anyCallIndex(id)], m_effects[returningCallIndex(id)], collected);
        });
        Vector<uint32_t> firstDependent(FillWith { }, 2 * numberOfFunctions + 1, 0u);
        for (auto [dependent, summary] : parts)
            ++firstDependent[summary + 1];
        for (unsigned summary = 0; summary < 2 * numberOfFunctions; ++summary)
            firstDependent[summary + 1] += firstDependent[summary];
        Vector<unsigned> dependents(parts.size());
        Vector<uint32_t> nextDependent(firstDependent.span().first(2 * numberOfFunctions));
        for (auto [dependent, summary] : parts)
            dependents[nextDependent[summary]++] = dependent;

        Vector<unsigned> worklist;
        for (unsigned summary = 0; summary < m_effects.size(); ++summary) {
            if (m_effects[summary] != Effects { })
                worklist.append(summary);
        }
        while (!worklist.isEmpty()) {
            unsigned summary = worklist.takeLast();
            for (unsigned dependent : dependents.span().subspan(firstDependent[summary], firstDependent[summary + 1] - firstDependent[summary])) {
                if (m_effects[dependent].add(m_effects[summary]))
                    worklist.append(dependent);
            }
        }

        forEachIndexInParallel(numberOfFunctions, [&](unsigned i) {
            FunctionID id(i);
            auto& function = m_program.functions.atKeepingValueIndex(i);
            function.writes = m_effects[anyCallIndex(id)].writes;
            function.writesShapeWords = m_effects[anyCallIndex(id)].writesShapeWords;
            function.writesOnReturn = m_effects[returningCallIndex(id)].writes;
            function.writesShapeWordsOnReturn = m_effects[returningCallIndex(id)].writesShapeWords;
            function.convertsArrayStorage = m_effects[anyCallIndex(id)].convertsArrayStorage;
            function.convertsArrayStorageOnReturn = m_effects[returningCallIndex(id)].convertsArrayStorage;
        });
    }

    Writes writesOf(const Function& function, const Block& block, size_t index) { return effectsOf(function, block, index).writes; }

private:
    struct Effects {
        Writes writes { Writes::Nothing };
        bool writesShapeWords { false };
        bool convertsArrayStorage { false };

        bool add(Effects other)
        {
            if (writes >= other.writes && (writesShapeWords || !other.writesShapeWords) && (convertsArrayStorage || !other.convertsArrayStorage))
                return false;
            writes = std::max(writes, other.writes);
            writesShapeWords |= other.writesShapeWords;
            convertsArrayStorage |= other.convertsArrayStorage;
            return true;
        }

        friend bool operator==(const Effects&, const Effects&) = default;
    };

    static unsigned anyCallIndex(FunctionID id) { return 2 * id.index(); }
    static unsigned returningCallIndex(FunctionID id) { return 2 * id.index() + 1; }

    Effects changes(Writes writes) const { return { writes, writes == Writes::Heap && m_program.castsMayRetag, writes == Writes::Heap }; }

    const Type& typeOf(const Function& function, ValueID id) const { return m_program.typeOf(function, id); }

    void computeCastEffects()
    {
        m_program.castsMayRetag = false;
        for (auto& layout : m_program.layouts)
            m_program.castsMayRetag |= !layout.specializations.isEmpty() || layout.mayDespecialize || layout.kind == LayoutKind::Record;
    }

    Effects elementReadEffects(const Value& value) const
    {
        bool convertsArrayStorage = isNumericOperand(m_program.at(value.type)) || (value.prediction() && isNumericOperand(m_program.at(value.prediction())));
        Writes writes = writesOfElementRead(m_program, m_program.at(value.type));
        return { writes, writes != Writes::Nothing && m_program.castsMayRetag, convertsArrayStorage };
    }

    BitVector possiblyReturningBlocks(const Function& function) const
    {
        unsigned numberOfBlocks = function.blocks.size();
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        BitVector result(numberOfBlocks);
        Vector<unsigned> worklist;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            bool reachesEnd = true;
            for (ValueID id : function.blocks[b].values)
                reachesEnd &= function.at(id).type != m_program.types.never;
            auto& terminator = function.blocks[b].terminator;
            if (!reachesEnd) {
                if (terminator.kind == TerminatorKind::Invoke)
                    predecessors[terminator.targets[1].block.index()].append(b);
                continue;
            }
            for (auto& target : terminator.targets)
                predecessors[target.block.index()].append(b);
            if (terminator.kind == TerminatorKind::Return) {
                result.set(b);
                worklist.append(b);
            }
        }
        while (!worklist.isEmpty()) {
            for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                if (!result.set(predecessor))
                    worklist.append(predecessor);
            }
        }
        return result;
    }

    void summarizeBody(FunctionID id, Effects& anyCallEffects, Effects& returningCallEffects, Vector<std::pair<unsigned, unsigned>>& parts)
    {
        ImmutableFunctionScope immutableFunctionScope;
        auto& function = std::as_const(m_program).at(id);
        Effects maximum = changes(Writes::Heap);
        if (function.blocks.isEmpty()) {
            anyCallEffects = maximum;
            returningCallEffects = maximum;
            return;
        }
        BitVector mayReturn = possiblyReturningBlocks(function);
        std::optional<bool> hasLoopIfKnown;
        auto hasLoop = [&] {
            if (!hasLoopIfKnown) {
                hasLoopIfKnown = false;
                for (auto& block : function.blocks) {
                    for (ValueID value : block.values)
                        *hasLoopIfKnown |= function.at(value).opcode == Opcode::LoopHint;
                }
            }
            return *hasLoopIfKnown;
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = function.at(block.values[index]);
                if (value.opcode != Opcode::CallDirect) {
                    Effects effects = effectsOf(function, block, index);
                    if (effects.convertsArrayStorage && effects.writes != Writes::Heap)
                        effects.convertsArrayStorage = hasLoop();
                    anyCallEffects.add(effects);
                    if (mayReturn.get(b))
                        returningCallEffects.add(effects);
                    continue;
                }
                FunctionID callee = value.function();
                parts.append({ anyCallIndex(id), anyCallIndex(callee) });
                if (block.terminator.kind != TerminatorKind::Invoke) {
                    if (mayReturn.get(b))
                        parts.append({ returningCallIndex(id), returningCallIndex(callee) });
                    continue;
                }
                if (mayReturn.get(block.terminator.targets[0].block.index()))
                    parts.append({ returningCallIndex(id), returningCallIndex(callee) });
                if (mayReturn.get(block.terminator.targets[1].block.index()))
                    parts.append({ returningCallIndex(id), anyCallIndex(callee) });
            }
            if (returningCallEffects == maximum)
                return;
        }
    }

    bool isAccessToField(const Function& function, const Value& value)
    {
        auto access = fieldOf(m_program, typeOf(function, value.child(0)), value.string());
        if (!access)
            return false;
        return m_isOnlyInFields.ensure(static_cast<uint64_t>(access->layout.index()) << 32 | access->field, [&] {
            return prototypeChainLacksProperty(m_program, access->layout, value.string());
        }).iterator->value;
    }

    bool mayTransition(const Function& function, const Value& value)
    {
        auto access = fieldOf(m_program, typeOf(function, value.child(0)), value.string());
        return m_storeMayTransition.ensure(static_cast<uint64_t>(access->layout.index()) << 32 | access->field, [&] {
            return storeMayTransition(m_program, access->layout, value.string());
        }).iterator->value;
    }

    bool mayDespecializeReceiver(const Function& function, const Block& block, size_t index) const
    {
        return storeMayDespecializeObject(m_program, function, block, index, typeOf(function, function.at(block.values[index]).child(0)).singleLayout());
    }

    bool isEngineGlobal(const Value& value) const
    {
        BuiltinName name = m_program.builtinName(value.string());
        return Builtins::singleton().isGlobal(name) && !m_program.isExtended(name);
    }

    Effects effectsOf(const Function& function, const Block& block, size_t index)
    {
        auto& value = function.at(block.values[index]);
        if (value.opcode == Opcode::IsPlainRecord && value.immediates[1] == plainRecordIsConverted)
            return { Writes::RuntimeState, m_program.castsMayRetag, false };
        switch (value.opcode) {
        case Opcode::Parameter:
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::GetModuleVariable:
        case Opcode::GetField:
        case Opcode::GetCapture:
        case Opcode::GetArgument:
        case Opcode::GetRestLength:
        case Opcode::LogicalNot:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::IsPlainRecord:
        case Opcode::GetPlainRecordField:
        case Opcode::IsClassObjectUnmodified:
        case Opcode::IsClosureOf:
        case Opcode::Results:
        case Opcode::Unspecified:
        case Opcode::ResultAt:
        case Opcode::TypeOfIs:
        case Opcode::LoopHint:
        case Opcode::UnlikelyHint:
        case Opcode::AssertNotAllocated:
            return changes(Writes::Nothing);
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::ConstantSymbol:
        case Opcode::FunctionReference:
        case Opcode::IsBuiltinFunction:
        case Opcode::BuiltinFunction:
        case Opcode::NewObjectWithShape:
        case Opcode::MaterializeEnvironment:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
        case Opcode::NewArray:
        case Opcode::NewObject:
        case Opcode::CreateRest:
        case Opcode::CreateThis:
        case Opcode::StrictEq:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::Strcat:
        case Opcode::CatchValue:
            return changes(Writes::RuntimeState);
        case Opcode::GetExternalModule:
        case Opcode::CallModuleFunction:
        case Opcode::JumpToModuleEvaluator:
            return changes(Writes::Heap);
        case Opcode::ModuleEvaluationWord:
        case Opcode::OffsetIndex:
        case Opcode::GetModuleVariableAt:
            return changes(Writes::Nothing);
        case Opcode::PutModuleVariableAt:
        case Opcode::FailModuleCycle:
            return changes(Writes::Fields);
        case Opcode::AddToModuleEvaluationDepth:
            return changes(Writes::RuntimeState);
        case Opcode::PutModuleVariable:
            return changes(Writes::Fields);
        case Opcode::GetStaticField:
        case Opcode::PutStaticField:
            return changes(Writes::Heap);
        case Opcode::PutField:
            if (auto& layout = m_program.at(value.layout()); m_reportsFieldStores && isObjectLayout(layout.kind)) {
                auto mayConsultPrototypeChain = [&](LayoutID supertype) {
                    return storeMayConsultPrototypeChain(m_program, function, block, index, supertype, layout.fields[value.field()].name);
                };
                if (mayConsultPrototypeChain(typeOf(function, value.child(0)).singleLayout()) || mayConsultPrototypeChain(value.layout()))
                    return changes(Writes::Heap);
            }
            if (isLocalAllocation(function, value.child(0)))
                return changes(Writes::RuntimeState);
            return { Writes::Fields, mayDespecializeReceiver(function, block, index) || storeMayDespecializeObject(m_program, function, block, index, value.layout()) };
        case Opcode::GetById:
        case Opcode::InById:
            if (!isAccessToField(function, value) && !(value.isAtKnownSlot() && !Options::validateAOTTypedFacts()))
                return changes(Writes::Heap);
            if (fieldReadMayAllocate(m_program, function, value))
                return { Writes::RuntimeState, m_program.castsMayRetag };
            if (m_program.lazyClosureNames.get(value.string().index()))
                return changes(Writes::RuntimeState);
            return changes(Writes::Nothing);
        case Opcode::PutById:
            if (isInitializingStore(m_program, function, block, index))
                return changes(Writes::RuntimeState);
            if (!isAccessToField(function, value))
                return changes(Writes::Heap);
            if (isLocalAllocation(function, value.child(0)))
                return changes(Writes::RuntimeState);
            return { Writes::Fields, mayTransition(function, value) || mayDespecializeReceiver(function, block, index) };
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
            return changes(isEngineGlobal(value) ? Writes::RuntimeState : Writes::Heap);
        case Opcode::GetByVal:
            switch (keyedAccessOf(m_program, function, value)) {
            case KeyedAccess::Array:
                return elementReadEffects(value);
            case KeyedAccess::TypedArray:
                return changes(Writes::Nothing);
            case KeyedAccess::String:
                return changes(Writes::RuntimeState);
            case KeyedAccess::Generic:
                return changes(Writes::Heap);
            }
            break;
        case Opcode::GetMyArgumentByVal:
            return changes(typeOf(function, value.child(0)).isNumber() ? Writes::Nothing : Writes::Heap);
        case Opcode::PutByVal:
            return changes(keyedAccessOf(m_program, function, value) == KeyedAccess::TypedArray ? Writes::TypedArrayProperties : Writes::Heap);
        case Opcode::IteratorValue:
            if (keyedAccessOf(m_program, function, value) == KeyedAccess::Array)
                return elementReadEffects(value);
            return changes(keyedAccessOf(m_program, function, value) == KeyedAccess::Generic ? Writes::Heap : Writes::Nothing);
        case Opcode::IteratorOpen:
            if (keyedAccessOf(m_program, function, value) == KeyedAccess::TypedArray)
                return changes(Writes::RuntimeState);
            [[fallthrough]];
        case Opcode::IteratorHasNext:
        case Opcode::IteratorClose:
            return changes(keyedAccessOf(m_program, function, value) == KeyedAccess::Generic ? Writes::Heap : Writes::Nothing);
        case Opcode::Cast:
            return { writesOfCast(m_program, function, value), castWritesShapeWords(m_program, function, value) };
        case Opcode::ToString:
            return changes(typeOf(function, value.child(0)).hasObjects() ? Writes::Heap : Writes::RuntimeState);
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div:
        case Opcode::Mod:
        case Opcode::Pow:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::LShift:
        case Opcode::RShift:
        case Opcode::URShift:
        case Opcode::Negate:
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
        case Opcode::BitNot:
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
        case Opcode::Eq:
            return changes(writesOfArithmetic(typeOf(function, value.child(0)), value.child(1) ? &typeOf(function, value.child(1)) : nullptr));
        case Opcode::IsInstanceOfClass:
            return changes(mayWalkPrototypeChain(m_program, function, value) ? Writes::Heap : Writes::Nothing);
        case Opcode::CallBuiltin: {
            Effects effects = changes(writesOfBuiltinCall(m_program, function, value));
            if (value.type && writesOfElementRead(m_program, m_program.at(value.type)) != Writes::Nothing)
                effects.add({ Writes::RuntimeState, m_program.castsMayRetag });
            return effects;
        }
        case Opcode::PutGlobal:
        case Opcode::DeleteById:
        case Opcode::InByVal:
        case Opcode::DeleteByVal:
        case Opcode::InstanceOf:
        case Opcode::ToPropertyKey:
        case Opcode::Call:
        case Opcode::Construct:
        case Opcode::CallMethod:
        case Opcode::CallForwardVarargs:
        case Opcode::ConstructSuper:
        case Opcode::ConstructSuperForwardVarargs:
        case Opcode::Operation:
            return changes(Writes::Heap);
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Program& m_program;
    bool m_reportsFieldStores;
    Vector<Effects> m_effects;
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_isOnlyInFields;
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_storeMayTransition;
};

} // anonymous namespace

Writes writesOf(const Program& program, const Function& function, const Block& block, size_t index)
{
    return EffectSummaries(const_cast<Program&>(program)).writesOf(function, block, index);
}

static void reportStoresConsultingPrototypeChain(Program& program)
{
    EffectSummaries(program, false).run();
    auto summaries = program.functions.map([](auto& function) {
        return std::pair { function.writes, function.writesOnReturn };
    });
    EffectSummaries(program).run();
    unsigned numberOfStores = 0;
    unsigned affectedFunctionCount = 0;
    unsigned raisedCount = 0;
    unsigned raisedOnReturnCount = 0;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = std::as_const(program).functions[i];
        unsigned before = numberOfStores;
        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = function.at(block.values[index]);
                if (value.opcode != Opcode::PutField || !isObjectLayout(program.at(value.layout()).kind))
                    continue;
                StringID name = program.at(value.layout()).fields[value.field()].name;
                numberOfStores += storeMayConsultPrototypeChain(program, function, block, index, program.typeOf(function, value.child(0)).singleLayout(), name) || storeMayConsultPrototypeChain(program, function, block, index, value.layout(), name);
            }
        }
        affectedFunctionCount += numberOfStores != before;
        raisedCount += function.writes != summaries[i].first;
        raisedOnReturnCount += function.writesOnReturn != summaries[i].second;
    }
    dataLogLn("AOT: ", numberOfStores, " field stores may consult the prototype chain, in ", affectedFunctionCount, " functions; for that reason alone ", raisedCount, " functions are summarized as writing the heap, ", raisedOnReturnCount, " where they return");
}

void computeEffectSummaries(Program& program)
{
    if (Options::verboseAOTCompilation() && !Options::auditAOTTypedAssertions()) [[unlikely]] {
        reportStoresConsultingPrototypeChain(program);
        return;
    }
    EffectSummaries(program).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
