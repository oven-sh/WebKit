/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ChainEnd : uint8_t { Array, Reduce, Length, Some, Every, Find, FindIndex };

struct Link {
    unsigned index { 0 };
    ValueID callback;
    FunctionID function;
    bool isFilter { false };
    bool testsInput { false };
    bool isFlatMap { false };
    unsigned greatestNumberOfParts { 0 };
    FunctionID functionReturningParts;
};

struct Chain {
    unsigned block { 0 };
    unsigned firstIndex { 0 };
    unsigned lastIndex { 0 };
    ValueID receiver;
    bool isSliced { false };
    ValueID sliceStart;
    ValueID sliceEnd;
    Vector<Link, 4> stages;
    ChainEnd end { ChainEnd::Array };
    Link last;
    ValueID initial;
    bool testsAccumulator { false };
    Vector<ValueID, 4> accumulatorStores;
    Vector<unsigned, 8> moved;
};

class CallbackAnalysis {
public:
    struct Facts {
        uint8_t nullableParametersRead { 0 };
        bool resultIsNeverNullish { true };
    };

    explicit CallbackAnalysis(const Program& program)
        : m_program(program)
    {
    }

    ASCIILiteral reason() const { return m_reason; }

    FunctionID functionReturnedBy(FunctionID id) const
    {
        auto& callee = m_program.at(id);
        if (callee.blocks.isEmpty() || callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator))
            return { };
        FunctionID only;
        for (auto& block : callee.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            if (!block.terminator.value)
                return { };
            auto& returned = callee.at(m_program.originOf(callee, block.terminator.value));
            if ((returned.opcode != Opcode::NewClosure && returned.opcode != Opcode::FunctionReference) || (only && only != returned.function()))
                return { };
            only = returned.function();
        }
        return only;
    }

    FunctionID functionHeldBy(const Function& function, ValueID value) const
    {
        auto& definition = function.at(m_program.originOf(function, value));
        switch (definition.opcode) {
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
            return definition.function();
        case Opcode::CallDirect:
            return definition.isJumpToContinuation() ? FunctionID() : functionReturnedBy(definition.function());
        default:
            return { };
        }
    }

    bool isNeverNullish(const Function& function, ValueID value) const
    {
        if (!m_program.typeOf(function, value).mayBeUndefinedOrNull())
            return true;
        ValueID origin = m_program.originOf(function, value);
        return function.at(origin).opcode == Opcode::NewObjectWithShape || !m_program.typeOf(function, origin).mayBeUndefinedOrNull();
    }

    bool readsOnlyFieldsOf(FunctionID id, unsigned entryParameter, unsigned depth = 0) const
    {
        static constexpr unsigned maxDepth = 3;
        auto& function = m_program.at(id);
        if (depth > maxDepth || function.blocks.isEmpty() || entryParameter >= function.blocks[0].parameters.size())
            return false;
        Vector<ValueID, 4> aliases { function.blocks[0].parameters[entryParameter] };
        for (unsigned next = 0; next < aliases.size(); ++next) {
            ValueID object = aliases[next];
            for (auto& block : function.blocks) {
                if (block.terminator.value == object)
                    return false;
                for (auto& target : block.terminator.targets) {
                    if (target.arguments.contains(object))
                        return false;
                }
                for (ValueID user : block.values) {
                    auto& value = function.at(user);
                    switch (value.opcode) {
                    case Opcode::GetById:
                    case Opcode::GetField:
                    case Opcode::IsUndefinedOrNull:
                        continue;
                    case Opcode::Cast:
                        if (value.child(0) == object)
                            aliases.append(user);
                        continue;
                    case Opcode::CallDirect:
                        if (value.isJumpToContinuation() && value.arguments.contains(object))
                            return false;
                        for (unsigned k = 0; k < value.arguments.size(); ++k) {
                            if (value.arguments[k] == object && !readsOnlyFieldsOf(value.function(), k, depth + 1))
                                return false;
                        }
                        continue;
                    default:
                        break;
                    }
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= child == object;
                    });
                    if (uses)
                        return false;
                }
            }
        }
        return true;
    }

    static BitVector blocksThatOnlyReturnTheirParameter(const Function& function)
    {
        BitVector result(function.blocks.size());
        for (bool changed = true; std::exchange(changed, false);) {
            for (unsigned b = 1; b < function.blocks.size(); ++b) {
                auto& block = function.blocks[b];
                auto& terminator = block.terminator;
                if (result.get(b) || block.parameters.size() != 1 || !block.values.isEmpty())
                    continue;
                bool returns = terminator.kind == TerminatorKind::Return && terminator.value == block.parameters[0];
                bool handsOn = terminator.kind == TerminatorKind::Jump && result.get(terminator.targets[0].block.index()) && terminator.targets[0].arguments[0] == block.parameters[0];
                if (returns || handsOn) {
                    result.set(b);
                    changed = true;
                }
            }
        }
        return result;
    }

    static bool forEachLiteralReturnedBy(const Function& function, const Invocable<bool(unsigned block, ValueID literal)> auto& functor)
    {
        Vector<uint32_t> useCounts(FillWith { }, function.values.size(), 0u);
        auto use = [&](ValueID child) {
            ++useCounts[child.index()];
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values)
                function.at(id).forEachChild(use);
            if (block.terminator.value)
                use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
            }
        }
        BitVector onlyReturns = blocksThatOnlyReturnTheirParameter(function);
        auto visit = [&](unsigned block, ValueID value) {
            return function.at(value).opcode == Opcode::NewArray && useCounts[value.index()] == 1 && function.blocks[block].values.contains(value) && functor(block, value);
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (onlyReturns.get(b)) {
                if (useCounts[block.parameters[0].index()] != 1)
                    return false;
                continue;
            }
            if (block.terminator.kind == TerminatorKind::Return) {
                if (!block.terminator.value || !visit(b, block.terminator.value))
                    return false;
                continue;
            }
            for (auto& target : block.terminator.targets) {
                if (onlyReturns.get(target.block.index()) && (block.terminator.kind != TerminatorKind::Jump || !visit(b, target.arguments[0])))
                    return false;
            }
        }
        return true;
    }

    std::optional<unsigned> greatestLiteralReturnedBy(FunctionID id) const
    {
        static constexpr unsigned maxParts = 3;
        auto& function = m_program.at(id);
        if (function.blocks.isEmpty() || !function.resultTypes.isEmpty())
            return std::nullopt;
        unsigned greatest = 0;
        bool areAllLiterals = forEachLiteralReturnedBy(function, [&](unsigned, ValueID literal) {
            auto& parts = function.at(literal).arguments;
            greatest = std::max<unsigned>(greatest, parts.size());
            return parts.size() <= maxParts && std::ranges::none_of(parts, [&](ValueID part) {
                return m_program.typeOf(function, part).primitives & Primitive::Absent;
            });
        });
        return areAllLiterals ? std::optional(greatest) : std::nullopt;
    }

    std::optional<Facts> factsOf(FunctionID id, std::span<const FunctionID> functionsCaptured, uint8_t parametersNeverNullish, unsigned maxParameters, unsigned depth = 0)
    {
        static constexpr unsigned maxDepth = 3;
        auto refuse = [&](ASCIILiteral reason) -> std::optional<Facts> {
            m_reason = reason;
            return std::nullopt;
        };
        if (depth > maxDepth)
            return refuse("calls too deep"_s);
        auto& function = bodyOf(id);
        if (function.blocks.isEmpty() || (function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Plain))
            return refuse("not a plain function"_s);
        if (function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionHasRestParameter | Format::FunctionHasThis | Format::FunctionHasNewTarget | Format::FunctionIsSloppy | FunctionUsesArgumentBuffer))
            return refuse("async, generator, rest, this, new.target, sloppy or arguments"_s);
        unsigned captureCount = function.captureTypes.size();
        auto& entry = function.blocks[0].parameters;
        if (function.parameterTypes.size() > maxParameters || function.parameterTypes.size() > 8 || entry.size() != captureCount + function.parameterTypes.size())
            return refuse("too many parameters"_s);
        if (!blocksInLoops(function).isEmpty())
            return refuse("a loop"_s);

        Facts facts;
        BitVector isNeverNullishValue(function.values.size());
        for (unsigned k = 0; k < function.parameterTypes.size(); ++k) {
            if (parametersNeverNullish >> k & 1)
                isNeverNullishValue.set(entry[captureCount + k].index());
        }
        auto originOf = [&](ValueID value) {
            return m_program.originOf(function, value);
        };
        auto isObject = [&](ValueID value) {
            return isNeverNullish(function, value) || isNeverNullishValue.get(originOf(value).index());
        };
        auto needsObject = [&](ValueID value) {
            if (isObject(value))
                return true;
            size_t at = entry.find(originOf(value));
            if (at == notFound || at < captureCount)
                return false;
            facts.nullableParametersRead |= 1 << (at - captureCount);
            return true;
        };
        auto functionIn = [&](ValueID value) -> FunctionID {
            ValueID origin = originOf(value);
            if (function.at(origin).opcode == Opcode::FunctionReference)
                return function.at(origin).function();
            size_t at = entry.find(origin);
            return at < captureCount && at < functionsCaptured.size() ? functionsCaptured[at] : FunctionID();
        };
        auto call = [&](ValueID result, FunctionID callee, std::span<const ValueID> captures, std::span<const ValueID> arguments) {
            Vector<FunctionID, 4> functionsPassed;
            for (ValueID capture : captures)
                functionsPassed.append(functionIn(capture));
            uint8_t neverNullish = 0;
            for (unsigned k = 0; k < arguments.size() && k < 8; ++k)
                neverNullish |= isObject(arguments[k]) << k;
            auto inner = factsOf(callee, functionsPassed.span(), neverNullish, 8, depth + 1);
            if (!inner)
                return false;
            for (unsigned k = 0; k < 8; ++k) {
                if (!(inner->nullableParametersRead >> k & 1))
                    continue;
                if (k >= arguments.size() || !needsObject(arguments[k])) {
                    m_reason = "a function it calls reads what may be undefined or null"_s;
                    return false;
                }
            }
            if (inner->resultIsNeverNullish)
                isNeverNullishValue.set(result.index());
            return true;
        };
        auto isFieldOfClosedLayout = [&](const Value& read) {
            auto& receiver = m_program.typeOf(function, read.child(0));
            if (read.opcode == Opcode::GetField)
                return isObjectLayout(m_program.at(read.layout()).kind) && !m_program.at(read.layout()).acceptsUntypedObjects;
            auto access = fieldOf(m_program, receiver, read.string());
            return access && !receiver.acceptsUntypedObjects(m_program) && !m_program.at(access->layout).acceptsUntypedObjects;
        };

        for (auto& block : function.blocks) {
            for (size_t index = 0; index < block.values.size(); ++index) {
                ValueID valueID = block.values[index];
                auto& value = function.at(valueID);
                switch (value.opcode) {
                case Opcode::Call: {
                    FunctionID callee = functionIn(value.child(0));
                    if (!callee || function.at(originOf(value.child(1))).opcode != Opcode::Undefined)
                        return refuse("a call of a function that is not known"_s);
                    if (!call(valueID, callee, { }, value.arguments.span()))
                        return std::nullopt;
                    continue;
                }
                case Opcode::CallDirect: {
                    unsigned passed = m_program.at(value.function()).captureTypes.size();
                    if (value.isJumpToContinuation() || value.arguments.size() < passed)
                        return refuse("a call of a function that is not known"_s);
                    if (!call(valueID, value.function(), value.arguments.span().first(passed), value.arguments.span().subspan(passed)))
                        return std::nullopt;
                    continue;
                }
                case Opcode::Await:
                case Opcode::Yield:
                case Opcode::GetResumeMode:
                    return refuse("a suspension"_s);
                default:
                    break;
                }
                if (writesOf(m_program, function, block, index) > Writes::RuntimeState)
                    return refuse("a store, or an operation that may run other code"_s);
                if (!mayThrow(m_program, function, value))
                    continue;
                switch (value.opcode) {
                case Opcode::GetById:
                case Opcode::GetField:
                    if (!isFieldOfClosedLayout(value))
                        return refuse("a read that is not of a field of a closed layout"_s);
                    if (!needsObject(value.child(0)))
                        return refuse("a read of what may be undefined or null"_s);
                    break;
                case Opcode::CallBuiltin: {
                    auto& builtin = Builtins::singleton().at(value.builtin());
                    if (builtin.effects & Effect::MayThrow || !guaranteesResultType(m_program, function, value))
                        return refuse("a built-in that may throw"_s);
                    if (builtin.hasReceiver() && !needsObject(value.child(0)))
                        return refuse("a read of what may be undefined or null"_s);
                    break;
                }
                default:
                    return refuse("an operation that may throw"_s);
                }
            }
            switch (block.terminator.kind) {
            case TerminatorKind::Jump:
            case TerminatorKind::Branch:
            case TerminatorKind::Switch:
            case TerminatorKind::Unreachable:
                break;
            case TerminatorKind::Return:
                facts.resultIsNeverNullish &= block.terminator.value && isObject(block.terminator.value);
                break;
            default:
                return refuse("throw or a handler"_s);
            }
        }
        return facts;
    }

private:
    const Function& bodyOf(FunctionID id)
    {
        return *m_bodies.ensure(id.index(), [&] {
            auto body = makeUniqueWithoutFastMallocCheck<Function>(m_program.at(id));
            if (!body->blocks.isEmpty()) {
                resolveBuiltins(m_program, *body);
                removeUnusedValues(m_program, *body);
            }
            return body;
        }).iterator->value;
    }

    const Program& m_program;
    ASCIILiteral m_reason;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Function>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_bodies;
};

class ChainFinder {
public:
    ChainFinder(const Program& program, FunctionID id)
        : m_program(program)
        , m_id(id)
        , m_function(program.at(id))
        , m_builtins(Builtins::singleton())
    {
    }

    Vector<Chain> run()
    {
        Vector<Chain> chains;
        bool isHotOutsideLoops = m_program.isHotOutsideLoops(m_id);
        BitVector isInLoop;
        if (!isHotOutsideLoops) {
            isInLoop = blocksInLoops(m_function);
            if (isInLoop.isEmpty() && !Options::verboseAOTCompilation())
                return chains;
        }
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            bool isHot = isHotOutsideLoops || isInLoop.get(b);
            if (!isHot && !Options::verboseAOTCompilation())
                continue;
            auto& values = m_function.blocks[b].values;
            for (unsigned index = 0; index < values.size(); ++index) {
                auto& value = m_function.at(values[index]);
                if (value.opcode != Opcode::CallMethod || !(isSlice(value) || (value.arguments.size() == 1 && isStageName(m_program.builtinName(value.string())))))
                    continue;
                ASCIILiteral reason;
                auto chain = chainStartingAt(b, index, reason);
                if (Options::verboseAOTCompilation() && (chain || !reason.isNull())) [[unlikely]]
                    dataLogLn("AOT: a chain of array methods in ", isHot ? "hot" : "cold", " code, f", m_id.index(), " b", b, chain ? (isHot ? " runs as one loop" : " could run as one loop") : " is left as written: ", chain ? ""_s : reason);
                if (!chain || !isHot)
                    continue;
                index = chain->lastIndex;
                chains.append(WTF::move(*chain));
            }
        }
        return chains;
    }

    static bool mayStartChain(BuiltinName name)
    {
        static const BuiltinName slice = named("slice"_s);
        return name && (name == slice || isStageName(name));
    }

private:
    static BuiltinName named(ASCIILiteral text) { return Builtins::singleton().nameOf(asBytes(text.span8())); }

    static bool isStageName(BuiltinName name)
    {
        static const BuiltinName filter = named("filter"_s);
        static const BuiltinName map = named("map"_s);
        static const BuiltinName flatMap = named("flatMap"_s);
        return name && (name == filter || name == map || name == flatMap);
    }

    static std::optional<ChainEnd> endNamed(BuiltinName name, size_t numberOfArguments)
    {
        static const BuiltinName reduce = named("reduce"_s);
        static const BuiltinName some = named("some"_s);
        static const BuiltinName every = named("every"_s);
        static const BuiltinName find = named("find"_s);
        static const BuiltinName findIndex = named("findIndex"_s);
        if (!name)
            return std::nullopt;
        if (name == reduce)
            return numberOfArguments == 2 ? std::optional(ChainEnd::Reduce) : std::nullopt;
        if (numberOfArguments != 1)
            return std::nullopt;
        if (name == some)
            return ChainEnd::Some;
        if (name == every)
            return ChainEnd::Every;
        if (name == find)
            return ChainEnd::Find;
        if (name == findIndex)
            return ChainEnd::FindIndex;
        return std::nullopt;
    }

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    ValueID originOf(ValueID value) const { return m_program.originOf(m_function, value); }

    bool isSlice(const Value& value) const
    {
        static const BuiltinName slice = named("slice"_s);
        if (value.opcode != Opcode::CallMethod || m_program.builtinName(value.string()) != slice || value.arguments.isEmpty() || value.arguments.size() > 2)
            return false;
        return std::ranges::all_of(value.arguments, [&](ValueID argument) {
            return typeOf(argument).isInteger() && typeOf(argument).integers.minimum >= 0;
        });
    }

    unsigned useCountOf(ValueID value)
    {
        if (m_useCounts.isEmpty()) {
            m_useCounts.fill(0, m_function.values.size());
            auto use = [&](ValueID child) {
                ++m_useCounts[child.index()];
            };
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values)
                    m_function.at(id).forEachChild(use);
                if (block.terminator.value)
                    use(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        use(argument);
                }
            }
        }
        return m_useCounts[value.index()];
    }

    bool mayBeArray(ValueID value) const
    {
        auto& type = typeOf(value);
        return type.anyObject || type.engineClasses.contains(m_builtins.arrayClass().index);
    }

    bool staysAheadOfLoop(const Value& value, ValueID current, ValueID methodRead) const
    {
        bool usesChain = false;
        value.forEachChild([&](ValueID child) {
            usesChain |= child == current || child == methodRead;
        });
        if (usesChain)
            return false;
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewObjectWithShape:
            return true;
        case Opcode::PutField:
            return m_function.at(value.child(0)).opcode == Opcode::NewObjectWithShape;
        case Opcode::Cast:
            return value.castReason() == CastReason::Upcast;
        default:
            return false;
        }
    }

    std::optional<Chain> chainStartingAt(unsigned block, unsigned firstIndex, ASCIILiteral& reason)
    {
        static const BuiltinName filter = named("filter"_s);
        static const BuiltinName flatMap = named("flatMap"_s);
        static const BuiltinName length = named("length"_s);
        auto& values = m_function.blocks[block].values;
        Chain chain;
        chain.block = block;
        chain.firstIndex = firstIndex;
        chain.receiver = m_function.at(values[firstIndex]).child(0);
        if (!mayBeArray(chain.receiver))
            return std::nullopt;
        ValueID current;
        ValueID methodRead;
        size_t movedCountAtLastCall = 0;
        bool isEnded = false;
        for (unsigned index = firstIndex; index < values.size() && !isEnded; ++index) {
            auto& value = m_function.at(values[index]);
            if (current && !methodRead && value.opcode == Opcode::Cast && value.child(0) == current && useCountOf(current) == 1 && (value.castReason() == CastReason::Upcast || value.castReason() == CastReason::As) && m_program.at(value.type).singleEngineClass() == &m_builtins.arrayClass()) {
                chain.moved.append(index);
                current = values[index];
                continue;
            }
            if (current && !methodRead && value.opcode == Opcode::GetById && value.child(0) == current) {
                if (m_program.builtinName(value.string()) == length && useCountOf(current) == 1) {
                    chain.end = ChainEnd::Length;
                    chain.moved.append(index);
                    chain.lastIndex = index;
                    movedCountAtLastCall = chain.moved.size();
                    break;
                }
                if (useCountOf(values[index]) != 1 || useCountOf(current) != 2)
                    break;
                chain.moved.append(index);
                methodRead = values[index];
                continue;
            }
            if (staysAheadOfLoop(value, current, methodRead))
                continue;
            bool callsWhatWasRead = methodRead && value.opcode == Opcode::Call && value.child(0) == methodRead && value.child(1) == current;
            if (!callsWhatWasRead && (methodRead || value.opcode != Opcode::CallMethod || (current && (value.child(0) != current || useCountOf(current) != 1))))
                break;
            if (value.arguments.isEmpty() || value.arguments.contains(current))
                break;
            if (!chain.stages.isEmpty() && chain.stages.last().isFlatMap)
                break;
            if (!current && isSlice(value)) {
                chain.isSliced = true;
                chain.sliceStart = value.arguments[0];
                if (value.arguments.size() == 2)
                    chain.sliceEnd = value.arguments[1];
                chain.moved.append(index);
                current = values[index];
                continue;
            }
            BuiltinName name = m_program.builtinName(callsWhatWasRead ? m_function.at(methodRead).string() : value.string());
            methodRead = { };
            auto& callback = m_function.at(originOf(value.arguments[0]));
            bool isLiteral = callback.opcode == Opcode::FunctionReference || callback.opcode == Opcode::NewClosure;
            Link link;
            link.index = index;
            link.callback = value.arguments[0];
            if (isLiteral)
                link.function = callback.function();
            link.isFilter = name == filter;
            link.isFlatMap = name == flatMap;
            if (isStageName(name) && value.arguments.size() == 1) {
                if (!isLiteral) {
                    if (!chain.stages.isEmpty())
                        reason = "a callback that is not a function literal"_s;
                    break;
                }
                chain.stages.append(link);
            } else if (auto end = current ? endNamed(name, value.arguments.size()) : std::nullopt) {
                if (!isLiteral) {
                    reason = "a callback that is not a function literal"_s;
                    break;
                }
                chain.end = *end;
                chain.last = link;
                if (*end == ChainEnd::Reduce)
                    chain.initial = value.arguments[1];
                isEnded = true;
            } else
                break;
            chain.moved.append(index);
            chain.lastIndex = index;
            movedCountAtLastCall = chain.moved.size();
            current = values[index];
        }
        chain.moved.shrink(movedCountAtLastCall);
        if (chain.end == ChainEnd::Array && !chain.stages.isEmpty() && !chain.stages.last().isFilter && isJoined(values[chain.stages.last().index])) {
            chain.stages.removeLast();
            if (chain.stages.isEmpty())
                return std::nullopt;
            chain.lastIndex = chain.stages.last().index;
            while (chain.moved.last() > chain.lastIndex)
                chain.moved.removeLast();
        }
        if (chain.stages.isEmpty() || chain.isSliced + chain.stages.size() + (chain.end != ChainEnd::Array) + chain.stages.last().isFlatMap < 2)
            return std::nullopt;
        if (chain.lastIndex + 1 == values.size() && m_function.blocks[block].terminator.kind == TerminatorKind::Invoke) {
            reason = "an exception handler"_s;
            return std::nullopt;
        }
        reason = { };
        if (!areCallbacksUnobservable(chain, reason))
            return std::nullopt;
        return chain;
    }

    bool isJoined(ValueID map) const
    {
        for (auto& value : m_function.values) {
            if (value.opcode == Opcode::CallMethod && value.isFusedWithMap && value.child(0) == map)
                return true;
        }
        return false;
    }

    bool areCallbacksUnobservable(Chain& chain, ASCIILiteral& reason)
    {
        CallbackAnalysis analysis(m_program);
        auto functionsCapturedBy = [&](ValueID callback) {
            Vector<FunctionID, 4> functions;
            auto& definition = m_function.at(originOf(callback));
            if (definition.opcode == Opcode::NewClosure) {
                for (ValueID capture : definition.arguments)
                    functions.append(analysis.functionHeldBy(m_function, capture));
            }
            return functions;
        };
        bool inputIsNeverNullish = false;
        for (auto& stage : chain.stages) {
            auto facts = analysis.factsOf(stage.function, functionsCapturedBy(stage.callback).span(), inputIsNeverNullish, 1);
            if (!facts) {
                reason = analysis.reason();
                return false;
            }
            if (facts->nullableParametersRead & 1) {
                stage.testsInput = true;
                inputIsNeverNullish = true;
            }
            if (!stage.isFilter)
                inputIsNeverNullish = facts->resultIsNeverNullish;
            if (!stage.isFlatMap)
                continue;
            auto greatest = analysis.greatestLiteralReturnedBy(stage.function);
            if (!greatest) {
                reason = "a callback of flatMap that returns something else than array literals"_s;
                return false;
            }
            stage.greatestNumberOfParts = *greatest;
        }
        if (chain.end == ChainEnd::Array || chain.end == ChainEnd::Length)
            return true;
        auto functions = functionsCapturedBy(chain.last.callback);
        if (chain.end != ChainEnd::Reduce) {
            auto facts = analysis.factsOf(chain.last.function, functions.span(), inputIsNeverNullish, 1);
            if (!facts) {
                reason = analysis.reason();
                return false;
            }
            chain.last.testsInput = facts->nullableParametersRead & 1;
            return true;
        }
        bool accumulatorIsNeverNullish = analysis.isNeverNullish(m_function, chain.initial);
        auto facts = analysis.factsOf(chain.last.function, functions.span(), accumulatorIsNeverNullish | inputIsNeverNullish << 1, 2);
        if (facts && accumulatorIsNeverNullish && !facts->resultIsNeverNullish)
            facts = analysis.factsOf(chain.last.function, functions.span(), inputIsNeverNullish << 1, 2);
        if (!facts) {
            reason = analysis.reason();
            return false;
        }
        chain.testsAccumulator = facts->nullableParametersRead & 1;
        chain.last.testsInput = facts->nullableParametersRead >> 1 & 1;
        if (facts->resultIsNeverNullish)
            findAccumulatorStores(chain, analysis);
        return true;
    }

    void findAccumulatorStores(Chain& chain, const CallbackAnalysis& analysis)
    {
        auto& literal = m_function.at(chain.initial);
        auto& reducer = m_program.at(chain.last.function);
        if (literal.opcode != Opcode::NewObjectWithShape || literal.child(0) || !reducer.worker || reducer.nullishResultKind)
            return;
        auto fieldsStoredIn = [](const Function& function, const Block& block, ValueID object, Vector<ValueID, 4>* stores) {
            uint64_t fields = 0;
            for (ValueID id : block.values) {
                auto& store = function.at(id);
                if (store.opcode != Opcode::PutField || store.child(0) != object)
                    continue;
                if (store.layout() != function.at(object).layout() || store.field() >= 64 || fields >> store.field() & 1)
                    return uint64_t { 0 };
                fields |= 1ull << store.field();
                if (stores)
                    stores->append(id);
            }
            return fields;
        };
        Vector<ValueID, 4> stores;
        uint64_t initialFields = fieldsStoredIn(m_function, m_function.blocks[chain.block], chain.initial, &stores);
        if (!initialFields || useCountOf(chain.initial) != stores.size() + 1)
            return;
        unsigned numberOfLiterals = 0;
        for (auto& block : reducer.blocks) {
            for (ValueID id : block.values) {
                if (reducer.at(id).opcode != Opcode::NewObjectWithShape)
                    continue;
                if (++numberOfLiterals > 1 || reducer.at(id).layout() != literal.layout() || fieldsStoredIn(reducer, block, id, nullptr) != initialFields)
                    return;
            }
        }
        if (numberOfLiterals != 1 || !analysis.readsOnlyFieldsOf(chain.last.function, reducer.captureTypes.size()))
            return;
        chain.accumulatorStores = WTF::move(stores);
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    const Builtins& m_builtins;
    Vector<uint32_t> m_useCounts;
};

struct IndexTypes {
    TypeID zero;
    TypeID one;
    TypeID minusOne;
    TypeID index;
    TypeID indexBelowLength;
    TypeID count;
    TypeID indexOrMinusOne;
    static constexpr int64_t lengthsMultipliedEnd = 1 << 28;
    TypeID endOfLengthsMultiplied;
    TypeID lengthMultiplied;
    TypeID greatestIndex;
    TypeID sum;
    TypeID sumWithRoom;
    std::array<TypeID, 4> numberOfParts;
    std::array<TypeID, 4> upToParts;
};

class FusedLoopEmitter {
public:
    FusedLoopEmitter(const Program& program, Function& function, const IndexTypes& types)
        : m_program(program)
        , m_function(function)
        , m_builtins(Builtins::singleton())
        , m_types(types)
    {
    }

    void emit(const Chain& chain)
    {
        BlockID site(chain.block);
        BlockValues values = std::exchange(m_function.at(site).values, { });
        Terminator terminator = std::exchange(m_function.at(site).terminator, { });
        ValueID result = values[chain.lastIndex];
        Value original = m_function.at(result);
        m_position = original.position;
        m_inlineFrame = original.inlineFrame;
        m_resultType = original.type;

        m_after = addBlock();
        m_function.at(m_after).values.append(values.subspan(chain.lastIndex + 1));
        m_function.at(m_after).terminator = WTF::move(terminator);
        Value parameter = make(Opcode::Parameter, original.type);
        parameter.copyPredictionFrom(original);
        parameter.immediates = { m_after.index(), 0 };
        m_function.at(result) = WTF::move(parameter);
        m_function.at(m_after).parameters.append(result);

        m_asWritten = addBlock();
        BitVector isMoved;
        for (unsigned index : chain.moved)
            isMoved.set(index);
        moveTo(m_asWritten);
        append(make(Opcode::UnlikelyHint, m_program.types.undefined));
        for (unsigned index = 0; index < chain.lastIndex; ++index) {
            if (!isMoved.get(index)) {
                m_function.at(site).values.append(values[index]);
                continue;
            }
            auto& moved = m_function.at(values[index]);
            moved.isOnSlowPath = moved.opcode == Opcode::CallMethod || moved.opcode == Opcode::Call;
            m_function.at(m_asWritten).values.append(values[index]);
        }
        original.isOnSlowPath = original.opcode == Opcode::CallMethod || original.opcode == Opcode::Call;
        original.isInTailPosition = false;
        jump(m_after, { append(WTF::move(original)) });

        moveTo(site);
        auto& arrayClass = m_builtins.arrayClass();
        Value guard = make(Opcode::CallBuiltin, m_program.types.boolean);
        guard.immediates = { m_builtins.hasOriginalStructureOperation().index(), arrayClass.index };
        guard.arguments.append(chain.receiver);
        continueIf(append(WTF::move(guard)));
        m_arrayType = m_program.engineClassTypes[arrayClass.index];
        m_array = claim(chain.receiver, m_arrayType);
        if (mayContainNumbers(chain.receiver)) {
            BlockID holdsDoubles = addBlock();
            BlockID holdsValues = addBlock();
            branch(operation(Operations::IsDoubleArray, m_program.types.boolean, { m_array }), { holdsDoubles, { } }, { holdsValues, { } });
            moveTo(holdsDoubles);
            emitLoop(chain, true);
            moveTo(holdsValues);
        }
        continueIf(operation(Operations::IsContiguousArray, m_program.types.boolean, { m_array }));
        emitLoop(chain, false);
    }

private:
    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }

    bool mayContainNumbers(ValueID array) const
    {
        for (;; array = m_function.at(array).child(0)) {
            TypeID elements = typeOf(array).elements;
            if (elements && !(m_program.at(elements).primitives & Primitive::Number))
                return false;
            if (m_function.at(array).opcode != Opcode::Cast)
                return true;
        }
    }

    BlockID addBlock()
    {
        m_function.blocks.append(Block { });
        return BlockID(m_function.blocks.size() - 1);
    }

    void moveTo(BlockID block) { m_block = block; }

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

    ValueID append(Value&& value)
    {
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(value));
        m_function.at(m_block).values.append(id);
        return id;
    }

    ValueID addParameter(BlockID block, TypeID type)
    {
        Value parameter = make(Opcode::Parameter, type);
        parameter.immediates = { block.index(), static_cast<uint32_t>(m_function.at(block).parameters.size()) };
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(parameter));
        m_function.at(block).parameters.append(id);
        return id;
    }

    void jump(BlockID block, Arguments&& arguments)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = TerminatorKind::Jump;
        terminator.targets.append(Target { block, WTF::move(arguments) });
    }

    void branch(ValueID condition, Target&& taken, Target&& notTaken)
    {
        auto& terminator = m_function.at(m_block).terminator;
        terminator.kind = TerminatorKind::Branch;
        terminator.value = condition;
        terminator.targets.append(WTF::move(taken));
        terminator.targets.append(WTF::move(notTaken));
    }

    void continueIf(ValueID condition)
    {
        BlockID next = addBlock();
        branch(condition, { next, { } }, { m_asWritten, { } });
        moveTo(next);
    }

    void continueUnless(ValueID condition)
    {
        BlockID next = addBlock();
        branch(condition, { m_asWritten, { } }, { next, { } });
        moveTo(next);
    }

    ValueID operation(BuiltinID which, TypeID type, std::initializer_list<ValueID> arguments)
    {
        Value value = make(Opcode::Operation, type);
        value.immediates = { which.index(), Format::none };
        value.arguments = Arguments(arguments);
        return append(WTF::move(value));
    }

    ValueID unary(Opcode opcode, TypeID type, ValueID operand)
    {
        Value value = make(opcode, type);
        value.children[0] = operand;
        return append(WTF::move(value));
    }

    ValueID binary(Opcode opcode, TypeID type, ValueID left, ValueID right)
    {
        Value value = make(opcode, type);
        value.children[0] = left;
        value.children[1] = right;
        return append(WTF::move(value));
    }

    ValueID integer(int64_t number, TypeID type)
    {
        Value value = make(Opcode::Integer, type);
        value.setBits(static_cast<uint64_t>(number));
        return append(WTF::move(value));
    }

    ValueID boolean(bool isTrue)
    {
        Value value = make(Opcode::Boolean, m_program.types.boolean);
        value.immediates[0] = isTrue;
        return append(WTF::move(value));
    }

    ValueID undefined() { return append(make(Opcode::Undefined, m_program.types.undefined)); }

    ValueID cast(ValueID value, TypeID type, CastReason reason)
    {
        Value cast = make(Opcode::Cast, type);
        cast.children[0] = value;
        cast.immediates[1] = static_cast<uint32_t>(reason);
        return append(WTF::move(cast));
    }

    ValueID claim(ValueID value, TypeID type)
    {
        if (m_program.castIsRedundant(typeOf(value), m_program.at(type), CastReason::Claim))
            return value;
        return cast(value, type, CastReason::Claim);
    }

    ValueID choose(ValueID condition, const Invocable<ValueID()> auto& whenTrue, const Invocable<ValueID()> auto& whenFalse)
    {
        BlockID isTrue = addBlock();
        BlockID isFalse = addBlock();
        BlockID joined = addBlock();
        ValueID chosen = addParameter(joined, m_types.index);
        branch(condition, { isTrue, { } }, { isFalse, { } });
        moveTo(isTrue);
        jump(joined, { whenTrue() });
        moveTo(isFalse);
        jump(joined, { whenFalse() });
        moveTo(joined);
        return chosen;
    }

    ValueID atMost(ValueID index, ValueID length)
    {
        return choose(binary(Opcode::Less, m_program.types.boolean, index, length), [&] { return unary(Opcode::ToNumeric, m_types.indexBelowLength, index); }, [&] { return length; });
    }

    ValueID plusOne(ValueID index)
    {
        return binary(Opcode::Add, m_types.count, unary(Opcode::ToNumeric, m_types.indexBelowLength, index), integer(1, m_types.one));
    }

    TypeID resultTypeOf(FunctionID function) const
    {
        TypeID type = m_program.at(function).resultType;
        return type ? type : m_program.types.anything;
    }

    ValueID call(const Link& link, std::initializer_list<ValueID> arguments)
    {
        Value value = make(Opcode::Call, resultTypeOf(link.function));
        value.children[0] = link.callback;
        value.children[1] = undefined();
        value.arguments = Arguments(arguments);
        return append(WTF::move(value));
    }

    ValueID callForParts(const Link& link, ValueID element)
    {
        auto& callee = m_program.at(link.function);
        Value value = make(Opcode::CallDirect, m_program.types.undefined);
        value.immediates = { link.functionReturningParts.index(), Format::none };
        auto& closure = m_function.at(m_program.originOf(m_function, link.callback));
        if (closure.opcode == Opcode::NewClosure)
            value.arguments = closure.arguments;
        if (!callee.parameterTypes.isEmpty()) {
            TypeID type = callee.parameterTypes[0];
            CastReason reason = callee.parameterCastReason(0);
            if (type && !m_program.castIsRedundant(typeOf(element), m_program.at(type), reason)) {
                element = cast(element, type, reason);
                m_function.at(element).setParameterCheckCallee(link.function);
            }
            value.arguments.append(element);
        }
        return append(WTF::move(value));
    }

    ValueID resultAt(ValueID call, unsigned index, TypeID type)
    {
        Value value = make(Opcode::ResultAt, type);
        value.children[0] = call;
        value.immediates = { index, Format::none };
        return append(WTF::move(value));
    }

    void continueUnlessNullish(ValueID value)
    {
        if (typeOf(value).mayBeUndefinedOrNull())
            continueUnless(unary(Opcode::IsUndefinedOrNull, m_program.types.boolean, value));
    }

    void finish(ValueID result)
    {
        jump(m_after, { claim(result, m_resultType) });
    }

    void emitLoop(const Chain& chain, bool holdsDoubles)
    {
        bool hasFilter = chain.stages.containsIf([](auto& stage) {
            return stage.isFilter;
        });
        bool stagesMayRestart = chain.stages.containsIf([](auto& stage) {
            return stage.testsInput;
        });
        bool exitsEarly = chain.end == ChainEnd::Some || chain.end == ChainEnd::Every || chain.end == ChainEnd::Find || chain.end == ChainEnd::FindIndex;
        bool visitsAllElements = exitsEarly && stagesMayRestart;
        const Link* flattened = chain.stages.last().isFlatMap ? &chain.stages.last() : nullptr;
        bool countsResults = chain.end == ChainEnd::Length || ((chain.end == ChainEnd::Array || chain.end == ChainEnd::FindIndex) && (hasFilter || chain.isSliced || flattened));

        ValueID length = [&] {
            Value read = make(Opcode::GetById, m_types.index);
            read.children[0] = m_array;
            read.immediates = { m_program.stringOf(m_builtins.nameOf(asBytes("length"_s.span8()))).index(), Format::none };
            return append(WTF::move(read));
        }();
        ValueID zero = integer(0, m_types.zero);
        ValueID first = zero;
        ValueID capacity = length;
        if (chain.isSliced) {
            first = atMost(chain.sliceStart, length);
            if (chain.sliceEnd)
                length = atMost(chain.sliceEnd, length);
            if (chain.end == ChainEnd::Array)
                capacity = choose(binary(Opcode::Less, m_program.types.boolean, first, length), [&] { return binary(Opcode::Sub, m_types.index, length, first); }, [&] { return zero; });
        }
        if (flattened && chain.end == ChainEnd::Array) {
            continueIf(binary(Opcode::Less, m_program.types.boolean, capacity, integer(IndexTypes::lengthsMultipliedEnd, m_types.endOfLengthsMultiplied)));
            unsigned factor = std::max(flattened->greatestNumberOfParts, 1u);
            capacity = binary(Opcode::Mul, m_types.index, unary(Opcode::ToNumeric, m_types.lengthMultiplied, capacity), integer(factor, m_types.numberOfParts[factor]));
        }
        ValueID made;
        if (chain.end == ChainEnd::Array)
            made = operation(countsResults ? Operations::NewArrayWithCapacity : Operations::NewArrayWithSpecies, m_arrayType, { capacity, m_array });

        Vector<TypeID, 4> stateTypes;
        Arguments initialState;
        auto addState = [&](TypeID type, ValueID initial) {
            stateTypes.append(type);
            initialState.append(initial);
            return stateTypes.size() - 1;
        };
        size_t countState = notFound;
        size_t accumulatorState = notFound;
        size_t isDoneState = notFound;
        size_t foundState = notFound;
        bool sumsParts = flattened && chain.end == ChainEnd::Length;
        if (countsResults)
            countState = addState(sumsParts ? m_types.sum : m_types.index, zero);
        auto fieldTypeOf = [&](ValueID store) {
            TypeID type = m_program.at(m_function.at(store).layout()).fields[m_function.at(store).field()].type;
            return type ? type : m_program.types.anything;
        };
        auto makeAccumulator = [&](const Arguments& held, size_t firstField) {
            ValueID object = append(Value(m_function.at(chain.initial)));
            for (size_t k = 0; k < chain.accumulatorStores.size(); ++k) {
                Value store(m_function.at(chain.accumulatorStores[k]));
                store.children[0] = object;
                store.children[1] = held[firstField + k];
                append(WTF::move(store));
            }
            return object;
        };
        if (!chain.accumulatorStores.isEmpty()) {
            for (ValueID store : chain.accumulatorStores) {
                size_t added = addState(fieldTypeOf(store), m_function.at(store).child(1));
                if (accumulatorState == notFound)
                    accumulatorState = added;
            }
        } else if (chain.end == ChainEnd::Reduce) {
            TypeID returned = resultTypeOf(chain.last.function);
            TypeID held = typeOf(chain.initial).isSubsetOf(m_program.at(returned), m_program) ? returned : m_program.types.anything;
            accumulatorState = addState(held, chain.initial);
        }
        TypeID foundType = chain.end == ChainEnd::Find ? m_program.types.anything : chain.end == ChainEnd::FindIndex ? m_types.indexOrMinusOne : m_program.types.boolean;
        auto notFoundResult = [&] {
            switch (chain.end) {
            case ChainEnd::Some:
                return boolean(false);
            case ChainEnd::Every:
                return boolean(true);
            case ChainEnd::Find:
                return undefined();
            default:
                return integer(-1, m_types.minusOne);
            }
        };
        if (visitsAllElements) {
            isDoneState = addState(m_program.types.boolean, boolean(false));
            foundState = addState(foundType, notFoundResult());
        }

        BlockID head = addBlock();
        BlockID body = addBlock();
        BlockID next = addBlock();
        BlockID done = addBlock();
        ValueID index = addParameter(head, m_types.index);
        Arguments state;
        Arguments stateAtNext;
        for (TypeID type : stateTypes) {
            state.append(addParameter(head, type));
            stateAtNext.append(addParameter(next, type));
        }
        {
            Arguments entered { first };
            entered.appendVector(initialState);
            jump(head, WTF::move(entered));
        }

        moveTo(head);
        branch(binary(Opcode::Less, m_program.types.boolean, index, length), { body, { } }, { done, { } });

        moveTo(next);
        {
            Arguments repeated { plusOne(index) };
            repeated.appendVector(stateAtNext);
            jump(head, WTF::move(repeated));
        }

        moveTo(done);
        switch (chain.end) {
        case ChainEnd::Array:
            finish(operation(Operations::TruncateNewArray, m_arrayType, { made, countsResults ? state[countState] : length }));
            break;
        case ChainEnd::Length:
            if (sumsParts) {
                continueIf(binary(Opcode::LessEq, m_program.types.boolean, state[countState], integer(UINT32_MAX, m_types.greatestIndex)));
                finish(unary(Opcode::ToNumeric, m_types.index, state[countState]));
                break;
            }
            finish(state[countState]);
            break;
        case ChainEnd::Reduce:
            finish(chain.accumulatorStores.isEmpty() ? state[accumulatorState] : makeAccumulator(state, accumulatorState));
            break;
        default:
            finish(visitsAllElements ? state[foundState] : notFoundResult());
            break;
        }

        moveTo(body);
        append(make(Opcode::LoopHint, m_program.types.undefined));
        ValueID element;
        if (holdsDoubles) {
            element = operation(Operations::GetDoubleIndex, m_program.types.number, { m_array, index });
            continueIf(binary(Opcode::StrictEq, m_program.types.boolean, element, element));
        } else {
            continueIf(operation(Operations::HasContiguousIndex, m_program.types.boolean, { m_array, index }));
            element = operation(Operations::GetContiguousIndex, m_program.types.anything, { m_array, index });
        }
        for (auto& stage : chain.stages) {
            if (stage.testsInput)
                continueUnlessNullish(element);
            if (stage.isFlatMap) {
                element = callForParts(stage, element);
                break;
            }
            ValueID returned = call(stage, { element });
            if (!stage.isFilter) {
                element = returned;
                continue;
            }
            BlockID passed = addBlock();
            branch(unary(Opcode::ToBoolean, m_program.types.boolean, returned), { passed, { } }, { next, Arguments(state) });
            moveTo(passed);
        }

        Arguments changed(state);
        if (isDoneState != notFound) {
            BlockID searching = addBlock();
            branch(state[isDoneState], { next, Arguments(state) }, { searching, { } });
            moveTo(searching);
        }
        if (flattened) {
            unsigned greatest = flattened->greatestNumberOfParts;
            ValueID numberOfParts = resultAt(element, 0, m_types.upToParts[greatest]);
            if (sumsParts)
                changed[countState] = binary(Opcode::Add, m_types.sum, unary(Opcode::ToNumeric, m_types.sumWithRoom, state[countState]), numberOfParts);
            for (unsigned k = 0; k < greatest && !sumsParts; ++k) {
                BlockID hasPart = addBlock();
                BlockID behindPart = addBlock();
                ValueID countBehindPart = addParameter(behindPart, m_types.index);
                branch(binary(Opcode::Less, m_program.types.boolean, integer(k, m_types.numberOfParts[k]), numberOfParts), { hasPart, { } }, { behindPart, { changed[countState] } });
                moveTo(hasPart);
                continueIf(operation(Operations::TryPutByValDirect, m_program.types.boolean, { made, changed[countState], resultAt(element, k + 1, m_program.types.anything) }));
                jump(behindPart, { plusOne(changed[countState]) });
                moveTo(behindPart);
                changed[countState] = countBehindPart;
            }
            jump(next, WTF::move(changed));
            return;
        }
        switch (chain.end) {
        case ChainEnd::Array:
            continueIf(operation(Operations::TryPutByValDirect, m_program.types.boolean, { made, countsResults ? state[countState] : index, element }));
            if (countsResults)
                changed[countState] = plusOne(state[countState]);
            break;
        case ChainEnd::Length:
            changed[countState] = plusOne(state[countState]);
            break;
        case ChainEnd::Reduce: {
            if (chain.last.testsInput)
                continueUnlessNullish(element);
            if (!chain.accumulatorStores.isEmpty()) {
                ValueID returned = call(chain.last, { makeAccumulator(state, accumulatorState), element });
                for (size_t k = 0; k < chain.accumulatorStores.size(); ++k) {
                    auto& store = m_function.at(chain.accumulatorStores[k]);
                    Value read = make(Opcode::GetById, fieldTypeOf(chain.accumulatorStores[k]));
                    read.children[0] = returned;
                    read.immediates = { m_program.at(store.layout()).fields[store.field()].name.index(), Format::none };
                    changed[accumulatorState + k] = append(WTF::move(read));
                }
                break;
            }
            if (chain.testsAccumulator)
                continueUnlessNullish(state[accumulatorState]);
            ValueID returned = call(chain.last, { state[accumulatorState], element });
            changed[accumulatorState] = typeOf(returned).isSubsetOf(m_program.at(stateTypes[accumulatorState]), m_program) ? returned : cast(returned, stateTypes[accumulatorState], CastReason::Upcast);
            break;
        }
        default: {
            if (chain.last.testsInput)
                continueUnlessNullish(element);
            ValueID answer = unary(Opcode::ToBoolean, m_program.types.boolean, call(chain.last, { element }));
            BlockID decided = addBlock();
            BlockID undecided = addBlock();
            if (chain.end == ChainEnd::Every)
                branch(answer, { undecided, { } }, { decided, { } });
            else
                branch(answer, { decided, { } }, { undecided, { } });
            moveTo(decided);
            ValueID found = chain.end == ChainEnd::Some ? boolean(true) : chain.end == ChainEnd::Every ? boolean(false) : chain.end == ChainEnd::Find ? element : countsResults ? state[countState] : index;
            if (visitsAllElements) {
                Arguments decidedState(state);
                decidedState[isDoneState] = boolean(true);
                decidedState[foundState] = found;
                jump(next, WTF::move(decidedState));
            } else
                finish(found);
            moveTo(undecided);
            if (countsResults)
                changed[countState] = plusOne(state[countState]);
            break;
        }
        }
        jump(next, WTF::move(changed));
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    const IndexTypes& m_types;
    uint32_t m_position { 0 };
    uint32_t m_inlineFrame { 0 };
    TypeID m_resultType;
    TypeID m_arrayType;
    ValueID m_array;
    BlockID m_after;
    BlockID m_asWritten;
    BlockID m_block;
};

FunctionID addFunctionReturningParts(Program& program, FunctionID id, unsigned greatest, const IndexTypes& types)
{
    Function function = std::as_const(program).functions[id.index()];
    auto add = [&](unsigned block, Opcode opcode, TypeID type, uint32_t position) {
        Value value;
        value.opcode = opcode;
        value.position = position;
        value.type = type;
        value.rep = repOf(program.at(type));
        ValueID made(function.values.size());
        function.values.append(WTF::move(value));
        function.blocks[block].values.append(made);
        return made;
    };
    Vector<std::pair<unsigned, ValueID>, 4> literals;
    BitVector onlyReturns = CallbackAnalysis::blocksThatOnlyReturnTheirParameter(function);
    bool areAllLiterals = CallbackAnalysis::forEachLiteralReturnedBy(function, [&](unsigned block, ValueID literal) {
        literals.append({ block, literal });
        return true;
    });
    RELEASE_ASSERT(areAllLiterals);
    for (auto [block, literal] : literals) {
        Arguments parts = function.at(literal).arguments;
        uint32_t position = function.at(literal).position;
        uint32_t inlineFrame = function.at(literal).inlineFrame;
        function.blocks[block].values.removeFirst(literal);
        Arguments results;
        results.append(add(block, Opcode::Integer, types.numberOfParts[parts.size()], position));
        function.at(results.last()).setBits(parts.size());
        for (unsigned k = 0; k < greatest; ++k) {
            if (k >= parts.size()) {
                results.append(add(block, Opcode::Unspecified, program.types.anything, position));
                continue;
            }
            if (function.at(parts[k]).type == program.types.anything) {
                results.append(parts[k]);
                continue;
            }
            results.append(add(block, Opcode::Cast, program.types.anything, position));
            function.at(results.last()).children[0] = parts[k];
            function.at(results.last()).immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
        }
        ValueID returned = add(block, Opcode::Results, program.types.undefined, position);
        function.at(returned).arguments = WTF::move(results);
        for (ValueID made : function.blocks[block].values) {
            if (made.index() >= std::as_const(program).functions[id.index()].values.size())
                function.at(made).inlineFrame = inlineFrame;
        }
        auto& terminator = function.blocks[block].terminator;
        terminator = Terminator { };
        terminator.kind = TerminatorKind::Return;
        terminator.value = returned;
    }
    for (size_t b : onlyReturns) {
        auto& block = function.blocks[b];
        block.parameters.clear();
        block.terminator = Terminator { };
        block.terminator.kind = TerminatorKind::Unreachable;
    }
    function.resultType = program.types.undefined;
    function.resultTypes.append(types.upToParts[greatest]);
    for (unsigned k = 0; k < greatest; ++k)
        function.resultTypes.append(program.types.anything);
    function.flags |= Format::FunctionIsHiddenInStackTraces;
    function.worker = { };
    function.nullishResultKind = 0;

    FunctionID made(program.functions.size());
    program.functions.append(WTF::move(function));
    program.numberOfDirectCalls.append(1);
    auto copyBit = [&](BitVector& bits) {
        if (bits.get(id.index()))
            bits.set(made.index());
    };
    copyBit(program.staticallyHotFunctions);
    copyBit(program.staticallyWarmFunctions);
    copyBit(program.functionsHotOnlyInLoops);
    copyBit(program.functionsInCycles);
    copyBit(program.functionsWithInliningDisabled);
    copyBit(program.callGraphColdFunctions);
    copyBit(program.functionsWithSiteCaches);
    return made;
}

} // anonymous namespace

void fuseArrayMethodChains(Program& program)
{
    static constexpr OpcodeSet methodCalls { Opcode::CallMethod };
    auto& arrayClass = Builtins::singleton().arrayClass();
    if (!program.engineClassTypes[arrayClass.index] || !program.stringOf(Builtins::singleton().nameOf(asBytes("length"_s.span8()))))
        return;
    struct Found {
        unsigned function;
        Vector<Chain> chains;
    };
    auto found = collectInParallel<Found>(program.functions.size(), [&](unsigned i, Vector<Found>& collected) {
        if (!program.functions.mayContain(i, methodCalls))
            return;
        auto& function = std::as_const(program).functions[i];
        if (function.blocks.isEmpty() || function.has(Format::FunctionIsBuiltin) || function.isGenericEntrypoint())
            return;
        unsigned numberOfLinks = 0;
        std::as_const(program).functions.forEachIndexedValue(i, methodCalls, [&](ValueID id) {
            numberOfLinks += ChainFinder::mayStartChain(program.builtinName(function.at(id).string()));
        });
        if (!numberOfLinks)
            return;
        auto chains = ChainFinder(program, FunctionID(i)).run();
        if (!chains.isEmpty())
            collected.append(Found { i, WTF::move(chains) });
    });
    if (found.isEmpty())
        return;
    program.types.thaw();
    IndexTypes types;
    types.zero = program.types.intern(Type::integer(Range::one(0)));
    types.one = program.types.intern(Type::integer(Range::one(1)));
    types.minusOne = program.types.intern(Type::integer(Range::one(-1)));
    types.index = program.types.intern(Type::integer(Range::uint32()));
    types.indexBelowLength = program.types.intern(Type::integer({ 0, UINT32_MAX - 1 }));
    types.count = program.types.intern(Type::integer({ 1, UINT32_MAX }));
    types.indexOrMinusOne = program.types.intern(Type::integer({ -1, UINT32_MAX }));
    types.endOfLengthsMultiplied = program.types.intern(Type::integer(Range::one(IndexTypes::lengthsMultipliedEnd)));
    types.lengthMultiplied = program.types.intern(Type::integer({ 0, IndexTypes::lengthsMultipliedEnd - 1 }));
    types.greatestIndex = program.types.intern(Type::integer(Range::one(UINT32_MAX)));
    types.sum = program.types.intern(Type::integer({ 0, maxExactInteger }));
    types.sumWithRoom = program.types.intern(Type::integer({ 0, maxExactInteger - 4 }));
    for (unsigned k = 0; k < 4; ++k) {
        types.numberOfParts[k] = program.types.intern(Type::integer(Range::one(k)));
        types.upToParts[k] = program.types.intern(Type::integer({ 0, k }));
    }
    program.types.freeze();
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsReturningParts;
    for (auto& entry : found) {
        for (auto& chain : entry.chains) {
            auto& stage = chain.stages.last();
            if (!stage.isFlatMap)
                continue;
            stage.functionReturningParts = functionsReturningParts.ensure(stage.function.index(), [&] {
                return addFunctionReturningParts(program, stage.function, stage.greatestNumberOfParts, types);
            }).iterator->value;
        }
    }
    forEachIndexInParallel(found.size(), [&](unsigned k) {
        auto& function = program.functions[found[k].function];
        FusedLoopEmitter emitter(program, function, types);
        for (auto& chain : found[k].chains | std::views::reverse)
            emitter.emit(chain);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
