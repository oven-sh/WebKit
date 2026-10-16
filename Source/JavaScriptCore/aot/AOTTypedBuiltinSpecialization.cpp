/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#include "AOTTypedThreads.h"

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
    {
    }

    void leaveColdTextToStubs()
    {
        BitVector isInLoop = blocksInLoops(m_function);
        m_isColdByText.fill(true, m_function.blocks.size());
        for (size_t block : isInLoop)
            m_isColdByText[block] = false;
    }

    unsigned run(CleanUpTiming timing = CleanUpTiming::Immediate)
    {
        noteRefutedReceivers();
        unsigned changed = 0;
        // What comes after a value that was replaced is in a new block, which has its turn.
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (unsigned i = m_numberOfValuesLeftBehind[b]; i < m_function.blocks[b].values.size(); ++i) {
                auto& value = m_function.at(m_function.blocks[b].values[i]);
                if (value.opcode == Opcode::Operation && value.builtin() == m_builtins.callIntrinsic()) {
                    convertCallIntrinsicToCall(value);
                    ++changed;
                    continue;
                }
                m_blockOfSite = BlockID(b);
                if ((m_mode == Mode::Speculation && (takeTimeOfDates(BlockID(b), i) || readNumberOnEachWay(BlockID(b), i))) || specialize(BlockID(b), i)) {
                    ++changed;
                    break;
                }
            }
            if (unsigned leftBehind = std::exchange(m_numberOfValuesLeftBehind[b], 0))
                m_function.blocks[b].values.removeAt(0, leftBehind);
        }
        if (changed && timing == CleanUpTiming::Immediate)
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

    FunctionID preludeFunctionFor(BuiltinID entry, const Value& call) const
    {
        FunctionID callee = preludeFunctionOf(entry);
        if (!callee || call.arguments.isEmpty())
            return callee;
        BuiltinName callback = globalDefinedBy(call.arguments[0]);
        if (!callback)
            return callee;
        static const BuiltinName booleanConstructor = Builtins::singleton().nameOf(asBytes("Boolean"_s.span8()));
        static const BuiltinName numberConstructor = Builtins::singleton().nameOf(asBytes("Number"_s.span8()));
        PreludeFunction which = m_builtins.at(entry).implementation.prelude;
        FunctionID withoutCalls;
        if (which == PreludeFunction::ArrayFilter && callback == booleanConstructor)
            withoutCalls = m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayFilterTruthy)];
        else if (which == PreludeFunction::ArrayMap && callback == numberConstructor)
            withoutCalls = m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayMapByNumber)];
        return withoutCalls ? withoutCalls : callee;
    }

    bool isStringClass(const EngineClass& engineClass) const { return engineClass.index == m_builtins.stringClass().index; }

    const EngineClass* classSpeculatedFor(ValueID receiver, BuiltinName name) const
    {
        auto& type = typeOf(receiver);
        const EngineClass* speculated;
        if (TypeID prediction = m_function.at(receiver).prediction()) {
            Type expected = m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull);
            speculated = expected.isString() ? &m_builtins.stringClass() : expected.onlyEngineClass();
            if (!speculated) {
                speculated = m_builtins.classSpeculatedFor(name);
                if (speculated && !(isStringClass(*speculated) ? expected.primitives & Primitive::String : expected.anyObject || expected.engineClasses.contains(speculated->index)))
                    speculated = nullptr;
            }
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
        case Opcode::Call: {
            static const BuiltinName number = Builtins::singleton().nameOf(asBytes("Number"_s.span8()));
            if (m_function.at(id).arguments.size() != 1 || globalDefinedBy(m_function.at(id).child(0)) != number)
                return false;
            operands.append(m_function.at(id).arguments[0]);
            break;
        }
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
            if (TypeID prediction = m_function.at(operand).prediction(); !dateClass && prediction && typeOf(operand).anyObject)
                dateClass = m_program.at(prediction).onlyEngineClass();
            if (!dateClass || dateClass->type != JSDateType || !dateClass->canCheckOriginalStructure())
                return false;
        }
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, *dateClass, m_builtins.nameOf(asBytes("getTime"_s.span8()))), { });
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

    bool readNumberOnEachWay(BlockID block, unsigned index)
    {
        auto& values = m_function.at(block).values;
        ValueID id = values[index];
        auto& read = m_function.at(id);
        if (read.opcode != Opcode::GetById || read.isOnSlowPath || read.hasWordOfKnownPlace() || read.immediates[1] != Format::none || !read.type || index + 1 == values.size())
            return false;
        switch (auto& next = m_function.at(values[index + 1]); next.opcode) {
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            if (next.child(0) != id && next.child(1) != id)
                return false;
            break;
        default:
            return false;
        }
        ValueID receiver = read.child(0);
        TypeID prediction = m_function.at(receiver).prediction();
        if (!prediction || !typeOf(receiver).anyObject)
            return false;
        auto* engineClass = m_program.at(prediction).withoutPrimitives(Primitive::UndefinedOrNull).onlyEngineClass();
        if (!engineClass || engineClass->type == ArrayType || !engineClass->canCheckOriginalStructure())
            return false;
        if (auto& refuted = m_refuted[block.index()]; refuted.engineClass == engineClass->index && refuted.receiver == originOf(receiver))
            return false;
        BuiltinName name = m_program.builtinName(read.string());
        BuiltinID entry = name ? m_builtins.findForInstance(BuiltinKind::Getter, *engineClass, name) : BuiltinID();
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        TypeID yielded = m_program.integersYieldedByBuiltins[entry.index()];
        auto& number = m_program.at(m_program.types.number);
        if (!yielded || !m_program.at(yielded).isWithin(number, m_program) || read.type == m_program.types.number || !number.isWithin(m_program.at(read.type), m_program))
            return false;
        if (m_isInLoop.isEmpty()) {
            m_isInLoop.fill(false, m_function.blocks.size());
            for (size_t inLoop : blocksInLoops(m_function))
                m_isInLoop[inLoop] = true;
        }
        if (!m_isInLoop[block.index()] || numberOfUsesOf(id) != 1)
            return false;

        Value original = read;
        open(block, index);
        Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
        guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), engineClass->index };
        guard.arguments.append(receiver);
        guard.guardsFastPath = true;
        BlockID slow = branch(append(WTF::move(guard)));
        Value onObject = original;
        onObject.children[0] = held(receiver, m_program.engineClassTypes[engineClass->index]);
        onObject.type = yielded;
        onObject.rep = repOf(m_program.at(onObject.type));
        onObject.takePredictionOf(Value { });
        finish(appendThrowing(WTF::move(onObject)));

        moveTo(slow);
        m_refuted[slow.index()] = { originOf(receiver), engineClass->index };
        original.isOnSlowPath = true;
        finish(appendThrowing(WTF::move(original)));
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
            BuiltinID takingReceiver = applicableEntry(m_builtins.hasOwn(), argumentTypes.span());
            bool isCalledAsValue = !takingReceiver && argumentTypes.size() == 2 && m_builtins.hasCell(method);
            return m_mode == Mode::Speculation && guardCallee(block, index, *site, method, isCalledAsValue ? std::nullopt : std::optional(takingReceiver), isCalledAsValue);
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

        BuiltinID first = m_builtins.findForInstance(BuiltinKind::Method, *engineClass, name);
        BuiltinID entry = applicableEntry(first, argumentTypes.span());
        bool argumentIsTestedByStub = false;
        if (!entry && !isKnown && engineClass->type == RegExpObjectType && m_mode == Mode::Speculation && (m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call)) {
            Vector<Type, 4> strings;
            strings.fill(m_program.at(m_program.types.string), argumentTypes.size());
            entry = applicableEntry(first, strings.span());
            if (entry && methodAskedByStub(entry, m_function.at(id).arguments) == MethodAskedByStub::None)
                return false;
            argumentIsTestedByStub = !!entry;
        }
        if (entry && !isKnown && isStringClass(*engineClass) && m_mode == Mode::Speculation && (m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call) && methodAskedByStub(entry, m_function.at(id).arguments) == MethodAskedByStub::StringMatch)
            argumentIsTestedByStub = true;
        if (!entry || m_builtins.at(entry).implementation.kind == BuiltinImplementation::Generic)
            return false;
        FunctionID callee;
        if (m_mode == Mode::Prelude) {
            callee = preludeFunctionFor(entry, m_function.at(id));
            if (!callee)
                return false;
        } else if (isKnown)
            return false;
        if ((argumentIsTestedByStub || (!m_isColdByText.isEmpty() && m_isColdByText[block.index()])) && (m_function.at(id).opcode == Opcode::CallMethod || m_function.at(id).opcode == Opcode::Call)) {
            if (auto method = methodAskedByStub(entry, m_function.at(id).arguments); method != MethodAskedByStub::None) {
                m_function.at(id).methodAskedByStub = method;
                leaveReadOfMethodToCall(block, index);
                return false;
            }
            auto& implementation = m_builtins.at(entry).implementation;
            if ((implementation.kind == BuiltinImplementation::Call || (implementation.kind == BuiltinImplementation::Inline && implementation.inlined != InlineBuiltin::ArrayEntries)) && engineClass->index < std::numeric_limits<uint16_t>::max()) {
                leaveReadOfMethodToCall(block, index);
                if (m_function.at(id).opcode == Opcode::CallMethod) {
                    m_function.at(id).classExpectedOfReceiverPlusOne = engineClass->index + 1;
                    return false;
                }
            }
        }
        if (!isKnown)
            leaveReadOfMethodToCall(block, index);
        if (!callee && carryReadOfMethod(id, entry, *engineClass))
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
        original.isInTailPosition &= !m_function.isGuardedPreludeEntry;
        finish(appendThrowing(WTF::move(original)));
        return true;
    }

    void leaveReadOfMethodToCall(BlockID block, unsigned index)
    {
        auto& values = m_function.at(block).values;
        auto& call = m_function.at(values[index]);
        if (call.opcode != Opcode::Call)
            return;
        ValueID read = call.child(0);
        if (m_function.at(read).opcode != Opcode::GetById || m_function.at(read).child(0) != call.child(1))
            return;
        unsigned at = index;
        while (true) {
            if (at-- == m_numberOfValuesLeftBehind[block.index()])
                return;
            if (values[at] == read)
                break;
            switch (m_function.at(values[at]).opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
            case Opcode::BigInt:
            case Opcode::FunctionReference:
            case Opcode::NewClosure:
                break;
            default:
                return;
            }
        }
        if (numberOfUsesOf(read) != 1)
            return;
        call.opcode = Opcode::CallMethod;
        call.immediates = { m_function.at(read).string().index(), call.immediates[1] };
        call.children = { call.child(1), ValueID(), ValueID() };
        Value nothing;
        nothing.opcode = Opcode::Undefined;
        nothing.position = m_function.at(read).position;
        nothing.inlineFrame = m_function.at(read).inlineFrame;
        nothing.type = m_program.types.undefined;
        nothing.rep = repOf(m_program.at(nothing.type));
        m_function.at(read) = WTF::move(nothing);
    }

    unsigned numberOfUsesOf(ValueID value) const
    {
        unsigned uses = 0;
        auto count = [&](ValueID used) {
            uses += used == value;
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                std::as_const(m_function.at(id)).forEachChild(count);
            block.terminator.forEachChild(count);
        }
        return uses;
    }

    bool carryReadOfMethod(ValueID id, BuiltinID entry, const EngineClass& engineClass)
    {
        auto& call = m_function.at(id);
        auto& implementation = m_builtins.at(entry).implementation;
        bool isLoweredWhereExpected = implementation.kind == BuiltinImplementation::Call || (implementation.kind == BuiltinImplementation::Inline && implementation.inlined != InlineBuiltin::ArrayEntries);
        if (call.opcode != Opcode::Call || !isLoweredWhereExpected || engineClass.index >= std::numeric_limits<uint16_t>::max() || typeOf(call.child(1)).onlyView())
            return false;
        ValueID read = call.child(0);
        auto& method = m_function.at(read);
        if (method.opcode != Opcode::GetById || method.child(0) != call.child(1) || method.immediates[1] != Format::none || !m_program.at(method.type).isAnything() || numberOfUsesOf(read) != 1)
            return false;
        method.immediates[1] = entry.index();
        method.classExpectedOfReceiverPlusOne = engineClass.index + 1;
        call.opcode = Opcode::CallMethod;
        call.immediates = { method.string().index(), call.immediates[1] };
        call.children = { call.child(1), read, ValueID() };
        call.classExpectedOfReceiverPlusOne = engineClass.index + 1;
        return true;
    }

    MethodAskedByStub methodAskedByStub(BuiltinID entry, const Arguments& arguments) const
    {
        auto& implementation = m_builtins.at(entry).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || arguments.isEmpty())
            return MethodAskedByStub::None;
        size_t numberOfArguments = arguments.size();
        if (implementation.inlined == InlineBuiltin::ArrayPush)
            return numberOfArguments == 1 ? MethodAskedByStub::ArrayPush : MethodAskedByStub::None;
        auto& typeOfKey = m_program.at(m_function.at(arguments[0]).type);
        if (!typeOfKey.isAnything() && (!typeOfKey.isString() || implementation.inlined == InlineBuiltin::SetAdd))
            return MethodAskedByStub::None;
        switch (implementation.inlined) {
        case InlineBuiltin::MapGet:
            return numberOfArguments == 1 ? MethodAskedByStub::MapGet : MethodAskedByStub::None;
        case InlineBuiltin::MapHas:
            return numberOfArguments == 1 ? MethodAskedByStub::MapHas : MethodAskedByStub::None;
        case InlineBuiltin::MapSet:
            return numberOfArguments == 2 ? MethodAskedByStub::MapSet : MethodAskedByStub::None;
        case InlineBuiltin::SetHas:
            return numberOfArguments == 1 ? MethodAskedByStub::SetHas : MethodAskedByStub::None;
        case InlineBuiltin::SetAdd:
            return numberOfArguments == 1 ? MethodAskedByStub::SetAdd : MethodAskedByStub::None;
        case InlineBuiltin::RegExpExec:
            return numberOfArguments == 1 ? MethodAskedByStub::RegExpExec : MethodAskedByStub::None;
        case InlineBuiltin::RegExpTest:
            return numberOfArguments == 1 ? MethodAskedByStub::RegExpTest : MethodAskedByStub::None;
        case InlineBuiltin::StringMatch:
            return numberOfArguments == 1 && typeOfKey.isAnything() ? MethodAskedByStub::StringMatch : MethodAskedByStub::None;
        default:
            return MethodAskedByStub::None;
        }
    }

    bool splitOnNumber(BlockID block, unsigned index, Site site, BuiltinName name, std::span<const Type> argumentTypes)
    {
        auto& receiver = typeOf(site.receiver);
        TypeID prediction = m_function.at(site.receiver).prediction();
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
    bool guardCallee(BlockID block, unsigned index, Site site, BuiltinID entry, std::optional<BuiltinID> takingReceiver = std::nullopt, bool isCalledAsValue = false)
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
        if (isCalledAsValue) {
            Value function = make(Opcode::BuiltinFunction, m_program.types.function);
            function.immediates = { entry.index(), Format::none };
            call.opcode = Opcode::Call;
            call.children = { append(WTF::move(function)), site.receiver, ValueID() };
            call.immediates[1] = m_inliningDepth;
            call.arguments = original.arguments;
        } else if (takingReceiver) {
            call.immediates[0] = takingReceiver->index();
            call.arguments.append(site.receiver);
            call.arguments.appendVector(original.arguments);
        } else {
            call.children[0] = site.receiver;
            call.immediates[0] = entry.index();
            call.arguments = original.arguments;
        }
        if (auto& yielded = m_builtins.at(isCalledAsValue ? entry : call.builtin()).result; !isCalledAsValue && yielded.kinds == BuiltinResult::Boolean && !yielded.arguments && m_program.at(call.type).isAnything()) {
            call.type = m_program.types.boolean;
            call.rep = repOf(m_program.at(call.type));
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
        m_numberOfValuesLeftBehind.fill(0, m_function.blocks.size());
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
        m_numberOfValuesLeftBehind.append(0);
        if (!m_isColdByText.isEmpty())
            m_isColdByText.append(m_isColdByText[m_blockOfSite.index()]);
        if (!m_isInLoop.isEmpty())
            m_isInLoop.append(m_isInLoop[m_blockOfSite.index()]);
        return BlockID(m_function.blocks.size() - 1);
    }

    Value make(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.position = m_position;
        value.inlineFrame = m_inlineFrame;
        value.isInTailPosition = m_isInTailPosition && (opcode == Opcode::Call || opcode == Opcode::CallMethod || opcode == Opcode::CallDirect || opcode == Opcode::CallBuiltin);
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
        m_isInTailPosition = m_function.at(id).isInTailPosition;
        m_inliningDepth = m_function.at(id).opcode == Opcode::Construct ? Format::none : m_function.at(id).immediates[1];
        m_isWatched = m_function.at(block).terminator.kind == TerminatorKind::Invoke && index + 1 == m_function.at(block).values.size();
        m_after = addBlock();
        {
            auto& from = m_function.at(block);
            unsigned leftBehind = std::exchange(m_numberOfValuesLeftBehind[block.index()], 0);
            ValuesOfBlock ahead;
            ahead.append(from.values.subspan(leftBehind, index - leftBehind));
            m_function.at(m_after).values = std::exchange(from.values, WTF::move(ahead));
            m_numberOfValuesLeftBehind[m_after.index()] = index + 1;
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
    // By block.
    Vector<Refuted> m_refuted;
    Vector<bool> m_holdsDatesAsTheyWere;
    Vector<unsigned> m_numberOfValuesLeftBehind;
    Vector<bool> m_isColdByText;
    Vector<bool> m_isInLoop;
    BlockID m_blockOfSite;
    // Of the value that is being replaced.
    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
    uint32_t m_inliningDepth { Format::none };
    bool m_isInTailPosition { false };
    bool m_isWatched { false };
    Target m_handler;
    BlockID m_after;
    // Where values are added.
    BlockID m_block;
    bool m_lastMayThrow { false };
};

class ReceiverGuardElimination {
public:
    ReceiverGuardElimination(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
        , m_builtins(Builtins::singleton())
    {
    }

    unsigned run()
    {
        collectReceivers();
        if (m_receivers.isEmpty())
            return 0;
        findUses();
        computePredecessors();
        unsigned eliminated = 0;
        for (auto& receiver : m_receivers)
            eliminated += eliminate(receiver);
        return eliminated;
    }

    uint32_t classOfFreshResult()
    {
        uint32_t engineClass = Format::none;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            if (!block.terminator.value)
                return Format::none;
            ValueID allocation = rootOf(block.terminator.value);
            uint32_t allocatedClass = allocatedClassOf(m_function.at(allocation));
            if (allocatedClass == Format::none || (engineClass != Format::none && allocatedClass != engineClass) || !m_builtins.classes()[allocatedClass].canCheckOriginalStructure())
                return Format::none;
            engineClass = allocatedClass;
            ensureReceiver(allocation, allocatedClass).returns.append({ b, static_cast<unsigned>(block.values.size()) });
        }
        if (engineClass == Format::none)
            return Format::none;
        collectReceivers();
        findUses();
        computePredecessors();
        for (auto& receiver : m_receivers) {
            if (receiver.returns.isEmpty())
                continue;
            if (receiver.definition.block == none)
                return Format::none;
            propagateEscapes(receiver);
            for (Place place : receiver.returns) {
                if (isAfterEscape(place))
                    return Format::none;
            }
        }
        return engineClass;
    }

    ValueID allocationOf(ValueID id) { return rootOf(id); }
    uint32_t classOfAllocation(ValueID id) { return allocatedClassOf(m_function.at(rootOf(id))); }

    void removeUnreachableBlocks()
    {
        BitVector isReached;
        Vector<unsigned, 16> worklist { 0u };
        while (!worklist.isEmpty()) {
            unsigned block = worklist.takeLast();
            if (isReached.set(block))
                continue;
            for (auto& target : m_function.blocks[block].terminator.targets)
                worklist.append(target.block.index());
        }
        Value undefinedValue;
        undefinedValue.opcode = Opcode::Undefined;
        undefinedValue.type = m_program.types.undefined;
        undefinedValue.rep = repOf(m_program.at(undefinedValue.type));
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (isReached.get(b))
                continue;
            for (ValueID id : m_function.blocks[b].values)
                m_function.at(id) = undefinedValue;
            for (ValueID id : m_function.blocks[b].parameters)
                m_function.at(id) = undefinedValue;
            m_function.blocks[b] = Block { };
        }
        compactBlocks(m_function);
    }

private:
    static constexpr unsigned none = UINT_MAX;

    struct Place {
        unsigned block { none };
        unsigned index { none };
    };

    struct Guard {
        ValueID id;
        Place place;
        bool holds { true };
        BitVector failureRegion;
    };

    struct Call {
        ValueID id;
        Place place;
    };

    struct Receiver {
        ValueID allocation;
        uint32_t engineClass { Format::none };
        Place definition;
        Vector<Guard, 2> guards;
        Vector<Call, 4> calls;
        Vector<Place, 1> returns;
        Vector<ValueID, 2> casts;
        Vector<Place, 4> escapes;
    };

    bool isGuard(const Value& value) const
    {
        return value.opcode == Opcode::CallBuiltin && value.builtin() == m_builtins.hasOriginalStructureOperation();
    }

    ValueID rootOf(ValueID id)
    {
        if (m_roots.isEmpty())
            m_roots.fill(ValueID(), m_function.values.size());
        ValueID& known = m_roots[id.index()];
        if (known)
            return known;
        ValueID root = id;
        for (;;) {
            root = m_program.originOf(m_function, root);
            auto& value = m_function.at(root);
            if (value.opcode != Opcode::CallBuiltin)
                break;
            auto& result = m_builtins.at(value.builtin()).result;
            ValueID operand;
            if (result.kinds == BuiltinResult::This && !result.arguments)
                operand = value.child(0);
            else if (!result.kinds && hasOneBitSet(result.arguments) && getLSBSet(result.arguments) < value.arguments.size())
                operand = value.arguments[getLSBSet(result.arguments)];
            if (!operand)
                break;
            root = operand;
        }
        m_roots[id.index()] = root;
        return root;
    }

    static bool returnsFreshObject(const Builtin& builtin)
    {
        if (builtin.kind == BuiltinKind::Constructor || builtin.implementation.kind == BuiltinImplementation::Prelude)
            return true;
        if (builtin.implementation.kind != BuiltinImplementation::Inline)
            return false;
        switch (builtin.implementation.inlined) {
        case InlineBuiltin::ArrayOf:
        case InlineBuiltin::ArrayWithLength:
        case InlineBuiltin::ArrayFrom:
        case InlineBuiltin::NewArrayWithCapacity:
        case InlineBuiltin::NewArrayWithSpecies:
        case InlineBuiltin::NewContiguousArray:
        case InlineBuiltin::NewDoubleArray:
        case InlineBuiltin::TruncateNewArray:
        case InlineBuiltin::StringSplit:
        case InlineBuiltin::ObjectEntries:
        case InlineBuiltin::ObjectGetOwnPropertyNames:
        case InlineBuiltin::ObjectGetOwnPropertySymbols:
        case InlineBuiltin::ObjectKeys:
        case InlineBuiltin::ObjectValues:
        case InlineBuiltin::ReflectOwnKeys:
        case InlineBuiltin::RegExpNew:
            return true;
        default:
            return false;
        }
    }

    uint32_t allocatedClassOf(const Value& value) const
    {
        auto classOf = [&](BuiltinID entry) -> uint32_t {
            auto& builtin = m_builtins.at(entry);
            if (builtin.result.kinds != BuiltinResult::Instance || builtin.result.arguments || !returnsFreshObject(builtin))
                return Format::none;
            return builtin.result.engineClass;
        };
        switch (value.opcode) {
        case Opcode::NewArray:
        case Opcode::CreateRest:
            return m_builtins.arrayClass().index;
        case Opcode::CallBuiltin:
            return m_builtins.at(value.builtin()).implementation.kind == BuiltinImplementation::Prelude ? Format::none : classOf(value.builtin());
        case Opcode::CallDirect:
            for (unsigned i = 0; i < m_program.preludeFunctions.size(); ++i) {
                if (m_program.preludeFunctions[i] != value.function())
                    continue;
                BuiltinID entry = m_builtins.entryCarriedOutBy(static_cast<PreludeFunction>(i));
                return entry ? classOf(entry) : Format::none;
            }
            if (value.function().index() < m_program.classesOfFreshResults.size() && !value.constructs && m_program.classesOfFreshResults[value.function().index()] != UINT16_MAX)
                return m_program.classesOfFreshResults[value.function().index()];
            return Format::none;
        default:
            return Format::none;
        }
    }

    void collectReceivers()
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& values = m_function.blocks[b].values;
            for (unsigned i = 0; i < values.size(); ++i) {
                auto& value = m_function.at(values[i]);
                if (value.opcode != Opcode::CallBuiltin || value.engineClass() == Format::none || !m_builtins.classes()[value.engineClass()].canCheckOriginalStructure())
                    continue;
                bool isGuardValue = isGuard(value);
                if (!isGuardValue && !hasGenericFallback(m_program, m_function, value))
                    continue;
                ValueID allocation = rootOf(isGuardValue ? value.arguments[0] : value.child(0));
                if (allocatedClassOf(m_function.at(allocation)) != value.engineClass())
                    continue;
                if (isGuardValue)
                    ensureReceiver(allocation, value.engineClass()).guards.append(Guard { values[i], { b, i }, true, { } });
                else
                    ensureReceiver(allocation, value.engineClass()).calls.append(Call { values[i], { b, i } });
            }
        }
    }

    Receiver& ensureReceiver(ValueID allocation, uint32_t engineClass)
    {
        if (m_receiverIndex.isEmpty())
            m_receiverIndex.fill(none, m_function.values.size());
        unsigned& number = m_receiverIndex[allocation.index()];
        if (number == none) {
            number = m_receivers.size();
            m_receivers.append(Receiver { allocation, engineClass, { }, { }, { }, { }, { }, { } });
        }
        return m_receivers[number];
    }

    bool doesNotEscapeThrough(ValueID id, const Value& user, const Receiver& receiver)
    {
        auto aliasesReceiver = [&](ValueID operand) {
            return operand && rootOf(operand) == receiver.allocation;
        };
        switch (user.opcode) {
        case Opcode::Cast:
            return rootOf(id) == receiver.allocation;
        case Opcode::GetById:
        case Opcode::InById:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
        case Opcode::IsUndefinedOrNull:
        case Opcode::IsInstanceOfClass:
        case Opcode::IteratorOpen:
            return true;
        case Opcode::CallMethod: {
            for (ValueID argument : user.arguments) {
                if (aliasesReceiver(argument))
                    return false;
            }
            BuiltinName name = m_program.builtinName(user.string());
            BuiltinID entry = name ? m_builtins.findForInstance(BuiltinKind::Method, m_builtins.classes()[receiver.engineClass], name) : BuiltinID();
            if (!entry)
                return false;
            uint16_t uses = 0;
            for (; entry; entry = m_builtins.next(entry))
                uses |= m_builtins.at(entry).usesOfThis;
            return !(uses & (OperandUse::WritesProperties | OperandUse::Keeps | OperandUse::Calls | OperandUse::MayBeResult | OperandUse::HandsToUserCode));
        }
        case Opcode::GetByVal:
            return !aliasesReceiver(user.child(1));
        case Opcode::PutByVal: {
            auto& key = m_program.typeOf(m_function, user.child(1));
            return receiver.engineClass == m_builtins.arrayClass().index && !aliasesReceiver(user.child(1)) && !aliasesReceiver(user.child(2)) && key.isInteger() && key.integers.isWithin(Range::length());
        }
        case Opcode::CallBuiltin: {
            if (isGuard(user))
                return true;
            auto& builtin = m_builtins.at(user.builtin());
            if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::AwaitBySavingFrame)
                return !std::ranges::any_of(user.arguments.subspan(0, 2), aliasesReceiver);
            if (aliasesReceiver(user.child(1)) || aliasesReceiver(user.child(2)) || (aliasesReceiver(user.child(0)) && !builtin.takesReceiver()))
                return false;
            uint16_t uses = aliasesReceiver(user.child(0)) ? builtin.usesOfThis : 0;
            bool isPassedAsArgument = false;
            for (unsigned i = 0; i < user.arguments.size(); ++i) {
                if (!aliasesReceiver(user.arguments[i]))
                    continue;
                isPassedAsArgument = true;
                uses |= builtin.usesOfArgument(i);
            }
            if (uses & (OperandUse::WritesProperties | OperandUse::Keeps | OperandUse::Calls | OperandUse::HandsToUserCode))
                return false;
            if (uses & OperandUse::MayBeResult && rootOf(id) != receiver.allocation)
                return false;
            if (!(uses & OperandUse::WritesContents))
                return true;
            if (builtin.kind == BuiltinKind::Operation)
                return true;
            return builtin.takesReceiver() && !isPassedAsArgument && user.engineClass() == receiver.engineClass;
        }
        default:
            return false;
        }
    }

    void findUses()
    {
        Vector<unsigned, 4> used;
        auto note = [&](ValueID operand) {
            unsigned number = m_receiverIndex[rootOf(operand).index()];
            if (number != none && !used.contains(number))
                used.append(number);
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                if (unsigned number = m_receiverIndex[id.index()]; number != none)
                    m_receivers[number].definition = { b, i };
                used.shrink(0);
                std::as_const(m_function.at(id)).forEachChild(note);
                for (unsigned number : used) {
                    if (!doesNotEscapeThrough(id, m_function.at(id), m_receivers[number]))
                        m_receivers[number].escapes.append({ b, i });
                    else if (m_function.at(id).opcode == Opcode::Cast)
                        m_receivers[number].casts.append(id);
                }
            }
            used.shrink(0);
            block.terminator.forEachChild(note);
            for (unsigned number : used)
                m_receivers[number].escapes.append({ b, static_cast<unsigned>(block.values.size()) });
        }
    }

    void computePredecessors()
    {
        m_predecessors.resize(m_function.blocks.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (auto& target : m_function.blocks[b].terminator.targets)
                m_predecessors[target.block.index()].append(b);
        }
    }

    void computeFailureRegion(Guard& guard)
    {
        auto& terminator = m_function.blocks[guard.place.block].terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.value != guard.id || terminator.targets[0].block == terminator.targets[1].block)
            return;
        unsigned first = terminator.targets[1].block.index();
        if (m_predecessors[first].size() != 1)
            return;
        auto& region = guard.failureRegion;
        region.set(first);
        Vector<unsigned, 8> worklist { first };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned next = target.block.index();
                if (region.get(next))
                    continue;
                bool isWithin = true;
                for (unsigned predecessor : m_predecessors[next])
                    isWithin &= region.get(predecessor);
                if (!isWithin)
                    continue;
                region.set(next);
                worklist.append(next);
            }
        }
    }

    bool isAfterEscape(Place place) const
    {
        if (m_isReachableAfterEscape.get(place.block))
            return true;
        for (Place escape : m_escapes) {
            if (escape.block == place.block && escape.index < place.index)
                return true;
        }
        return false;
    }

    void propagateEscapes(Receiver& receiver)
    {
        for (auto& guard : receiver.guards)
            computeFailureRegion(guard);
        for (bool changed = true; changed;) {
            changed = false;
            m_isReachableAfterEscape.clearAll();
            m_escapes.shrink(0);
            Vector<unsigned, 8> worklist;
            auto pushSuccessors = [&](unsigned block) {
                for (auto& target : m_function.blocks[block].terminator.targets)
                    worklist.append(target.block.index());
            };
            for (Place escape : receiver.escapes) {
                bool isInFailureRegion = false;
                for (auto& guard : receiver.guards)
                    isInFailureRegion |= guard.holds && guard.failureRegion.get(escape.block);
                if (isInFailureRegion)
                    continue;
                m_escapes.append(escape);
                pushSuccessors(escape.block);
            }
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                if (block == receiver.definition.block || m_isReachableAfterEscape.set(block))
                    continue;
                pushSuccessors(block);
            }
            for (auto& guard : receiver.guards) {
                if (!guard.holds || !isAfterEscape(guard.place))
                    continue;
                guard.holds = false;
                changed = true;
            }
        }
    }

    unsigned eliminate(Receiver& receiver)
    {
        if (receiver.definition.block == none)
            return 0;
        propagateEscapes(receiver);
        unsigned eliminated = 0;
        for (auto& guard : receiver.guards) {
            if (!guard.holds)
                continue;
            auto& value = m_function.at(guard.id);
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                value.receiverHasOriginalStructure = true;
                continue;
            }
            value.opcode = Opcode::Boolean;
            value.children = { };
            value.arguments.clear();
            value.immediates = { 1, Format::none };
            if (auto& terminator = m_function.blocks[guard.place.block].terminator; terminator.kind == TerminatorKind::Branch && terminator.value == guard.id) {
                terminator.kind = TerminatorKind::Jump;
                terminator.value = ValueID();
                terminator.targets.shrink(1);
            }
            ++eliminated;
        }
        for (auto& call : receiver.calls) {
            if (isAfterEscape(call.place))
                continue;
            auto& value = m_function.at(call.id);
            value.receiverHasOriginalStructure = true;
            ValueID read = std::exchange(value.children[1], ValueID());
            if (!read || m_function.at(read).opcode != Opcode::GetById || m_function.at(read).readsMethodFor() != value.builtin())
                continue;
            Value undefinedValue;
            undefinedValue.opcode = Opcode::Undefined;
            undefinedValue.position = m_function.at(read).position;
            undefinedValue.inlineFrame = m_function.at(read).inlineFrame;
            undefinedValue.type = m_program.types.undefined;
            undefinedValue.rep = repOf(m_program.at(undefinedValue.type));
            m_function.at(read) = WTF::move(undefinedValue);
            ++eliminated;
        }
        for (ValueID cast : receiver.casts) {
            auto* engineClass = m_program.at(m_function.at(cast).type).onlyEngineClass();
            m_function.at(cast).operandClassIsProven = engineClass && engineClass->index == receiver.engineClass;
        }
        return eliminated;
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    Vector<ValueID> m_roots;
    Vector<unsigned> m_receiverIndex;
    Vector<Receiver, 2> m_receivers;
    Vector<Vector<unsigned, 2>> m_predecessors;
    BitVector m_isReachableAfterEscape;
    Vector<Place, 4> m_escapes;
};

struct ModuleVariableFacts {
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr uint16_t isNeverStored = UINT16_MAX - 1;
    static constexpr uint16_t escapes = UINT16_MAX;

    bool isCandidate(uint32_t variable) const { return classes[variable] < isNeverStored; }

    Vector<uint16_t> classes;
    BitVector mayHoldNil;
    Vector<uint32_t> variablesReturned;
    Vector<BuiltinID> methodsCarriedOut;
};

class ModuleVariableAliases {
public:
    static constexpr uint32_t none = ModuleVariableFacts::none;

    ModuleVariableAliases(const Program& program, FunctionID id, const ModuleVariableFacts& facts)
        : m_program(program)
        , m_id(id)
        , m_function(program.at(id))
        , m_facts(facts)
        , m_builtins(Builtins::singleton())
    {
    }

    bool find()
    {
        static constexpr OpcodeSet sources { Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::CallDirect };
        m_program.functions.forEachValueInOrder(m_id.index(), sources, [&](ValueID id) {
            auto& value = m_function.at(id);
            uint32_t variable = value.opcode == Opcode::CallDirect ? m_facts.variablesReturned[value.function().index()] : value.variable().index();
            if (variable == none || !m_facts.isCandidate(variable))
                return;
            ValueID alias = value.opcode == Opcode::PutModuleVariable ? allocationStoredBy(value) : id;
            if (!typeOf(alias).hasObjects())
                return;
            if (m_variableOf.isEmpty())
                m_variableOf.fill(none, m_function.values.size());
            add(alias, variable);
            if (value.opcode == Opcode::GetModuleVariable && (m_program.variables[variable].startsUndefined || m_program.variables[variable].startsZero || m_facts.mayHoldNil.get(variable)))
                m_mayBeNil.set(id.index());
        });
        if (m_variableOf.isEmpty())
            return false;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (ValueID source = sameObjectAs(id); source && variableOf(source) != none)
                        changed |= add(id, variableOf(source));
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (variableOf(target.arguments[i]) != none)
                            changed |= add(parameters[i], variableOf(target.arguments[i]));
                    }
                }
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    ValueID source = sameObjectAs(id);
                    if (!source || variableOf(source) == none)
                        continue;
                    if (m_mayBeAnotherObject.get(source.index()))
                        changed |= !m_mayBeAnotherObject.set(id.index());
                    if (m_mayBeOtherPrimitive.get(source.index()))
                        changed |= !m_mayBeOtherPrimitive.set(id.index());
                    if (m_mayBeNil.get(source.index()) && m_function.at(id).opcode == Opcode::Cast && typeOf(id).mayBeUndefinedOrNull())
                        changed |= !m_mayBeNil.set(id.index());
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (variableOf(parameters[i]) == none)
                            continue;
                        ValueID argument = target.arguments[i];
                        bool isAlias = variableOf(argument) != none;
                        auto& type = typeOf(argument);
                        if (isAlias ? m_mayBeAnotherObject.get(argument.index()) : type.hasObjects())
                            changed |= !m_mayBeAnotherObject.set(parameters[i].index());
                        if (isAlias ? m_mayBeOtherPrimitive.get(argument.index()) : !!(type.primitives & ~Primitive::UndefinedOrNull))
                            changed |= !m_mayBeOtherPrimitive.set(parameters[i].index());
                        if (isAlias ? m_mayBeNil.get(argument.index()) : type.mayBeUndefinedOrNull())
                            changed |= !m_mayBeNil.set(parameters[i].index());
                    }
                }
            }
        }
        return true;
    }

    void forEachEscape(const Invocable<void(uint32_t)> auto& functor)
    {
        for (uint32_t variable : m_variablesOfOneValue)
            functor(variable);
        computeFailureRegions();
        Vector<uint32_t, 4> used;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            auto isReached = [&](uint32_t variable) {
                return !m_failureRegions.containsIf([&](auto& region) {
                    return region.variable == variable && region.blocks.get(b);
                });
            };
            for (ValueID id : block.values) {
                used.shrink(0);
                m_function.at(id).forEachChild([&](ValueID operand) {
                    if (uint32_t variable = variableOf(operand); variable != none && !used.contains(variable))
                        used.append(variable);
                });
                for (uint32_t variable : used) {
                    if (isReached(variable) && !isHarmless(id, variable))
                        functor(variable);
                }
            }
            auto& terminator = block.terminator;
            uint32_t variable = terminator.value ? variableOf(terminator.value) : none;
            if (variable == none || terminator.kind == TerminatorKind::Branch || !isReached(variable))
                continue;
            if (terminator.kind != TerminatorKind::Return || m_facts.variablesReturned[m_id.index()] != variable)
                functor(variable);
        }
    }

    std::pair<unsigned, unsigned> eliminate(Function& function)
    {
        ASSERT(&function == &m_function);
        unsigned eliminated = 0;
        unsigned numberOfCalls = 0;
        auto isProven = [&](ValueID operand, uint32_t engineClass) {
            if (!operand || variableOf(operand) == none || m_mayBeAnotherObject.get(operand.index()) || m_facts.classes[variableOf(operand)] != engineClass)
                return false;
            return !typeOf(operand).primitives || (!m_mayBeNil.get(operand.index()) && !m_mayBeOtherPrimitive.get(operand.index()));
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::Cast) {
                    auto* engineClass = m_program.at(value.type).onlyEngineClass();
                    if (engineClass && isProven(value.child(0), engineClass->index))
                        value.operandClassIsProven = true;
                    continue;
                }
                if (value.opcode != Opcode::CallBuiltin || value.engineClass() == Format::none)
                    continue;
                if (value.builtin() == m_builtins.hasOriginalStructureOperation()) {
                    if (!isProven(value.arguments[0], value.engineClass()))
                        continue;
                    if (Options::validateAOTTypedFacts()) [[unlikely]] {
                        value.receiverHasOriginalStructure = true;
                        continue;
                    }
                    value.opcode = Opcode::Boolean;
                    value.children = { };
                    value.arguments.clear();
                    value.immediates = { 1, Format::none };
                    if (auto& terminator = block.terminator; terminator.kind == TerminatorKind::Branch && terminator.value == id) {
                        terminator.kind = TerminatorKind::Jump;
                        terminator.value = ValueID();
                        terminator.targets.shrink(1);
                    }
                    ++eliminated;
                    continue;
                }
                if (value.receiverHasOriginalStructure || !isProven(value.child(0), value.engineClass()) || !hasGenericFallback(m_program, function, value))
                    continue;
                value.receiverHasOriginalStructure = true;
                ++numberOfCalls;
                ValueID read = std::exchange(value.children[1], ValueID());
                if (!read || function.at(read).opcode != Opcode::GetById || function.at(read).readsMethodFor() != value.builtin())
                    continue;
                Value undefinedValue;
                undefinedValue.opcode = Opcode::Undefined;
                undefinedValue.position = function.at(read).position;
                undefinedValue.inlineFrame = function.at(read).inlineFrame;
                undefinedValue.type = m_program.types.undefined;
                undefinedValue.rep = repOf(m_program.at(undefinedValue.type));
                function.at(read) = WTF::move(undefinedValue);
                ++eliminated;
            }
        }
        return { eliminated, numberOfCalls };
    }

private:
    struct FailureRegion {
        uint32_t variable;
        BitVector blocks;
    };

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    uint32_t variableOf(ValueID id) const { return m_variableOf[id.index()]; }

    bool add(ValueID id, uint32_t variable)
    {
        uint32_t& known = m_variableOf[id.index()];
        if (known == variable)
            return false;
        if (known == none) {
            known = variable;
            return true;
        }
        if (!m_variablesOfOneValue.contains(known))
            m_variablesOfOneValue.append(known);
        if (!m_variablesOfOneValue.contains(variable))
            m_variablesOfOneValue.append(variable);
        return false;
    }

    ValueID sameObjectAs(ValueID id) const
    {
        auto& value = m_function.at(id);
        if (value.opcode == Opcode::Cast)
            return m_program.originOf(m_function, id) == id ? ValueID() : value.child(0);
        if (value.opcode != Opcode::CallBuiltin)
            return { };
        auto& result = m_builtins.at(value.builtin()).result;
        if (result.kinds == BuiltinResult::This && !result.arguments)
            return value.child(0);
        if (!result.kinds && hasOneBitSet(result.arguments) && getLSBSet(result.arguments) < value.arguments.size())
            return value.arguments[getLSBSet(result.arguments)];
        return { };
    }

    ValueID allocationStoredBy(const Value& store) const
    {
        ValueID id = store.child(0);
        while (ValueID source = sameObjectAs(id))
            id = source;
        return id;
    }

    void computeFailureRegions()
    {
        Vector<Vector<unsigned, 2>> predecessors;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            if (terminator.kind != TerminatorKind::Branch || terminator.targets[0].block == terminator.targets[1].block)
                continue;
            auto& guard = m_function.at(terminator.value);
            if (guard.opcode != Opcode::CallBuiltin || guard.builtin() != m_builtins.hasOriginalStructureOperation())
                continue;
            uint32_t variable = variableOf(guard.arguments[0]);
            if (variable == none || m_facts.classes[variable] != guard.engineClass())
                continue;
            if (predecessors.isEmpty()) {
                predecessors.resize(m_function.blocks.size());
                for (unsigned from = 0; from < m_function.blocks.size(); ++from) {
                    for (auto& target : m_function.blocks[from].terminator.targets)
                        predecessors[target.block.index()].append(from);
                }
            }
            unsigned first = terminator.targets[1].block.index();
            if (predecessors[first].size() != 1)
                continue;
            BitVector region;
            region.set(first);
            Vector<unsigned, 8> worklist { first };
            while (!worklist.isEmpty()) {
                for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                    unsigned next = target.block.index();
                    if (region.get(next))
                        continue;
                    bool isWithin = true;
                    for (unsigned predecessor : predecessors[next])
                        isWithin &= region.get(predecessor);
                    if (!isWithin)
                        continue;
                    region.set(next);
                    worklist.append(next);
                }
            }
            m_failureRegions.append({ variable, WTF::move(region) });
        }
    }

    bool isKeptOnlyByIteratorItYields(const Builtin& builtin) const
    {
        return builtin.kind == BuiltinKind::Method && builtin.usesOfThis == OperandUse::Keeps && (builtin.name == "entries"_s || builtin.name == "keys"_s || builtin.name == "values"_s || builtin.name == "@@iterator"_s);
    }

    bool readsUnnamedArguments(FunctionID id) const
    {
        return id && m_program.at(id).values.containsIf([&](auto& value) {
            switch (value.opcode) {
            case Opcode::GetArgument:
            case Opcode::CreateRest:
            case Opcode::GetRestLength:
            case Opcode::GetMyArgumentByVal:
            case Opcode::CallForwardVarargs:
            case Opcode::ConstructSuperForwardVarargs:
                return true;
            case Opcode::Operation:
            case Opcode::CallBuiltin:
                return languageOperationOf(m_program, value) == LanguageOperation::CreateArguments || languageOperationOf(m_program, value) == LanguageOperation::DirectEval;
            default:
                return false;
            }
        });
    }

    bool takesAtMostTwoArguments(ValueID id) const
    {
        auto& callback = m_function.at(m_program.originOf(m_function, id));
        if (callback.opcode != Opcode::NewClosure && callback.opcode != Opcode::FunctionReference)
            return false;
        auto& callee = m_program.at(callback.function());
        if (callee.blocks.isEmpty() || callee.parameterTypes.size() > 2 || callee.declaredAs || callee.resumeFunction || callee.startFunction || callee.has(Format::FunctionHasRestParameter | Format::FunctionIsSloppy))
            return false;
        FunctionID generalEntry = m_program.genericEntrypoints.get(callback.function().index());
        for (FunctionID body : { callback.function(), generalEntry, generalEntry ? m_program.at(generalEntry).valueEntry : FunctionID(), callee.rampForClosures, callee.writtenEntry, callee.secondBody, callee.worker }) {
            if (readsUnnamedArguments(body))
                return false;
        }
        return true;
    }

    bool isHarmless(ValueID id, uint32_t variable) const
    {
        auto& user = m_function.at(id);
        uint32_t engineClass = m_facts.classes[variable];
        auto aliases = [&](ValueID operand) {
            return operand && variableOf(operand) == variable;
        };
        constexpr uint16_t harmfulUses = OperandUse::WritesProperties | OperandUse::Keeps | OperandUse::Calls | OperandUse::HandsToUserCode;
        switch (user.opcode) {
        case Opcode::Cast:
            return !!sameObjectAs(id);
        case Opcode::GetById:
        case Opcode::InById:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
        case Opcode::IsUndefinedOrNull:
        case Opcode::IsInstanceOfClass:
        case Opcode::IteratorOpen:
            return true;
        case Opcode::PutModuleVariable:
            return user.variable().index() == variable;
        case Opcode::CallMethod: {
            if (std::ranges::any_of(user.arguments, aliases))
                return false;
            BuiltinName name = m_program.builtinName(user.string());
            BuiltinID entry = name ? m_builtins.findForInstance(BuiltinKind::Method, m_builtins.classes()[engineClass], name) : BuiltinID();
            if (!entry)
                return false;
            uint16_t uses = 0;
            for (; entry; entry = m_builtins.next(entry))
                uses |= isKeptOnlyByIteratorItYields(m_builtins.at(entry)) ? 0 : m_builtins.at(entry).usesOfThis;
            return !(uses & (harmfulUses | OperandUse::MayBeResult));
        }
        case Opcode::GetByVal:
            return !aliases(user.child(1));
        case Opcode::PutByVal: {
            auto& key = typeOf(user.child(1));
            return engineClass == m_builtins.arrayClass().index && !aliases(user.child(1)) && !aliases(user.child(2)) && key.isInteger() && key.integers.isWithin(Range::length());
        }
        case Opcode::CallBuiltin: {
            if (user.builtin() == m_builtins.hasOriginalStructureOperation())
                return true;
            auto& builtin = m_builtins.at(user.builtin());
            if (builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::AwaitBySavingFrame)
                return !std::ranges::any_of(user.arguments.subspan(0, 2), aliases);
            if (aliases(user.child(1)) || aliases(user.child(2)) || (aliases(user.child(0)) && !builtin.takesReceiver()))
                return false;
            bool isPassedAsArgument = false;
            uint16_t uses = 0;
            for (unsigned i = 0; i < user.arguments.size(); ++i) {
                if (!aliases(user.arguments[i]))
                    continue;
                isPassedAsArgument = true;
                uses |= builtin.usesOfArgument(i);
            }
            if (aliases(user.child(0)))
                uses |= isKeptOnlyByIteratorItYields(builtin) ? 0 : builtin.usesOfThis;
            if (uses & harmfulUses)
                return false;
            if (uses & OperandUse::MayBeResult && !aliases(sameObjectAs(id)))
                return false;
            if (!(uses & OperandUse::WritesContents) || builtin.kind == BuiltinKind::Operation)
                return true;
            return builtin.takesReceiver() && !isPassedAsArgument && user.engineClass() == engineClass;
        }
        case Opcode::Call: {
            if (aliases(user.child(0)) || aliases(user.child(1)) || !takesAtMostTwoArguments(user.child(0)))
                return false;
            return !std::ranges::any_of(user.arguments.subspan(0, std::min<size_t>(2, user.arguments.size())), aliases);
        }
        case Opcode::CallDirect: {
            if (user.constructs || user.arguments.isEmpty() || std::ranges::any_of(user.children, aliases) || std::ranges::any_of(user.arguments.subspan(1), aliases))
                return false;
            BuiltinID entry = m_facts.methodsCarriedOut[user.function().index()];
            if (!entry)
                return false;
            auto& builtin = m_builtins.at(entry);
            auto* holder = m_builtins.classNamed(builtin.holderName);
            if (!holder || holder->index != engineClass || builtin.usesOfThis & (harmfulUses & ~OperandUse::HandsToUserCode) || builtin.usesOfThis & OperandUse::MayBeResult)
                return false;
            return !(builtin.usesOfThis & OperandUse::HandsToUserCode) || (user.arguments.size() > 1 && takesAtMostTwoArguments(user.arguments[1]));
        }
        default:
            return false;
        }
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    const ModuleVariableFacts& m_facts;
    const Builtins& m_builtins;
    Vector<uint32_t> m_variableOf;
    BitVector m_mayBeAnotherObject;
    BitVector m_mayBeOtherPrimitive;
    BitVector m_mayBeNil;
    Vector<uint32_t, 2> m_variablesOfOneValue;
    Vector<FailureRegion, 2> m_failureRegions;
};

} // anonymous namespace

unsigned callGuardedPreludeEntries(const Program& program, FunctionID id, Function& function, ReceiversThatMayYetBeProven receiversThatMayYetBeProven)
{
    if (program.guardedPreludeEntries.isEmpty() || function.isGuardedPreludeEntry || program.isHotOutsideItsLoops(id) || function.kind == FunctionKind::Module)
        return 0;
    Vector<FunctionID> entriesWanted = std::exchange(function.guardedPreludeEntriesWantedLater, { });
    auto& builtins = Builtins::singleton();
    Vector<unsigned> numberOfPredecessors;
    numberOfPredecessors.fill(0, function.blocks.size());
    for (auto& block : function.blocks) {
        for (auto& target : block.terminator.targets)
            ++numberOfPredecessors[target.block.index()];
    }
    auto withoutCasts = [&](ValueID value) {
        while (function.at(value).opcode == Opcode::Cast)
            value = function.at(value).child(0);
        return value;
    };
    auto isRematerializedInEachArm = [&](Opcode opcode) {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
            return true;
        default:
            return false;
        }
    };
    auto isSame = [&](ValueID left, ValueID right) {
        left = withoutCasts(left);
        right = withoutCasts(right);
        if (left == right)
            return true;
        auto& one = function.at(left);
        auto& other = function.at(right);
        if (one.opcode != other.opcode || !isRematerializedInEachArm(one.opcode) || one.immediates != other.immediates || one.type != other.type || one.arguments.size() != other.arguments.size())
            return false;
        for (unsigned i = 0; i < one.children.size(); ++i) {
            if (!!one.children[i] != !!other.children[i] || (one.children[i] && withoutCasts(one.children[i]) != withoutCasts(other.children[i])))
                return false;
        }
        for (unsigned i = 0; i < one.arguments.size(); ++i) {
            if (withoutCasts(one.arguments[i]) != withoutCasts(other.arguments[i]))
                return false;
        }
        return true;
    };
    auto holdsEnvironment = [&](ValueID value) {
        return value && std::ranges::any_of(program.typeOf(function, value).views, [&](LayoutID view) {
            return program.at(view).kind == LayoutKind::Environment;
        });
    };
    auto capturesEnvironment = [&](const Value& closure) {
        return std::ranges::any_of(closure.children, holdsEnvironment) || std::ranges::any_of(closure.arguments, holdsEnvironment);
    };
    auto mayBeReadFromModuleVariable = [&](ValueID value) {
        value = withoutCasts(value);
        if (function.at(value).opcode == Opcode::GetModuleVariable)
            return true;
        if (function.at(value).opcode != Opcode::Parameter)
            return false;
        if (auto& parameter = function.at(value); !parameter.parameterBlock().index() && parameter.parameterIndex() < function.blocks[0].parameters.size() && function.blocks[0].parameters[parameter.parameterIndex()] == value)
            return false;
        for (unsigned join = 1; join < function.blocks.size(); ++join) {
            size_t index = function.blocks[join].parameters.span().size();
            for (size_t i = 0; i < function.blocks[join].parameters.size(); ++i) {
                if (function.blocks[join].parameters[i] == value)
                    index = i;
            }
            if (index == function.blocks[join].parameters.size())
                continue;
            for (auto& other : function.blocks) {
                for (auto& target : other.terminator.targets) {
                    if (target.block.index() == join && function.at(withoutCasts(target.arguments[index])).opcode == Opcode::GetModuleVariable)
                        return true;
                }
            }
            return false;
        }
        return false;
    };
    Vector<unsigned> numbersOfUses;
    auto numberOfUsesOf = [&](ValueID value) {
        if (numbersOfUses.isEmpty()) {
            numbersOfUses.fill(0, function.values.size());
            auto count = [&](ValueID used) {
                if (used)
                    ++numbersOfUses[used.index()];
            };
            for (auto& other : function.blocks) {
                for (ValueID user : other.values)
                    std::as_const(function.at(user)).forEachChild(count);
                other.terminator.forEachChild(count);
            }
        }
        return numbersOfUses[value.index()];
    };
    auto forgetUsesBy = [&](auto& user) {
        if (numbersOfUses.isEmpty())
            return;
        user.forEachChild([&](ValueID used) {
            if (used)
                --numbersOfUses[used.index()];
        });
    };
    auto countUsesBy = [&](auto& user) {
        if (numbersOfUses.isEmpty())
            return;
        user.forEachChild([&](ValueID used) {
            if (used)
                ++numbersOfUses[used.index()];
        });
    };
    std::optional<BitVector> isInLoop;
    unsigned changed = 0;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& block = function.blocks[b];
        if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.value != block.values.last())
            continue;
        auto& guard = function.at(block.terminator.value);
        if (guard.opcode != Opcode::CallBuiltin || guard.builtin() != builtins.hasOriginalStructureOperation() || guard.arguments.size() != 1)
            continue;
        auto& fastEdge = block.terminator.targets[0];
        auto& slowEdge = block.terminator.targets[1];
        if (!fastEdge.arguments.isEmpty() || !slowEdge.arguments.isEmpty() || fastEdge.block == slowEdge.block || numberOfPredecessors[fastEdge.block.index()] != 1 || numberOfPredecessors[slowEdge.block.index()] != 1)
            continue;
        auto& slow = function.at(slowEdge.block);
        auto unwindsAsSlowPathDoes = [&](const Terminator& terminator) {
            return terminator.kind == TerminatorKind::Invoke && slow.terminator.kind == TerminatorKind::Invoke && terminator.targets[1].block == slow.terminator.targets[1].block && std::ranges::equal(terminator.targets[1].arguments, slow.terminator.targets[1].arguments);
        };
        Vector<BlockID, 1> blocksOfCasts;
        BlockID fastBlock = fastEdge.block;
        while (unwindsAsSlowPathDoes(function.at(fastBlock).terminator) && function.at(fastBlock).terminator.targets[0].arguments.isEmpty() && numberOfPredecessors[function.at(fastBlock).terminator.targets[0].block.index()] == 1
            && std::ranges::all_of(function.at(fastBlock).values, [&](ValueID value) { return function.at(value).opcode == Opcode::Cast; })) {
            blocksOfCasts.append(fastBlock);
            fastBlock = function.at(fastBlock).terminator.targets[0].block;
        }
        auto& fast = function.at(fastBlock);
        if (fast.terminator.kind == TerminatorKind::Jump ? slow.terminator.kind != TerminatorKind::Jump : !unwindsAsSlowPathDoes(fast.terminator))
            continue;
        if (fast.terminator.targets[0].block != slow.terminator.targets[0].block || slow.values.isEmpty() || fast.values.isEmpty())
            continue;
        ValueID receiver = guard.arguments[0];
        ValueID genericCall = slow.values.last();
        ValueID directCall = fast.values.last();
        auto& generic = function.at(genericCall);
        auto& call = function.at(directCall);
        if (generic.arguments.size() > Program::maxArgumentsOfGuardedPreludeEntry || call.opcode != Opcode::CallDirect || generic.isInTailPosition)
            continue;
        bool holdsResultToType = generic.type != program.types.anything;
        if (holdsResultToType ? generic.rep != call.rep || call.type != program.at(call.function()).resultType || !program.at(generic.type).isWithin(program.at(call.type), program) : call.rep != Rep::JSValue && call.rep != Rep::Cell)
            continue;
        bool isResolved = generic.opcode == Opcode::CallBuiltin;
        if (isResolved) {
            auto& builtin = builtins.at(generic.builtin());
            if (!holdsResultToType || builtin.kind != BuiltinKind::Method || builtin.implementation.kind != BuiltinImplementation::Prelude || program.preludeFunctions[static_cast<unsigned>(builtin.implementation.prelude)] != call.function())
                continue;
            if (generic.child(0) != receiver || generic.child(1) || generic.child(2))
                continue;
        }
        ValueID read;
        if (generic.opcode == Opcode::Call) {
            read = generic.child(0);
            if (function.at(read).opcode != Opcode::GetById || function.at(read).child(0) != receiver || generic.child(1) != receiver)
                continue;
            bool isFound = false;
            bool isUnobservable = true;
            for (unsigned i = block.values.size() - 1; i-- && !isFound && isUnobservable;) {
                isFound = block.values[i] == read;
                switch (function.at(block.values[i]).opcode) {
                case Opcode::Undefined:
                case Opcode::Null:
                case Opcode::Boolean:
                case Opcode::Integer:
                case Opcode::Double:
                case Opcode::String:
                case Opcode::BigInt:
                case Opcode::FunctionReference:
                case Opcode::NewClosure:
                    break;
                default:
                    isUnobservable = isFound;
                }
            }
            if (!isFound || numberOfUsesOf(read) != 1)
                continue;
        } else if (!isResolved && (generic.opcode != Opcode::CallMethod || generic.child(0) != receiver))
            continue;
        if (call.arguments.size() <= generic.arguments.size() || withoutCasts(call.arguments[0]) != withoutCasts(receiver))
            continue;
        bool matches = true;
        std::optional<unsigned> numberOfCapturesOfCopiedArgument;
        for (unsigned i = 0; i < generic.arguments.size(); ++i) {
            ValueID argument = generic.arguments[i];
            if (function.at(argument).opcode != Opcode::NewClosure)
                continue;
            unsigned usesByTheTwoCalls = 1 + (i + 1 < call.arguments.size() && call.arguments[i + 1] == argument);
            if (numberOfUsesOf(argument) != usesByTheTwoCalls)
                continue;
            matches &= !i && !capturesEnvironment(function.at(argument)) && program.makesSlimClosures(program.at(function.at(argument).function()));
            numberOfCapturesOfCopiedArgument = function.at(argument).arguments.size();
        }
        FunctionID entry = program.guardedPreludeEntries.get(Program::keyOfGuardedPreludeEntry(call.function(), generic.arguments.size(), matches ? numberOfCapturesOfCopiedArgument : std::nullopt, holdsResultToType));
        if (!entry || !matches)
            continue;
        for (unsigned i = 0; i + 1 < fast.values.size(); ++i) {
            Opcode opcode = function.at(fast.values[i]).opcode;
            matches &= opcode == Opcode::Cast || isRematerializedInEachArm(opcode);
        }
        for (unsigned i = 0; i + 1 < slow.values.size(); ++i)
            matches &= isRematerializedInEachArm(function.at(slow.values[i]).opcode);
        for (unsigned i = 1; i < call.arguments.size(); ++i) {
            if (i <= generic.arguments.size())
                matches &= isSame(call.arguments[i], generic.arguments[i - 1]);
            else
                matches &= function.at(call.arguments[i]).opcode == Opcode::Undefined;
        }
        auto& fastArguments = fast.terminator.targets[0].arguments;
        auto& slowArguments = slow.terminator.targets[0].arguments;
        matches &= fastArguments.size() == slowArguments.size();
        for (unsigned i = 0; matches && i < fastArguments.size(); ++i)
            matches = fastArguments[i] == slowArguments[i] ? fastArguments[i] != directCall : fastArguments[i] == directCall && slowArguments[i] == genericCall;
        if (!matches)
            continue;
        if (!isInLoop)
            isInLoop = blocksInLoops(function);
        if (isInLoop->get(b))
            continue;
        Opcode definitionOfReceiver = function.at(withoutCasts(receiver)).opcode;
        if (receiversThatMayYetBeProven == ReceiversThatMayYetBeProven::Wait && (definitionOfReceiver == Opcode::NewArray || definitionOfReceiver == Opcode::CreateRest || definitionOfReceiver == Opcode::CallBuiltin || definitionOfReceiver == Opcode::CallDirect || mayBeReadFromModuleVariable(receiver))) {
            if (!function.guardedPreludeEntriesWantedLater.contains(entry))
                function.guardedPreludeEntriesWantedLater.append(entry);
            continue;
        }
        if (receiversThatMayYetBeProven == ReceiversThatMayYetBeProven::AreTaken && !entriesWanted.contains(entry))
            continue;
        if (read) {
            Value undefinedValue;
            undefinedValue.opcode = Opcode::Undefined;
            undefinedValue.position = function.at(read).position;
            undefinedValue.inlineFrame = function.at(read).inlineFrame;
            undefinedValue.type = program.types.undefined;
            undefinedValue.rep = repOf(program.at(undefinedValue.type));
            forgetUsesBy(std::as_const(function.at(read)));
            function.at(read) = WTF::move(undefinedValue);
        }
        Value callOfEntry;
        callOfEntry.opcode = Opcode::CallDirect;
        callOfEntry.position = generic.position;
        callOfEntry.inlineFrame = generic.inlineFrame;
        callOfEntry.isInTailPosition = generic.isInTailPosition;
        callOfEntry.type = holdsResultToType ? call.type : generic.type;
        callOfEntry.rep = generic.rep;
        callOfEntry.setPrediction(generic.prediction());
        callOfEntry.immediates = { entry.index(), Format::none };
        callOfEntry.arguments.append(receiver);
        callOfEntry.arguments.appendVector(generic.arguments);
        forgetUsesBy(std::as_const(generic));
        generic = WTF::move(callOfEntry);
        countUsesBy(std::as_const(generic));
        forgetUsesBy(std::as_const(function.at(block.values.last())));
        block.values.removeLast();
        block.values.appendVector(slow.values);
        forgetUsesBy(block.terminator);
        block.terminator = WTF::move(slow.terminator);
        slow.values.clear();
        slow.terminator = Terminator { };
        if (receiversThatMayYetBeProven == ReceiversThatMayYetBeProven::Wait) {
            auto empty = [&](Block& left) {
                for (ValueID value : left.values)
                    forgetUsesBy(std::as_const(function.at(value)));
                forgetUsesBy(left.terminator);
                left.values.clear();
                left.terminator = Terminator { };
            };
            empty(fast);
            for (BlockID blockOfCasts : blocksOfCasts)
                empty(function.at(blockOfCasts));
        }
        ++changed;
    }
    if (changed && receiversThatMayYetBeProven == ReceiversThatMayYetBeProven::Wait)
        cleanUp(program, function);
    else if (changed)
        ReceiverGuardElimination(program, function).removeUnreachableBlocks();
    return changed;
}

unsigned resolvePreludeCalls(const Program& program, Function& function, CleanUpTiming timing)
{
    return BuiltinSpecialization(program, function, BuiltinSpecialization::Mode::Prelude).run(timing);
}

unsigned speculateOnReceivers(const Program& program, FunctionID id, Function& function)
{
    BuiltinSpecialization speculation(program, function, BuiltinSpecialization::Mode::Speculation);
    if (!program.isHotOutsideItsLoops(id) && function.kind != FunctionKind::Module)
        speculation.leaveColdTextToStubs();
    return speculation.run();
}

unsigned eliminateGuardsOnUnescapedReceivers(const Program& program, Function& function)
{
    return ReceiverGuardElimination(program, function).run();
}

void eliminateGuardsOnFreshResults(Program& program)
{
    unsigned count = program.functions.size();
    program.classesOfFreshResults.fill(UINT16_MAX, count);
    for (unsigned round = 0; round < 4; ++round) {
        Vector<uint16_t> found = program.classesOfFreshResults;
        forEachIndexInParallel(count, [&](unsigned i) {
            auto& function = program.functions.withSameValues(i);
            if (found[i] != UINT16_MAX || function.blocks.isEmpty() || function.declaredAs || !function.resultTypes.isEmpty() || function.flags & (FunctionIsHalfOfHiddenCoroutine | FunctionIsHalfOfAsyncFunction))
                return;
            if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method)
                return;
            if (uint32_t engineClass = ReceiverGuardElimination(program, function).classOfFreshResult(); engineClass != Format::none)
                found[i] = static_cast<uint16_t>(engineClass);
        });
        if (found == program.classesOfFreshResults)
            break;
        program.classesOfFreshResults = WTF::move(found);
    }
    forEachIndexInParallel(count, [&](unsigned i) {
        bool callsSuchFunction = std::as_const(program).functions[i].values.containsIf([&](auto& value) {
            return value.opcode == Opcode::CallDirect && value.function().index() < count && program.classesOfFreshResults[value.function().index()] != UINT16_MAX;
        });
        if (std::as_const(program).functions[i].blocks.isEmpty() || !callsSuchFunction)
            return;
        ReceiverGuardElimination guards(program, program.functions[i]);
        if (guards.run())
            guards.removeUnreachableBlocks();
    });
}

void eliminateGuardsOnUnescapedModuleVariables(Program& program)
{
    auto& builtins = Builtins::singleton();
    auto& functions = std::as_const(program).functions;
    unsigned count = functions.size();
    ModuleVariableFacts facts;
    facts.classes.fill(ModuleVariableFacts::isNeverStored, program.variables.size());
    facts.variablesReturned.fill(ModuleVariableFacts::none, count);
    static constexpr OpcodeSet stores { Opcode::PutModuleVariable };
    static constexpr uint16_t isNil = ModuleVariableFacts::isNeverStored;
    Vector<unsigned> functionsWithStores;
    for (unsigned i = 0; i < count; ++i) {
        if (!functions[i].blocks.isEmpty() && functions.mayHold(i, stores))
            functionsWithStores.append(i);
    }
    Vector<Vector<std::pair<uint32_t, uint16_t>>> whatIsStored(functionsWithStores.size());
    forEachIndexInParallel(functionsWithStores.size(), [&](unsigned indexOfFunction) {
        unsigned i = functionsWithStores[indexOfFunction];
        auto& function = functions[i];
        auto& found = whatIsStored[indexOfFunction];
        ReceiverGuardElimination allocations(program, program.functions.withSameValues(i));
        Vector<std::pair<ValueID, uint32_t>, 4> stored;
        functions.forEachValueInOrder(i, stores, [&](ValueID id) {
            auto& store = function.at(id);
            uint32_t variable = store.variable().index();
            if (auto& type = program.typeOf(function, store.child(0)); !type.hasObjects()) {
                found.append({ variable, type.hasOnlyPrimitives(Primitive::UndefinedOrNull) ? isNil : ModuleVariableFacts::escapes });
                return;
            }
            uint32_t engineClass = allocations.classOfAllocation(store.child(0));
            if (engineClass == Format::none || !builtins.classes()[engineClass].canCheckOriginalStructure()) {
                found.append({ variable, ModuleVariableFacts::escapes });
                return;
            }
            found.append({ variable, static_cast<uint16_t>(engineClass) });
            stored.append({ allocations.allocationOf(store.child(0)), variable });
        });
        if (function.kind != FunctionKind::Module || stored.isEmpty())
            return;
        uint32_t returned = ModuleVariableFacts::none;
        bool returnsOneVariable = true;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            uint32_t variable = ModuleVariableFacts::none;
            for (auto& [allocation, variableStoredTo] : stored) {
                if (block.terminator.value && allocation == allocations.allocationOf(block.terminator.value))
                    variable = variableStoredTo;
            }
            returnsOneVariable &= variable != ModuleVariableFacts::none && (returned == ModuleVariableFacts::none || returned == variable);
            returned = variable;
        }
        if (returnsOneVariable)
            facts.variablesReturned[i] = returned;
    });
    for (auto& found : whatIsStored) {
        for (auto [variable, engineClass] : found) {
            uint16_t& known = facts.classes[variable];
            if (engineClass == isNil)
                facts.mayHoldNil.set(variable);
            else if (known == ModuleVariableFacts::isNeverStored || known == engineClass)
                known = engineClass;
            else
                known = ModuleVariableFacts::escapes;
        }
    }
    if (!facts.classes.containsIf([](uint16_t engineClass) { return engineClass < ModuleVariableFacts::isNeverStored; }))
        return;
    facts.methodsCarriedOut.fill(BuiltinID(), count);
    for (unsigned i = 0; i < program.preludeFunctions.size(); ++i) {
        BuiltinID entry = builtins.entryCarriedOutBy(static_cast<PreludeFunction>(i));
        if (program.preludeFunctions[i] && entry && builtins.at(entry).kind == BuiltinKind::Method)
            facts.methodsCarriedOut[program.preludeFunctions[i].index()] = entry;
    }
    for (FunctionID prelude : program.preludeFunctions) {
        if (!prelude || !facts.methodsCarriedOut[prelude.index()] || functions[prelude.index()].blocks.isEmpty() || functions[prelude.index()].blocks[0].parameters.size() < 2)
            continue;
        auto& function = functions[prelude.index()];
        for (auto& value : function.values) {
            if (value.opcode != Opcode::CallDirect || value.arguments.size() < 2 || functions[value.function().index()].module != function.module || facts.methodsCarriedOut[value.function().index()])
                continue;
            if (program.originOf(function, value.arguments[0]) == function.blocks[0].parameters[0] && program.originOf(function, value.arguments[1]) == function.blocks[0].parameters[1])
                facts.methodsCarriedOut[value.function().index()] = facts.methodsCarriedOut[prelude.index()];
        }
    }
    for (FunctionID guardedEntry : program.guardedPreludeEntries.values()) {
        for (auto& value : functions[guardedEntry.index()].values) {
            if (value.opcode == Opcode::CallDirect && !functions[value.function().index()].isGuardedPreludeEntry && facts.methodsCarriedOut[value.function().index()])
                facts.methodsCarriedOut[guardedEntry.index()] = facts.methodsCarriedOut[value.function().index()];
        }
    }
    Vector<uint8_t> escapes(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> holdsAliases(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        if (functions[i].blocks.isEmpty())
            return;
        ModuleVariableAliases aliases(program, FunctionID(i), facts);
        if (!aliases.find())
            return;
        holdsAliases[i] = 1;
        aliases.forEachEscape([&](uint32_t variable) {
            WTF::atomicStore(&escapes[variable], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    unsigned numberOfVariables = 0;
    for (unsigned i = 0; i < facts.classes.size(); ++i) {
        if (escapes[i] || !facts.isCandidate(i))
            facts.classes[i] = ModuleVariableFacts::escapes;
        else
            ++numberOfVariables;
    }
    std::atomic<unsigned> numberEliminated { 0 };
    std::atomic<unsigned> numberOfCalls { 0 };
    forEachIndexInParallel(numberOfVariables ? count : 0, [&](unsigned i) {
        if (!holdsAliases[i])
            return;
        ModuleVariableAliases aliases(program, FunctionID(i), facts);
        if (!aliases.find())
            return;
        auto [eliminated, calls] = aliases.eliminate(program.functions.withSameValues(i));
        numberOfCalls += calls;
        if (!eliminated)
            return;
        numberEliminated += eliminated;
        ReceiverGuardElimination(program, program.functions[i]).removeUnreachableBlocks();
    });
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: ", numberOfVariables, " module variables hold objects that only built-in operations see: ", numberEliminated.load(), " guards and method reads eliminated, ", numberOfCalls.load(), " calls of built-in methods without a generic fallback");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
