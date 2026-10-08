/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

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

// The field, of the layout the object was made with, that the value stores to.
static std::optional<uint32_t> fieldStoredTo(const Program& program, const Function& function, ValueID object, const Value& store)
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
    for (size_t i = index + 1; i--;) {
        if (block.values[i] == object)
            return true;
        if (!fieldStoredTo(program, function, object, function.at(block.values[i])))
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

void assignAllocations(Program& program)
{
    for (auto& function : program.functions) {
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
                        auto field = fieldStoredTo(program, function, object, function.at(block.values[next]));
                        if (!field)
                            break;
                        if (!allocation.order.contains(*field))
                            allocation.order.append(static_cast<uint16_t>(*field));
                    }
                } else {
                    for (unsigned field = 0; field < layout.fields.size(); ++field)
                        allocation.order.append(static_cast<uint16_t>(field));
                }
                allocation.born = static_cast<uint16_t>(allocation.order.size());
                function.at(object).immediates[1] = function.allocationOf(WTF::move(allocation));
            }
        }
    }
}

// Casts.

// Whether holding an array to the type may replace elements: of the array by copies of plain objects, or of arrays within it.
static bool coercesElements(const Program& program, const Type& type)
{
    if (!type.elements)
        return false;
    auto& elements = program.at(type.elements);
    return !elements.views.isEmpty() || coercesElements(program, elements);
}

// Whether anything can be made of an object that is not of the type.
static bool coerces(const Program& program, const Type& from, const Type& to)
{
    return from.hasObjects() && (!to.views.isEmpty() || coercesElements(program, to));
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
}

bool coercionMayChangeObjects(const Program& program, const Type& from, const Type& to)
{
    // Then every object that is not of the type goes to the run time, which says so.
    if (Options::reportAOTTypedCoercions() && from.hasObjects() && to.hasObjects()) [[unlikely]]
        return true;
    if (!coerces(program, from, to))
        return false;
    // Plain data is copied, and an array that nothing is known of is gone through.
    if (from.anyObject || from.admitsForeignObjects(program))
        return true;
    return narrowingMayRetag(program, to);
}

static Writes writesOfMakingObjects(const Program&, const Type& from, const Type& to);

Writes writesOfCast(const Program& program, const Function& function, const Value& value)
{
    auto& from = program.typeOf(function, value.child(0));
    switch (value.castReason()) {
    case CastReason::Upcast:
        return Writes::Pure;
    case CastReason::Store:
    case CastReason::Claim:
    case CastReason::NonNull:
        return Writes::Nothing;
    case CastReason::Flow:
        return narrowingMayRetag(program, program.at(value.type)) ? Writes::RuntimeState : Writes::Nothing;
    case CastReason::As:
        return writesOfMakingObjects(program, from, program.at(value.type));
    case CastReason::Coerce:
        break;
    }
    auto& to = program.at(value.coercedType());
    Writes writes = Writes::Nothing;
    // ToNumber, ToString and ToPrimitive of an object run its methods. Of anything else they make a string at most.
    if (!from.isWithin(program.at(value.type), program))
        writes = from.hasObjects() && !to.hasObjects() ? Writes::Heap : Writes::RuntimeState;
    if (coercionMayChangeObjects(program, from, to))
        writes = std::max(writes, writesOfMakingObjects(program, from, to));
    return writes;
}

static Writes writesOfMakingObjects(const Program& program, const Type& from, const Type& to)
{
    if (!coerces(program, from, to))
        return Writes::Nothing;
    // An object of the program is made one of another layout where it is, or refused. Only a plain object is copied, field by field,
    // and only an array is gone through. A plain object that a type which admits foreign objects keeps gets other values for its
    // properties: those compiled code reads through stubs only, which take nothing for unchanged.
    bool mayBeAnyObject = from.anyObject || from.admitsForeignObjects(program);
    if ((mayBeAnyObject || from.engineClasses.contains(Builtins::singleton().arrayClass().index)) && coercesElements(program, to))
        return Writes::Heap;
    // What an object of the program has as extra properties is moved into the fields that its new layout has more, and coerced to them.
    if (mayBeAnyObject || program.castsMayRetag) {
        for (LayoutID view : to.views) {
            if (program.layoutsCoercedThroughArrays.get(view.index()))
                return Writes::Heap;
        }
    }
    return Writes::RuntimeState;
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
    if (value.castReason() == CastReason::Flow)
        return narrowingMayRetag(program, program.at(value.type));
    if (!program.castsMayRetag)
        return false;
    auto& from = program.typeOf(function, value.child(0));
    // Also of an object of the program that a plain object or an array leads to.
    if (value.castReason() == CastReason::As)
        return coerces(program, from, program.at(value.type));
    // What runs code of the program may come to a cast.
    return value.castReason() == CastReason::Coerce && (coercionMayChangeObjects(program, from, program.at(value.coercedType())) || writesOfCast(program, function, value) == Writes::Heap);
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
    if (builtin.effects & (Effect::RunsUnknownCode | Effect::WritesArguments))
        return Writes::Heap;
    if (builtin.effects & Effect::WritesThis && !isViewOnBuffer(receiver))
        return Writes::Heap;
    if ((builtin.effects & Effect::ReadsArguments && hasObjects(arguments)) || convertsObjects(builtin, arguments))
        return Writes::Heap;
    // A method of every object, called on any: what looking into that comes down to is not known.
    if (builtin.effects & Effect::ReadsThis && receiver && receiver->hasObjects() && !receiver->onlyEngineClass())
        return Writes::Heap;
    if (builtin.effects & Effect::WritesThis)
        return Writes::TypedArrayProperties;
    if (builtin.effects & (Effect::WritesWorld | Effect::Allocates))
        return Writes::RuntimeState;
    // A property of `undefined` or `null` is not read: that throws.
    if (builtin.effects || (receiver && receiver->mayBeUndefinedOrNull()))
        return Writes::Nothing;
    return Writes::Pure;
}

Writes writesOfBuiltinCall(const Program& program, const Function& function, const Value& value)
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
    explicit EffectSummaries(Program& program)
        : m_program(program)
    {
    }

    // From the functions that call nobody upwards: a function changes what it changes itself and what those it calls by name change.
    //
    // Each function is summed up twice: all a call of it may change, and what a call that comes back may have changed. What a function
    // does on its way to throwing is nothing to a caller that does not catch: it does not go on either.
    void run()
    {
        findWhatCastsMayDo();
        unsigned numberOfFunctions = m_program.functions.size();
        m_effects.fill({ }, 2 * numberOfFunctions);
        m_dependents.fill({ }, 2 * numberOfFunctions);
        for (unsigned i = 0; i < numberOfFunctions; ++i)
            sumUpBody(FunctionID(i));

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
        unsigned numberOfLayouts = m_program.layouts.size();
        auto& reached = m_program.layoutsCoercedThroughArrays;
        reached.clearAll();
        reached.ensureSize(numberOfLayouts);
        m_program.castsMayRetag = false;
        // By layout: those with a field for an object of it, which a plain object is turned into along with the one that holds it, and
        // the views it is a member of: a cast to a view may make a plain object one of a member.
        Vector<Vector<LayoutID, 2>> holders(numberOfLayouts);
        Vector<LayoutID> worklist;
        for (unsigned i = 0; i < numberOfLayouts; ++i) {
            auto& layout = m_program.layouts[i];
            m_program.castsMayRetag |= !layout.specializations.isEmpty();
            if (!isObjectLayout(layout.kind))
                continue;
            for (LayoutID member : layout.members)
                holders[member.index()].append(LayoutID(i));
            for (auto& field : layout.fields) {
                if (!field.type)
                    continue;
                auto& type = m_program.at(field.type);
                if (coercesElements(m_program, type) && !reached.set(i))
                    worklist.append(LayoutID(i));
                for (LayoutID view : type.views)
                    holders[view.index()].append(LayoutID(i));
            }
        }
        while (!worklist.isEmpty()) {
            for (LayoutID holder : holders[worklist.takeLast().index()]) {
                if (!reached.set(holder.index()))
                    worklist.append(holder);
            }
        }
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
    void sumUpBody(FunctionID id)
    {
        auto& function = m_program.at(id);
        auto& ofAny = m_effects[ofAnyCall(id)];
        auto& ofReturning = m_effects[ofCallThatReturns(id)];
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
                dependsOn(ofAnyCall(id), ofAnyCall(callee));
                if (block.terminator.kind != TerminatorKind::Invoke) {
                    if (mayReturn.get(b))
                        dependsOn(ofCallThatReturns(id), ofCallThatReturns(callee));
                    continue;
                }
                // A handler watches. Where that leads to a Return, this function comes back from a call that does not.
                if (mayReturn.get(block.terminator.targets[0].block.index()))
                    dependsOn(ofCallThatReturns(id), ofCallThatReturns(callee));
                if (mayReturn.get(block.terminator.targets[1].block.index()))
                    dependsOn(ofCallThatReturns(id), ofAnyCall(callee));
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
        case Opcode::GetScope:
        case Opcode::GetArgument:
        case Opcode::LogicalNot:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::IsClosureOf:
        case Opcode::LoopHint:
            return changes(Writes::Nothing);
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        case Opcode::NewObjectWithShape:
        case Opcode::NewClosure:
        case Opcode::NewArray:
        case Opcode::NewObject:
        case Opcode::CreateRest:
        case Opcode::StrictEq:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::Strcat:
        case Opcode::CatchValue:
        case Opcode::GetExternalModule:
            return changes(Writes::RuntimeState);
        case Opcode::PutModuleVariable:
            return changes(Writes::Fields);
        // Nobody who was running before can have looked into what was made since.
        case Opcode::PutField:
            return changes(isLocalAllocation(function, value.child(0)) ? Writes::RuntimeState : Writes::Fields);
        case Opcode::GetById:
        case Opcode::InById:
            return changes(isAccessToField(function, value) ? Writes::Nothing : Writes::Heap);
        case Opcode::PutById:
            if (isInitializingStore(m_program, function, block, index))
                return changes(Writes::RuntimeState);
            if (!isAccessToField(function, value))
                return changes(Writes::Heap);
            return changes(isLocalAllocation(function, value.child(0)) ? Writes::RuntimeState : Writes::Fields);
        // What the engine has under the name from the start, and the program leaves there, is not looked up.
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
            return changes(isGlobalOfEngine(value) ? Writes::RuntimeState : Writes::Heap);
        case Opcode::GetByVal:
            switch (keyedAccessOf(m_program, function, value)) {
            case KeyedAccess::Array:
            case KeyedAccess::TypedArray:
                return changes(Writes::Nothing);
            // Looking at the characters flattens a rope.
            case KeyedAccess::String:
                return changes(Writes::RuntimeState);
            case KeyedAccess::Generic:
                return changes(Writes::Heap);
            }
            break;
        // What is stored to a typed array is a number already. It stays as long as it is, with its elements where they are.
        case Opcode::PutByVal:
            return changes(keyedAccessOf(m_program, function, value) == KeyedAccess::TypedArray ? Writes::TypedArrayProperties : Writes::Heap);
        case Opcode::IteratorOpen:
        case Opcode::IteratorHasNext:
        case Opcode::IteratorValue:
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
        case Opcode::CallBuiltin:
            return changes(writesOfBuiltinCall(m_program, function, value));
        case Opcode::PutGlobal:
        case Opcode::DeleteById:
        case Opcode::InByVal:
        case Opcode::DeleteByVal:
        case Opcode::InstanceOf:
        case Opcode::ToPropertyKey:
        case Opcode::Call:
        case Opcode::Construct:
        case Opcode::CallMethod:
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
    // Two for each function, see ofAnyCall() and ofCallThatReturns().
    Vector<Effects> m_effects;
    // By summary: those it is part of.
    Vector<Vector<unsigned, 2>> m_dependents;
    // By view and field.
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_onlyFieldsSupply;
};

} // anonymous namespace

void computeEffectSummaries(Program& program)
{
    EffectSummaries(program).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
