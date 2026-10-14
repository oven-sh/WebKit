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

// What the back end is told of a call, so that it keeps in registers what the call cannot change.
//
// A type is a fact. So an access to a field of a typed receiver is one load or one store of one slot and runs no code, so is an access
// to an element of what is typed as an array, and a test that fails does not come back: it changes nothing anyone gets to see. What is
// left to change everything is what may run code that is not known. Of a function of the program that is called by name, what it
// changes is summed up from what it does.
//
// Shape words are a matter of their own. Only `as` and `!` write one, and so does whatever may come to run such a cast.
//
// The lowering holds every store and every call it emits against what is found here, and does not go on if it is more.
//
// What is expected of a value is no fact, and is nothing to go by here. An operation on operands of which something is expected is summed up
// as the general way to carry it out, which is one of the two ways it is lowered. The other changes no more than that.

// Allocations.

FunctionID constructedBy(const Program& program, const Function& function, ValueID object)
{
    return (program.allocations.isEmpty() ? function.allocations : program.allocations)[function.at(object).allocation()].constructedBy;
}

// The field, of the layout the object was made with, that the value stores to. `isForFunction`: isBornUnderAnyPrototype().
static std::optional<uint32_t> fieldStoredTo(const Program& program, const Function& function, ValueID object, const Value& store, bool isForFunction)
{
    if (store.child(0) != object || store.child(1) == object)
        return std::nullopt;
    LayoutID made = function.at(object).layout();
    switch (store.opcode) {
    case Opcode::PutField:
        if (store.layout() == made)
            return store.field();
        return program.at(made).fieldNamed(program.at(store.layout()).fields[store.field()].name);
    case Opcode::PutById:
        // It is [[Set]], which asks the prototype chain: see mayInherit(). An object of a class that is made here has the prototype of the class, and a
        // literal Object.prototype.
        if ((program.prototypePropertyNames.get(store.string().index()) && inheritsByItsLayout(program, made, store.string())) || (program.at(made).kind == LayoutKind::Record && program.at(made).mayHaveOtherPrototype && isForFunction))
            return std::nullopt;
        if (program.prototypePropertyNames.get(store.string().index()) && program.at(made).kind != LayoutKind::Record)
            return std::nullopt;
        return program.at(made).fieldNamed(store.string());
    default:
        return std::nullopt;
    }
}

bool isInitializingStore(const Program& program, const Function& function, const Block& block, size_t index)
{
    ValueID object = function.at(block.values[index]).child(0);
    if (!object || function.at(object).opcode != Opcode::NewObjectWithShape)
        return false;
    bool isForFunction = isBornUnderAnyPrototype(program, function, object);
    for (size_t i = index + 1; i--;) {
        if (block.values[i] == object)
            return true;
        if (!fieldStoredTo(program, function, object, function.at(block.values[i]), isForFunction))
            return false;
    }
    return false;
}

bool isLocalAllocation(const Function& function, ValueID id)
{
    // A cast leaves an object of the program where it is, whatever it makes of it.
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

void setBornFields(const Layout& layout, Allocation& allocation)
{
    for (unsigned field = 0; field < layout.fields.size(); ++field) {
        if (!(layout.fields[field].flags & fieldIsAbsentAtBirth))
            allocation.order.append(static_cast<uint16_t>(field));
    }
    allocation.born = static_cast<uint16_t>(allocation.order.size());
}

void assignAllocations(Program& program)
{
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (size_t at = 0; at < block.values.size(); ++at) {
                ValueID object = block.values[at];
                if (function.at(object).opcode != Opcode::NewObjectWithShape)
                    continue;
                Allocation allocation;
                allocation.layout = function.at(object).layout();
                auto& layout = program.at(allocation.layout);
                if (layout.kind == LayoutKind::Record) {
                    for (size_t next = at + 1; next < block.values.size(); ++next) {
                        auto field = fieldStoredTo(program, function, object, function.at(block.values[next]), false);
                        if (!field)
                            break;
                        if (!allocation.order.contains(*field))
                            allocation.order.append(static_cast<uint16_t>(*field));
                    }
                    allocation.born = static_cast<uint16_t>(allocation.order.size());
                } else
                    setBornFields(layout, allocation);
                // As it is read, the second immediate names the property that holds the child. From here on the child is there if and only
                // if the allocation has a discriminant.
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
    });
}

// Casts.

// Whether a cast can convert an object. A container is tested for its kind only: its elements are converted when they are read.
static bool coerces(const Type& from, const Type& to)
{
    return from.hasObjects() && !to.views.isEmpty();
}

void findViewsRetaggedTo(Program& program)
{
    // As Coercion in AOTTypedObject.cpp goes about it.
    unsigned numberOfLayouts = program.layouts.size();
    BitVector isSpecialization(numberOfLayouts);
    for (auto& layout : program.layouts) {
        for (LayoutID specialization : layout.specializations)
            isSpecialization.set(specialization.index());
    }
    auto& retaggedTo = program.viewsRetaggedTo;
    retaggedTo.clearAll();
    retaggedTo.ensureSize(numberOfLayouts);
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        auto& layout = program.layouts[i];
        if (layout.isAllocated) {
            if (isSpecialization.get(i))
                retaggedTo.set(i);
            continue;
        }
        for (LayoutID member : layout.members) {
            if (isSpecialization.get(member.index())) {
                retaggedTo.set(i);
                break;
            }
        }
    }
    auto& leftAlone = program.viewsLeftAlone;
    leftAlone.clearAll();
    if (Options::auditAOTTypedAssertions()) [[unlikely]]
        return;
    leftAlone.ensureSize(numberOfLayouts);
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (!retaggedTo.get(i) && !isSpecialization.get(i) && !program.layouts[i].isOpenedForLog)
            leftAlone.set(i);
    }
    // The largest set that holds: a view goes out of it where something can be made of what a field holds: a plain object is copied where the type of the field admits none as it is, and whatever can be made of an object
    // through one of the views of that type can be made of it here.
    BitVector hasOnlyFieldsLeftAlone(numberOfLayouts);
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (unsigned i = 0; i < numberOfLayouts; ++i) {
            auto& layout = program.layouts[i];
            hasOnlyFieldsLeftAlone.set(i, layout.kind != LayoutKind::Record || !layout.fields.containsIf([&](auto& field) {
                if (!field.type || program.at(field.type).views.isEmpty())
                    return false;
                auto& type = program.at(field.type);
                return !type.admitsForeignObjects(program) || type.views.containsIf([&](LayoutID view) {
                    return !leftAlone.get(view.index());
                });
            }));
        }
        for (unsigned i = 0; i < numberOfLayouts; ++i) {
            if (!leftAlone.get(i))
                continue;
            bool holds = hasOnlyFieldsLeftAlone.get(i) && std::ranges::all_of(program.layouts[i].members, [&](LayoutID member) {
                return hasOnlyFieldsLeftAlone.get(member.index());
            });
            if (!holds) {
                leftAlone.clear(i);
                hasChanged = true;
            }
        }
    }
}

bool coercionMayChangeObjects(const Program& program, const Type& from, const Type& to, bool isOfFreshData)
{
    if (!coerces(from, to))
        return false;
    // Plain data that nobody else can hold is copied. Any other object that is not of the program stays what it is.
    if (isOfFreshData && (from.anyObject || from.admitsForeignObjects(program)))
        return true;
    return narrowingMayRetag(program, to);
}

// A shape transition in place, or a copy of plain data, field by field. A plain object that a type admitting foreign objects keeps gets
// its properties replaced. Plain data that has the room becomes its copy in its own cell (State::become()): another header, other slots:
// whoever may do that is said to write shape words, see castWritesShapeWords(), whose range has both: nothing that compiled code has
// loaded from such an object ahead of the cast serves behind it.
static Writes writesOfMakingObjects(const Type& from, const Type& to)
{
    return coerces(from, to) ? Writes::RuntimeState : Writes::Nothing;
}

Writes writesOfElementRead(const Program& program, const Type& type)
{
    for (LayoutID view : type.views) {
        if (program.at(view).kind == LayoutKind::Record && !program.at(view).admitsForeignObjects)
            return Writes::RuntimeState;
    }
    return Writes::Nothing;
}

bool claimMayMakeObjects(const Program& program, const Function& function, const Value& value)
{
    if ((value.castReason() != CastReason::Claim && value.castReason() != CastReason::Flow) || value.string() || value.accessedName() || Options::auditAOTTypedAssertions())
        return false;
    auto& from = program.typeOf(function, value.child(0));
    return (from.anyObject || from.admitsForeignObjects(program)) && writesOfElementRead(program, program.at(value.type)) != Writes::Nothing;
}

bool fieldReadMayMakeObjects(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::GetById || !value.type || Options::auditAOTTypedAssertions() || writesOfElementRead(program, program.at(value.type)) == Writes::Nothing)
        return false;
    auto access = fieldOf(program, program.typeOf(function, value.child(0)), value.string());
    if (!access)
        return false;
    TypeID held = program.at(access->layout).fields[access->field].type;
    return !held || program.at(held).anyObject;
}

Writes writesOfCast(const Program& program, const Function& function, const Value& value)
{
    auto& from = program.typeOf(function, value.child(0));
    CastReason reason = value.castReason();
    // What the cast holds its operand to is not its type then.
    if (Options::auditAOTTypedAssertions() && (reason == CastReason::As || reason == CastReason::Flow || reason == CastReason::Coerce)) [[unlikely]]
        return Writes::Heap;
    switch (reason) {
    case CastReason::Upcast:
        return Writes::Pure;
    case CastReason::Claim:
        return claimMayMakeObjects(program, function, value) ? Writes::RuntimeState : Writes::Nothing;
    case CastReason::Store:
    case CastReason::NonNull:
        return Writes::Nothing;
    case CastReason::Flow:
        return narrowingMayRetag(program, program.at(value.type)) || claimMayMakeObjects(program, function, value) ? Writes::RuntimeState : Writes::Nothing;
    case CastReason::As:
        return writesOfMakingObjects(from, program.at(value.type));
    case CastReason::Coerce:
        break;
    case CastReason::CoerceFreshObject:
    case CastReason::CoerceFreshGraph:
    case CastReason::AsFreshObject:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto& to = program.at(value.coercedType());
    Writes writes = Writes::Nothing;
    // ToNumber, ToString and ToPrimitive of an object run its methods. Of anything else they make a string at most.
    if (!from.isWithin(program.at(value.type), program))
        writes = from.hasObjects() && !to.hasObjects() ? Writes::Heap : Writes::RuntimeState;
    if (coercionMayChangeObjects(program, from, to, value.coercesFreshData()))
        writes = std::max(writes, writesOfMakingObjects(from, to));
    return writes;
}

bool narrowingMayRetag(const Program& program, const Type& type)
{
    for (LayoutID view : type.views) {
        if (program.viewsRetaggedTo.get(view.index()))
            return true;
    }
    return false;
}

bool castWritesShapeWords(const Program& program, const Function& function, const Value& value)
{
    if (Options::auditAOTTypedAssertions()) [[unlikely]]
        return writesOfCast(program, function, value) == Writes::Heap;
    if (value.castReason() == CastReason::Flow)
        return narrowingMayRetag(program, program.at(value.type)) || (program.castsMayRetag && claimMayMakeObjects(program, function, value));
    if (!program.castsMayRetag)
        return false;
    if (value.castReason() == CastReason::Claim)
        return claimMayMakeObjects(program, function, value);
    auto& from = program.typeOf(function, value.child(0));
    // Also of an object of the program that a plain object leads to.
    if (value.castReason() == CastReason::As)
        return coerces(from, program.at(value.type));
    // What runs code of the program may come to a cast.
    return value.castReason() == CastReason::Coerce && (coercionMayChangeObjects(program, from, program.at(value.coercedType()), value.coercesFreshData()) || writesOfCast(program, function, value) == Writes::Heap);
}

// Built-ins.

// Making a number, a string or a key of an object runs its methods.
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

// A typed array or a DataView. What a method writes to one is bytes of its buffer: only a method of the buffer changes how many there are or where.
static bool isViewOnBuffer(const Type* receiver)
{
    auto* engineClass = receiver ? receiver->onlyEngineClass() : nullptr;
    return engineClass && (engineClass->typedArrayType != NotTypedArray || engineClass->type == DataViewType);
}

// As the language sees it. The receiver is of the class the entry is listed under: that is a fact, and the entry is what is carried out.
static Writes writesOf(const Builtin& builtin, const Type* receiver, std::span<const Type> arguments)
{
    if (builtin.effects & (Effect::CallsUserCode | Effect::WritesArguments))
        return Writes::Heap;
    if (builtin.effects & Effect::WritesReceiver && !isViewOnBuffer(receiver))
        return Writes::Heap;
    if ((builtin.effects & Effect::ReadsArguments && hasObjects(arguments)) || convertsObjects(builtin, arguments))
        return Writes::Heap;
    // A method of every object, called on any: what looking into that comes down to is not known.
    if (builtin.effects & Effect::ReadsReceiver && receiver && receiver->hasObjects() && !receiver->onlyEngineClass())
        return Writes::Heap;
    if (builtin.effects & Effect::WritesReceiver)
        return Writes::TypedArrayProperties;
    if (builtin.effects & (Effect::WritesRuntimeState | Effect::Allocates))
        return Writes::RuntimeState;
    // A property of `undefined` or `null` is not read: that throws.
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
    const Type* receiver = builtin.takesReceiver() ? &program.typeOf(function, value.child(0)) : nullptr;
    Vector<Type, 4> arguments;
    for (ValueID argument : value.arguments)
        arguments.append(program.typeOf(function, argument));
    // The function of the engine is called, unless there is code in place.
    Writes ofImplementation = Writes::RuntimeState;
    if (builtin.implementation.kind == BuiltinImplementation::Inline) {
        if (auto inPlace = writesOfInlineBuiltin(builtin.implementation.inlined, arguments.span()))
            ofImplementation = *inPlace;
    }
    return std::max(writesOf(builtin, receiver, arguments.span()), ofImplementation);
}

// Operators.

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
    explicit EffectSummaries(Program& program, bool asksStoresToFields = true)
        : m_program(program)
        , m_asksStoresToFields(asksStoresToFields)
    {
    }

    // From the functions that call nobody upwards: a function changes what it changes itself and what those it calls by name change.
    //
    // Each function is summed up twice: all a call of it may change, and what a call that comes back may have changed. What a function
    // does on its way to throwing is nothing to a caller that does not catch: it does not go on either.
    void run()
    {
        findWhatCastsMayDo();
        // Every function is left with the most a function may do.
        if (Options::auditAOTTypedAssertions()) [[unlikely]]
            return;
        unsigned numberOfFunctions = m_program.functions.size();
        m_effects.fill({ }, 2 * numberOfFunctions);
        m_dependents.fill({ }, 2 * numberOfFunctions);
        Vector<Vector<std::pair<unsigned, unsigned>>> partsOf(numberOfFunctions);
        forEachIndexInParallel(numberOfFunctions, [&](unsigned i) {
            FunctionID id(i);
            EffectSummaries(m_program, m_asksStoresToFields).sumUpBody(id, m_effects[ofAnyCall(id)], m_effects[ofCallThatReturns(id)], partsOf[i]);
        });
        for (auto& parts : partsOf) {
            for (auto [dependent, summary] : parts)
                dependsOn(dependent, summary);
        }

        Vector<unsigned> worklist;
        for (unsigned summary = 0; summary < m_effects.size(); ++summary) {
            if (m_effects[summary] != Effects { })
                worklist.append(summary);
        }
        while (!worklist.isEmpty()) {
            unsigned summary = worklist.takeLast();
            for (unsigned dependent : m_dependents[summary]) {
                if (m_effects[dependent].add(m_effects[summary]))
                    worklist.append(dependent);
            }
        }

        for (unsigned i = 0; i < numberOfFunctions; ++i) {
            FunctionID id(i);
            auto& function = m_program.at(id);
            function.writes = m_effects[ofAnyCall(id)].writes;
            function.writesShapeWords = m_effects[ofAnyCall(id)].writesShapeWords;
            function.writesIfItReturns = m_effects[ofCallThatReturns(id)].writes;
            function.writesShapeWordsIfItReturns = m_effects[ofCallThatReturns(id)].writesShapeWords;
        }
    }

    Writes writesOf(const Function& function, const Block& block, size_t index) { return effectsOf(function, block, index).writes; }

private:
    struct Effects {
        Writes writes { Writes::Nothing };
        bool writesShapeWords { false };

        // Yields whether that is news.
        bool add(Effects other)
        {
            if (writes >= other.writes && (writesShapeWords || !other.writesShapeWords))
                return false;
            writes = std::max(writes, other.writes);
            writesShapeWords |= other.writesShapeWords;
            return true;
        }

        friend bool operator==(const Effects&, const Effects&) = default;
    };

    static unsigned ofAnyCall(FunctionID id) { return 2 * id.index(); }
    static unsigned ofCallThatReturns(FunctionID id) { return 2 * id.index() + 1; }

    // Of anything but a cast and a call by name. The lowering does not tell what changes the objects of the engine from what runs code
    // that is not known, which may come to a cast.
    Effects changes(Writes writes) const { return { writes, writes == Writes::Heap && m_program.castsMayRetag }; }

    const Type& typeOf(const Function& function, ValueID id) const { return m_program.typeOf(function, id); }

    void findWhatCastsMayDo()
    {
        m_program.castsMayRetag = false;
        for (auto& layout : m_program.layouts)
            m_program.castsMayRetag |= !layout.specializations.isEmpty() || layout.mayGiveUp || layout.kind == LayoutKind::Record;
    }

    // The slow path converts a plain-data element as an `As` cast would, stores it back and rejoins.
    Effects ofElementRead(const Value& value) const
    {
        if (value.readsElementKnownByTheStores() && !Options::validateAOTTypedFacts() && !Options::auditAOTTypedAssertions())
            return { Writes::Nothing, false };
        Writes writes = writesOfElementRead(m_program, m_program.at(value.type));
        return { writes, writes != Writes::Nothing && m_program.castsMayRetag };
    }

    // The blocks from which there is a way to a Return, also by way of a handler.
    BitVector blocksThatMayReturn(const Function& function) const
    {
        unsigned numberOfBlocks = function.blocks.size();
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        BitVector result(numberOfBlocks);
        Vector<unsigned> worklist;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            // There is no value of type `never`: what is to yield one throws, and the block gets no further, but for a handler.
            bool comesToItsEnd = true;
            for (ValueID id : function.blocks[b].values)
                comesToItsEnd &= function.at(id).type != m_program.types.never;
            auto& terminator = function.blocks[b].terminator;
            if (!comesToItsEnd) {
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

    void dependsOn(unsigned dependent, unsigned summary)
    {
        auto& dependents = m_dependents[summary];
        if (dependents.isEmpty() || dependents.last() != dependent)
            dependents.append(dependent);
    }

    // Without what the functions it calls by name change: what is summed up for those is noted to be part of it.
    void sumUpBody(FunctionID id, Effects& ofAny, Effects& ofReturning, Vector<std::pair<unsigned, unsigned>>& parts)
    {
        auto& function = m_program.at(id);
        Effects most = changes(Writes::Heap);
        if (function.blocks.isEmpty()) {
            ofAny = most;
            ofReturning = most;
            return;
        }
        BitVector mayReturn = blocksThatMayReturn(function);
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = function.at(block.values[index]);
                if (value.opcode != Opcode::CallDirect) {
                    Effects effects = effectsOf(function, block, index);
                    ofAny.add(effects);
                    if (mayReturn.get(b))
                        ofReturning.add(effects);
                    continue;
                }
                FunctionID callee = value.function();
                parts.append({ ofAnyCall(id), ofAnyCall(callee) });
                if (block.terminator.kind != TerminatorKind::Invoke) {
                    if (mayReturn.get(b))
                        parts.append({ ofCallThatReturns(id), ofCallThatReturns(callee) });
                    continue;
                }
                // A handler watches. Where that leads to a Return, this function comes back from a call that does not.
                if (mayReturn.get(block.terminator.targets[0].block.index()))
                    parts.append({ ofCallThatReturns(id), ofCallThatReturns(callee) });
                if (mayReturn.get(block.terminator.targets[1].block.index()))
                    parts.append({ ofCallThatReturns(id), ofAnyCall(callee) });
            }
            if (ofReturning == most)
                return;
        }
    }

    // Whether the access by name comes down to the field and nothing else, whichever member of the view the receiver is of.
    bool isAccessToField(const Function& function, const Value& value)
    {
        auto access = fieldOf(m_program, typeOf(function, value.child(0)), value.string());
        if (!access)
            return false;
        return m_onlyFieldsSupply.ensure(static_cast<uint64_t>(access->layout.index()) << 32 | access->field, [&] {
            return onlyFieldsSupply(m_program, access->layout, value.string());
        }).iterator->value;
    }

    // Of a store for which isAccessToField() holds.
    bool mayTransition(const Function& function, const Value& value)
    {
        auto access = fieldOf(m_program, typeOf(function, value.child(0)), value.string());
        return m_storeMayTransition.ensure(static_cast<uint64_t>(access->layout.index()) << 32 | access->field, [&] {
            return storeMayTransition(m_program, access->layout, value.string());
        }).iterator->value;
    }

    bool mayMakeReceiverGiveUp(const Function& function, const Block& block, size_t index) const
    {
        return storeMayMakeObjectGiveUp(m_program, function, block, index, typeOf(function, function.at(block.values[index]).child(0)).onlyView());
    }

    bool isGlobalOfEngine(const Value& value) const
    {
        BuiltinName name = m_program.builtinName(value.string());
        return Builtins::singleton().isGlobal(name) && !m_program.isExtended(name);
    }

    Effects effectsOf(const Function& function, const Block& block, size_t index)
    {
        auto& value = function.at(block.values[index]);
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
        case Opcode::IsClassObjectAsDeclared:
        case Opcode::IsClosureOf:
        case Opcode::Results:
        case Opcode::Unspecified:
        case Opcode::ResultAt:
        case Opcode::TypeOfIs:
        case Opcode::LoopHint:
        case Opcode::AssertNotAllocated:
            return changes(Writes::Nothing);
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        // The function is looked up on first use.
        case Opcode::IsBuiltinFunction:
        case Opcode::BuiltinFunction:
        case Opcode::NewObjectWithShape:
        case Opcode::MaterializeEnvironment:
        case Opcode::NewClosure:
        // Nothing else looks at the variable it keeps the closure in.
        case Opcode::NewClosureOnce:
        case Opcode::NewArray:
        case Opcode::NewObject:
        case Opcode::CreateRest:
        // The `prototype` of a function is made when it is first asked for.
        case Opcode::CreateThis:
        case Opcode::StrictEq:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::Strcat:
        case Opcode::CatchValue:
            return changes(Writes::RuntimeState);
        // Where the host has no record of the module it is required: its code runs.
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
        // Nobody who was running before can have looked into what was made since.
        case Opcode::PutField:
            if (auto& layout = m_program.at(value.layout()); m_asksStoresToFields && isObjectLayout(layout.kind)) {
                auto mayAsk = [&](LayoutID view) {
                    return storeMayAskPrototypeChain(m_program, function, block, index, view, layout.fields[value.field()].name);
                };
                if (mayAsk(typeOf(function, value.child(0)).onlyView()) || mayAsk(value.layout()))
                    return changes(Writes::Heap);
            }
            if (isLocalAllocation(function, value.child(0)))
                return changes(Writes::RuntimeState);
            return { Writes::Fields, mayMakeReceiverGiveUp(function, block, index) || storeMayMakeObjectGiveUp(m_program, function, block, index, value.layout()) };
        case Opcode::GetById:
        case Opcode::InById:
            if (!isAccessToField(function, value) && !(value.isAtKnownPlace() && !Options::validateAOTTypedFacts()))
                return changes(Writes::Heap);
            if (fieldReadMayMakeObjects(m_program, function, value))
                return { Writes::RuntimeState, m_program.castsMayRetag };
            return changes(Writes::Nothing);
        case Opcode::PutById:
            if (isInitializingStore(m_program, function, block, index))
                return changes(Writes::RuntimeState);
            if (!isAccessToField(function, value))
                return changes(Writes::Heap);
            if (isLocalAllocation(function, value.child(0)))
                return changes(Writes::RuntimeState);
            return { Writes::Fields, mayTransition(function, value) || mayMakeReceiverGiveUp(function, block, index) };
        // What the engine has under the name from the start, and the program leaves there, is not looked up.
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
            return changes(isGlobalOfEngine(value) ? Writes::RuntimeState : Writes::Heap);
        case Opcode::GetByVal:
            switch (keyedAccessOf(m_program, function, value)) {
            case KeyedAccess::Array:
                return ofElementRead(value);
            case KeyedAccess::TypedArray:
                return changes(Writes::Nothing);
            // Looking at the characters flattens a rope.
            case KeyedAccess::String:
                return changes(Writes::RuntimeState);
            case KeyedAccess::Generic:
                return changes(Writes::Heap);
            }
            break;
        case Opcode::GetMyArgumentByVal:
            return changes(typeOf(function, value.child(0)).isNumber() ? Writes::Nothing : Writes::Heap);
        // What is stored to a typed array is a number already. It stays as long as it is, with its elements where they are.
        case Opcode::PutByVal:
            return changes(keyedAccessOf(m_program, function, value) == KeyedAccess::TypedArray ? Writes::TypedArrayProperties : Writes::Heap);
        case Opcode::IteratorValue:
            if (keyedAccessOf(m_program, function, value) == KeyedAccess::Array)
                return ofElementRead(value);
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
        // Only an object has a say in what string it becomes.
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
        // Of an object of the program the shape says it. The prototype chain of any other is walked, which may have a proxy on it.
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
    bool m_asksStoresToFields;
    // Two for each function, see ofAnyCall() and ofCallThatReturns().
    Vector<Effects> m_effects;
    // By summary: those it is part of.
    Vector<Vector<unsigned, 2>> m_dependents;
    // By view and field.
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_onlyFieldsSupply;
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_storeMayTransition;
};

} // anonymous namespace

Writes writesOf(const Program& program, const Function& function, const Block& block, size_t index)
{
    // Only run() changes the program.
    return EffectSummaries(const_cast<Program&>(program)).writesOf(function, block, index);
}

static void reportStoresThatMayAskPrototypeChain(Program& program)
{
    EffectSummaries(program, false).run();
    auto summaries = program.functions.map([](auto& function) {
        return std::pair { function.writes, function.writesIfItReturns };
    });
    EffectSummaries(program).run();
    unsigned numberOfStores = 0;
    unsigned numberOfFunctionsWithThem = 0;
    unsigned numberThatRise = 0;
    unsigned numberThatRiseIfTheyReturn = 0;
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        auto& function = program.functions[i];
        unsigned before = numberOfStores;
        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = function.at(block.values[index]);
                if (value.opcode != Opcode::PutField || !isObjectLayout(program.at(value.layout()).kind))
                    continue;
                StringID name = program.at(value.layout()).fields[value.field()].name;
                numberOfStores += storeMayAskPrototypeChain(program, function, block, index, program.typeOf(function, value.child(0)).onlyView(), name) || storeMayAskPrototypeChain(program, function, block, index, value.layout(), name);
            }
        }
        numberOfFunctionsWithThem += numberOfStores != before;
        numberThatRise += function.writes != summaries[i].first;
        numberThatRiseIfTheyReturn += function.writesIfItReturns != summaries[i].second;
    }
    dataLogLn("AOT: ", numberOfStores, " stores to fields may ask the prototype chain, in ", numberOfFunctionsWithThem, " functions; for that alone ", numberThatRise, " functions are summed up as changing the heap, ", numberThatRiseIfTheyReturn, " where they return");
}

void computeEffectSummaries(Program& program)
{
    if (Options::verboseAOTCompilation() && !Options::auditAOTTypedAssertions()) [[unlikely]] {
        reportStoresThatMayAskPrototypeChain(program);
        return;
    }
    EffectSummaries(program).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
