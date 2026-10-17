/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTEmitter.h"
#include "AirCode.h"
#include "B3Procedure.h"
#include "AOTTypedThreads.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class CoroutineSplitter {
public:
    struct Numbers {
        FunctionID ramp;
        FunctionID resume;
        LayoutID frameLayout;
        TypeID frameType;
        TypeID resumeModeType;
        TypeID driverType;
        bool hasStartFunction { false };
        bool savesFrameOnSuspend { false };
        Vector<ASCIILiteral, 16> namesByFirstUse;
    };

    static bool canSaveFrame(const Function& function)
    {
        if (function.has(Format::FunctionIsGenerator | Format::FunctionIsModuleBody | FunctionUsesArgumentBuffer) || function.kind == FunctionKind::Module)
            return false;
        return true;
    }

    static bool hasStartFunction(const Function& function)
    {
        if (function.has(Format::FunctionIsGenerator))
            return false;
        unsigned numberOfAwaits = 0;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values)
                numberOfAwaits += function.at(id).opcode == Opcode::Await;
        }
        if (numberOfAwaits > maxAwaitsWithStartFunction)
            return false;
        if (!numberOfAwaits)
            return true;
        bool mayReturnFirst = false;
        unsigned both = duplicatedValueCount(function, nullptr, &mayReturnFirst);
        return both < (mayReturnFirst ? minDuplicatedValuesIfMayReturnFirst : minDuplicatedValuesWithoutStartFunction);
    }

    static unsigned duplicatedValueCount(const Function& function, BitVector* awaitsOnlyInFirstActivation, bool* mayReturnFirst)
    {
        unsigned numberOfBlocks = function.blocks.size();
        Vector<unsigned> valuesBeforeAwait(numberOfBlocks, [&](size_t b) {
            unsigned ahead = 0;
            for (ValueID id : function.blocks[b].values) {
                if (function.at(id).opcode == Opcode::Await)
                    return ahead;
                ++ahead;
            }
            return UINT_MAX;
        });
        BitVector isInStart(numberOfBlocks);
        BitVector isInResume(numberOfBlocks);
        Vector<unsigned> worklist { 0u };
        Vector<unsigned> afterAwaits;
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            if (isInStart.set(b))
                continue;
            for (auto& target : function.blocks[b].terminator.targets)
                (valuesBeforeAwait[b] != UINT_MAX ? afterAwaits : worklist).append(target.block.index());
        }
        while (!afterAwaits.isEmpty()) {
            unsigned b = afterAwaits.takeLast();
            if (isInResume.set(b))
                continue;
            for (auto& target : function.blocks[b].terminator.targets)
                afterAwaits.append(target.block.index());
        }
        unsigned both = 0;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!isInStart.get(b))
                continue;
            if (isInResume.get(b))
                both += valuesBeforeAwait[b] != UINT_MAX ? valuesBeforeAwait[b] : function.blocks[b].values.size();
            else if (awaitsOnlyInFirstActivation && valuesBeforeAwait[b] != UINT_MAX)
                awaitsOnlyInFirstActivation->set(function.blocks[b].values[valuesBeforeAwait[b]].index());
        }
        if (mayReturnFirst) {
            Vector<unsigned> cycles = computeBlockCycles(function);
            BitVector cycleAwaits(numberOfBlocks);
            for (unsigned b = 0; b < numberOfBlocks; ++b) {
                if (valuesBeforeAwait[b] != UINT_MAX && cycles[b] != notInCycle)
                    cycleAwaits.set(cycles[b]);
            }
            BitVector isSeen(numberOfBlocks);
            worklist.append(0u);
            while (!worklist.isEmpty()) {
                unsigned b = worklist.takeLast();
                if (isSeen.set(b) || valuesBeforeAwait[b] != UINT_MAX || (cycles[b] != notInCycle && cycleAwaits.get(cycles[b])))
                    continue;
                *mayReturnFirst |= function.blocks[b].terminator.kind == TerminatorKind::Return;
                for (auto& target : function.blocks[b].terminator.targets)
                    worklist.append(target.block.index());
            }
        }
        return both;
    }

    static Numbers reserveNumbers(Program& program, FunctionID ramp, FunctionID resume, bool savesFrameOnSuspend)
    {
        auto& types = program.types;
        bool isGenerator = std::as_const(program).at(ramp).has(Format::FunctionIsGenerator);
        Numbers numbers;
        numbers.ramp = ramp;
        numbers.resume = resume;
        numbers.savesFrameOnSuspend = savesFrameOnSuspend;
        if (savesFrameOnSuspend) {
            numbers.hasStartFunction = true;
            numbers.driverType = types.intern(Type::engineClass(Builtins::singleton().promiseClass()));
            return numbers;
        }
        numbers.frameLayout = LayoutID(program.layouts.size());
        numbers.frameType = types.intern(Type::forLayout(numbers.frameLayout));
        int64_t maxResumeMode = isGenerator ? resumeModeReturn : resumeModeThrow;
        if (std::as_const(program).at(ramp).has(Format::FunctionIsAsync))
            maxResumeMode |= CoroutineFrame::isEnteredByJobLoop;
        numbers.resumeModeType = types.intern(Type::integer({ resumeModeNormal, maxResumeMode }));
        numbers.driverType = isGenerator ? types.object : types.intern(Type::engineClass(Builtins::singleton().promiseClass()));
        program.layouts.append(Layout { });
        return numbers;
    }

    static void replaceProvisionalNames(Program& program, const Numbers& numbers, std::span<const StringID> names)
    {
        if (!numbers.savesFrameOnSuspend) {
            for (auto& field : program.at(numbers.frameLayout).fields) {
                if (field.name && field.name.index() >= firstProvisionalName)
                    field.name = names[field.name.index() - firstProvisionalName];
            }
        }
    }

    static void removeUnusedParameters(const Program& program, Function& function)
    {
        Vector<unsigned> uses;
        for (;;) {
            uses.fill(0, function.values.size());
            auto use = [&](ValueID id) {
                ++uses[id.index()];
            };
            for (auto& block : function.blocks) {
                for (ValueID id : block.values)
                    std::as_const(function.at(id)).forEachChild(use);
                if (block.terminator.value)
                    use(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (target.arguments[i] != parameters[i])
                            use(target.arguments[i]);
                    }
                }
            }
            BitVector isRemoved;
            for (unsigned b = 1; b < function.blocks.size(); ++b) {
                for (ValueID parameter : function.blocks[b].parameters) {
                    if (!uses[parameter.index()])
                        isRemoved.set(parameter.index());
                }
            }
            if (isRemoved.isEmpty())
                return;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    Arguments kept;
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (!isRemoved.get(parameters[i].index()))
                            kept.append(target.arguments[i]);
                    }
                    if (kept.size() != target.arguments.size())
                        target.arguments = WTF::move(kept);
                }
            }
            for (unsigned b = 1; b < function.blocks.size(); ++b) {
                auto& block = function.blocks[b];
                if (!block.parameters.removeAllMatching([&](ValueID parameter) { return isRemoved.get(parameter.index()); }))
                    continue;
                for (unsigned i = 0; i < block.parameters.size(); ++i)
                    function.at(block.parameters[i]).immediates = { b, i };
            }
            for (size_t parameter : isRemoved) {
                Value nothing;
                nothing.position = function.values[parameter].position;
                nothing.inlineFrame = function.values[parameter].inlineFrame;
                nothing.type = program.types.undefined;
                nothing.rep = repOf(program.at(nothing.type));
                function.values[parameter] = WTF::move(nothing);
            }
        }
    }

    static void recordCellsLiveAcrossAwaits(const Program& program, Function& function)
    {
        auto savesFrame = [&](const Value& value) {
            if (value.opcode == Opcode::Operation)
                return program.operationOf(value) == Operations::AwaitBySavingFrame;
            if (value.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::AwaitBySavingFrame;
        };
        struct Site {
            unsigned block;
            unsigned index;
        };
        Vector<Site, 8> sites;
        BitVector isTracked(function.blocks.size());
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            bool awaits = false;
            for (unsigned i = 0; i < block.values.size(); ++i) {
                if (!savesFrame(function.at(block.values[i])))
                    continue;
                sites.append({ b, i });
                awaits = true;
            }
            if (!awaits)
                continue;
            for (auto& target : block.terminator.targets)
                isTracked.set(target.block.index());
        }
        if (sites.isEmpty())
            return;
        if (!program.hasAllocationAssertions)
            removeUnusedParameters(program, function);
        auto liveAtHead = computeLiveAtHeads(function, isTracked);
        BitVector isLive(function.values.size());
        Vector<uint64_t> firstUse(function.values.size());
        Vector<ValueID> live;
        Vector<ValueID> listed;
        Vector<Arguments> retainedAt(sites.size());
        for (size_t end = sites.size(); end;) {
            size_t begin = end;
            while (begin && sites[begin - 1].block == sites[end - 1].block)
                --begin;
            auto& block = function.blocks[sites[begin].block];
            uint64_t place = block.values.size();
            uint32_t ordinal = 0;
            auto use = [&](ValueID used) {
                uint32_t number = ordinal++;
                if (!used)
                    return;
                if (!isLive.set(used.index()))
                    live.append(used);
                else if (firstUse[used.index()] >> 32 == place)
                    return;
                firstUse[used.index()] = place << 32 | number;
            };
            use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
                for (uint32_t atHead : liveAtHead[target.block.index()])
                    use(ValueID(atHead));
            }
            size_t site = end;
            while (place-- > sites[begin].index) {
                ValueID id = block.values[place];
                isLive.clear(id.index());
                if (site > begin && sites[site - 1].index == place) {
                    --site;
                    Arguments kept = function.at(id).arguments;
                    kept.shrink(2);
                    live.removeAllMatching([&](ValueID one) {
                        return !isLive.get(one.index());
                    });
                    listed.shrink(0);
                    for (ValueID one : live) {
                        if (one != kept[1] && program.at(function.at(one).type).hasCells() && !isAlwaysRematerialized(program, function.at(one)))
                            listed.append(one);
                    }
                    std::ranges::sort(listed, { }, [&](ValueID one) {
                        return firstUse[one.index()];
                    });
                    kept.appendVector(listed);
                    retainedAt[site] = WTF::move(kept);
                }
                ordinal = 0;
                std::as_const(function.at(id)).forEachChild(use);
            }
            for (ValueID one : live)
                isLive.clear(one.index());
            live.shrink(0);
            end = begin;
        }
        for (size_t site = 0; site < sites.size(); ++site)
            function.at(function.blocks[sites[site].block].values[sites[site].index]).arguments = WTF::move(retainedAt[site]);
    }

    static BitVector objectsLiveAcrossSuspensions(const Function& function)
    {
        struct Site {
            unsigned block;
            unsigned index;
        };
        Vector<Site, 8> sites;
        BitVector isTracked(function.blocks.size());
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            bool suspends = false;
            for (unsigned i = 0; i < block.values.size(); ++i) {
                Opcode opcode = function.at(block.values[i]).opcode;
                if (opcode != Opcode::Await && opcode != Opcode::Yield)
                    continue;
                sites.append({ b, i });
                suspends = true;
            }
            if (!suspends)
                continue;
            for (auto& target : block.terminator.targets)
                isTracked.set(target.block.index());
        }
        BitVector isLiveAcross(function.values.size());
        if (sites.isEmpty())
            return isLiveAcross;
        auto liveAtHead = computeLiveAtHeads(function, isTracked);
        BitVector isCreatedAfter(function.values.size());
        for (auto [b, index] : sites) {
            auto& block = function.blocks[b];
            isCreatedAfter.clearAll();
            for (unsigned i = index; i < block.values.size(); ++i)
                isCreatedAfter.set(block.values[i].index());
            auto note = [&](ValueID live) {
                if (!live || isCreatedAfter.get(live.index()))
                    return;
                while (function.at(live).opcode == Opcode::Cast)
                    live = function.at(live).child(0);
                Opcode opcode = function.at(live).opcode;
                if (opcode == Opcode::NewObjectWithShape || opcode == Opcode::NewObject || opcode == Opcode::NewArray)
                    isLiveAcross.set(live.index());
            };
            for (unsigned i = index + 1; i < block.values.size(); ++i)
                function.at(block.values[i]).forEachChild(note);
            note(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    note(argument);
                for (uint32_t live : liveAtHead[target.block.index()])
                    note(ValueID(live));
            }
        }
        return isLiveAcross;
    }

    CoroutineSplitter(Program& program, Numbers& numbers, std::array<unsigned, 2> numberOfRegisters)
        : m_program(program)
        , m_numbers(numbers)
        , m_ramp(numbers.ramp)
        , m_numberOfRegisters(numberOfRegisters)
    {
    }

    void run()
    {
        m_isGenerator = std::as_const(m_program).at(m_ramp).has(Format::FunctionIsGenerator);
        m_isAsync = std::as_const(m_program).at(m_ramp).has(Format::FunctionIsAsync);
        m_startsEagerly = std::as_const(m_program).at(m_ramp).has(Format::FunctionStartsEagerly);
        m_position = std::as_const(m_program).at(m_ramp).position;
        m_hasStartFunction = m_numbers.hasStartFunction;
        if (m_numbers.savesFrameOnSuspend) {
            keepBodyUnsplit();
            return;
        }
        if (!m_hasStartFunction && !m_isGenerator)
            duplicatedValueCount(std::as_const(m_program).at(m_ramp), &m_awaitsOnlyInFirstActivation, nullptr);
        createFrameLayout();
        if (m_hasStartFunction)
            copyBodyToStartFunction();
        moveBodyToResumeFunction();
        createDispatchBlock();
        replaceResumeModeReads();
        splitAtSuspensionPoints();
        rewriteReturns();
        spillAndReload();
        finishDispatchBlock();
        m_program.at(m_resume) = WTF::move(m_body);
        if (m_hasStartFunction) {
            finishStartFunction();
            m_program.at(FunctionID(m_resume.index() + 1)) = WTF::move(m_startBody);
        }
        buildRamp();
    }

    void splitAgain(Function&& body)
    {
        m_isGenerator = true;
        m_isAsync = body.has(Format::FunctionIsAsync);
        m_startsEagerly = body.has(Format::FunctionStartsEagerly);
        m_position = body.position;
        m_frameLayout = m_numbers.frameLayout;
        m_frameType = m_numbers.frameType;
        m_resumeModeType = m_numbers.resumeModeType;
        m_driverType = m_numbers.driverType;
        m_resumeFunctionResultType = m_program.types.anything;
        m_resume = m_numbers.resume;
        auto& fields = m_program.at(m_frameLayout).fields;
        Vector<TypeID> argumentTypes;
        for (unsigned i = 0; i < body.blocks[0].parameters.size(); ++i)
            argumentTypes.append(fields[CoroutineFrame::firstArgumentField + i].type);
        fields.shrink(CoroutineFrame::firstArgumentField);
        auto& split = m_program.at(m_resume);
        split.blocks.clear();
        split.values.clear();
        split.suspensionPositions.clear();
        m_body = WTF::move(split);
        m_body.blocks = WTF::move(body.blocks);
        m_body.values = WTF::move(body.values);
        m_body.inlineFrames = WTF::move(body.inlineFrames);
        m_body.allocations = WTF::move(body.allocations);
        m_body.allocationNotes = WTF::move(body.allocationNotes);
        m_body.allocationNoteIndices = WTF::move(body.allocationNoteIndices);
        m_body.cleanUpMovedStores = body.cleanUpMovedStores;
        createDispatchBlock();
        RELEASE_ASSERT(m_argumentTypes == argumentTypes);
        replaceResumeModeReads();
        splitAtSuspensionPoints();
        rewriteReturns();
        spillAndReload();
        finishDispatchBlock();
        m_program.at(m_resume) = WTF::move(m_body);
    }

private:
    struct SuspensionPoint {
        BlockID suspendBlock;
        BlockID resumeBlock;
        unsigned spillIndex { 0 };
        ValueID suspension;
        bool isReachable { true };
        bool isReachableInResumeFunction { true };
        Vector<std::pair<ValueID, uint32_t>> spilled { };
        ValueID registration { };
    };

    static constexpr uint32_t firstProvisionalName = 0xc0000000;
    static constexpr unsigned maxAwaitsWithStartFunction = 32;
    static constexpr unsigned minDuplicatedValuesWithoutStartFunction = 8;
    static constexpr unsigned minDuplicatedValuesIfMayReturnFirst = 64;
    static constexpr unsigned maxSuspensionsKeptInRegisters = 32;
    static constexpr int64_t resumeModeNormal = 0;
    static constexpr int64_t resumeModeThrow = 1;
    static constexpr int64_t resumeModeReturn = 2;

    StringID provisionalNameOf(ASCIILiteral name)
    {
        auto& names = m_numbers.namesByFirstUse;
        size_t index = names.findIf([&](ASCIILiteral other) {
            return equalSpans(other.span8(), name.span8());
        });
        if (index == notFound) {
            index = names.size();
            names.append(name);
        }
        return StringID(firstProvisionalName + static_cast<uint32_t>(index));
    }

    ValueID addValue(Function& function, Value&& value)
    {
        ValueID id(function.values.size());
        value.position = m_position;
        function.values.append(WTF::move(value));
        return id;
    }

    ValueID append(Function& function, BlockID block, Value&& value)
    {
        ValueID id = addValue(function, WTF::move(value));
        function.at(block).values.append(id);
        return id;
    }

    BlockID addBlock(Function& function)
    {
        function.blocks.append(Block { });
        return BlockID(function.blocks.size() - 1);
    }

    Value typed(Opcode opcode, TypeID type) const
    {
        Value value;
        value.opcode = opcode;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        return value;
    }

    ValueID addParameter(Function& function, BlockID block, TypeID type)
    {
        Value parameter = typed(Opcode::Parameter, type);
        parameter.immediates = { block.index(), static_cast<uint32_t>(function.at(block).parameters.size()) };
        ValueID id = addValue(function, WTF::move(parameter));
        function.at(block).parameters.append(id);
        return id;
    }

    Value undefined() const { return typed(Opcode::Undefined, m_program.types.undefined); }

    Value integer(int64_t number, TypeID type) const
    {
        Value value = typed(Opcode::Integer, type);
        value.setBits(static_cast<uint64_t>(number));
        return value;
    }

    Value getField(ValueID frame, uint32_t field) const
    {
        Value value = typed(Opcode::GetField, m_program.at(m_frameLayout).fields[field].type);
        value.children[0] = frame;
        value.immediates = { m_frameLayout.index(), field };
        return value;
    }

    Value putField(ValueID frame, uint32_t field, ValueID stored) const
    {
        Value value = typed(Opcode::PutField, m_program.types.undefined);
        value.children[0] = frame;
        value.children[1] = stored;
        value.immediates = { m_frameLayout.index(), field };
        return value;
    }

    Value cast(ValueID operand, TypeID type, CastReason reason) const
    {
        Value value = typed(Opcode::Cast, type);
        value.children[0] = operand;
        value.immediates[1] = static_cast<uint32_t>(reason);
        return value;
    }

    Value operation(BuiltinID which, TypeID type, std::initializer_list<ValueID> arguments) const
    {
        Value value = typed(Opcode::Operation, type);
        value.immediates[0] = which.index();
        for (ValueID argument : arguments)
            value.arguments.append(argument);
        return value;
    }

    Value callResumeFunction(ValueID frame, ValueID mode, ValueID resumeValue) const
    {
        Value value = typed(Opcode::CallDirect, m_resumeFunctionResultType);
        value.immediates[0] = m_resume.index();
        value.arguments.append(frame);
        value.arguments.append(mode);
        value.arguments.append(resumeValue);
        return value;
    }

    uint32_t addFrameField(ASCIILiteral name, TypeID type, uint32_t flags = 0)
    {
        Field field;
        field.name = provisionalNameOf(name);
        field.type = type;
        field.flags = flags | fieldHasWideReference;
        auto& fields = m_program.at(m_frameLayout).fields;
        fields.append(WTF::move(field));
        return fields.size() - 1;
    }

    void createFrameLayout()
    {
        auto& types = m_program.types;
        m_frameLayout = m_numbers.frameLayout;
        m_frameType = m_numbers.frameType;
        m_resumeModeType = m_numbers.resumeModeType;
        m_driverType = m_numbers.driverType;
        m_resumeFunctionResultType = m_isGenerator ? types.anything : types.undefined;

        Layout layout;
        layout.kind = LayoutKind::Environment;
        layout.name = std::as_const(m_program).at(m_ramp).name;
        layout.isCoroutineFrame = true;
        layout.type = m_frameType;
        m_program.at(m_frameLayout) = WTF::move(layout);
        static_assert(!CoroutineFrame::stateField);
        RELEASE_ASSERT(addFrameField("state"_s, types.int32) == CoroutineFrame::stateField);
        RELEASE_ASSERT(addFrameField("promise"_s, m_driverType, fieldIsImmutable) == CoroutineFrame::promiseField);
        RELEASE_ASSERT(addFrameField("resumeClosure"_s, types.function, fieldIsImmutable) == CoroutineFrame::resumeClosureField);
    }

    static uint32_t flagsForSplitFunction(const Function& ramp, uint32_t kept)
    {
        return (ramp.flags & (kept | Format::FunctionIsModuleBody | Format::FunctionIsSloppy)) | (ramp.has(Format::FunctionIsHiddenInStackTraces) ? FunctionIsPartOfHiddenCoroutine : 0)
            | (ramp.has(Format::FunctionIsAsync) && !ramp.has(Format::FunctionIsGenerator) ? FunctionIsPartOfAsyncFunction : 0);
    }

    void copyBodyToStartFunction()
    {
        auto& ramp = std::as_const(m_program).at(m_ramp);
        m_startBody.name = ramp.name;
        m_startBody.module = ramp.module;
        m_startBody.position = ramp.position;
        m_startBody.kind = FunctionKind::Plain;
        m_startBody.flags = flagsForSplitFunction(ramp, Format::FunctionIsTyped);
        m_startBody.resultType = m_program.types.undefined;
        m_startBody.blocks = ramp.blocks;
        m_startBody.values = ramp.values;
    }

    void moveBodyToResumeFunction()
    {
        auto& ramp = m_program.at(m_ramp);
        m_resume = m_numbers.resume;
        m_body.name = ramp.name;
        m_body.module = ramp.module;
        m_body.position = ramp.position;
        m_body.kind = FunctionKind::Plain;
        m_body.flags = flagsForSplitFunction(ramp, Format::FunctionIsTyped | Format::FunctionHasExplicitName);
        m_body.captureTypes.append(m_frameType);
        m_body.parameterTypes.append(m_resumeModeType);
        m_body.parameterTypes.append(m_program.types.anything);
        m_body.resultType = m_resumeFunctionResultType;
        m_body.capturesExceptions = m_isGenerator && m_isAsync;
        m_body.blocks = std::exchange(ramp.blocks, { });
        m_body.values = std::exchange(ramp.values, { });
        ramp.flags &= ~(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionStartsEagerly);
        ramp.resumeFunction = m_resume;
        if (m_hasStartFunction)
            ramp.startFunction = FunctionID(m_resume.index() + 1);
    }

    void createDispatchBlock()
    {
        m_start = addBlock(m_body);
        m_body.at(m_start) = std::exchange(m_body.blocks[0], Block { });
        for (auto& block : m_body.blocks) {
            for (auto& target : block.terminator.targets) {
                if (!target.block.index())
                    target.block = m_start;
            }
        }
        m_frame = addParameter(m_body, BlockID(0u), m_frameType);
        m_resumeMode = addParameter(m_body, BlockID(0u), m_resumeModeType);
        m_resumeValue = addParameter(m_body, BlockID(0u), m_program.types.anything);

        m_arguments = Vector<ValueID>(std::exchange(m_body.at(m_start).parameters, { }).span());
        for (ValueID argument : m_arguments) {
            m_argumentTypes.append(m_body.at(argument).type);
            uint32_t field = addFrameField("argument"_s, m_argumentTypes.last(), fieldIsImmutable);
            Value load = getField(m_frame, field);
            load.position = m_body.at(argument).position;
            load.copyPredictionFrom(m_body.at(argument));
            m_body.at(argument) = WTF::move(load);
        }
        m_body.at(m_start).values.insertVector(0, m_arguments);
    }

    static bool isAlwaysRematerialized(const Program& program, const Value& value)
    {
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::BuiltinFunction:
            return true;
        case Opcode::GetGlobal: {
            BuiltinName name = program.builtinName(value.string());
            return Builtins::singleton().isGlobal(name) && !program.isExtended(name);
        }
        default:
            return false;
        }
    }

    static Vector<Vector<uint32_t>> computeLiveAtHeads(const Function& function, const BitVector& isTracked)
    {
        unsigned numberOfBlocks = function.blocks.size();
        unsigned numberOfValues = function.values.size();
        static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<Vector<uint32_t>> liveAtHead(numberOfBlocks);
        Vector<Vector<uint32_t, 2>> predecessors(numberOfBlocks);
        Vector<uint32_t> definedIn;
        definedIn.fill(none, numberOfValues);
        Vector<Vector<uint32_t, 2>> usedIn(numberOfValues);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = function.blocks[b];
            auto use = [&](ValueID used) {
                if (used && (usedIn[used.index()].isEmpty() || usedIn[used.index()].last() != b))
                    usedIn[used.index()].append(b);
            };
            for (ValueID parameter : block.parameters)
                definedIn[parameter.index()] = b;
            for (ValueID id : block.values) {
                definedIn[id.index()] = b;
                function.at(id).forEachChild([&](ValueID child) {
                    use(child);
                });
            }
            use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                predecessors[target.block.index()].append(b);
                for (ValueID argument : target.arguments)
                    use(argument);
            }
        }
        Vector<uint32_t> lastValueLiveInto;
        lastValueLiveInto.fill(none, numberOfBlocks);
        Vector<uint32_t> worklist;
        for (unsigned value = 0; value < numberOfValues; ++value) {
            worklist.appendVector(usedIn[value]);
            while (!worklist.isEmpty()) {
                uint32_t b = worklist.takeLast();
                if (b == definedIn[value] || lastValueLiveInto[b] == value)
                    continue;
                lastValueLiveInto[b] = value;
                if (isTracked.get(b))
                    liveAtHead[b].append(value);
                worklist.appendVector(predecessors[b]);
            }
        }
        return liveAtHead;
    }

    void keepBodyUnsplit()
    {
        auto& ramp = m_program.at(m_ramp);
        m_driverType = m_numbers.driverType;
        auto& function = m_startBody;
        function.name = ramp.name;
        function.module = ramp.module;
        function.position = ramp.position;
        function.kind = FunctionKind::Plain;
        function.flags = flagsForSplitFunction(ramp, Format::FunctionIsTyped);
        function.resultType = m_program.types.undefined;
        function.blocks = std::exchange(ramp.blocks, { });
        function.values = std::exchange(ramp.values, { });
        function.savesFrameOnSuspend = true;
        ramp.savesFrameOnSuspend = true;
        ramp.flags &= ~(Format::FunctionIsAsync | Format::FunctionIsGenerator | Format::FunctionStartsEagerly);
        ramp.startFunction = m_numbers.resume;
        for (ValueID argument : function.blocks[0].parameters)
            m_argumentTypes.append(function.at(argument).type);

        BlockID first = addBlock(function);
        function.at(first) = std::exchange(function.blocks[0], Block { });
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                if (!target.block.index())
                    target.block = first;
            }
        }
        BlockID entry(0u);
        function.at(entry).terminator.kind = TerminatorKind::Jump;
        function.at(entry).terminator.targets.append(Target { first, { } });
        for (TypeID type : m_argumentTypes) {
            function.parameterTypes.append(type);
            ValueID argument = addParameter(function, entry, type);
            function.at(entry).terminator.targets[0].arguments.append(argument);
        }
        function.parameterTypes.append(m_driverType);
        ValueID promise = addParameter(function, entry, m_driverType);
        for (ValueID argument : CompactList<ValueID>(function.at(first).parameters))
            function.at(argument).immediates[0] = first.index();

        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            size_t index = function.blocks[b].values.findIf([&](ValueID id) {
                return function.at(id).opcode == Opcode::Await;
            });
            if (index == notFound)
                continue;
            BlockID block(b);
            ValueID suspension = function.at(block).values[index];
            Value original = function.at(suspension);
            m_position = original.position;
            bool isHeld = original.assertedType() || !m_program.at(original.type).isAnything();
            Value awaited = operation(Operations::AwaitBySavingFrame, m_program.types.anything, { original.child(0), promise });
            ValueID resumedValue = suspension;
            if (isHeld) {
                resumedValue = addValue(function, WTF::move(awaited));
                Value held = cast(resumedValue, original.type, CastReason::Claim);
                held.immediates[1] |= Value::castChecksAwaitResult;
                held.position = m_position;
                held.setAssertedType(original.assertedType());
                held.copyPredictionFrom(original);
                function.at(suspension) = WTF::move(held);
            } else {
                awaited.position = m_position;
                awaited.copyPredictionFrom(original);
                function.at(suspension) = WTF::move(awaited);
            }
            BlockID behind = addBlock(function);
            auto& from = function.at(block);
            auto& to = function.at(behind);
            bool isWatched = from.terminator.kind == TerminatorKind::Invoke && index + 1 == from.values.size();
            if (isHeld)
                to.values.append(suspension);
            for (size_t i = index + 1; i < from.values.size(); ++i)
                to.values.append(from.values[i]);
            from.values.shrink(index);
            from.values.append(resumedValue);
            to.terminator = from.terminator;
            if (isWatched) {
                from.terminator.targets[0] = Target { behind, { } };
                if (!isHeld) {
                    Target normal = to.terminator.targets[0];
                    to.terminator = Terminator { };
                    to.terminator.kind = TerminatorKind::Jump;
                    to.terminator.targets.append(WTF::move(normal));
                }
            } else {
                from.terminator = Terminator { };
                from.terminator.kind = TerminatorKind::Jump;
                from.terminator.targets.append(Target { behind, { } });
            }
        }

        m_position = function.position;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            BlockID block(b);
            if (function.at(block).terminator.kind != TerminatorKind::Return)
                continue;
            ValueID result = function.at(block).terminator.value;
            if (!result)
                result = append(function, block, undefined());
            append(function, block, operation(Operations::ResolvePromise, m_program.types.undefined, { promise, result }));
            function.at(block).terminator.value = append(function, block, undefined());
        }

        m_program.at(m_numbers.resume) = WTF::move(function);
        buildRamp();
    }

    void replaceResumeModeReads()
    {
        if (m_isGenerator && m_isAsync) {
            for (auto& block : m_body.blocks) {
                for (size_t i = 0; i < block.values.size(); ++i) {
                    ValueID read = block.values[i];
                    if (m_body.at(read).opcode != Opcode::GetResumeMode)
                        continue;
                    ValueID mask = addValue(m_body, integer(resumeModeThrow | resumeModeReturn, m_resumeModeType));
                    block.values.insert(i++, mask);
                    Value withoutFlags = typed(Opcode::BitAnd, m_resumeModeType);
                    withoutFlags.children[0] = m_resumeMode;
                    withoutFlags.children[1] = mask;
                    m_body.at(read) = WTF::move(withoutFlags);
                }
            }
            return;
        }
        auto replace = [&](ValueID& child) {
            if (m_body.at(child).opcode == Opcode::GetResumeMode)
                child = m_resumeMode;
        };
        for (auto& value : m_body.values)
            value.forEachChild(replace);
        for (auto& block : m_body.blocks) {
            block.terminator.forEachChild(replace);
            block.values.removeAllMatching([&](ValueID id) {
                return m_body.at(id).opcode == Opcode::GetResumeMode;
            });
        }
    }

    void splitAtSuspensionPoints()
    {
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (unsigned i = 0; i < m_body.blocks[b].values.size(); ++i) {
                Opcode opcode = m_body.at(m_body.blocks[b].values[i]).opcode;
                if (opcode != Opcode::Await && opcode != Opcode::Yield)
                    continue;
                if (opcode == Opcode::Await && mergeAwaitIntoYield(BlockID(b), i)) {
                    --i;
                    continue;
                }
                splitAt(BlockID(b), i);
                break;
            }
        }
    }

    bool mergeAwaitIntoYield(BlockID block, unsigned index)
    {
        if (!m_isGenerator || !m_isAsync)
            return false;
        ValueID await = m_body.at(block).values[index];
        bool endsBlock = index + 1 == m_body.at(block).values.size();
        auto& terminator = m_body.at(block).terminator;
        if (endsBlock && (terminator.kind != TerminatorKind::Invoke || !terminator.targets[0].arguments.isEmpty() || m_body.at(terminator.targets[0].block).values.isEmpty()))
            return false;
        ValueID yield = endsBlock ? m_body.at(terminator.targets[0].block).values[0] : m_body.at(block).values[index + 1];
        if (m_body.at(yield).opcode != Opcode::Yield || m_body.at(yield).child(0) != await)
            return false;
        m_body.at(yield).children[0] = m_body.at(await).child(0);
        m_awaitingYields.set(yield.index());
        m_body.at(block).values.removeAt(index);
        m_body.at(await) = undefined();
        if (endsBlock) {
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.shrink(1);
        }
        return true;
    }

    void splitAt(BlockID suspendBlock, unsigned index)
    {
        ValueID suspension = m_body.at(suspendBlock).values[index];
        ValueID operand = m_body.at(suspension).child(0);
        TypeID type = m_body.at(suspension).type;
        bool isYield = m_body.at(suspension).opcode == Opcode::Yield;
        m_position = m_body.at(suspension).position;

        BlockID continuation = addBlock(m_body);
        {
            auto& from = m_body.at(suspendBlock);
            auto& to = m_body.at(continuation);
            to.values.append(from.values.span().subspan(index + 1));
            to.terminator = std::exchange(from.terminator, Terminator { });
            from.values.shrink(index);
        }
        std::optional<Target> handler;
        if (m_body.at(continuation).terminator.kind == TerminatorKind::Invoke && m_body.at(continuation).values.isEmpty()) {
            auto& terminator = m_body.at(continuation).terminator;
            handler = terminator.targets[1];
            Target normal = terminator.targets[0];
            terminator = Terminator { };
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.append(WTF::move(normal));
        }
        Value original = m_body.at(suspension);
        bool isHeld = original.assertedType() || !m_program.at(type).isAnything();
        Value parameter = typed(Opcode::Parameter, type);
        parameter.immediates = { continuation.index(), 0 };
        parameter.position = m_position;
        parameter.copyPredictionFrom(original);
        m_body.at(suspension) = WTF::move(parameter);
        m_body.at(continuation).parameters.append(suspension);
        BlockID mergeBlock;
        auto continueWith = [&](BlockID from, ValueID returnedValue) {
            if (isHeld && !isYield && from != mergeBlock) {
                bool isFirst = !mergeBlock;
                if (isFirst)
                    mergeBlock = addBlock(m_body);
                m_body.at(from).terminator.kind = TerminatorKind::Jump;
                m_body.at(from).terminator.targets.append(Target { mergeBlock, { returnedValue } });
                if (!isFirst)
                    return;
                from = mergeBlock;
                returnedValue = addParameter(m_body, mergeBlock, m_program.types.anything);
            }
            if (isHeld) {
                Value held = cast(returnedValue, type, CastReason::Claim);
                if (!isYield)
                    held.immediates[1] |= Value::castChecksAwaitResult;
                held.setAssertedType(original.assertedType());
                held.copyPredictionFrom(original);
                returnedValue = append(m_body, from, WTF::move(held));
            }
            auto& terminator = m_body.at(from).terminator;
            terminator.kind = isHeld && handler ? TerminatorKind::Invoke : TerminatorKind::Jump;
            terminator.targets.append(Target { continuation, { returnedValue } });
            if (terminator.kind == TerminatorKind::Invoke)
                terminator.targets.append(*handler);
        };
        auto branch = [&](BlockID from, ValueID condition, BlockID taken, BlockID notTaken) {
            auto& terminator = m_body.at(from).terminator;
            terminator.kind = TerminatorKind::Branch;
            terminator.value = condition;
            terminator.targets.append(Target { taken, { } });
            terminator.targets.append(Target { notTaken, { } });
        };

        BlockID resumeBlock = addBlock(m_body);
        if (isYield) {
            continueWith(resumeBlock, m_resumeValue);
        } else {
            BlockID normalBlock = addBlock(m_body);
            continueWith(normalBlock, m_resumeValue);
            BlockID throwBlock = addBlock(m_body);
            m_body.at(throwBlock).terminator.value = m_resumeValue;
            m_body.at(throwBlock).terminator.kind = TerminatorKind::Throw;
            if (handler)
                m_body.at(throwBlock).terminator.targets.append(*handler);

            ValueID throwMode = append(m_body, resumeBlock, integer(resumeModeThrow, m_resumeModeType));
            if (m_isGenerator && !m_isAsync) {
                Value isThrow = typed(Opcode::StrictEq, m_program.types.boolean);
                isThrow.children[0] = m_resumeMode;
                isThrow.children[1] = throwMode;
                branch(resumeBlock, append(m_body, resumeBlock, WTF::move(isThrow)), throwBlock, normalBlock);
            } else {
                static_assert(!resumeModeNormal && hasOneBitSet(resumeModeThrow));
                Value withoutFlags = typed(Opcode::BitAnd, m_resumeModeType);
                withoutFlags.children[0] = m_resumeMode;
                withoutFlags.children[1] = throwMode;
                Value isNormal = typed(Opcode::StrictEq, m_program.types.boolean);
                isNormal.children[0] = append(m_body, resumeBlock, WTF::move(withoutFlags));
                isNormal.children[1] = append(m_body, resumeBlock, integer(resumeModeNormal, m_resumeModeType));
                branch(resumeBlock, append(m_body, resumeBlock, WTF::move(isNormal)), normalBlock, throwBlock);
            }
        }

        if (!isYield && !m_awaitsOnlyInFirstActivation.get(suspension.index())) {
            BlockID inlineBlock = addBlock(m_body);
            BlockID parkBlock = addBlock(m_body);
            m_inlineBlocks.set(inlineBlock.index());
            branch(suspendBlock, append(m_body, suspendBlock, operation(Operations::AwaitMayContinue, m_program.types.boolean, { operand, m_resumeMode })), inlineBlock, parkBlock);
            continueWith(inlineBlock, append(m_body, inlineBlock, operation(Operations::AwaitedValue, m_program.types.anything, { operand })));
            suspendBlock = parkBlock;
        }

        m_suspensionPoints.append({ suspendBlock, resumeBlock, static_cast<unsigned>(m_body.at(suspendBlock).values.size()), suspension });
        m_body.suspensionPositions.append(m_position);
        ValueID state = append(m_body, suspendBlock, integer(m_suspensionPoints.size(), m_program.types.int32));
        append(m_body, suspendBlock, putField(m_frame, CoroutineFrame::stateField, state));
        ValueID returned = operand;
        if (!isYield || m_awaitingYields.get(suspension.index())) {
            ValueID closure = append(m_body, suspendBlock, getField(m_frame, m_isGenerator ? CoroutineFrame::promiseField : CoroutineFrame::resumeClosureField));
            m_suspensionPoints.last().registration = append(m_body, suspendBlock, operation(isYield ? Operations::AsyncGeneratorYield : m_isGenerator ? Operations::Await : Operations::SuspendAsyncFunction, m_program.types.undefined, { closure, operand }));
            returned = append(m_body, suspendBlock, undefined());
        }
        m_body.at(suspendBlock).terminator.kind = TerminatorKind::Return;
        m_body.at(suspendBlock).terminator.value = returned;
    }

    void rewriteReturns()
    {
        BitVector isSuspendBlock(m_body.blocks.size());
        for (auto& point : m_suspensionPoints)
            isSuspendBlock.set(point.suspendBlock.index());
        BitVector isReachedByFirstActivation(m_body.blocks.size());
        if (!m_hasStartFunction && !m_isGenerator) {
            Vector<BlockID> worklist { m_start };
            while (!worklist.isEmpty()) {
                BlockID block = worklist.takeLast();
                if (m_inlineBlocks.get(block.index()) || isReachedByFirstActivation.set(block.index()))
                    continue;
                for (auto& target : m_body.at(block).terminator.targets)
                    worklist.append(target.block);
            }
        }
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            if (m_body.blocks[b].terminator.kind != TerminatorKind::Return || isSuspendBlock.get(b))
                continue;
            BlockID block(b);
            ValueID result = m_body.at(block).terminator.value;
            if (!result)
                result = append(m_body, block, undefined());
            if (m_isGenerator) {
                ValueID completed = append(m_body, block, integer(CoroutineFrame::completedState, m_program.types.int32));
                append(m_body, block, putField(m_frame, CoroutineFrame::stateField, completed));
                m_body.at(block).terminator.value = result;
                continue;
            }
            Value read = getField(m_frame, CoroutineFrame::promiseField);
            if (isReachedByFirstActivation.get(b))
                read.setPrediction(m_driverType);
            ValueID promise = append(m_body, block, WTF::move(read));
            append(m_body, block, operation(Operations::ResolvePromise, m_program.types.undefined, { promise, result }));
            m_body.at(block).terminator.value = append(m_body, block, undefined());
        }
    }

    Vector<BlockID> resumeBlocksBySuspendBlock() const
    {
        Vector<BlockID> result(m_body.blocks.size());
        for (auto& point : m_suspensionPoints)
            result[point.suspendBlock.index()] = point.resumeBlock;
        return result;
    }

    Vector<Vector<uint32_t>> computeLiveAtHead()
    {
        unsigned numberOfBlocks = m_body.blocks.size();
        unsigned numberOfValues = m_body.values.size();
        static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<Vector<uint32_t>> liveAtHead(numberOfBlocks);
        BitVector isTracked(numberOfBlocks);
        isTracked.set(0);
        Vector<Vector<uint32_t, 2>> predecessors(numberOfBlocks);
        for (auto& point : m_suspensionPoints) {
            isTracked.set(point.resumeBlock.index());
            predecessors[point.resumeBlock.index()].append(point.suspendBlock.index());
        }
        Vector<uint32_t> definedIn;
        definedIn.fill(none, numberOfValues);
        Vector<Vector<uint32_t, 2>> usedIn(numberOfValues);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_body.blocks[b];
            auto use = [&](ValueID used) {
                if (used && (usedIn[used.index()].isEmpty() || usedIn[used.index()].last() != b))
                    usedIn[used.index()].append(b);
            };
            for (ValueID parameter : block.parameters)
                definedIn[parameter.index()] = b;
            for (ValueID id : block.values) {
                definedIn[id.index()] = b;
                m_body.at(id).forEachChild([&](ValueID child) {
                    use(child);
                });
            }
            use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                predecessors[target.block.index()].append(b);
                for (ValueID argument : target.arguments)
                    use(argument);
            }
        }
        Vector<uint32_t> lastValueLiveInto;
        lastValueLiveInto.fill(none, numberOfBlocks);
        Vector<uint32_t> worklist;
        for (unsigned value = 0; value < numberOfValues; ++value) {
            worklist.appendVector(usedIn[value]);
            while (!worklist.isEmpty()) {
                uint32_t b = worklist.takeLast();
                if (b == definedIn[value] || lastValueLiveInto[b] == value)
                    continue;
                lastValueLiveInto[b] = value;
                if (isTracked.get(b))
                    liveAtHead[b].append(value);
                worklist.appendVector(predecessors[b]);
            }
        }
        return liveAtHead;
    }

    bool isRematerializable(ValueID id) const
    {
        auto& value = m_body.at(id);
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::FunctionReference:
        case Opcode::BuiltinFunction:
            return true;
        case Opcode::GetGlobal: {
            BuiltinName name = m_program.builtinName(value.string());
            return Builtins::singleton().isGlobal(name) && !m_program.isExtended(name);
        }
        case Opcode::GetField:
            return value.child(0) == m_frame && value.layout() == m_frameLayout && m_program.at(m_frameLayout).fields[value.field()].flags & fieldIsImmutable;
        default:
            return false;
        }
    }

    void clearUnreachableBlocks()
    {
        Vector<BlockID> resumeBlockOf = resumeBlocksBySuspendBlock();
        auto reachableFrom = [&](Vector<BlockID> worklist) {
            BitVector isReachable(m_body.blocks.size());
            isReachable.set(0);
            while (!worklist.isEmpty()) {
                BlockID block = worklist.takeLast();
                if (isReachable.set(block.index()))
                    continue;
                if (resumeBlockOf[block.index()])
                    worklist.append(resumeBlockOf[block.index()]);
                for (auto& target : m_body.at(block).terminator.targets)
                    worklist.append(target.block);
            }
            return isReachable;
        };
        BitVector isReachable = reachableFrom({ m_start });
        for (auto& point : m_suspensionPoints)
            point.isReachable = isReachable.get(point.suspendBlock.index());
        if (m_hasStartFunction) {
            Vector<BlockID> resumeBlocks;
            for (auto& point : m_suspensionPoints) {
                if (point.isReachable)
                    resumeBlocks.append(point.resumeBlock);
            }
            isReachable = reachableFrom(WTF::move(resumeBlocks));
        }
        for (auto& point : m_suspensionPoints)
            point.isReachableInResumeFunction = isReachable.get(point.suspendBlock.index());
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            if (isReachable.get(b))
                continue;
            m_body.blocks[b] = Block { };
            m_body.blocks[b].terminator.kind = TerminatorKind::Unreachable;
        }
    }

    using ValueByBlock = UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    struct Definitions {
        ValueID variable;
        TypeID type;
        ValueByBlock atTail;
        ValueByBlock atHead;
        Vector<BlockID, 4> liveResumeBlocks;
    };

    void spillAndReload()
    {
        clearUnreachableBlocks();
        auto liveAtHead = computeLiveAtHead();
        RELEASE_ASSERT(liveAtHead[0].isEmpty());

        unsigned numberOfOriginalValues = m_body.values.size();
        Vector<BlockID> definingBlock(numberOfOriginalValues);
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (ValueID id : m_body.blocks[b].parameters)
                definingBlock[id.index()] = BlockID(b);
            for (ValueID id : m_body.blocks[b].values)
                definingBlock[id.index()] = BlockID(b);
        }

        UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> spillFields;
        UncheckedKeyHashMap<uint32_t, Definitions, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> definitions;
        for (auto& point : m_suspensionPoints) {
            if (!point.isReachable)
                continue;
            Vector<ValueID> spills;
            for (unsigned index : liveAtHead[point.resumeBlock.index()]) {
                ValueID variable(index);
                if (variable == m_frame || variable == m_resumeMode || variable == m_resumeValue)
                    continue;
                TypeID type = m_body.at(variable).type;
                definitions.ensure(index, [&] {
                    Definitions result;
                    result.variable = variable;
                    result.type = type;
                    if (definingBlock[index])
                        result.atTail.add(definingBlock[index].index(), variable);
                    return result;
                }).iterator->value.liveResumeBlocks.append(point.resumeBlock);
                if (isRematerializable(variable))
                    continue;
                uint32_t field = spillFields.ensure(index, [&] {
                    return addFrameField("spill"_s, type);
                }).iterator->value;
                spills.append(addValue(m_body, putField(m_frame, field, variable)));
                point.spilled.append({ variable, field });
            }
            if (point.isReachableInResumeFunction)
                m_body.at(point.suspendBlock).values.insertVector(point.spillIndex, spills);
        }

        computePredecessors();
        UncheckedKeyHashMap<uint32_t, Vector<Use>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> uses;
        for (auto& entry : definitions)
            uses.add(entry.key, Vector<Use>());
        for (ValueID argument : m_arguments)
            uses.add(argument.index(), Vector<Use>());
        auto note = [&](ValueID used, Use use) {
            if (!used)
                return;
            auto found = uses.find(used.index());
            if (found != uses.end() && (found->value.isEmpty() || found->value.last() != use))
                found->value.append(use);
        };
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (ValueID id : m_body.blocks[b].values) {
                m_body.at(id).forEachChild([&](ValueID child) {
                    note(child, Use { BlockID(b), id });
                });
            }
            m_body.blocks[b].terminator.forEachChild([&](ValueID& child) {
                note(child, Use { BlockID(b), ValueID() });
            });
        }
        chooseFieldResidentValues(definingBlock, spillFields, uses, definitions);
        struct Kept {
            ValueID variable;
            std::optional<uint32_t> field;
            Definitions* definitions;
            Vector<Use>* uses;
            ReloadPlan plan;
        };
        Vector<Kept> kept;
        for (auto& entry : definitions) {
            auto field = spillFields.find(entry.key);
            kept.append(Kept { ValueID(entry.key), field == spillFields.end() ? std::nullopt : std::optional<uint32_t>(field->value), &entry.value, &uses.find(entry.key)->value, { } });
        }
        auto plan = [&](Kept& one) {
            one.plan = planReloads(definingBlock[one.variable.index()], one.field, *one.uses, m_livesInField.get(one.variable.index()), m_program.at(m_body.at(one.variable).type).hasCells(), one.definitions->liveResumeBlocks);
        };
        bool preplans = m_suspensionPoints.size() > maxSuspensionsKeptInRegisters;
        if (preplans) {
            forEachIndexInParallel(kept.size(), [&](unsigned i) {
                plan(kept[i]);
            });
        }
        for (auto& one : kept) {
            if (!preplans)
                plan(one);
            placeReloads(one.variable, definingBlock[one.variable.index()], one.field, *one.definitions, *one.uses, one.plan);
            repairSSA(one.variable, definingBlock[one.variable.index()], *one.definitions, *one.uses);
            one.plan = { };
        }
        if (m_hasStartFunction || !m_predecessors[m_start.index()].isEmpty())
            return;
        Vector<BlockID, 4> reachableResumeBlocks;
        for (auto& point : m_suspensionPoints) {
            if (point.isReachable)
                reachableResumeBlocks.append(point.resumeBlock);
        }
        for (unsigned i = 0; i < m_arguments.size(); ++i) {
            if (!m_program.at(m_argumentTypes[i]).hasCells())
                continue;
            BitVector hasRead(m_body.blocks.size());
            for (auto& use : uses.find(m_arguments[i].index())->value) {
                if (use.block != m_start)
                    hasRead.set(use.block.index());
            }
            ReloadPlan plan;
            planClears(plan, m_start, hasRead, true, reachableResumeBlocks);
            clearField(plan, CoroutineFrame::firstArgumentField + i, m_argumentTypes[i]);
        }
    }

    void computePredecessors()
    {
        m_predecessors.fill({ }, m_body.blocks.size());
        for (unsigned b = 0; b < m_body.blocks.size(); ++b) {
            for (auto& target : m_body.blocks[b].terminator.targets) {
                if (!m_predecessors[target.block.index()].contains(BlockID(b)))
                    m_predecessors[target.block.index()].append(BlockID(b));
            }
        }
    }

    ValueID definitionAtHead(Definitions& definitions, BlockID asked)
    {
        struct Pending {
            BlockID block;
            ValueID phi;
            unsigned answered { 0 };
        };
        Vector<Pending, 32> pending;
        ValueID answer;
        auto lookupAtHead = [&](BlockID block) {
            auto found = definitions.atHead.find(block.index());
            if (found != definitions.atHead.end()) {
                answer = found->value;
                return true;
            }
            auto& predecessors = m_predecessors[block.index()];
            RELEASE_ASSERT(!predecessors.isEmpty());
            unsigned numberOfEdges = 0;
            for (BlockID predecessor : predecessors) {
                for (auto& target : m_body.at(predecessor).terminator.targets)
                    numberOfEdges += target.block == block;
            }
            ValueID phi;
            if (numberOfEdges != 1) {
                phi = addParameter(m_body, block, definitions.type);
                m_body.at(phi).copyPredictionFrom(m_body.at(definitions.variable));
                definitions.atHead.add(block.index(), phi);
            }
            pending.append(Pending { block, phi });
            return false;
        };
        auto lookupAtTail = [&](BlockID block) {
            auto found = definitions.atTail.find(block.index());
            if (found != definitions.atTail.end()) {
                answer = found->value;
                return true;
            }
            return lookupAtHead(block);
        };
        if (lookupAtHead(asked))
            return answer;
        bool isAnswered = false;
        while (!pending.isEmpty()) {
            BlockID block = pending.last().block;
            ValueID phi = pending.last().phi;
            auto& predecessors = m_predecessors[block.index()];
            if (isAnswered) {
                unsigned index = pending.last().answered++;
                if (phi) {
                    for (auto& target : m_body.at(predecessors[index]).terminator.targets) {
                        if (target.block == block)
                            target.arguments.append(answer);
                    }
                }
            }
            if (pending.last().answered < (phi ? predecessors.size() : 1)) {
                isAnswered = lookupAtTail(predecessors[pending.last().answered]);
                continue;
            }
            if (phi)
                answer = phi;
            else
                definitions.atHead.add(block.index(), answer);
            pending.removeLast();
            isAnswered = true;
        }
        return answer;
    }

    struct Use {
        BlockID block;
        ValueID user;
        friend bool operator==(const Use&, const Use&) = default;
    };

    void repairSSA(ValueID variable, BlockID definingBlock, Definitions& definitions, const Vector<Use>& uses)
    {
        for (auto& use : uses) {
            if (use.block == definingBlock)
                continue;
            auto local = definitions.atTail.find(use.block.index());
            ValueID with = local != definitions.atTail.end() ? local->value : definitionAtHead(definitions, use.block);
            auto replace = [&](ValueID& child) {
                if (child == variable)
                    child = with;
            };
            if (use.user)
                m_body.at(use.user).forEachChild(replace);
            else
                m_body.at(use.block).terminator.forEachChild(replace);
        }
    }

    struct ReloadPlan {
        BitVector reloadBlocks;
        BitVector isLiveAtHead;
        BitVector spillsToRemove;
        BitVector clearsAtHead;
        BitVector clearsAtTail;
        unsigned keptSpillCount { 0 };
    };

    bool isSpillOf(const Use& use, std::optional<uint32_t> field)
    {
        if (!use.user || !field)
            return false;
        auto& user = m_body.at(use.user);
        return user.opcode == Opcode::PutField && user.child(0) == m_frame && user.field() == *field;
    }

    void planClears(ReloadPlan& plan, BlockID definingBlock, const BitVector& hasRead, bool isStoredAtCreation, const Vector<BlockID, 4>& liveResumeBlocks)
    {
        unsigned numberOfBlocks = m_body.blocks.size();
        Vector<BlockID> worklist;
        BitVector isLiveAtHead(numberOfBlocks);
        for (unsigned b : hasRead)
            worklist.append(BlockID(b));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == definingBlock || !block.index() || isLiveAtHead.set(block.index()))
                continue;
            worklist.appendVector(m_predecessors[block.index()]);
            if (BlockID suspendBlock = m_suspendBlockOf[block.index()])
                worklist.append(suspendBlock);
        }
        auto clearDeadSlots = [&](BlockID block) {
            for (unsigned steps = 0; steps < numberOfBlocks; ++steps) {
                if (isLiveAtHead.get(block.index()) || !m_reachesSuspension.get(block.index()) || (isStoredAtCreation && block == definingBlock))
                    return;
                auto& terminator = m_body.at(block).terminator;
                if (terminator.kind != TerminatorKind::Jump || m_resumeBlockOf[block.index()] || block == definingBlock || (!isStoredAtCreation && terminator.targets[0].block == definingBlock))
                    break;
                block = terminator.targets[0].block;
            }
            plan.clearsAtHead.set(block.index());
        };
        auto leave = [&](BlockID block) {
            if (BlockID resumeBlock = m_resumeBlockOf[block.index()]) {
                if (!isLiveAtHead.get(resumeBlock.index()))
                    plan.clearsAtTail.set(block.index());
            }
            for (auto& target : m_body.at(block).terminator.targets)
                clearDeadSlots(target.block);
        };
        BitVector holdsAtHead(numberOfBlocks);
        auto enter = [&](BlockID block) {
            if (isLiveAtHead.get(block.index()) && !holdsAtHead.set(block.index()))
                worklist.append(block);
        };
        for (BlockID resumeBlock : liveResumeBlocks)
            enter(resumeBlock);
        if (isStoredAtCreation && definingBlock) {
            leave(definingBlock);
            for (auto& target : m_body.at(definingBlock).terminator.targets)
                enter(target.block);
        }
        while (!worklist.isEmpty()) {
            for (auto& target : m_body.at(worklist.takeLast()).terminator.targets)
                enter(target.block);
        }
        for (unsigned b : holdsAtHead)
            leave(BlockID(b));
    }

    void clearField(const ReloadPlan& plan, uint32_t field, TypeID type)
    {
        auto clearAt = [&](unsigned b, size_t index) {
            auto& values = m_body.blocks[b].values;
            ValueID nothing = addValue(m_body, typed(Opcode::Unspecified, type));
            values.insert(index, nothing);
            values.insert(index + 1, addValue(m_body, putField(m_frame, field, nothing)));
        };
        for (unsigned b : plan.clearsAtHead)
            clearAt(b, !m_body.blocks[b].values.isEmpty() && m_body.at(m_body.blocks[b].values[0]).opcode == Opcode::CatchValue ? 1 : 0);
        for (unsigned b : plan.clearsAtTail) {
            auto& values = m_body.blocks[b].values;
            ValueID registration = m_registrationOf[b];
            clearAt(b, registration ? values.find(registration) : values.size());
        }
    }

    ReloadPlan planReloads(BlockID definingBlock, std::optional<uint32_t> field, const Vector<Use>& uses, bool livesInField, bool holdsCells, const Vector<BlockID, 4>& liveResumeBlocks)
    {
        unsigned numberOfBlocks = m_body.blocks.size();
        ReloadPlan plan;
        Vector<BlockID> worklist;
        BitVector isReachedWithoutSuspension(numberOfBlocks);
        if (definingBlock)
            worklist.append(definingBlock);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (auto& target : m_body.at(block).terminator.targets) {
                if (target.block != definingBlock && !isReachedWithoutSuspension.set(target.block.index()))
                    worklist.append(target.block);
            }
        }

        BitVector hasUse(numberOfBlocks);
        BitVector hasRead(numberOfBlocks);
        for (unsigned i = 0; i < uses.size(); ++i) {
            auto& use = uses[i];
            if (use.block == definingBlock)
                continue;
            if (isSpillOf(use, field)) {
                if (livesInField || !isReachedWithoutSuspension.get(use.block.index())) {
                    plan.spillsToRemove.set(i);
                    continue;
                }
                ++plan.keptSpillCount;
            } else
                hasRead.set(use.block.index());
            hasUse.set(use.block.index());
        }
        if (field && holdsCells)
            planClears(plan, definingBlock, hasRead, livesInField, liveResumeBlocks);

        BitVector& isLiveAtHead = plan.isLiveAtHead;
        isLiveAtHead.ensureSize(numberOfBlocks);
        for (unsigned b : hasUse)
            worklist.append(BlockID(b));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == definingBlock || !block.index() || isLiveAtHead.set(block.index()))
                continue;
            worklist.appendVector(m_predecessors[block.index()]);
        }
        BitVector hasSeveralLiveSuccessors(numberOfBlocks);
        for (unsigned b : isLiveAtHead) {
            BlockID first;
            for (auto& target : m_body.blocks[b].terminator.targets) {
                if (!isLiveAtHead.get(target.block.index()) || target.block == first)
                    continue;
                if (first) {
                    hasSeveralLiveSuccessors.set(b);
                    break;
                }
                first = target.block;
            }
        }

        if (livesInField) {
            BitVector isMissingAtHead(numberOfBlocks);
            auto enter = [&](BlockID block) {
                if (isLiveAtHead.get(block.index()) && !isMissingAtHead.set(block.index()) && !hasUse.get(block.index()))
                    worklist.append(block);
            };
            for (BlockID resumeBlock : liveResumeBlocks)
                enter(resumeBlock);
            while (!worklist.isEmpty()) {
                for (auto& target : m_body.at(worklist.takeLast()).terminator.targets)
                    enter(target.block);
            }
            plan.reloadBlocks = WTF::move(isMissingAtHead);
            plan.reloadBlocks.filter(hasUse);
            return plan;
        }

        auto computeReloadBlocks = [&](bool reloadsBeforeSplits) {
            BitVector isAvailableAtHead(numberOfBlocks);
            BitVector isAvailableAtTail(numberOfBlocks);
            auto makeAvailableAtTail = [&](BlockID block) {
                if (!isAvailableAtTail.set(block.index()))
                    worklist.append(block);
            };
            if (definingBlock)
                makeAvailableAtTail(definingBlock);
            for (unsigned b : hasUse)
                makeAvailableAtTail(BlockID(b));
            if (reloadsBeforeSplits) {
                for (unsigned b : hasSeveralLiveSuccessors)
                    makeAvailableAtTail(BlockID(b));
            }
            while (!worklist.isEmpty()) {
                BlockID block = worklist.takeLast();
                for (auto& target : m_body.at(block).terminator.targets) {
                    BlockID next = target.block;
                    if (!isLiveAtHead.get(next.index()) || isAvailableAtHead.set(next.index()))
                        continue;
                    makeAvailableAtTail(next);
                    for (BlockID predecessor : m_predecessors[next.index()])
                        makeAvailableAtTail(predecessor);
                }
            }
            isAvailableAtTail.exclude(isAvailableAtHead);
            if (definingBlock)
                isAvailableAtTail.clear(definingBlock.index());
            return isAvailableAtTail;
        };
        plan.reloadBlocks = computeReloadBlocks(false);
        if (!hasSeveralLiveSuccessors.isEmpty()) {
            BitVector other = computeReloadBlocks(true);
            if (other.bitCount() < plan.reloadBlocks.bitCount())
                plan.reloadBlocks = WTF::move(other);
        }
        return plan;
    }

    BitVector rareBlocks()
    {
        unsigned numberOfBlocks = m_body.blocks.size();
        BitVector isRare(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_body.blocks[b];
            if (block.terminator.kind == TerminatorKind::Throw || block.terminator.kind == TerminatorKind::Unreachable || (!block.values.isEmpty() && m_body.at(block.values[0]).opcode == Opcode::CatchValue))
                isRare.set(b);
            bool switchesOnResumeMode = block.terminator.kind == TerminatorKind::Switch && (block.terminator.value == m_resumeMode || (m_body.at(block.terminator.value).opcode == Opcode::BitAnd && m_body.at(block.terminator.value).child(0) == m_resumeMode));
            if (switchesOnResumeMode) {
                for (unsigned i = 0; i < block.terminator.cases.size(); ++i)
                    isRare.set(block.terminator.targets[i].block.index());
            }
        }
        bool changed;
        do {
            changed = false;
            for (unsigned b = numberOfBlocks; b-- > 1;) {
                if (isRare.get(b))
                    continue;
                auto& targets = m_body.blocks[b].terminator.targets;
                bool onlyLeadsToRareBlocks = m_resumeBlockOf[b] ? isRare.get(m_resumeBlockOf[b].index()) : !targets.isEmpty() && std::ranges::all_of(targets, [&](auto& target) {
                    return isRare.get(target.block.index());
                });
                auto& predecessors = m_predecessors[b];
                bool isOnlyReachedFromRareBlocks = m_suspendBlockOf[b] ? isRare.get(m_suspendBlockOf[b].index()) : !predecessors.isEmpty() && std::ranges::all_of(predecessors, [&](BlockID predecessor) {
                    return isRare.get(predecessor.index());
                });
                if (onlyLeadsToRareBlocks || isOnlyReachedFromRareBlocks) {
                    isRare.set(b);
                    changed = true;
                }
            }
        } while (changed);
        return isRare;
    }

    bool mayLiveInField(ValueID variable, BlockID definingBlock)
    {
        if (!definingBlock)
            return true;
        auto& block = m_body.at(definingBlock);
        return block.terminator.kind != TerminatorKind::Invoke || block.values.isEmpty() || block.values.last() != variable;
    }

    template<typename Fields, typename Uses, typename DefinitionsByValue>
    void chooseFieldResidentValues(const Vector<BlockID>& definingBlock, const Fields& spillFields, const Uses& uses, const DefinitionsByValue& definitions)
    {
        m_livesInField.clearAll();
        m_resumeBlockOf = resumeBlocksBySuspendBlock();
        m_suspendBlockOf.fill(BlockID(), m_body.blocks.size());
        m_registrationOf.fill(ValueID(), m_body.blocks.size());
        for (auto& point : m_suspensionPoints)
            m_registrationOf[point.suspendBlock.index()] = point.registration;
        m_reachesSuspension.clearAll();
        Vector<BlockID> reaching;
        for (unsigned b = 0; b < m_resumeBlockOf.size(); ++b) {
            if (!m_resumeBlockOf[b])
                continue;
            m_suspendBlockOf[m_resumeBlockOf[b].index()] = BlockID(b);
            reaching.append(BlockID(b));
        }
        while (!reaching.isEmpty()) {
            BlockID block = reaching.takeLast();
            if (!m_reachesSuspension.set(block.index()))
                reaching.appendVector(m_predecessors[block.index()]);
        }
        bool isGiant = m_suspensionPoints.size() > maxSuspensionsKeptInRegisters;
        std::array<unsigned, 2> numberOfRegisters = isGiant ? std::array<unsigned, 2> { 0, 0 } : m_numberOfRegisters;
        auto bankOf = [&](ValueID variable) -> unsigned {
            return repOf(m_program.at(m_body.at(variable).type)) == Rep::Double;
        };
        BitVector isInLoop = blocksInLoops(m_body);
        auto paysBarrierInLoop = [&](ValueID variable) {
            BlockID defining = definingBlock[variable.index()];
            return !isGiant && m_program.at(m_body.at(variable).type).hasCells() && (!m_isGenerator || (defining && isInLoop.get(defining.index())));
        };
        Vector<std::array<unsigned, 2>> crossing;
        crossing.fill({ 0, 0 }, m_suspensionPoints.size());
        for (unsigned k = 0; k < m_suspensionPoints.size(); ++k) {
            auto& point = m_suspensionPoints[k];
            if (!point.isReachable || !point.isReachableInResumeFunction)
                continue;
            for (auto& spill : point.spilled)
                crossing[k][bankOf(spill.first)] += !paysBarrierInLoop(spill.first);
        }
        UncheckedKeyHashMap<uint32_t, Vector<unsigned>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> pointsCrossedBy;
        BitVector crossesCrowdedPoint;
        for (unsigned k = 0; k < m_suspensionPoints.size(); ++k) {
            auto& point = m_suspensionPoints[k];
            if (!point.isReachable || !point.isReachableInResumeFunction)
                continue;
            for (auto& spill : point.spilled) {
                if (paysBarrierInLoop(spill.first))
                    continue;
                pointsCrossedBy.add(spill.first.index(), Vector<unsigned>()).iterator->value.append(k);
                unsigned bank = bankOf(spill.first);
                if (crossing[k][bank] > numberOfRegisters[bank])
                    crossesCrowdedPoint.set(spill.first.index());
            }
        }
        static constexpr unsigned parkSavings = 13;
        static constexpr unsigned storeAtCreationCost = 5;
        BitVector isRare = rareBlocks();
        struct Candidate {
            uint32_t variable;
            unsigned gain;
        };
        Vector<Candidate> candidates;
        for (unsigned index : crossesCrowdedPoint) {
            ValueID variable(index);
            BlockID defining = definingBlock[index];
            if (!mayLiveInField(variable, defining))
                continue;
            if (isGiant) {
                candidates.append({ index, 1 });
                continue;
            }
            std::optional<uint32_t> field = spillFields.get(index);
            auto& variableUses = uses.find(index)->value;
            bool holdsCells = m_program.at(m_body.at(variable).type).hasCells();
            auto& liveResumeBlocks = definitions.find(index)->value.liveResumeBlocks;
            ReloadPlan asBefore = planReloads(defining, field, variableUses, false, holdsCells, liveResumeBlocks);
            unsigned before = asBefore.keptSpillCount + asBefore.reloadBlocks.bitCount();
            if (before < 3)
                continue;
            before += asBefore.clearsAtHead.bitCount() + asBefore.clearsAtTail.bitCount();
            ReloadPlan kept = planReloads(defining, field, variableUses, true, holdsCells, liveResumeBlocks);
            unsigned inField = !!defining + kept.reloadBlocks.bitCount() + kept.clearsAtHead.bitCount() + kept.clearsAtTail.bitCount();
            if (holdsCells && defining) {
                auto nonRareCount = [&](const BitVector& blocks) {
                    unsigned result = 0;
                    for (unsigned b : blocks)
                        result += !isRare.get(b);
                    return result;
                };
                unsigned parks = 0;
                for (unsigned k : pointsCrossedBy.find(index)->value)
                    parks += !isRare.get(m_suspensionPoints[k].suspendBlock.index());
                unsigned clearsOnBothPaths = nonRareCount(asBefore.clearsAtHead) + nonRareCount(asBefore.clearsAtTail);
                unsigned clears = nonRareCount(kept.clearsAtHead) + nonRareCount(kept.clearsAtTail);
                unsigned paid = (isRare.get(defining.index()) ? 0 : storeAtCreationCost) + (clears > clearsOnBothPaths ? clears - clearsOnBothPaths : 0);
                if (parks * parkSavings <= paid) {
                    for (unsigned k : pointsCrossedBy.find(index)->value)
                        --crossing[k][bankOf(variable)];
                    continue;
                }
            }
            if (inField + 1 < before)
                candidates.append({ index, before - inField });
        }
        std::ranges::sort(candidates, [](auto& a, auto& b) {
            return a.gain != b.gain ? a.gain > b.gain : a.variable < b.variable;
        });
        for (auto& candidate : candidates) {
            unsigned bank = bankOf(ValueID(candidate.variable));
            auto& points = pointsCrossedBy.find(candidate.variable)->value;
            if (std::ranges::none_of(points, [&](unsigned k) { return crossing[k][bank] > numberOfRegisters[bank]; }))
                continue;
            m_livesInField.set(candidate.variable);
            for (unsigned k : points)
                --crossing[k][bank];
        }
    }

    void placeReloads(ValueID variable, BlockID definingBlock, std::optional<uint32_t> field, Definitions& definitions, Vector<Use>& uses, const ReloadPlan& plan)
    {
        bool livesInField = m_livesInField.get(variable.index());
        if (livesInField && definingBlock) {
            auto& values = m_body.at(definingBlock).values;
            size_t index = values.find(variable);
            if (index == notFound)
                index = !values.isEmpty() && m_body.at(values[0]).opcode == Opcode::CatchValue ? 1 : 0;
            else {
                ++index;
                for (size_t next = index; next < values.size(); ++next) {
                    auto& value = m_body.at(values[next]);
                    if ((value.opcode == Opcode::PutField || value.opcode == Opcode::PutById) && value.child(0) == variable)
                        index = next + 1;
                    else if (value.opcode != Opcode::GetField || value.child(0) != m_frame)
                        break;
                }
            }
            values.insert(index, addValue(m_body, putField(m_frame, *field, variable)));
        }
        for (unsigned i : plan.spillsToRemove)
            m_body.at(uses[i].block).values.removeFirst(uses[i].user);
        unsigned kept = 0;
        for (unsigned i = 0; i < uses.size(); ++i) {
            if (!plan.spillsToRemove.get(i))
                uses[kept++] = uses[i];
        }
        uses.shrink(kept);

        for (unsigned b : plan.reloadBlocks) {
            RELEASE_ASSERT(plan.isLiveAtHead.get(b));
            ValueID reload;
            if (field) {
                reload = addValue(m_body, getField(m_frame, *field));
                m_body.at(reload).copyPredictionFrom(m_body.at(variable));
            } else
                reload = addValue(m_body, Value(m_body.at(variable)));
            auto& values = m_body.blocks[b].values;
            size_t index = !values.isEmpty() && m_body.at(values[0]).opcode == Opcode::CatchValue ? 1 : 0;
            if (livesInField) {
                size_t first = uses.findIf([&](const Use& use) {
                    return use.block.index() == b;
                });
                if (first != notFound)
                    index = uses[first].user ? values.find(uses[first].user) : values.size() - (m_body.blocks[b].terminator.kind == TerminatorKind::Invoke && !values.isEmpty());
            }
            values.insert(index, reload);
            definitions.atTail.add(b, reload);
        }
        if (field)
            clearField(plan, *field, m_body.at(variable).type);
    }

    void finishDispatchBlock()
    {
        if (!m_isGenerator && std::ranges::none_of(m_suspensionPoints, &SuspensionPoint::isReachable))
            return;
        BlockID dispatch(0u);
        ValueID state = append(m_body, dispatch, getField(m_frame, CoroutineFrame::stateField));
        auto& terminator = m_body.at(dispatch).terminator;
        terminator.kind = TerminatorKind::Switch;
        terminator.value = state;
        for (unsigned i = 0; i < m_suspensionPoints.size(); ++i) {
            terminator.cases.append(i + 1);
            terminator.targets.append(Target { m_suspensionPoints[i].resumeBlock, { } });
        }
        terminator.targets.append(Target { m_start, { } });
    }

    void finishStartFunction()
    {
        auto& function = m_startBody;
        BlockID first = addBlock(function);
        function.at(first) = std::exchange(function.blocks[0], Block { });
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                if (!target.block.index())
                    target.block = first;
            }
        }
        m_position = function.position;
        BlockID entry(0u);
        function.at(entry).terminator.kind = TerminatorKind::Jump;
        function.at(entry).terminator.targets.append(Target { first, { } });
        for (TypeID type : m_argumentTypes) {
            function.parameterTypes.append(type);
            ValueID argument = addParameter(function, entry, type);
            function.at(entry).terminator.targets[0].arguments.append(argument);
        }
        function.parameterTypes.append(m_driverType);
        ValueID promise = addParameter(function, entry, m_driverType);
        auto arguments = function.at(first).parameters;
        for (ValueID argument : arguments)
            function.at(argument).immediates[0] = first.index();

        UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> pointOf;
        for (unsigned i = 0; i < m_suspensionPoints.size(); ++i)
            pointOf.add(m_suspensionPoints[i].suspension.index(), i);
        BitVector isFieldRead;
        {
            auto& resume = std::as_const(m_program).at(m_resume);
            BitVector isReached(resume.blocks.size());
            Vector<unsigned, 16> worklist { 0u };
            isReached.set(0);
            while (!worklist.isEmpty()) {
                auto& reached = resume.blocks[worklist.takeLast()];
                for (ValueID id : reached.values) {
                    auto& value = resume.at(id);
                    if (value.opcode == Opcode::GetField && value.layout() == m_frameLayout)
                        isFieldRead.set(value.field());
                }
                for (auto& target : reached.terminator.targets) {
                    if (!isReached.set(target.block.index()))
                        worklist.append(target.block.index());
                }
            }
        }
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            BlockID block(b);
            size_t index = function.at(block).values.findIf([&](ValueID id) {
                return function.at(id).opcode == Opcode::Await;
            });
            if (index == notFound) {
                if (function.at(block).terminator.kind != TerminatorKind::Return)
                    continue;
                ValueID result = function.at(block).terminator.value;
                if (!result)
                    result = append(function, block, undefined());
                append(function, block, operation(Operations::ResolvePromise, m_program.types.undefined, { promise, result }));
                function.at(block).terminator.value = append(function, block, undefined());
                continue;
            }
            ValueID suspension = function.at(block).values[index];
            ValueID operand = function.at(suspension).child(0);
            unsigned point = pointOf.get(suspension.index());
            m_position = function.at(suspension).position;
            function.at(block).values.shrink(index);
            function.at(block).terminator = Terminator { };
            ValueID state = append(function, block, integer(point + 1, m_program.types.int32));
            Value allocation = typed(Opcode::NewObjectWithShape, m_frameType);
            allocation.immediates[0] = m_frameLayout.index();
            ValueID frame = append(function, block, WTF::move(allocation));
            append(function, block, putField(frame, CoroutineFrame::stateField, state));
            append(function, block, putField(frame, CoroutineFrame::promiseField, promise));
            for (unsigned i = 0; i < arguments.size(); ++i) {
                if (isFieldRead.get(CoroutineFrame::firstArgumentField + i))
                    append(function, block, putField(frame, CoroutineFrame::firstArgumentField + i, arguments[i]));
            }
            for (auto [variable, field] : m_suspensionPoints[point].spilled)
                append(function, block, putField(frame, field, variable));
            Value closure = typed(Opcode::NewClosure, m_program.types.function);
            closure.immediates[0] = m_resume.index();
            closure.arguments.append(frame);
            ValueID resumeClosure = append(function, block, WTF::move(closure));
            append(function, block, putField(frame, CoroutineFrame::resumeClosureField, resumeClosure));
            append(function, block, operation(Operations::SuspendAsyncFunction, m_program.types.undefined, { resumeClosure, operand }));
            function.at(block).terminator.kind = TerminatorKind::Return;
            function.at(block).terminator.value = append(function, block, undefined());
        }

        BitVector isReachable(function.blocks.size());
        Vector<BlockID> worklist { entry };
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (isReachable.set(block.index()))
                continue;
            for (auto& target : function.at(block).terminator.targets)
                worklist.append(target.block);
        }
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            if (isReachable.get(b))
                continue;
            function.blocks[b] = Block { };
            function.blocks[b].terminator.kind = TerminatorKind::Unreachable;
        }
    }

    void returnFrom(Function& ramp, BlockID block, ValueID driver)
    {
        if (ramp.resultType && !m_program.at(m_driverType).isSubsetOf(m_program.at(ramp.resultType), m_program))
            driver = append(ramp, block, cast(driver, ramp.resultType, CastReason::Store));
        else
            ramp.resultType = m_driverType;
        ramp.at(block).terminator.kind = TerminatorKind::Return;
        ramp.at(block).terminator.value = driver;
    }

    void buildRamp()
    {
        auto& ramp = m_program.at(m_ramp);
        m_position = ramp.position;
        BlockID entry = addBlock(ramp);
        Vector<ValueID> arguments;
        for (TypeID type : m_argumentTypes)
            arguments.append(addParameter(ramp, entry, type));

        if (m_hasStartFunction) {
            ValueID promise = append(ramp, entry, operation(Operations::NewPromise, m_driverType, { }));
            Value start = typed(Opcode::CallDirect, m_program.types.undefined);
            start.immediates[0] = ramp.startFunction.index();
            start.arguments.appendVector(arguments);
            start.arguments.append(promise);
            append(ramp, entry, WTF::move(start));
            returnFrom(ramp, entry, promise);
            return;
        }

        ValueID promise;
        if (!m_isGenerator)
            promise = append(ramp, entry, operation(Operations::NewPromise, m_driverType, { }));
        ValueID notStarted = append(ramp, entry, integer(0, m_program.types.int32));
        Value allocation = typed(Opcode::NewObjectWithShape, m_frameType);
        allocation.immediates[0] = m_frameLayout.index();
        ValueID frame = append(ramp, entry, WTF::move(allocation));
        append(ramp, entry, putField(frame, CoroutineFrame::stateField, notStarted));
        if (promise)
            append(ramp, entry, putField(frame, CoroutineFrame::promiseField, promise));
        for (unsigned i = 0; i < arguments.size(); ++i)
            append(ramp, entry, putField(frame, CoroutineFrame::firstArgumentField + i, arguments[i]));
        Value closure = typed(Opcode::NewClosure, m_program.types.function);
        closure.immediates[0] = m_resume.index();
        closure.arguments.append(frame);
        ValueID resumeClosure = append(ramp, entry, WTF::move(closure));
        append(ramp, entry, putField(frame, CoroutineFrame::resumeClosureField, resumeClosure));
        if (!m_isGenerator) {
            ValueID mode = append(ramp, entry, integer(resumeModeNormal, m_resumeModeType));
            ValueID resumeValue = append(ramp, entry, undefined());
            append(ramp, entry, callResumeFunction(frame, mode, resumeValue));
            returnFrom(ramp, entry, promise);
            return;
        }

        ValueID generator = append(ramp, entry, operation(m_isAsync ? Operations::NewAsyncGenerator : Operations::NewGenerator, m_driverType, { resumeClosure }));
        if (m_isAsync)
            append(ramp, entry, putField(frame, CoroutineFrame::promiseField, generator));
        if (m_startsEagerly && m_isAsync)
            append(ramp, entry, operation(Operations::StartAsyncGenerator, m_program.types.undefined, { generator }));
        else if (m_startsEagerly) {
            ValueID mode = append(ramp, entry, integer(resumeModeNormal, m_resumeModeType));
            ValueID resumeValue = append(ramp, entry, undefined());
            append(ramp, entry, callResumeFunction(frame, mode, resumeValue));
        }
        returnFrom(ramp, entry, generator);
    }

    Program& m_program;
    Numbers& m_numbers;
    FunctionID m_ramp;
    FunctionID m_resume;
    Function m_body;
    Function m_startBody;
    LayoutID m_frameLayout;
    TypeID m_frameType;
    TypeID m_resumeModeType;
    TypeID m_driverType;
    TypeID m_resumeFunctionResultType;
    Vector<TypeID> m_argumentTypes;
    Vector<ValueID> m_arguments;
    uint32_t m_position { 0 };
    BlockID m_start;
    ValueID m_frame;
    ValueID m_resumeMode;
    ValueID m_resumeValue;
    bool m_isGenerator { false };
    bool m_isAsync { false };
    bool m_startsEagerly { false };
    bool m_hasStartFunction { false };
    BitVector m_awaitsOnlyInFirstActivation;
    BitVector m_inlineBlocks;
    BitVector m_awaitingYields;
    Vector<SuspensionPoint> m_suspensionPoints;
    Vector<Vector<BlockID, 2>> m_predecessors;
    BitVector m_livesInField;
    BitVector m_reachesSuspension;
    Vector<BlockID> m_resumeBlockOf;
    Vector<BlockID> m_suspendBlockOf;
    Vector<ValueID> m_registrationOf;
    std::array<unsigned, 2> m_numberOfRegisters;
};

} // anonymous namespace

void recordCellsLiveAcrossAwaits(const Program& program, Function& function)
{
    if (function.savesFrameOnSuspend)
        CoroutineSplitter::recordCellsLiveAcrossAwaits(program, function);
}

BitVector objectsLiveAcrossSuspensions(const Function& function)
{
    return CoroutineSplitter::objectsLiveAcrossSuspensions(function);
}

Vector<std::pair<FunctionID, Function>> splitCoroutines(Program& program)
{
    B3::Procedure procedure(/* usesSIMD = */ false);
    pinRegisters(procedure);
    std::array<unsigned, 2> numberOfRegisters { static_cast<unsigned>(procedure.code().regsInPriorityOrder(B3::GP).size()), static_cast<unsigned>(procedure.code().regsInPriorityOrder(B3::FP).size()) };
    unsigned numberOfFunctions = program.functions.size();
    unsigned followingFunctionCount = numberOfFunctions;
    Vector<CoroutineSplitter::Numbers> coroutines;
    Vector<uint8_t> splitFunctionCount(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0));
    forEachIndexInParallel(numberOfFunctions, [&](unsigned i) {
        auto& function = std::as_const(program).functions[i];
        if ((function.has(Format::FunctionIsAsync) || function.has(Format::FunctionIsGenerator)) && !function.blocks.isEmpty())
            splitFunctionCount[i] = CoroutineSplitter::canSaveFrame(function) ? 3 : CoroutineSplitter::hasStartFunction(function) ? 2 : 1;
    });
    for (unsigned i = 0; i < numberOfFunctions; ++i) {
        if (!splitFunctionCount[i])
            continue;
        bool savesFrameOnSuspend = splitFunctionCount[i] == 3;
        coroutines.append(CoroutineSplitter::reserveNumbers(program, FunctionID(i), FunctionID(followingFunctionCount), savesFrameOnSuspend));
        if (!savesFrameOnSuspend)
            coroutines.last().hasStartFunction = splitFunctionCount[i] == 2;
        followingFunctionCount += savesFrameOnSuspend ? 1 : splitFunctionCount[i];
    }
    program.functions.grow(followingFunctionCount);
    Vector<std::pair<FunctionID, Function>> generators;
    Vector<unsigned> indexAmongGenerators(coroutines.size(), [&](size_t k) {
        auto& function = std::as_const(program).at(coroutines[k].ramp);
        if (!function.has(Format::FunctionIsGenerator) || program.implementsBuiltin(function) || program.isInBuiltinFunctionModule(function))
            return UINT_MAX;
        generators.append({ coroutines[k].ramp, Function() });
        return static_cast<unsigned>(generators.size() - 1);
    });
    forEachIndexInParallel(coroutines.size(), [&](unsigned k) {
        removeTrivialParameters(program, program.at(coroutines[k].ramp));
        if (indexAmongGenerators[k] != UINT_MAX)
            generators[indexAmongGenerators[k]].second = std::as_const(program).at(coroutines[k].ramp);
        CoroutineSplitter(program, coroutines[k], numberOfRegisters).run();
    });
    Vector<Vector<StringID, 16>> names(coroutines.size());
    UncheckedKeyHashMap<const char*, StringID> internedLiterals;
    for (unsigned k = 0; k < coroutines.size(); ++k) {
        for (ASCIILiteral name : coroutines[k].namesByFirstUse) {
            names[k].append(internedLiterals.ensure(name.characters(), [&] {
                return program.intern(asBytes(name.span8()));
            }).iterator->value);
        }
    }
    forEachIndexInParallel(coroutines.size(), [&](unsigned k) {
        CoroutineSplitter::replaceProvisionalNames(program, coroutines[k], names[k].span());
    });
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        std::as_const(program).functions.forEachValue(i, directCalls, [&](ValueID id) {
            auto& value = std::as_const(program).functions[i].at(id);
            auto& callee = std::as_const(program).at(value.function());
            if (!callee.resumeFunction && !callee.startFunction)
                return;
            TypeID created = callee.resultType;
            if (!created || created == value.type || !program.at(created).isSubsetOf(program.at(value.type), program))
                return;
            auto& call = program.functions.atKeepingValueIndex(i).at(id);
            call.type = created;
            call.rep = repOf(program.at(created));
        });
    });
    return generators;
}

void splitOptimizedGenerators(Program& program, Vector<std::pair<FunctionID, Function>>&& generators)
{
    B3::Procedure procedure(/* usesSIMD = */ false);
    pinRegisters(procedure);
    std::array<unsigned, 2> numberOfRegisters { static_cast<unsigned>(procedure.code().regsInPriorityOrder(B3::GP).size()), static_cast<unsigned>(procedure.code().regsInPriorityOrder(B3::FP).size()) };
    Vector<CoroutineSplitter::Numbers> coroutines(generators.size());
    forEachIndexInParallel(generators.size(), [&](unsigned k) {
        auto& numbers = coroutines[k];
        numbers.ramp = generators[k].first;
        numbers.resume = std::as_const(program).at(numbers.ramp).resumeFunction;
        auto& split = std::as_const(program).at(numbers.resume);
        numbers.frameType = split.captureTypes[0];
        numbers.frameLayout = program.at(numbers.frameType).singleLayout();
        numbers.resumeModeType = split.parameterTypes[0];
        numbers.driverType = std::as_const(program).at(numbers.frameLayout).fields[CoroutineFrame::promiseField].type;
        CoroutineSplitter(program, numbers, numberOfRegisters).splitAgain(WTF::move(generators[k].second));
    });
    Vector<Vector<StringID, 16>> names(coroutines.size());
    UncheckedKeyHashMap<const char*, StringID> internedLiterals;
    for (unsigned k = 0; k < coroutines.size(); ++k) {
        for (ASCIILiteral name : coroutines[k].namesByFirstUse) {
            names[k].append(internedLiterals.ensure(name.characters(), [&] {
                return program.intern(asBytes(name.span8()));
            }).iterator->value);
        }
    }
    forEachIndexInParallel(coroutines.size(), [&](unsigned k) {
        auto& numbers = coroutines[k];
        CoroutineSplitter::replaceProvisionalNames(program, numbers, names[k].span());
        auto& layout = program.at(numbers.frameLayout);
        layout.indexFields();
        for (FunctionID id : { numbers.ramp, std::as_const(program).at(numbers.ramp).rampForClosures }) {
            if (!id)
                continue;
            auto& function = program.at(id);
            for (auto& value : function.values) {
                if (value.opcode != Opcode::NewObjectWithShape || value.layout() != numbers.frameLayout)
                    continue;
                Allocation allocation;
                allocation.layout = numbers.frameLayout;
                setInitializedFields(layout, allocation);
                function.allocations[value.allocation()] = WTF::move(allocation);
            }
        }
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
