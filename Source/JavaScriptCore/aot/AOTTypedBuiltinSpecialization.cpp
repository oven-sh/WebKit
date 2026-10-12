/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Gives a call of a built-in method a fast path behind a check of the receiver:
//
//     guard = CallBuiltin HasOriginalStructure[class] (receiver)
//     Branch guard ? fast : slow
//
// `slow` holds the call as it was. In `fast` the receiver is an instance of the class that finds what the class has under the name.
//
// Prelude: the table binds the method to a function of the prelude. `fast` calls it directly, so that it is inlined, and the callback
// into it. The receiver is of the class by its type, or is speculated to be. Where nothing can have changed there is no guard, and the
// call is replaced: a receiver that is a string, a static function, a global function, a constructor.
// Speculation: the class of the receiver is not known. `fast` holds the same call on the receiver cast to the class, for
// resolveBuiltins().
//
// The class a receiver of unknown class is speculated to be of is the one the checker predicts, or else the only one with a method of the
// name: Builtins::classSpeculatedFor().
//
// Speculation also gives a call of a function of a replaceable global, `console.log(..)`, a fast path. The object and the function are
// read as the language says, and what is found is compared with the function the realm started with:
//
//     method = GetById object, name
//     guard = IsBuiltinFunction[entry] (method)
//     Branch guard ? fast : slow
//
// `fast` calls that function, of which the entry holds, with the object for `this`. `slow` calls what was found.
//
// A receiver that is predicted to be a number, and may be something else by its type, is asked whether it is one:
//
//     guard = TypeOfIs receiver number
//     Branch guard ? fast : slow
//
// A number finds what Number.prototype has, which cannot change. `fast` holds the call on the receiver cast to a number, `slow` the call as it was.
//
// The same for `o.hasOwnProperty(key)`, whatever `o` is: every object inherits the function, and any may have another in front of it. `fast` is
// Object.hasOwn(o, key): see Builtins::hasOwn().
class BuiltinSpecialization {
public:
    enum class Mode : uint8_t {
        Prelude,
        Speculation,
    };

    BuiltinSpecialization(const Program& program, Function& function, Mode mode)
        : m_program(program)
        , m_function(function)
        , m_builtins(Builtins::singleton())
        , m_mode(mode)
        , m_callIntrinsic(program.stringOf(m_builtins.at(m_builtins.callIntrinsic()).nameName))
    {
    }

    unsigned run()
    {
        noteRefutedReceivers();
        unsigned changed = 0;
        // What comes after a value that was replaced is in a new block, which has its turn.
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (unsigned i = 0; i < m_function.blocks[b].values.size(); ++i) {
                auto& value = m_function.at(m_function.blocks[b].values[i]);
                if (value.opcode == Opcode::Operation && m_callIntrinsic && value.string() == m_callIntrinsic) {
                    convertCallIntrinsicToCall(value);
                    ++changed;
                    continue;
                }
                if ((m_mode == Mode::Speculation && takeTimeOfDates(BlockID(b), i)) || specialize(BlockID(b), i)) {
                    ++changed;
                    break;
                }
            }
        }
        if (changed)
            cleanUp(m_program, m_function);
        return changed;
    }

private:
    struct Site {
        ValueID receiver;
        StringID name;
    };

    struct Refuted {
        ValueID receiver;
        uint32_t engineClass { Format::none };
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }

    ValueID originOf(ValueID value) const
    {
        return m_program.originOf(m_function, value);
    }

    // `o.m(..)`, or a call of what a GetById has read from `o` with `o` for `this`.
    std::optional<Site> siteOf(const Value& value) const
    {
        if (value.opcode == Opcode::CallMethod)
            return Site { value.child(0), value.string() };
        if (value.opcode != Opcode::Call)
            return std::nullopt;
        auto& read = m_function.at(originOf(value.child(0)));
        if (read.opcode != Opcode::GetById || originOf(read.child(0)) != originOf(value.child(1)))
            return std::nullopt;
        return Site { value.child(1), read.string() };
    }

    // The global object of the table that the value is, going by how it is defined.
    BuiltinName globalDefinedBy(ValueID id) const
    {
        auto& value = m_function.at(originOf(id));
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = m_program.builtinName(value.string());
        return m_builtins.isGlobal(name) ? name : BuiltinName();
    }

    // The entries of the static function, the global function or the constructor that the value calls.
    BuiltinID firstEntryOfGlobalCalledBy(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::CallMethod:
            return m_builtins.findStatic(BuiltinKind::StaticFunction, globalDefinedBy(value.child(0)), m_program.builtinName(value.string()));
        case Opcode::Call: {
            if (BuiltinName global = globalDefinedBy(value.child(0)))
                return m_builtins.find(BuiltinKind::Function, { }, global);
            auto site = siteOf(value);
            return site ? m_builtins.findStatic(BuiltinKind::StaticFunction, globalDefinedBy(site->receiver), m_program.builtinName(site->name)) : BuiltinID();
        }
        case Opcode::Construct:
            if (BuiltinName global = globalDefinedBy(value.child(0)))
                return m_builtins.findConstructor(global);
            return { };
        default:
            return { };
        }
    }

    // None: the entry is bound to no function of the prelude, or the program has no such function.
    FunctionID preludeFunctionOf(BuiltinID entry) const
    {
        if (!entry || m_builtins.at(entry).implementation.kind != BuiltinImplementation::Prelude)
            return { };
        return m_program.preludeFunctions[static_cast<unsigned>(m_builtins.at(entry).implementation.prelude)];
    }

    bool isStringClass(const EngineClass& engineClass) const { return engineClass.index == m_builtins.stringClass().index; }

    const EngineClass* classSpeculatedFor(ValueID receiver, BuiltinName name) const
    {
        auto& type = typeOf(receiver);
        const EngineClass* speculated;
        if (TypeID prediction = m_function.at(receiver).prediction) {
            Type expected = m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull);
            speculated = expected.isString() ? &m_builtins.stringClass() : expected.onlyEngineClass();
        } else
            speculated = m_builtins.classSpeculatedFor(name);
        if (!speculated)
            return nullptr;
        if (isStringClass(*speculated))
            return type.primitives & Primitive::String ? speculated : nullptr;
        if (!speculated->canCheckOriginalStructure())
            return nullptr;
        return type.anyObject || type.engineClasses.contains(speculated->index) ? speculated : nullptr;
    }

    bool takeTimeOfDates(BlockID block, unsigned index)
    {
        if (m_holdsDatesAsTheyWere[block.index()])
            return false;
        ValueID id = m_function.at(block).values[index];
        Vector<ValueID, 2> operands;
        switch (m_function.at(id).opcode) {
        case Opcode::ToNumber:
            operands.append(m_function.at(id).child(0));
            break;
        case Opcode::Call:
            if (m_function.at(id).arguments.size() != 1 || globalDefinedBy(m_function.at(id).child(0)) != m_builtins.nameOf("Number"_s.span8()))
                return false;
            operands.append(m_function.at(id).arguments[0]);
            break;
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            operands.append(m_function.at(id).child(0));
            operands.append(m_function.at(id).child(1));
            break;
        default:
            return false;
        }
        const EngineClass* dateClass = nullptr;
        for (ValueID operand : operands) {
            dateClass = typeOf(operand).onlyEngineClass();
            if (TypeID prediction = m_function.at(operand).prediction; !dateClass && prediction && typeOf(operand).anyObject)
                dateClass = m_program.at(prediction).onlyEngineClass();
            if (!dateClass || dateClass->type != JSDateType || !dateClass->canCheckOriginalStructure())
                return false;
        }
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, *dateClass, m_builtins.nameOf("getTime"_s.span8())), { });
        TypeID typeOfClass = m_program.engineClassTypes[dateClass->index];
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic || !typeOfClass)
            return false;

        Value original = m_function.at(id);
        open(block, index);
        Vector<BlockID, 2> otherwise;
        Vector<ValueID, 2> times;
        for (ValueID operand : operands) {
            Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
            guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), dateClass->index };
            guard.arguments.append(operand);
            otherwise.append(branch(append(WTF::move(guard))));
            Value call = make(Opcode::CallBuiltin, m_program.types.number);
            call.children[0] = held(operand, typeOfClass);
            call.immediates[0] = entry.index();
            times.append(appendThrowing(WTF::move(call)));
        }
        if (operands.size() == 1)
            finish(held(times[0], original.type));
        else {
            Value comparison = original;
            comparison.children = { times[0], times[1], ValueID() };
            finish(appendThrowing(WTF::move(comparison)));
        }
        for (BlockID slow : otherwise) {
            moveTo(slow);
            m_holdsDatesAsTheyWere[slow.index()] = true;
            finish(appendThrowing(Value { original }));
        }
        return true;
    }

    // Whether the value at the index was replaced: then the block ends before it.
    bool specialize(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        Vector<Type, 4> argumentTypes;
        for (ValueID argument : m_function.at(id).arguments)
            argumentTypes.append(typeOf(argument));
        if (m_mode == Mode::Prelude) {
            if (FunctionID callee = preludeFunctionOf(applicableEntry(firstEntryOfGlobalCalledBy(m_function.at(id)), argumentTypes.span()))) {
                Value original = m_function.at(id);
                open(block, index);
                callDirectly(callee, { }, original);
                return true;
            }
        }

        auto site = siteOf(m_function.at(id));
        if (!site)
            return false;
        BuiltinName name = m_program.builtinName(site->name);
        if (!name)
            return false;
        if (BuiltinName holder = replaceableGlobalDefinedBy(site->receiver))
            return m_mode == Mode::Speculation && guardCallee(block, index, *site, applicableEntry(m_builtins.find(BuiltinKind::StaticFunction, holder, name), argumentTypes.span()));
        auto& receiver = typeOf(site->receiver);
        if (BuiltinID method = m_builtins.hasOwnProperty(name); method && receiver.hasObjects() && !receiver.onlyEngineClass()) {
            argumentTypes.insert(0, receiver);
            return m_mode == Mode::Speculation && guardCallee(block, index, *site, method, applicableEntry(m_builtins.hasOwn(), argumentTypes.span()));
        }
        if (m_mode == Mode::Speculation && splitOnNumber(block, index, *site, name, argumentTypes.span()))
            return true;
        const EngineClass* engineClass = receiver.onlyEngineClass();
        // A primitive needs no check: resolveBuiltins() does all there is to do, but for a method of strings that is bound to the prelude.
        if (!receiver.hasObjects()) {
            if (m_mode != Mode::Prelude || !(receiver.primitives & Primitive::String) || !receiver.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
                return false;
            engineClass = &m_builtins.stringClass();
        }
        bool isKnown = engineClass;
        if (isKnown ? !(isStringClass(*engineClass) || engineClass->canCheckOriginalStructure()) : !(engineClass = classSpeculatedFor(site->receiver, name)))
            return false;
        auto& refuted = m_refuted[block.index()];
        if (refuted.engineClass == engineClass->index && refuted.receiver == originOf(site->receiver))
            return false;

        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, *engineClass, name), argumentTypes.span());
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        FunctionID callee;
        if (m_mode == Mode::Prelude) {
            callee = preludeFunctionOf(entry);
            if (!callee)
                return false;
        } else if (isKnown)
            return false;
        TypeID typeOfClass = m_program.engineClassTypes[engineClass->index];
        RELEASE_ASSERT(typeOfClass);

        Value original = m_function.at(id);
        open(block, index);
        if (callee && receiver.isString()) {
            callDirectly(callee, site->receiver, original);
            return true;
        }
        Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
        guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), engineClass->index };
        guard.arguments.append(site->receiver);
        BlockID slow = branch(append(WTF::move(guard)));

        if (callee)
            callDirectly(callee, site->receiver, original);
        else {
            Value call = original;
            call.opcode = Opcode::CallMethod;
            call.children = { held(site->receiver, typeOfClass), ValueID(), ValueID() };
            call.immediates = { site->name.index(), m_inliningDepth };
            finish(appendThrowing(WTF::move(call)));
        }

        moveTo(slow);
        m_refuted[slow.index()] = { originOf(site->receiver), engineClass->index };
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    bool splitOnNumber(BlockID block, unsigned index, Site site, BuiltinName name, std::span<const Type> argumentTypes)
    {
        auto& receiver = typeOf(site.receiver);
        TypeID prediction = m_function.at(site.receiver).prediction;
        Type withoutNil = receiver.withoutPrimitives(Primitive::UndefinedOrNull);
        bool isNumberOrNil = withoutNil.isNumber();
        if (!(receiver.primitives & Primitive::Number) || receiver.isNumber() || !(isNumberOrNil || (prediction && m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull).isNumber())))
            return false;
        auto& numberClass = m_builtins.numberClass();
        if (auto& refuted = m_refuted[block.index()]; refuted.engineClass == numberClass.index && refuted.receiver == originOf(site.receiver))
            return false;
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, numberClass, name), argumentTypes);
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;

        Value original = m_function.at(m_function.at(block).values[index]);
        open(block, index);
        if (isNumberOrNil) {
            Value guard = make(Opcode::IsUndefinedOrNull, m_program.types.boolean);
            guard.children[0] = site.receiver;
            BlockID fast = branch(append(WTF::move(guard)));
            m_refuted[m_block.index()] = { originOf(site.receiver), numberClass.index };
            finish(appendThrowing(Value { original }));

            moveTo(fast);
            Value call = WTF::move(original);
            call.opcode = Opcode::CallMethod;
            call.children = { held(site.receiver, withoutNil.isInteger() && withoutNil.integers.isWithin(Range::int32()) ? m_program.types.int32 : m_program.types.number), ValueID(), ValueID() };
            call.immediates = { site.name.index(), m_inliningDepth };
            finish(appendThrowing(WTF::move(call)));
            return true;
        }
        Value guard = make(Opcode::TypeOfIs, m_program.types.boolean);
        guard.children[0] = site.receiver;
        guard.immediates[0] = static_cast<uint32_t>(TypeofType::Number);
        BlockID slow = branch(append(WTF::move(guard)));

        Value call = original;
        call.opcode = Opcode::CallMethod;
        call.children = { held(site.receiver, m_program.types.number), ValueID(), ValueID() };
        call.immediates = { site.name.index(), m_inliningDepth };
        finish(appendThrowing(WTF::move(call)));

        moveTo(slow);
        m_refuted[slow.index()] = { originOf(site.receiver), numberClass.index };
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    BuiltinName replaceableGlobalDefinedBy(ValueID id) const
    {
        auto& value = m_function.at(originOf(id));
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = m_program.builtinName(value.string());
        return m_builtins.isReplaceable(name) ? name : BuiltinName();
    }

    bool hasGuard(ValueID method) const
    {
        for (auto& value : m_function.values) {
            if (value.opcode == Opcode::IsBuiltinFunction && value.child(0) == method)
                return true;
        }
        return false;
    }

    // `takingReceiver`: the entry of a static function that is called in the place of the function, with the receiver for its first argument.
    bool guardCallee(BlockID block, unsigned index, Site site, BuiltinID entry, std::optional<BuiltinID> takingReceiver = std::nullopt)
    {
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        if (takingReceiver && (!*takingReceiver || m_builtins.at(*takingReceiver).implementation.kind == BuiltinImplementation::Generic))
            return false;
        Value original = m_function.at(m_function.at(block).values[index]);
        ValueID method = original.opcode == Opcode::Call ? original.child(0) : ValueID();
        if (method && hasGuard(method))
            return false;

        open(block, index);
        if (!method) {
            Value read = make(Opcode::GetById, m_program.types.anything);
            read.children[0] = site.receiver;
            read.immediates[0] = site.name.index();
            method = appendThrowing(WTF::move(read));
        }
        Value guard = make(Opcode::IsBuiltinFunction, m_program.types.boolean);
        guard.children[0] = method;
        guard.immediates[0] = entry.index();
        BlockID slow = branch(append(WTF::move(guard)));

        Value call = make(Opcode::CallBuiltin, original.type);
        if (takingReceiver) {
            call.immediates[0] = takingReceiver->index();
            call.arguments.append(site.receiver);
            call.arguments.appendVector(original.arguments);
        } else {
            call.children[0] = site.receiver;
            call.immediates[0] = entry.index();
            call.arguments = original.arguments;
        }
        finish(appendThrowing(WTF::move(call)));

        moveTo(slow);
        Value general = make(Opcode::Call, original.type);
        general.children = { method, site.receiver, ValueID() };
        general.immediates[1] = m_inliningDepth;
        general.arguments = WTF::move(original.arguments);
        finish(appendThrowing(WTF::move(general)));
        return true;
    }

    // The block a guard branches to where it fails holds the call as it was, which is not looked at again.
    void noteRefutedReceivers()
    {
        m_refuted.fill(Refuted { }, m_function.blocks.size());
        m_holdsDatesAsTheyWere.fill(false, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            auto& condition = m_function.at(block.terminator.value);
            if (condition.opcode == Opcode::CallBuiltin && condition.builtin() == m_builtins.hasOriginalStructureOperation())
                m_refuted[block.terminator.targets[1].block.index()] = { originOf(condition.arguments[0]), condition.engineClass() };
            if (condition.opcode == Opcode::TypeOfIs && condition.typeofType() == TypeofType::Number)
                m_refuted[block.terminator.targets[1].block.index()] = { originOf(condition.child(0)), m_builtins.numberClass().index };
        }
    }

    // The receiver, if there is one, then the arguments, as the entry block of the function takes them.
    Arguments passedTo(FunctionID id, ValueID receiver, std::span<const ValueID> given)
    {
        auto& callee = m_program.at(id);
        Arguments all;
        unsigned taken = 0;
        for (unsigned k = 0; k < callee.parameterTypes.size(); ++k) {
            ValueID argument;
            if (!k && receiver)
                argument = receiver;
            else if (taken < given.size())
                argument = given[taken++];
            else
                argument = append(make(Opcode::Undefined, m_program.types.undefined));
            all.append(held(argument, callee.parameterTypes[k]));
        }
        return all;
    }

    // In the place of the value that is being replaced.
    void callDirectly(FunctionID callee, ValueID receiver, const Value& original)
    {
        Value call = make(Opcode::CallDirect, m_program.at(callee).resultType ? m_program.at(callee).resultType : m_program.types.anything);
        call.arguments = passedTo(callee, receiver, original.arguments.span());
        call.immediates = { callee.index(), m_inliningDepth };
        finish(held(appendThrowing(WTF::move(call)), original.type));
    }

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        m_refuted.append(Refuted { });
        m_holdsDatesAsTheyWere.append(false);
        return BlockID(m_function.blocks.size() - 1);
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
        value.inlineFrame = m_inlineFrame;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    // Takes the value at the index out of its block, which ends there and is added to from here on. What came after the value is in a
    // block of its own, and the value is the parameter of that: it keeps its number, its type and its prediction.
    void open(BlockID block, unsigned index)
    {
        ValueID id = m_function.at(block).values[index];
        m_position = m_function.at(id).position;
        m_inlineFrame = m_function.at(id).inlineFrame;
        m_inliningDepth = m_function.at(id).opcode == Opcode::Construct ? Format::none : m_function.at(id).immediates[1];
        m_isWatched = m_function.at(block).terminator.kind == TerminatorKind::Invoke && index + 1 == m_function.at(block).values.size();
        m_after = addBlock();
        {
            auto& from = m_function.at(block);
            m_function.at(m_after).values.append(from.values.subspan(index + 1));
            from.values.shrink(index);
            m_function.at(m_after).terminator = std::exchange(from.terminator, Terminator { });
        }
        m_handler = { };
        if (m_isWatched) {
            auto& invoke = m_function.at(m_after).terminator;
            m_handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        Value parameter = make(Opcode::Parameter, m_function.at(id).type);
        parameter.takePredictionOf(m_function.at(id));
        parameter.immediates = { m_after.index(), 0 };
        m_function.at(id) = WTF::move(parameter);
        m_function.at(m_after).parameters.append(id);
        moveTo(block);
    }

    void moveTo(BlockID block)
    {
        m_block = block;
        m_lastMayThrow = false;
    }

    // Where a handler watches, what may throw ends its block.
    void endBlockAfterWatchedValue()
    {
        if (!m_lastMayThrow)
            return;
        m_lastMayThrow = false;
        BlockID next = addBlock();
        auto& invoke = m_function.at(m_block).terminator;
        invoke.kind = TerminatorKind::Invoke;
        invoke.targets.append(Target { next, { } });
        invoke.targets.append(m_handler);
        m_block = next;
    }

    ValueID append(Value&& value)
    {
        endBlockAfterWatchedValue();
        m_function.values.append(WTF::move(value));
        ValueID id(m_function.values.size() - 1);
        m_function.at(m_block).values.append(id);
        return id;
    }

    ValueID appendThrowing(Value&& value)
    {
        ValueID id = append(WTF::move(value));
        m_lastMayThrow = m_isWatched;
        return id;
    }

    // Goes on where the condition holds. Yields where to go on for where it does not.
    BlockID branch(ValueID condition)
    {
        endBlockAfterWatchedValue();
        BlockID then = addBlock();
        BlockID otherwise = addBlock();
        auto& branch = m_function.at(m_block).terminator;
        branch.kind = TerminatorKind::Branch;
        branch.value = condition;
        branch.targets.append(Target { then, { } });
        branch.targets.append(Target { otherwise, { } });
        moveTo(then);
        return otherwise;
    }

    // On to what came after the value that is replaced, which is this from here on.
    void finish(ValueID result)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = m_lastMayThrow ? TerminatorKind::Invoke : TerminatorKind::Jump;
        terminator.targets.append(Target { m_after, Arguments { result } });
        if (m_lastMayThrow)
            terminator.targets.append(m_handler);
        m_lastMayThrow = false;
    }

    // The value as one of the type. None: anything will do.
    ValueID held(ValueID value, TypeID type)
    {
        if (!type || m_program.isHeldAlready(typeOf(value), m_program.at(type), CastReason::Claim))
            return value;
        Value cast = make(Opcode::Cast, type);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(CastReason::Claim);
        return appendThrowing(WTF::move(cast));
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    Mode m_mode;
    StringID m_callIntrinsic;
    // By block.
    Vector<Refuted> m_refuted;
    Vector<bool> m_holdsDatesAsTheyWere;
    // Of the value that is being replaced.
    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
    uint32_t m_inliningDepth { Format::none };
    bool m_isWatched { false };
    Target m_handler;
    BlockID m_after;
    // Where values are added.
    BlockID m_block;
    bool m_lastMayThrow { false };
};

} // anonymous namespace

unsigned resolvePreludeCalls(const Program& program, Function& function)
{
    return BuiltinSpecialization(program, function, BuiltinSpecialization::Mode::Prelude).run();
}

unsigned speculateOnReceivers(const Program& program, Function& function)
{
    return BuiltinSpecialization(program, function, BuiltinSpecialization::Mode::Speculation).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
