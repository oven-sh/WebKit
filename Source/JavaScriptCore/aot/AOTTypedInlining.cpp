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

class Inlining {
public:
    Inlining(const Program& program, FunctionID id, Function& caller)
        : m_program(program)
        , m_id(id)
        , m_caller(caller)
    {
    }

    static void prepare(Program& program)
    {
        unsigned count = program.functions.size();
        Vector<unsigned> sizes(count);
        Vector<BitVector> neverReturn(count);
        Vector<Program::InliningFacts> facts(count);
        Vector<uint32_t> singleCreators(FillWith { }, count, Program::hasNoClosureCreator);
        forEachIndexInParallel(count, [&](unsigned i) {
            Function none;
            Inlining inlining(program, FunctionID(i), none);
            sizes[i] = inlining.sizeOf(std::as_const(program).functions[i], UINT_MAX);
            neverReturn[i] = inlining.noReturnBlocksOf(FunctionID(i));
            for (auto& value : std::as_const(program).functions[i].values) {
                if (value.opcode != Opcode::NewClosure && value.opcode != Opcode::NewClosureOnce && value.opcode != Opcode::FunctionReference)
                    continue;
                uint32_t* maker = &singleCreators[value.function().index()];
                uint32_t previous = WTF::atomicCompareExchangeStrong(maker, Program::hasNoClosureCreator, i, std::memory_order_relaxed);
                if (previous != Program::hasNoClosureCreator && previous != i)
                    WTF::atomicStore(maker, Program::hasMultipleClosureCreators, std::memory_order_relaxed);
            }
            if (!std::as_const(program).functions[i].blocks.isEmpty())
                facts[i] = { inlining.isAccessor(FunctionID(i)), inlining.returnsMethodTable(FunctionID(i)), inlining.calledParametersOf(FunctionID(i)), inlining.parametersCalledInLoopsOf(FunctionID(i)) };
        });
        program.singleClosureCreators = WTF::move(singleCreators);
        program.inliningFacts = WTF::move(facts);
        program.sizesForInlining = WTF::move(sizes);
        program.noReturnBlocks = WTF::move(neverReturn);
    }

    unsigned run(CleanUpTiming timing)
    {
        bool hasCandidateCall = m_caller.blocks.containsIf([&](auto& block) {
            return block.values.containsIf([&](ValueID id) {
                auto& value = m_caller.at(id);
                return value.opcode == Opcode::CallDirect && value.immediates[1] != afterHead && !value.isCallWithDeclinedInlining();
            });
        });
        if (!hasCandidateCall)
            return 0;
        unsigned replaced = 0;
        unsigned size = sizeOf(m_caller, UINT_MAX);
        m_inlinedFrom.fill(none, m_caller.blocks.size());
        m_neverReturns = noReturnBlocks(m_caller, [&](FunctionID callee) {
            return noReturnBlocksOf(callee).get(0);
        });
        m_isInCallerLoop = blocksInLoops(m_caller);
        if (m_caller.kind == FunctionKind::Module || m_program.isResumeFunction(m_caller))
            m_isInLoop = m_isInCallerLoop;
        BitVector acceptsTypeMismatches;
        if ((m_caller.has(Format::FunctionIsHiddenInStackTraces) && !m_caller.isGenericEntrypoint()) || m_caller.has(FunctionLogsTypeMismatches)) {
            for (auto& block : m_caller.blocks) {
                if (block.terminator.kind == TerminatorKind::Invoke)
                    acceptsTypeMismatches.set(block.terminator.targets[1].block.index());
            }
        }
        Vector<BlockID> worklist;
        for (unsigned b = m_caller.blocks.size(); b--;)
            worklist.append(BlockID(b));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (m_neverReturns.get(block.index()) || acceptsTypeMismatches.get(block.index()))
                continue;
            m_currentBlockIsInLoop = m_isInLoop.get(block.index());
            m_currentBlockIsInCallerLoop = m_isInCallerLoop.get(block.index());
            for (unsigned k = 0; k < m_caller.at(block).values.size(); ++k) {
                ValueID call = m_caller.at(block).values[k];
                if (m_caller.at(call).opcode != Opcode::CallDirect || m_caller.at(call).immediates[1] == afterHead || m_caller.at(call).isCallWithDeclinedInlining())
                    continue;
                FunctionID callee = m_caller.at(call).function();
                uint32_t maxInliningDepth = maxDepth + (isPassedCalledFunction(callee, m_caller.at(call)) || isPassedIteratedGenerator(m_program.at(callee), m_caller.at(call)));
                bool isTooDeep = depthOf(m_caller.at(call)) >= maxInliningDepth;
                bool isShallowerByNesting = isTooDeep && depthOf(m_caller.at(call), Copies::OnlyNestingLevels) < maxInliningDepth;
                if (m_program.at(callee).nullishResultKind && scalarizeNullableResult(m_program, m_caller, call)) {
                    didChangeCaller();
                    m_resultSearchValueCount = 0;
                    m_resultReads.clear();
                    ++replaced;
                    --k;
                    continue;
                }
                if (m_program.at(callee).resumeFunction || (m_caller.isGenericEntrypoint() && m_program.genericEntrypoints.get(callee.index()) == m_id) || m_caller.valueEntryTarget == callee)
                    continue;
                if (m_caller.resumeFunction == callee && !(m_caller.declaredAs & functionIsGenerator))
                    continue;
                if (closesCycle(callee, block)) {
                    m_caller.at(call).immediates[1] = maxDepth;
                    continue;
                }
                if (!canInline(m_program.at(callee), m_caller.at(call)) || consumesFactoryResult(callee))
                    continue;
                bool callerIsHot = m_currentBlockIsInCallerLoop || m_program.isHotOutsideLoops(m_id);
                if (m_program.at(callee).worker && !isOnlyRead(call))
                    continue;
                if (isOwnWorker(callee) && sizeOf(m_program.at(callee), tinySizeInColdCode + 1) > tinySizeInColdCode)
                    continue;
                if (k + 1 == m_caller.at(block).values.size() && m_caller.at(block).terminator.kind == TerminatorKind::Invoke && !initialStoresOf(m_program.at(callee)).empty() && !initialStoresCannotThrowAt(block, k, m_program.at(callee)))
                    continue;
                bool isBuiltin = m_program.at(callee).worker || isOwnWorker(callee) || (callerIsHot && m_program.at(callee).has(Format::FunctionIsBuiltin));
                bool isMoved = !isTooDeep && isOnlyCall(callee, m_caller.at(call));
                bool disablesNestedInlining = !m_currentBlockIsInCallerLoop && !constructsForDerivedClass(callee, m_caller.at(call));
                unsigned limit = depthOf(m_caller.at(call)) ? (disablesNestedInlining ? 0 : smallInCopy) : smallSize;
                unsigned defaultSizeLimit = limit;
                char licence = depthOf(m_caller.at(call)) ? 'c' : 's';
                bool isAllocatedAsCastType = returnsAllocationThatIsCast(callee, block, k);
                bool isFactory = !isAllocatedAsCastType && !isTooDeep && returnsMethodTable(callee) && isUsedOnlyAsReceiver(m_caller, call);
                if (isAllocatedAsCastType) {
                    limit = smallSizeWithExtraBenefit;
                    licence = 'b';
                } else if (onlyStoresToThis(m_program.at(callee)) || isAccessor(callee) || isPassedCalledFunction(callee, m_caller.at(call)) || isPassedIteratedGenerator(m_program.at(callee), m_caller.at(call))) {
                    limit = smallSizeWithExtraBenefit;
                    licence = isAccessor(callee) ? 'a' : onlyStoresToThis(m_program.at(callee)) ? 't' : 'h';
                } else if (m_currentBlockIsInCallerLoop && !depthOf(m_caller.at(call)) && m_program.numberOfDirectCalls[callee.index()] <= fewCalls) {
                    limit = smallSizeWithExtraBenefit;
                    licence = 'f';
                }
                if (!callerIsHot && !onlyStoresToThis(m_program.at(callee)) && !isAllocatedAsCastType && !isFactory && !isPassedCalledFunction(callee, m_caller.at(call), true) && !isPassedIteratedGenerator(m_program.at(callee), m_caller.at(call)))
                    limit = defaultSizeLimit = disablesNestedInlining && depthOf(m_caller.at(call)) ? 0 : tinySizeInColdCode;
                if (!isTooDeep && limit < maxPassedLiteralSize && isPassedArrayLiteral(m_caller.at(call))) {
                    limit = maxPassedLiteralSize;
                    licence = 'h';
                }
                if (!isTooDeep && limit < smallSizeWithExtraBenefit && ((callerSuspends() && isPassedReadOnlyObject(m_caller.at(call))) || isPassedDestructuredObject(m_caller.at(call)))) {
                    limit = smallSizeWithExtraBenefit;
                    licence = 'h';
                }
                if (isFactory) {
                    limit = maxFactorySize;
                    licence = 'r';
                }
                unsigned growsBy = sizeOf(m_program.at(callee), isBuiltin || isMoved ? UINT_MAX : limit + 1);
                if (growsBy <= defaultSizeLimit)
                    licence = depthOf(m_caller.at(call)) ? 'c' : 's';
                if (!isTooDeep && !isBuiltin && !isMoved && growsBy > limit) {
                    unsigned left = sizeAtCall(m_program.at(callee), m_caller.at(call), smallSizeWithExtraBenefit + 1);
                    if (left <= (m_currentBlockIsInCallerLoop ? smallSizeWithExtraBenefit : maxSizeAfterConstantFolding) && sizeOf(m_program.at(callee), 2 * left) >= 2 * left) {
                        growsBy = left;
                        limit = smallSizeWithExtraBenefit;
                        licence = 'k';
                    }
                }
                const BitVector* head = nullptr;
                if ((isTooDeep && !isShallowerByNesting) || (!isBuiltin && !isMoved && growsBy > limit)) {
                    if (!callerIsHot || runsOnceOrSuspends())
                        continue;
                    auto& found = headOf(callee);
                    if (!found.size)
                        continue;
                    head = &found.blocks;
                    growsBy = found.size;
                    isMoved = false;
                    licence = 'e';
                }
                if (size + growsBy > maxGrownTo && !m_program.at(callee).worker && !isOwnWorker(callee))
                    continue;
                uint8_t given = inlineReasonForLetter(head ? 'e' : m_program.at(callee).worker ? 'w' : isOwnWorker(callee) ? 'o' : isBuiltin ? 'p' : isMoved ? 'm' : licence);
                if (m_program.isInliningDisabled(m_id, given) && m_program.at(m_id).startFunction != callee) {
                    m_caller.at(call).immediates[1] = Value::inliningWasDeclined - given;
                    continue;
                }
                unsigned first = m_caller.blocks.size();
                copyIn(block, k, callee, head);
                didChangeCaller();
                m_caller.inlineFrames[m_lastInlineFrame].licence = given;
                if (isMoved)
                    m_movedFrames.set(m_lastInlineFrame);
                for (unsigned b = m_caller.blocks.size(); b-- > first;)
                    worklist.append(BlockID(b));
                size += growsBy;
                ++replaced;
                break;
            }
        }
        if (replaced && timing == CleanUpTiming::Immediate)
            cleanUp(m_program, m_caller);
        return replaced;
    }

    bool inlineOwnBody()
    {
        auto& values = m_caller.blocks[0].values;
        size_t k = values.findIf([&](ValueID id) {
            return m_caller.at(id).opcode == Opcode::CallDirect;
        });
        if (k == notFound || m_caller.at(values[k]).inlineFrame)
            return false;
        FunctionID callee = m_caller.at(values[k]).function();
        auto& function = m_program.at(callee);
        m_isInliningOwnBody = true;
        if (m_program.genericEntrypoints.get(callee.index()) != m_id || function.resumeFunction || (function.kind == FunctionKind::Constructor && !forwardsRestParameterToSuperclass(function)) || !canInline(function, m_caller.at(values[k])))
            return false;
        m_inlinedFrom.fill(none, m_caller.blocks.size());
        copyIn(BlockID(0u), k, callee);
        m_caller.inlineFrames[m_lastInlineFrame].licence = inlineReasonForLetter('o');
        cleanUp(m_program, m_caller);
        if (resolveCalls(m_program, m_caller, m_program.isStaticallyWarm(m_id)))
            cleanUp(m_program, m_caller);
        return true;
    }

private:
    bool constructsForDerivedClass(FunctionID callee, const Value& call) const
    {
        auto& constructor = m_program.at(callee);
        if (constructor.kind != FunctionKind::Constructor)
            return false;
        if (constructor.constructedClass && m_program.at(constructor.constructedClass).base)
            return true;
        return call.inlineFrame && m_program.at(m_caller.inlineFrames[call.inlineFrame].callee).kind == FunctionKind::Constructor;
    }

    bool forwardsRestParameterToSuperclass(const Function& constructor) const
    {
        if (!constructor.has(Format::FunctionHasRestParameter) || constructor.blocks.isEmpty() || constructor.blocks[0].parameters.isEmpty())
            return false;
        if (constructor.constructedClass && m_program.at(constructor.constructedClass).extendsGlobalClass)
            return false;
        ValueID rest = constructor.blocks[0].parameters.last();
        for (auto& block : constructor.blocks) {
            for (ValueID id : block.values) {
                auto& value = constructor.at(id);
                if (languageOperationOf(m_program, value) != LanguageOperation::SuperConstruct || (value.arguments.size() != 3 && value.arguments.size() != 4))
                    continue;
                if (value.arguments[2] == rest && value.arguments.size() == 3)
                    return true;
                auto withoutCasts = [&](ValueID operand) {
                    while (constructor.at(operand).opcode == Opcode::Cast)
                        operand = constructor.at(operand).child(0);
                    return operand;
                };
                auto& list = constructor.at(withoutCasts(value.arguments[2]));
                if (languageOperationOf(m_program, list) != LanguageOperation::AppendSpread || list.arguments.size() != 2 || constructor.at(list.arguments[0]).opcode != Opcode::NewArray || !constructor.at(list.arguments[0]).arguments.isEmpty())
                    continue;
                if (withoutCasts(list.arguments[1]) == rest)
                    return true;
                auto& spreadObject = constructor.at(withoutCasts(list.arguments[1]));
                if (languageOperationOf(m_program, spreadObject) == LanguageOperation::CreateArguments && spreadObject.arguments.size() == 1 && spreadObject.arguments[0] == rest)
                    return true;
            }
        }
        return false;
    }

    static constexpr unsigned smallSize = 13;
    static constexpr unsigned smallInCopy = 12;
    static constexpr unsigned tinySizeInColdCode = 7;
    static constexpr unsigned smallSizeWithExtraBenefit = 40;
    static constexpr unsigned maxSizeAfterConstantFolding = 10;
    static constexpr unsigned maxPredicate = 40;
    static constexpr unsigned maxAccessor = 20;
    static constexpr unsigned fewCalls = 3;
    static constexpr unsigned maxForwarder = 2;
    static constexpr unsigned maxHead = 6;
    static constexpr uint32_t afterHead = Format::none - 1;
    static constexpr unsigned maxMoved = 200;
    static constexpr unsigned maxFactorySize = 400;
    static constexpr unsigned maxPassedLiteralSize = 120;
    static constexpr unsigned minFactoryMethods = 8;
    static constexpr unsigned maxGrownTo = 4000;
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr uint32_t maxDepth = 2;

    using EdgesIntoBlocks = Vector<Vector<const Target*, 2>>;

    static EdgesIntoBlocks edgesIntoBlocks(const Function& function)
    {
        EdgesIntoBlocks result(function.blocks.size());
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets)
                result[target.block.index()].append(&target);
        }
        return result;
    }

    static ValueID originOf(const Function& function, ValueID value, EdgesIntoBlocks& edges)
    {
        auto skipCasts = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast)
                id = function.at(id).child(0);
            return id;
        };
        auto isBlockParameter = [&](ValueID id) {
            return function.at(id).opcode == Opcode::Parameter && function.at(id).immediates[0];
        };
        value = skipCasts(value);
        if (!isBlockParameter(value))
            return value;
        if (edges.isEmpty())
            edges = edgesIntoBlocks(function);
        static constexpr unsigned maxWeb = 16;
        Vector<ValueID, maxWeb> web { value };
        ValueID only;
        for (unsigned next = 0; next < web.size(); ++next) {
            auto& parameter = function.at(web[next]);
            if (parameter.immediates[0] >= edges.size())
                continue;
            for (const Target* target : edges[parameter.immediates[0]]) {
                if (parameter.immediates[1] >= target->arguments.size())
                    return value;
                ValueID handed = skipCasts(target->arguments[parameter.immediates[1]]);
                if (isBlockParameter(handed)) {
                    if (web.contains(handed))
                        continue;
                    if (web.size() == maxWeb)
                        return value;
                    web.append(handed);
                    continue;
                }
                if (only && only != handed)
                    return value;
                only = handed;
            }
        }
        return only ? only : value;
    }

    const Program::InliningFacts* factsOf(FunctionID id) const
    {
        return m_program.inliningFacts.isEmpty() || m_program.at(id).blocks.isEmpty() ? nullptr : &m_program.inliningFacts[id.index()];
    }

    const BitVector& calledParametersOf(FunctionID id)
    {
        if (auto* facts = factsOf(id))
            return facts->parametersCalled;
        return m_calledParameters.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            BitVector result;
            EdgesIntoBlocks edges;
            for (auto& value : function.values) {
                if (value.opcode != Opcode::Call)
                    continue;
                if (size_t index = function.blocks[0].parameters.find(originOf(function, value.child(0), edges)); index != notFound)
                    result.set(index);
            }
            return result;
        }).iterator->value;
    }

    const BitVector& parametersCalledInLoopsOf(FunctionID id)
    {
        if (auto* facts = factsOf(id))
            return facts->parametersCalledInLoops;
        return m_parametersCalledInLoops.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            BitVector result;
            EdgesIntoBlocks edges;
            Vector<unsigned> cycles;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                for (ValueID value : function.blocks[b].values) {
                    if (function.at(value).opcode != Opcode::Call)
                        continue;
                    if (cycles.isEmpty())
                        cycles = computeBlockCycles(function);
                    if (cycles[b] == notInCycle)
                        break;
                    if (size_t index = function.blocks[0].parameters.find(originOf(function, function.at(value).child(0), edges)); index != notFound)
                        result.set(index);
                }
            }
            return result;
        }).iterator->value;
    }

    bool isPassedIteratedGenerator(const Function& callee, const Value& call) const
    {
        BitVector generators;
        for (unsigned index = 0; index < call.arguments.size(); ++index) {
            ValueID argument = call.arguments[index];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            auto& created = m_caller.at(argument);
            if (created.opcode == Opcode::CallDirect && !created.isJumpToContinuation() && m_program.at(created.function()).resumeFunction && (m_program.at(created.function()).declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsGenerator)
                generators.set(index);
        }
        if (generators.isEmpty() || callee.blocks.isEmpty())
            return false;
        for (unsigned index : generators) {
            if (index >= callee.blocks[0].parameters.size())
                continue;
            Vector<ValueID, 4> aliases { callee.blocks[0].parameters[index] };
            ValueID iterator;
            unsigned steps = 0;
            bool isOnlyIterated = true;
            for (auto& block : callee.blocks) {
                for (ValueID id : block.values) {
                    auto& user = callee.at(id);
                    user.forEachChild([&](ValueID child) {
                        if (iterator && child == iterator) {
                            steps += user.opcode == Opcode::IteratorHasNext;
                            isOnlyIterated &= user.child(0) == iterator && (user.opcode == Opcode::IteratorHasNext || user.opcode == Opcode::IteratorValue || user.opcode == Opcode::IteratorClose);
                        } else if (!aliases.contains(child))
                            return;
                        else if (user.opcode == Opcode::Cast)
                            aliases.append(id);
                        else if (user.opcode == Opcode::IteratorOpen && !iterator)
                            iterator = id;
                        else
                            isOnlyIterated &= user.opcode == Opcode::StrictEq || user.opcode == Opcode::Eq || user.opcode == Opcode::IsUndefinedOrNull || user.opcode == Opcode::ToBoolean;
                    });
                }
                if (block.terminator.value)
                    isOnlyIterated &= block.terminator.value != iterator && !aliases.contains(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        isOnlyIterated &= argument != iterator && !aliases.contains(argument);
                }
            }
            if (iterator && steps == 1 && isOnlyIterated)
                return true;
        }
        return false;
    }

    bool isPassedCalledFunction(FunctionID callee, const Value& call, bool inLoop = false)
    {
        for (unsigned index : inLoop ? parametersCalledInLoopsOf(callee) : calledParametersOf(callee)) {
            if (index >= call.arguments.size())
                continue;
            ValueID argument = call.arguments[index];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            Opcode opcode = m_caller.at(argument).opcode;
            if (opcode == Opcode::FunctionReference || opcode == Opcode::NewClosure || opcode == Opcode::NewClosureOnce)
                return true;
        }
        return false;
    }

    bool onlyReadsParameter(const Function& callee, unsigned index) const
    {
        BitVector isAlias(callee.values.size());
        isAlias.set(callee.blocks[0].parameters[index].index());
        UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> heldIn;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : callee.blocks) {
                for (ValueID id : block.values) {
                    auto& value = callee.at(id);
                    if (isAlias.get(id.index()))
                        continue;
                    bool returnsIt = (value.opcode == Opcode::Cast && isAlias.get(value.child(0).index()))
                        || (value.opcode == Opcode::GetField && heldIn.contains(static_cast<uint64_t>(value.layout().index()) << 32 | value.field()));
                    if (returnsIt) {
                        isAlias.set(id.index());
                        changed = true;
                    }
                    if (value.opcode == Opcode::PutField && isAlias.get(value.child(1).index()) && callee.at(value.child(0)).opcode == Opcode::NewObjectWithShape)
                        changed |= heldIn.add(static_cast<uint64_t>(value.layout().index()) << 32 | value.field()).isNewEntry;
                }
            }
        }
        for (auto& block : callee.blocks) {
            for (ValueID id : block.values) {
                auto& value = callee.at(id);
                bool uses = false;
                value.forEachChild([&](ValueID child) {
                    uses |= isAlias.get(child.index());
                });
                if (!uses)
                    continue;
                switch (value.opcode) {
                case Opcode::Cast:
                case Opcode::ToBoolean:
                case Opcode::IsUndefinedOrNull:
                    break;
                case Opcode::GetById:
                    if (!equalSpans(m_program.string(value.string()), asBytes("length"_s.span8())))
                        return false;
                    break;
                case Opcode::GetByVal:
                    if (isAlias.get(value.child(1).index()))
                        return false;
                    break;
                case Opcode::PutField:
                    if (isAlias.get(value.child(0).index()) || callee.at(value.child(0)).opcode != Opcode::NewObjectWithShape)
                        return false;
                    break;
                default:
                    return false;
                }
            }
            if (block.terminator.value && block.terminator.kind != TerminatorKind::Branch && isAlias.get(block.terminator.value.index()))
                return false;
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments) {
                    if (isAlias.get(argument.index()))
                        return false;
                }
            }
        }
        return true;
    }

    bool isPassedArrayLiteral(const Value& call) const
    {
        auto& callee = m_program.at(call.function());
        if (callee.blocks.isEmpty() || callee.blocks[0].parameters.size() != call.arguments.size())
            return false;
        for (unsigned i = 0; i < call.arguments.size(); ++i) {
            ValueID argument = call.arguments[i];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            if (m_caller.at(argument).opcode == Opcode::NewArray && m_caller.at(argument).arguments.size() <= 4 && onlyReadsParameter(callee, i))
                return true;
        }
        return false;
    }

    bool onlyReadsFieldsOfParameter(const Function& callee, unsigned index) const
    {
        BitVector isAlias(callee.values.size());
        isAlias.set(callee.blocks[0].parameters[index].index());
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : callee.blocks) {
                for (ValueID id : block.values) {
                    auto& value = callee.at(id);
                    if (value.opcode == Opcode::Cast && isAlias.get(value.child(0).index()) && !isAlias.set(id.index()))
                        changed = true;
                }
            }
        }
        for (auto& block : callee.blocks) {
            for (ValueID id : block.values) {
                auto& value = callee.at(id);
                bool uses = false;
                value.forEachChild([&](ValueID child) {
                    uses |= isAlias.get(child.index());
                });
                if (!uses)
                    continue;
                switch (value.opcode) {
                case Opcode::Cast:
                case Opcode::GetField:
                case Opcode::ToBoolean:
                case Opcode::IsUndefinedOrNull:
                    break;
                case Opcode::GetById:
                    if (value.immediates[1] != Format::none || !fieldOf(m_program, m_program.typeOf(callee, value.child(0)), value.string()))
                        return false;
                    break;
                default:
                    return false;
                }
            }
            if (block.terminator.value && block.terminator.kind != TerminatorKind::Branch && isAlias.get(block.terminator.value.index()))
                return false;
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments) {
                    if (isAlias.get(argument.index()))
                        return false;
                }
            }
        }
        return true;
    }

    bool callerSuspends() const
    {
        return m_caller.savesFrameOnSuspend || m_caller.declaredAs & (functionIsAsync | functionIsGenerator) || m_program.isResumeFunction(m_caller);
    }

    bool isUsedAfterSuspension(ValueID object) const
    {
        auto suspends = [&](const Value& value) {
            if (value.opcode == Opcode::Await || value.opcode == Opcode::Yield)
                return true;
            if (value.opcode == Opcode::Operation)
                return m_program.operationOf(value) == Operations::AwaitBySavingFrame;
            if (value.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::AwaitBySavingFrame;
        };
        unsigned numberOfBlocks = m_caller.blocks.size();
        unsigned allocationBlock = numberOfBlocks;
        BitVector isAlias(m_caller.values.size());
        isAlias.set(object.index());
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (ValueID id : m_caller.blocks[b].values) {
                if (id == object)
                    allocationBlock = b;
                else if (m_caller.at(id).opcode == Opcode::Cast && isAlias.get(m_caller.at(id).child(0).index()))
                    isAlias.set(id.index());
            }
        }
        if (allocationBlock == numberOfBlocks)
            return false;
        BitVector isReached(numberOfBlocks);
        BitVector isReachedAfterSuspension(numberOfBlocks);
        Vector<std::pair<unsigned, bool>, 16> worklist;
        worklist.append({ allocationBlock, false });
        while (!worklist.isEmpty()) {
            auto [b, comesAfter] = worklist.takeLast();
            auto& block = m_caller.blocks[b];
            bool wasAllocated = b != allocationBlock;
            for (ValueID id : block.values) {
                if (!wasAllocated) {
                    wasAllocated = id == object;
                    continue;
                }
                auto& value = m_caller.at(id);
                if (comesAfter) {
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= isAlias.get(child.index());
                    });
                    if (uses)
                        return true;
                }
                comesAfter |= suspends(value);
            }
            if (comesAfter && block.terminator.value && isAlias.get(block.terminator.value.index()))
                return true;
            for (auto& target : block.terminator.targets) {
                if (comesAfter) {
                    for (ValueID argument : target.arguments) {
                        if (isAlias.get(argument.index()))
                            return true;
                    }
                }
                unsigned next = target.block.index();
                if (next != allocationBlock && !(comesAfter ? isReachedAfterSuspension : isReached).set(next))
                    worklist.append({ next, comesAfter });
            }
        }
        return false;
    }

    bool isPassedReadOnlyObject(const Value& call) const
    {
        auto& callee = m_program.at(call.function());
        if (callee.blocks.isEmpty() || callee.blocks[0].parameters.size() != call.arguments.size())
            return false;
        for (unsigned i = 0; i < call.arguments.size(); ++i) {
            ValueID argument = call.arguments[i];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            if (m_caller.at(argument).opcode == Opcode::NewObjectWithShape && isUsedAfterSuspension(argument) && onlyReadsFieldsOfParameter(callee, i))
                return true;
        }
        return false;
    }

    bool isPassedDestructuredObject(const Value& call) const
    {
        auto& callee = m_program.at(call.function());
        if (!callee.onlyDestructuresParameter || callee.blocks.isEmpty() || callee.blocks[0].parameters.size() != call.arguments.size())
            return false;
        for (unsigned i = 0; i < call.arguments.size(); ++i) {
            ValueID argument = call.arguments[i];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            if (m_caller.at(argument).opcode == Opcode::NewObjectWithShape && onlyReadsFieldsOfParameter(callee, i))
                return true;
        }
        return false;
    }

    bool returnsMethodTable(FunctionID id) const
    {
        if (auto* facts = factsOf(id))
            return facts->returnsMethodTable;
        auto& function = m_program.at(id);
        LayoutID resultLayout = m_program.at(resultTypeOf(function)).singleLayout();
        if (!resultLayout || m_program.at(resultLayout).kind != LayoutKind::Record || m_program.at(resultLayout).fields.size() < minFactoryMethods)
            return false;
        ValueID created;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            ValueID value = block.terminator.value;
            for (unsigned steps = 0; value && steps < 8 && function.at(value).opcode != Opcode::NewObjectWithShape; ++steps) {
                auto& definition = function.at(value);
                if (definition.opcode == Opcode::Cast) {
                    value = definition.child(0);
                    continue;
                }
                if (definition.opcode != Opcode::Parameter || !definition.immediates[0])
                    return false;
                ValueID only;
                for (auto& from : function.blocks) {
                    for (auto& target : from.terminator.targets) {
                        if (target.block.index() != definition.immediates[0] || definition.immediates[1] >= target.arguments.size() || target.arguments[definition.immediates[1]] == value)
                            continue;
                        if (only && only != target.arguments[definition.immediates[1]])
                            return false;
                        only = target.arguments[definition.immediates[1]];
                    }
                }
                value = only;
            }
            if (!value || function.at(value).opcode != Opcode::NewObjectWithShape || (created && created != value))
                return false;
            created = value;
        }
        if (!created)
            return false;
        unsigned methods = 0;
        for (auto& value : function.values)
            methods += value.opcode == Opcode::PutField && value.child(0) == created && function.at(value.child(1)).opcode == Opcode::NewClosure;
        return methods >= minFactoryMethods;
    }

    enum class ObjectUse : uint8_t { Reads, IsAlias, IsOtherUse };

    static ObjectUse classifyObjectUse(const Function& caller, const Value& value, ValueID object)
    {
        switch (value.opcode) {
        case Opcode::GetField:
        case Opcode::IsUndefinedOrNull:
        case Opcode::IteratorOpen:
            return ObjectUse::Reads;
        case Opcode::GetById:
            return value.methodGetByIdBuiltin() ? ObjectUse::IsOtherUse : ObjectUse::Reads;
        case Opcode::GetByVal:
            return value.child(0) != object || value.child(1) == object || caller.at(value.child(1)).opcode != Opcode::Integer ? ObjectUse::IsOtherUse : ObjectUse::Reads;
        case Opcode::Cast:
            return value.castReason() != CastReason::Upcast && value.castReason() != CastReason::Flow && value.castReason() != CastReason::NonNull ? ObjectUse::IsOtherUse : ObjectUse::IsAlias;
        default:
            return ObjectUse::IsOtherUse;
        }
    }

    void didChangeCaller()
    {
        m_queriesSinceCallerChanged = 0;
        m_firstUser.shrink(0);
    }

    void findUsers()
    {
        unsigned count = m_caller.values.size();
        m_firstUser.fill(0, count + 1);
        m_isUsedByTerminator.clearAll();
        m_isUsedByTerminator.ensureSize(count);
        for (auto& block : m_caller.blocks) {
            if (block.terminator.value)
                m_isUsedByTerminator.quickSet(block.terminator.value.index());
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments) {
                    if (argument)
                        m_isUsedByTerminator.quickSet(argument.index());
                }
            }
            for (ValueID user : block.values) {
                std::as_const(m_caller).at(user).forEachChild([&](ValueID child) {
                    if (child)
                        ++m_firstUser[child.index()];
                });
            }
        }
        uint32_t next = 0;
        for (auto& first : m_firstUser)
            first = std::exchange(next, next + first);
        m_users.resize(next);
        for (auto& block : m_caller.blocks) {
            for (ValueID user : block.values) {
                std::as_const(m_caller).at(user).forEachChild([&](ValueID child) {
                    if (child)
                        m_users[m_firstUser[child.index()]++] = user;
                });
            }
        }
        for (unsigned i = count; i; --i)
            m_firstUser[i] = m_firstUser[i - 1];
        m_firstUser[0] = 0;
    }

    bool isOnlyRead(ValueID result)
    {
        static constexpr unsigned maxQueriesWithoutUsers = 2;
        bool knowsUsers = m_queriesSinceCallerChanged++ >= maxQueriesWithoutUsers;
        if (knowsUsers && m_firstUser.isEmpty())
            findUsers();
        Vector<ValueID, 4> aliases { result };
        for (unsigned next = 0; next < aliases.size(); ++next) {
            ValueID object = aliases[next];
            if (knowsUsers) {
                if (m_isUsedByTerminator.quickGet(object.index()))
                    return false;
                for (ValueID user : m_users.span().subspan(m_firstUser[object.index()], m_firstUser[object.index() + 1] - m_firstUser[object.index()])) {
                    ObjectUse use = classifyObjectUse(m_caller, m_caller.at(user), object);
                    if (use == ObjectUse::IsOtherUse)
                        return false;
                    if (use == ObjectUse::IsAlias)
                        aliases.append(user);
                }
                continue;
            }
            for (auto& block : m_caller.blocks) {
                if (block.terminator.value == object)
                    return false;
                for (auto& target : block.terminator.targets) {
                    if (target.arguments.contains(object))
                        return false;
                }
                for (ValueID user : block.values) {
                    auto& value = std::as_const(m_caller).at(user);
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= child == object;
                    });
                    if (!uses)
                        continue;
                    ObjectUse use = classifyObjectUse(m_caller, value, object);
                    if (use == ObjectUse::IsOtherUse)
                        return false;
                    if (use == ObjectUse::IsAlias)
                        aliases.append(user);
                }
            }
        }
        return true;
    }

    static bool isUsedOnlyAsReceiver(const Function& caller, ValueID result)
    {
        BitVector isResult(caller.values.size());
        isResult.set(result.index());
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned v = 0; v < caller.values.size(); ++v) {
                auto& value = caller.values[v];
                if (value.opcode == Opcode::Cast && value.castReason() != CastReason::Coerce && value.castReason() != CastReason::As && isResult.get(value.child(0).index()))
                    changed |= !isResult.set(v);
            }
            for (auto& block : caller.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = caller.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                        if (isResult.get(target.arguments[i].index()))
                            changed |= !isResult.set(parameters[i].index());
                    }
                }
            }
        }
        for (auto& block : caller.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = caller.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                    if (isResult.get(parameters[i].index()) && !isResult.get(target.arguments[i].index()))
                        return false;
                }
            }
        }
        auto isLoadFromResult = [&](ValueID id) {
            while (caller.at(id).opcode == Opcode::Cast)
                id = caller.at(id).child(0);
            return caller.at(id).opcode == Opcode::GetById && isResult.get(caller.at(id).child(0).index());
        };
        for (auto& block : caller.blocks) {
            for (ValueID id : block.values) {
                auto& value = caller.at(id);
                unsigned uses = 0;
                value.forEachChild([&](ValueID child) {
                    uses += isResult.get(child.index());
                });
                if (!uses)
                    continue;
                switch (value.opcode) {
                case Opcode::Cast:
                    if (value.castReason() == CastReason::Coerce)
                        return false;
                    break;
                case Opcode::GetById:
                case Opcode::IsUndefinedOrNull:
                case Opcode::ToBoolean:
                    break;
                case Opcode::CallMethod:
                    if (uses != 1 || !isResult.get(value.child(0).index()))
                        return false;
                    break;
                case Opcode::Call:
                    if (uses != 1 || !value.child(1) || !isResult.get(value.child(1).index()) || !isLoadFromResult(value.child(0)))
                        return false;
                    break;
                default:
                    return false;
                }
            }
            if (block.terminator.value && isResult.get(block.terminator.value.index()))
                return false;
        }
        return true;
    }

    bool consumesFactoryResult(FunctionID id)
    {
        return m_consumesFactoryResult.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            for (unsigned v = 0; v < function.values.size(); ++v) {
                if (function.values[v].opcode == Opcode::CallDirect && returnsMethodTable(function.values[v].function()) && isUsedOnlyAsReceiver(function, ValueID(v)))
                    return true;
            }
            return false;
        }).iterator->value;
    }
    UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_consumesFactoryResult;

    bool isOnlyCall(FunctionID callee, const Value& call) const
    {
        if (m_caller.isGenericEntrypoint() || (call.inlineFrame && !m_movedFrames.get(call.inlineFrame)))
            return false;
        if (m_program.numberOfDirectCalls[callee.index()] != 1 || m_program.genericEntrypoints.contains(callee.index()))
            return false;
        return (m_program.at(m_id).resumeFunction || m_program.at(m_id).startFunction || m_currentBlockIsInCallerLoop || sizeOf(m_program.at(m_id), smallSize + 1) > smallSize) && sizeOf(m_program.at(callee), maxMoved + 1) <= maxMoved;
    }

    bool returnsOwnAllocation(FunctionID id, unsigned depth = 0) const
    {
        static constexpr unsigned maxDepth = 3;
        if (depth > maxDepth)
            return false;
        if (auto found = m_returnsOwnAllocation.find(id.index()); found != m_returnsOwnAllocation.end())
            return found->value;
        auto& function = m_program.at(id);
        auto origin = [&](ValueID value) {
            while (function.at(value).opcode == Opcode::Cast)
                value = function.at(value).child(0);
            return value;
        };
        bool result = false;
        for (auto& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            result = false;
            if (!block.terminator.value)
                break;
            auto& returned = function.at(origin(block.terminator.value));
            FunctionID called;
            switch (returned.opcode) {
            case Opcode::NewObjectWithShape:
            case Opcode::Construct:
                result = true;
                break;
            case Opcode::CallDirect:
                called = returned.function();
                break;
            case Opcode::Call: {
                auto& callee = function.at(origin(returned.child(0)));
                bool isNamed = callee.opcode == Opcode::FunctionReference || callee.opcode == Opcode::NewClosure || callee.opcode == Opcode::NewClosureOnce;
                called = isNamed ? callee.function() : callee.predictedClosure() ? callee.predictedClosure() : function.at(returned.child(0)).predictedClosure();
                break;
            }
            default:
                result = languageOperationOf(m_program, returned) == LanguageOperation::ConstructWith;
                break;
            }
            if (called && called != id)
                result = returnsOwnAllocation(called, depth + 1);
            if (!result)
                break;
        }
        if (result || !depth)
            m_returnsOwnAllocation.add(id.index(), result);
        return result;
    }

    bool returnsAllocationThatIsCast(FunctionID callee, BlockID block, unsigned k) const
    {
        auto& values = m_caller.at(block).values;
        LayoutID created = m_program.at(m_caller.at(values[k]).type).singleLayout();
        if (!created || m_program.at(created).specializations.isEmpty())
            return false;
        Vector<ValueID, 2> names { values[k] };
        for (unsigned i = k + 1; i < values.size(); ++i) {
            auto& user = m_caller.at(values[i]);
            bool uses = false;
            user.forEachChild([&](ValueID child) {
                uses |= names.contains(child);
            });
            if (!uses)
                continue;
            if (user.opcode != Opcode::Cast)
                return false;
            if (user.castReason() == CastReason::Upcast) {
                names.append(values[i]);
                continue;
            }
            if (user.castReason() != CastReason::As && user.castReason() != CastReason::Coerce)
                return false;
            LayoutID target = m_program.at(user.castReason() == CastReason::Coerce ? user.coercedType() : user.type).singleLayout();
            return target && std::ranges::binary_search(m_program.at(created).specializations, target) && returnsOwnAllocation(callee);
        }
        return false;
    }

    bool onlyStoresToThis(const Function& function) const
    {
        if (!function.has(Format::FunctionHasThis) || function.blocks.size() != 1 || function.blocks[0].terminator.kind != TerminatorKind::Return)
            return false;
        ValueID thisValue = function.blocks[0].parameters[function.captureTypes.size()];
        bool stores = false;
        for (ValueID id : function.blocks[0].values) {
            auto& value = function.at(id);
            switch (value.opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
                break;
            case Opcode::PutById:
                if (value.child(0) != thisValue || value.child(1) == thisValue)
                    return false;
                stores = true;
                break;
            default:
                return false;
            }
        }
        return stores;
    }

    bool runsOnceOrSuspends() const
    {
        return !m_currentBlockIsInLoop && (m_caller.kind == FunctionKind::Module || m_program.isResumeFunction(m_caller));
    }

    bool initialStoresCannotThrowAt(BlockID block, unsigned k, const Function& callee) const
    {
        auto withoutUpcasts = [&](ValueID value) {
            while (m_caller.at(value).opcode == Opcode::Cast && m_caller.at(value).castReason() == CastReason::Upcast)
                value = m_caller.at(value).child(0);
            return value;
        };
        auto& values = m_caller.at(block).values;
        if (!callee.has(Format::FunctionHasThis))
            return false;
        unsigned thisIndex = callee.captureTypes.size();
        ValueID object = withoutUpcasts(m_caller.at(values[k]).arguments[thisIndex]);
        if (m_caller.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        size_t allocationSite = values.find(object);
        if (allocationSite == notFound || allocationSite >= k)
            return false;
        for (size_t i = allocationSite + 1; i < k; ++i) {
            if (withoutUpcasts(values[i]) == object)
                continue;
            bool usesValue = false;
            m_caller.at(values[i]).forEachChild([&](ValueID child) {
                usesValue |= withoutUpcasts(child) == object;
            });
            if (usesValue)
                return false;
        }
        LayoutID created = m_caller.at(object).layout();
        auto& layout = m_program.at(created);
        if (!isObjectLayout(layout.kind) || (layout.kind == LayoutKind::Record && layout.mayHaveOtherPrototype))
            return false;
        ValueID thisValue = callee.blocks[0].parameters[thisIndex];
        for (ValueID id : initialStoresOf(callee)) {
            auto& store = callee.at(id);
            auto field = layout.fieldNamed(store.string());
            if (store.child(0) != thisValue || store.child(1) == thisValue || !field)
                return false;
            if ((m_program.prototypePropertyNames.get(store.string().index()) && layout.kind != LayoutKind::Record) || inheritsThroughLayoutPrototype(m_program, created, store.string()))
                return false;
            TypeID type = layout.fields[*field].type;
            if (type && !m_program.typeOf(callee, store.child(1)).isSubsetOf(m_program.at(type), m_program))
                return false;
        }
        return true;
    }

    bool isAccessor(FunctionID id) const
    {
        if (auto* facts = factsOf(id))
            return facts->isAccessor;
        return m_isAccessor.ensure(id.index(), [&] {
            bool reads = false;
            auto size = sizeAsAccessor(m_program.at(id), reads, 0);
            return size && *size <= (reads ? maxAccessor : maxPredicate);
        }).iterator->value;
    }

    std::optional<unsigned> sizeAsAccessor(const Function& function, bool& reads, unsigned nesting) const
    {
        unsigned size = 0;
        BitVector isInIntegerTestChain;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (runsInitializer(function, block))
                continue;
            size += collectIntegerTestChain(function, b, isInIntegerTestChain);
            bool isInChain = isInIntegerTestChain.get(b);
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (isInChain && id == block.terminator.value)
                    continue;
                if (value.opcode == Opcode::CallDirect) {
                    if (nesting == 2)
                        return std::nullopt;
                    auto calleeSize = sizeAsAccessor(m_program.at(value.function()), reads, nesting + 1);
                    if (!calleeSize)
                        return std::nullopt;
                    size += *calleeSize;
                } else if (readsMemory(value.opcode))
                    reads = true;
                else if (!isTest(value.opcode))
                    return std::nullopt;
                if (value.opcode != Opcode::CallDirect)
                    size += countsForSize(value.opcode);
            }
            if (!endsWithTest(block))
                return std::nullopt;
            size += !isInChain && (block.terminator.kind == TerminatorKind::Branch || block.terminator.kind == TerminatorKind::Switch);
            if (size > maxPredicate)
                return std::nullopt;
        }
        return size;
    }

    static bool isTest(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::Cast:
        case Opcode::ToBoolean:
        case Opcode::LogicalNot:
        case Opcode::StrictEq:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::BitNot:
        case Opcode::LShift:
        case Opcode::RShift:
        case Opcode::URShift:
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
        case Opcode::IsUndefinedOrNull:
            return true;
        default:
            return false;
        }
    }

    static bool readsMemory(Opcode opcode)
    {
        return opcode == Opcode::GetById || opcode == Opcode::GetField || opcode == Opcode::GetModuleVariable;
    }

    static bool endsWithTest(const Block& block)
    {
        switch (block.terminator.kind) {
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::Return:
        case TerminatorKind::Unreachable:
            return true;
        default:
            return false;
        }
    }

    static std::optional<unsigned> unlikelyTarget(const Function& function, const Block& block)
    {
        if (block.terminator.kind != TerminatorKind::Branch)
            return std::nullopt;
        ValueID condition = block.terminator.value;
        bool isNegated = false;
        for (;; condition = function.at(condition).child(0)) {
            Opcode opcode = function.at(condition).opcode;
            if (opcode == Opcode::LogicalNot)
                isNegated = !isNegated;
            else if (opcode != Opcode::ToBoolean && opcode != Opcode::Cast)
                break;
        }
        switch (function.at(condition).opcode) {
        case Opcode::StrictEq:
        case Opcode::IsUndefinedOrNull:
        case Opcode::BitAnd:
            return isNegated;
        case Opcode::GetById:
        case Opcode::GetField:
        case Opcode::GetModuleVariable:
        case Opcode::Parameter:
            return !isNegated;
        default:
            return std::nullopt;
        }
    }

    struct Head {
        BitVector blocks;
        unsigned size { 0 };
    };

    bool isUncheckedFieldRead(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::GetById)
            return false;
        auto& receiver = m_program.typeOf(function, value.child(0));
        LayoutID supertype = receiver.singleLayout();
        if (!supertype || !m_program.at(supertype).acceptsUntypedObjects || m_program.at(supertype).subtypes.isEmpty())
            return false;
        auto access = fieldOf(m_program, receiver, value.string());
        if (!access || !prototypeChainLacksPropertyForSubtypes(m_program, access->layout, value.string()))
            return false;
        return std::ranges::all_of(m_program.at(supertype).subtypes, [&](LayoutID subtype) {
            return !!m_program.at(subtype).fieldNamed(value.string());
        });
    }

    const Head& headOf(FunctionID id)
    {
        return m_heads.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            if (!function.resultTypes.isEmpty())
                return Head { };
            auto blockSize = [&](const Block& block) -> std::optional<unsigned> {
                if (!endsWithTest(block) || block.terminator.kind == TerminatorKind::Unreachable)
                    return std::nullopt;
                unsigned size = block.terminator.kind == TerminatorKind::Branch || block.terminator.kind == TerminatorKind::Switch;
                for (size_t i = 0; i < block.values.size(); ++i) {
                    auto& value = function.at(block.values[i]);
                    if (!isTest(value.opcode) && !readsMemory(value.opcode))
                        return std::nullopt;
                    bool isUncheckedRead = isUncheckedFieldRead(function, value);
                    if (!isUncheckedRead && (writesOf(m_program, function, block, i) > Writes::RuntimeState || (value.opcode == Opcode::Cast && castWritesShapeWords(m_program, function, value))))
                        return std::nullopt;
                    size += countsForSize(value.opcode);
                }
                return size;
            };
            Head head;
            BitVector seen;
            Vector<unsigned, 8> queue { 0u };
            for (unsigned next = 0; next < queue.size(); ++next) {
                unsigned b = queue[next];
                if (seen.get(b))
                    continue;
                seen.set(b);
                auto size = blockSize(function.blocks[b]);
                if (!size || head.size + *size > maxHead)
                    continue;
                head.blocks.set(b);
                head.size += *size;
                auto unlikely = unlikelyTarget(function, function.blocks[b]);
                auto& targets = function.blocks[b].terminator.targets;
                for (unsigned i = 0; i < targets.size(); ++i) {
                    if (i != unlikely)
                        queue.append(targets[i].block.index());
                }
            }
            bool changed;
            do {
                changed = false;
                for (unsigned b : queue) {
                    auto& terminator = function.blocks[b].terminator;
                    if (!head.blocks.get(b) || terminator.kind == TerminatorKind::Return)
                        continue;
                    if (terminator.targets.containsIf([&](auto& target) { return head.blocks.get(target.block.index()); }))
                        continue;
                    head.blocks.clear(b);
                    head.size -= *blockSize(function.blocks[b]);
                    changed = true;
                }
            } while (changed);
            bool returns = false;
            for (unsigned b : head.blocks)
                returns |= function.blocks[b].terminator.kind == TerminatorKind::Return;
            if (!returns || !head.blocks.get(0))
                return Head { };
            return head;
        }).iterator->value;
    }

    bool isOwnWorker(FunctionID callee) const { return m_program.at(m_id).worker == callee; }

    enum class Copies : bool { All, OnlyNestingLevels };
    uint32_t depthOf(const Value& call, Copies counted = Copies::All) const
    {
        if (m_program.at(call.function()).worker || isOwnWorker(call.function()))
            return 0;
        if (onlyStoresToThis(m_program.at(call.function())))
            return 0;
        if (runsOnceOrSuspends())
            return maxDepth;
        if (isAccessor(call.function()) || sizeOf(m_program.at(call.function()), maxForwarder + 1) <= maxForwarder)
            return 0;
        uint32_t depth = call.immediates[1] == Format::none ? 0 : call.immediates[1];
        uint32_t frames = 0;
        uint32_t innermostFrame = call.inlineFrame;
        uint32_t maker = call.function().index() < m_program.singleClosureCreators.size() ? m_program.singleClosureCreators[call.function().index()] : Program::hasNoClosureCreator;
        if (counted == Copies::OnlyNestingLevels && innermostFrame && maker < Program::hasMultipleClosureCreators && m_caller.inlineFrames[innermostFrame].callee.index() != maker) {
            uint32_t creatorFrame = innermostFrame;
            bool isInOwnInlinedCopy = false;
            while (creatorFrame && m_caller.inlineFrames[creatorFrame].callee.index() != maker) {
                isInOwnInlinedCopy |= m_caller.inlineFrames[creatorFrame].callee == call.function();
                creatorFrame = m_caller.inlineFrames[creatorFrame].parent;
            }
            if (!isInOwnInlinedCopy && (creatorFrame || maker == m_id.index())) {
                innermostFrame = creatorFrame;
                depth = 0;
            }
        }
        bool isInInnermostBuiltins = counted == Copies::OnlyNestingLevels;
        for (uint32_t frame = innermostFrame; frame; frame = m_caller.inlineFrames[frame].parent) {
            isInInnermostBuiltins = isInInnermostBuiltins && m_program.at(m_caller.inlineFrames[frame].callee).has(Format::FunctionIsBuiltin);
            frames += !m_program.at(m_caller.inlineFrames[frame].callee).worker && !isInInnermostBuiltins;
        }
        if (frames && m_caller.isGenericEntrypoint())
            --frames;
        return std::max(depth, frames);
    }

    template<typename NeverReturns>
    static BitVector noReturnBlocks(const Function& function, NOESCAPE const NeverReturns& neverReturns)
    {
        unsigned numberOfBlocks = function.blocks.size();
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        BitVector mayReturn(numberOfBlocks);
        Vector<unsigned> worklist;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            bool isDeadEnd = false;
            for (ValueID value : function.blocks[b].values)
                isDeadEnd |= function.at(value).opcode == Opcode::CallDirect && neverReturns(function.at(value).function());
            if (isDeadEnd)
                continue;
            auto& terminator = function.blocks[b].terminator;
            for (auto& target : terminator.targets)
                predecessors[target.block.index()].append(b);
            if (terminator.kind == TerminatorKind::Return) {
                mayReturn.set(b);
                worklist.append(b);
            }
        }
        while (!worklist.isEmpty()) {
            for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                if (!mayReturn.set(predecessor))
                    worklist.append(predecessor);
            }
        }
        BitVector result(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!mayReturn.get(b))
                result.set(b);
        }
        return result;
    }

    const BitVector& noReturnBlocksOf(FunctionID id)
    {
        if (!m_program.noReturnBlocks.isEmpty())
            return m_program.noReturnBlocks[id.index()];
        return m_noReturnBlocksCache.ensure(id.index(), [&] {
            return noReturnBlocks(m_program.at(id), [&](FunctionID callee) {
                return !m_program.at(callee).blocks.containsIf([](auto& block) {
                    return block.terminator.kind == TerminatorKind::Return;
                });
            });
        }).iterator->value;
    }

    struct Copy {
        FunctionID function;
        uint32_t outer;
    };

    static std::optional<std::pair<ValueID, int64_t>> trailingIntegerTest(const Function& function, const Block& block)
    {
        if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.values.last() != block.terminator.value)
            return std::nullopt;
        auto& test = function.at(block.terminator.value);
        if (test.opcode != Opcode::StrictEq)
            return std::nullopt;
        for (unsigned i = 0; i < 2; ++i) {
            if (function.at(test.child(i)).opcode == Opcode::Integer && function.at(test.child(1 - i)).opcode != Opcode::Integer)
                return std::pair { test.child(1 - i), function.at(test.child(i)).integer() };
        }
        return std::nullopt;
    }

    static unsigned collectIntegerTestChain(const Function& function, unsigned first, BitVector& isInChain)
    {
        static constexpr unsigned minTests = 4;
        static constexpr unsigned maxTargets = 3;
        static constexpr int64_t valuesInWindow = 64;
        static constexpr unsigned instructionsInUnit = 3;
        if (isInChain.get(first))
            return 0;
        auto tested = trailingIntegerTest(function, function.blocks[first]);
        if (!tested)
            return 0;
        auto placeOf = [&](const Target& target) {
            const Target* place = &target;
            for (unsigned steps = 0; steps < valuesInWindow && place->arguments.isEmpty(); ++steps) {
                auto& block = function.at(place->block);
                if (!block.values.isEmpty() || !block.parameters.isEmpty() || block.terminator.kind != TerminatorKind::Jump)
                    break;
                place = &block.terminator.targets[0];
            }
            return place;
        };
        auto isSameConstant = [&](ValueID a, ValueID b) {
            if (a == b)
                return true;
            auto& one = function.at(a);
            auto& other = function.at(b);
            return one.opcode == other.opcode && (one.opcode == Opcode::Boolean || one.opcode == Opcode::Integer) && one.immediates == other.immediates;
        };
        auto isSameTarget = [&](const Target* a, const Target* b) {
            if (a->block != b->block || a->arguments.size() != b->arguments.size())
                return false;
            for (unsigned i = 0; i < a->arguments.size(); ++i) {
                if (!isSameConstant(a->arguments[i], b->arguments[i]))
                    return false;
            }
            return true;
        };
        Vector<unsigned, 32> links;
        Vector<int64_t, 32> integers;
        Vector<const Target*, maxTargets> places;
        for (unsigned at = first; !isInChain.get(at) && !links.contains(at);) {
            auto& block = function.blocks[at];
            auto test = trailingIntegerTest(function, block);
            if (!test || test->first != tested->first)
                break;
            bool isCheckOnlyCast = at == first || std::ranges::all_of(block.values, [&](ValueID id) {
                return id == block.terminator.value || function.at(id).opcode == Opcode::Integer;
            });
            if (!isCheckOnlyCast || (at != first && !block.parameters.isEmpty()))
                break;
            const Target* place = placeOf(block.terminator.targets[0]);
            if (!places.containsIf([&](const Target* known) { return isSameTarget(known, place); })) {
                if (places.size() == maxTargets)
                    break;
                places.append(place);
            }
            links.append(at);
            integers.append(test->second);
            if (!block.terminator.targets[1].arguments.isEmpty())
                break;
            at = block.terminator.targets[1].block.index();
        }
        if (links.size() < minTests)
            return 0;
        std::ranges::sort(integers);
        unsigned windows = 0;
        for (size_t i = 0; i < integers.size(); ++windows) {
            int64_t start = integers[i];
            while (i < integers.size() && integers[i] - start < valuesInWindow)
                ++i;
        }
        unsigned instructions = 2 * (windows - 1) + windows * (3 + 3 * places.size());
        unsigned size = (instructions + instructionsInUnit - 1) / instructionsInUnit;
        if (size >= 2 * links.size())
            return 0;
        for (unsigned link : links)
            isInChain.set(link);
        return size;
    }

    static bool countsForSize(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::Cast:
        case Opcode::LoopHint:
        case Opcode::AssertNotAllocated:
        case Opcode::ToBoolean:
        case Opcode::LogicalNot:
            return false;
        default:
            return true;
        }
    }

    bool runsInitializer(const Function& function, const Block& block) const
    {
        return block.values.containsIf([&](ValueID id) {
            return function.at(id).opcode == Opcode::CallDirect && m_program.at(function.at(id).function()).kind == FunctionKind::Module;
        });
    }

    bool guardsInitializer(const Function& function, const Block& block) const
    {
        return block.terminator.kind == TerminatorKind::Branch && (runsInitializer(function, function.at(block.terminator.targets[0].block)) || runsInitializer(function, function.at(block.terminator.targets[1].block)));
    }

    unsigned sizeOf(const Function& function, unsigned limit) const
    {
        if (!m_program.sizesForInlining.isEmpty() && &function >= m_program.functions.begin() && &function < m_program.functions.end())
            return std::min(m_program.sizesForInlining[&function - m_program.functions.begin()], std::max(limit, 1u));
        unsigned size = 0;
        BitVector isInIntegerTestChain;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (runsInitializer(function, block))
                continue;
            size += collectIntegerTestChain(function, b, isInIntegerTestChain);
            if (size >= limit)
                return size;
            bool isInChain = isInIntegerTestChain.get(b);
            ValueID tested = isInChain || guardsInitializer(function, block) ? block.terminator.value : ValueID();
            for (ValueID id : block.values) {
                if (countsForSize(function.at(id).opcode) && id != tested && ++size >= limit)
                    return size;
            }
            if (!isInChain && (block.terminator.kind == TerminatorKind::Branch || block.terminator.kind == TerminatorKind::Switch) && ++size >= limit)
                return size;
        }
        return size;
    }

    struct CallSiteConstants {
        const Inlining& inlining;
        const Function& callee;
        const Value& call;
        StringID length;
        Vector<std::unique_ptr<Value>, 2> lengths { };
        Vector<ValueID> onlyPassed { };

        void findTrivialParameters()
        {
            onlyPassed.fill(ValueID(), callee.values.size());
            auto resolve = [&](ValueID value) {
                while (onlyPassed[value.index()])
                    value = onlyPassed[value.index()];
                return value;
            };
            Vector<Vector<Vector<ValueID, 2>>> given;
            for (auto& block : callee.blocks)
                given.append(Vector<Vector<ValueID, 2>>(block.parameters.size()));
            for (auto& block : callee.blocks) {
                for (auto& target : block.terminator.targets) {
                    for (unsigned i = 0; i < target.arguments.size(); ++i)
                        given[target.block.index()][i].append(target.arguments[i]);
                }
            }
            bool changed;
            do {
                changed = false;
                for (unsigned b = 1; b < callee.blocks.size(); ++b) {
                    auto& parameters = callee.blocks[b].parameters;
                    for (unsigned i = 0; i < parameters.size(); ++i) {
                        ValueID parameter = parameters[i];
                        if (onlyPassed[parameter.index()])
                            continue;
                        ValueID only;
                        bool isTrivial = true;
                        for (ValueID argument : given[b][i]) {
                            argument = resolve(argument);
                            if (argument == parameter || argument == only)
                                continue;
                            if (only) {
                                isTrivial = false;
                                break;
                            }
                            only = argument;
                        }
                        if (isTrivial && only) {
                            onlyPassed[parameter.index()] = only;
                            changed = true;
                        }
                    }
                }
            } while (changed);
        }

        ValueID definitionOf(ValueID id, bool throughChecks)
        {
            for (;;) {
                auto& value = callee.at(id);
                if (value.opcode == Opcode::Cast && (throughChecks ? value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce : value.castReason() == CastReason::Upcast)) {
                    id = value.child(0);
                    continue;
                }
                if (value.opcode != Opcode::Parameter || callee.blocks[0].parameters.contains(id))
                    return id;
                if (onlyPassed.isEmpty())
                    findTrivialParameters();
                if (!onlyPassed[id.index()])
                    return id;
                id = onlyPassed[id.index()];
            }
        }

        const Value* argumentFor(ValueID id)
        {
            id = definitionOf(id, true);
            if (callee.at(id).opcode != Opcode::Parameter)
                return nullptr;
            size_t index = callee.blocks[0].parameters.find(id);
            if (index == notFound || index >= call.arguments.size())
                return nullptr;
            auto& caller = inlining.m_caller;
            ValueID argument = call.arguments[index];
            while (caller.at(argument).opcode == Opcode::Cast && caller.at(argument).castReason() == CastReason::Upcast)
                argument = caller.at(argument).child(0);
            return &caller.at(argument);
        }

        static bool isConstant(const Value& value)
        {
            switch (value.opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::Double:
            case Opcode::String:
                return true;
            default:
                return false;
            }
        }

        const Value* constantFor(ValueID id)
        {
            id = definitionOf(id, false);
            auto& value = callee.at(id);
            if (isConstant(value))
                return &value;
            if (value.opcode == Opcode::GetById && value.string() == length) {
                auto* array = argumentFor(value.child(0));
                if (!array || array->opcode != Opcode::NewArray)
                    return nullptr;
                lengths.append(makeUniqueWithoutFastMallocCheck<Value>());
                lengths.last()->opcode = Opcode::Integer;
                lengths.last()->setBits(array->arguments.size());
                return lengths.last().get();
            }
            if (value.opcode == Opcode::GetById || value.opcode == Opcode::InById) {
                auto* object = argumentFor(value.child(0));
                if (!object || object->opcode != Opcode::NewObjectWithShape || !isObjectLayout(inlining.m_program.at(object->layout()).kind) || mayInherit(inlining.m_program, object->layout(), value.string()))
                    return nullptr;
                bool hasField = !!inlining.m_program.at(object->layout()).fieldNamed(value.string());
                if (value.opcode == Opcode::GetById && hasField)
                    return nullptr;
                lengths.append(makeUniqueWithoutFastMallocCheck<Value>());
                if (value.opcode == Opcode::InById) {
                    lengths.last()->opcode = Opcode::Boolean;
                    lengths.last()->immediates[0] = hasField;
                }
                return lengths.last().get();
            }
            auto* argument = argumentFor(id);
            return argument && isConstant(*argument) ? argument : nullptr;
        }

        bool folds(ValueID id)
        {
            auto& value = callee.at(id);
            switch (value.opcode) {
            case Opcode::StrictEq:
            case Opcode::Eq:
            case Opcode::IsUndefinedOrNull:
                return !!decide(id);
            case Opcode::GetById:
            case Opcode::InById:
                return constantFor(id);
            case Opcode::GetByVal: {
                auto* array = argumentFor(value.child(0));
                auto* key = array && array->opcode == Opcode::NewArray ? constantFor(value.child(1)) : nullptr;
                return key && key->opcode == Opcode::Integer;
            }
            default:
                return false;
            }
        }

        std::optional<bool> decide(ValueID id)
        {
            auto& value = callee.at(id);
            switch (value.opcode) {
            case Opcode::StrictEq:
            case Opcode::Eq: {
                auto* a = constantFor(value.child(0));
                auto* b = a ? constantFor(value.child(1)) : nullptr;
                return b ? areEqualConstants(inlining.m_program, *a, *b, value.opcode == Opcode::StrictEq) : std::nullopt;
            }
            case Opcode::LogicalNot:
                if (auto operand = decide(value.child(0)))
                    return !*operand;
                return std::nullopt;
            case Opcode::IsUndefinedOrNull:
                if (auto* operand = constantFor(value.child(0)))
                    return operand->opcode == Opcode::Undefined || operand->opcode == Opcode::Null;
                return std::nullopt;
            default:
                break;
            }
            ValueID operand = value.opcode == Opcode::ToBoolean ? value.child(0) : id;
            auto* constant = constantFor(operand);
            if (!constant)
                return value.opcode == Opcode::ToBoolean ? decide(operand) : std::nullopt;
            switch (constant->opcode) {
            case Opcode::Boolean:
                return constant->boolean();
            case Opcode::Integer:
                return !!constant->integer();
            case Opcode::Undefined:
            case Opcode::Null:
                return false;
            default:
                return std::nullopt;
            }
        }
    };

    unsigned sizeAtCall(const Function& callee, const Value& call, unsigned limit) const
    {
        if (call.arguments.size() != callee.blocks[0].parameters.size())
            return sizeOf(callee, limit);
        CallSiteConstants constants { *this, callee, call, m_program.find(asBytes("length"_s.span8())).value_or(StringID()) };
        unsigned size = 0;
        BitVector seen(callee.blocks.size());
        seen.set(0);
        Vector<unsigned, 16> worklist { 0u };
        BitVector isInIntegerTestChain;
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            auto& block = callee.blocks[b];
            bool isCounted = !runsInitializer(callee, block);
            if (isCounted && !(block.terminator.kind == TerminatorKind::Branch && constants.decide(block.terminator.value))) {
                size += collectIntegerTestChain(callee, b, isInIntegerTestChain);
                if (size >= limit)
                    return size;
            }
            bool isInChain = isInIntegerTestChain.get(b);
            ValueID tested = isInChain || guardsInitializer(callee, block) ? block.terminator.value : ValueID();
            for (ValueID id : block.values) {
                if (isCounted && countsForSize(callee.at(id).opcode) && id != tested && !constants.folds(id) && ++size >= limit)
                    return size;
            }
            auto& terminator = block.terminator;
            auto follow = [&](const Target& target) {
                if (!seen.set(target.block.index()))
                    worklist.append(target.block.index());
            };
            if (terminator.kind == TerminatorKind::Branch) {
                if (auto taken = constants.decide(terminator.value)) {
                    follow(terminator.targets[*taken ? 0 : 1]);
                    continue;
                }
            }
            if (isCounted && !isInChain && (terminator.kind == TerminatorKind::Branch || terminator.kind == TerminatorKind::Switch) && ++size >= limit)
                return size;
            for (auto& target : terminator.targets)
                follow(target);
        }
        return size;
    }

    bool closesCycle(FunctionID callee, BlockID block) const
    {
        if (callee == m_id)
            return true;
        for (uint32_t at = m_inlinedFrom[block.index()]; at != none; at = m_inlinedCopies[at].outer) {
            if (m_inlinedCopies[at].function == callee)
                return true;
        }
        return false;
    }

    TypeID resultTypeOf(const Function& callee) const { return callee.resultType ? callee.resultType : m_program.types.anything; }

    static bool hasHandlers(const Function& function)
    {
        return function.blocks.containsIf([](auto& block) {
            return block.terminator.kind == TerminatorKind::Invoke || (block.terminator.kind == TerminatorKind::Throw && !block.terminator.targets.isEmpty());
        });
    }

    bool containsCallNoInline(FunctionID id) const
    {
        return m_containsCallNoInline.ensure(id.index(), [&] {
            for (auto& value : m_program.at(id).values) {
                if (m_program.operationOf(value) == Operations::CallNoInline)
                    return true;
            }
            return false;
        }).iterator->value;
    }
    mutable UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_containsCallNoInline;

    bool canInline(const Function& callee, const Value& call) const
    {
        if (callee.blocks.isEmpty() || callee.kind == FunctionKind::Module || callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer | FunctionLogsTypeMismatches))
            return false;
        if (m_program.logsTypeErrors && callee.has(Format::FunctionIsHiddenInStackTraces) && !callee.has(Format::FunctionIsBuiltin) && hasHandlers(callee))
            return false;
        if (m_caller.inlineFrames.size() > maxInlineFrames || callee.readsHostObjectMember || callee.readsAsyncLocalStorage)
            return false;
        if (callee.savesFrameOnSuspend && m_program.at(m_id).startFunction != call.function())
            return false;
        if (m_caller.has(Format::FunctionIsGenerator) && (callee.resumeFunction || m_program.isResumeFunction(callee)))
            return false;
        if (callee.has(Format::FunctionIsBuiltin) && !m_program.implementsBuiltin(callee) && containsCallNoInline(call.function()))
            return false;
        if (call.function() == m_program.preludeFunctions[static_cast<unsigned>(PreludeFunction::ArrayMapJoin)])
            return false;
        if (!m_isInliningOwnBody && !isOwnWorker(call.function()) && m_program.at(m_id).startFunction != call.function() && hasHandlers(callee) && !hasHandlers(m_caller) && !isPassedIteratedGenerator(callee, call))
            return false;
        if (call.arguments.size() != callee.blocks[0].parameters.size())
            return false;
        auto& result = m_program.at(resultTypeOf(callee));
        if (!result.isSubsetOf(m_program.at(call.type), m_program))
            return false;
        bool returnsUndefined = m_program.at(m_program.types.undefined).isSubsetOf(result, m_program);
        for (auto& block : callee.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && !block.terminator.value && !returnsUndefined)
                return false;
        }
        return true;
    }

    BlockID addBlock(uint32_t inlinedFrom)
    {
        m_caller.blocks.append(Block { });
        m_inlinedFrom.append(inlinedFrom);
        if (m_currentBlockIsInLoop)
            m_isInLoop.set(m_caller.blocks.size() - 1);
        if (m_currentBlockIsInCallerLoop)
            m_isInCallerLoop.set(m_caller.blocks.size() - 1);
        return BlockID(m_caller.blocks.size() - 1);
    }

    ValueID addValue(Value&& value)
    {
        m_caller.values.append(WTF::move(value));
        return ValueID(m_caller.values.size() - 1);
    }

    void copyIn(BlockID block, unsigned k, FunctionID calleeID, const BitVector* head = nullptr)
    {
        auto& callee = m_program.at(calleeID);
        ValueID call = m_caller.at(block).values[k];
        Value restCall(m_caller.at(call));
        Arguments arguments = m_caller.at(call).arguments;
        uint32_t position = m_caller.at(call).position;
        uint32_t depth = m_isInliningOwnBody ? 0 : callee.worker ? (m_caller.at(call).immediates[1] == Format::none ? 0 : m_caller.at(call).immediates[1]) : depthOf(m_caller.at(call), Copies::OnlyNestingLevels) + !callee.has(Format::FunctionIsBuiltin);
        uint32_t outerFrame = m_caller.at(call).inlineFrame;
        if (m_caller.inlineFrames.isEmpty())
            m_caller.inlineFrames.append(InlineFrame { });
        uint32_t inlineFrame = m_caller.inlineFrames.size();
        m_caller.inlineFrames.append(InlineFrame { calleeID, position, outerFrame });
        m_caller.inlineFrames.last().isTailCall = m_caller.at(call).isInTailPosition;
        m_caller.inlineFrames.last().isConstruction = m_caller.at(call).constructs;
        m_lastInlineFrame = inlineFrame;
        for (unsigned i = 1; i < callee.inlineFrames.size(); ++i) {
            InlineFrame frame = callee.inlineFrames[i];
            frame.parent += inlineFrame;
            m_caller.inlineFrames.append(frame);
        }
        bool isWatched = m_caller.at(block).terminator.kind == TerminatorKind::Invoke && k + 1 == m_caller.at(block).values.size();

        BlockID after = addBlock(m_inlinedFrom[block.index()]);
        {
            auto& from = m_caller.at(block);
            m_caller.at(after).values.append(from.values.subspan(k + 1));
            from.values.shrink(k);
            m_caller.at(after).terminator = std::exchange(from.terminator, Terminator { });
        }
        Target handler;
        if (isWatched) {
            auto& invoke = m_caller.at(after).terminator;
            handler = invoke.targets[1];
            invoke.kind = TerminatorKind::Jump;
            invoke.targets.shrink(1);
        }
        {
            Value result;
            result.opcode = Opcode::Parameter;
            result.position = position;
            result.inlineFrame = outerFrame;
            result.type = resultTypeOf(callee);
            result.copyPredictionFrom(m_caller.at(call));
            result.rep = repOf(m_program.at(result.type));
            result.immediates = { after.index(), 0 };
            if (!callee.resultTypes.isEmpty()) {
                Arguments returnedValue;
                for (unsigned i = 0; i < callee.resultTypes.size(); ++i) {
                    Value parameter(result);
                    parameter.copyPredictionFrom(Value { });
                    parameter.type = callee.resultTypes[i];
                    parameter.rep = repOf(m_program.at(parameter.type));
                    parameter.immediates = { after.index(), i };
                    returnedValue.append(addValue(WTF::move(parameter)));
                    m_caller.at(after).parameters.append(returnedValue.last());
                }
                for (; m_resultSearchValueCount < m_caller.values.size(); ++m_resultSearchValueCount) {
                    auto& user = m_caller.values[m_resultSearchValueCount];
                    if (user.opcode == Opcode::ResultAt)
                        m_resultReads.add(user.child(0).index(), Vector<uint32_t, 2> { }).iterator->value.append(m_resultSearchValueCount);
                }
                for (uint32_t index : m_resultReads.take(call.index())) {
                    auto& user = m_caller.values[index];
                    if (user.opcode != Opcode::ResultAt || user.child(0) != call)
                        continue;
                    Value upcast;
                    upcast.opcode = Opcode::Cast;
                    upcast.position = user.position;
                    upcast.inlineFrame = user.inlineFrame;
                    upcast.type = user.type;
                    upcast.rep = user.rep;
                    upcast.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
                    upcast.children[0] = returnedValue[user.resultIndex()];
                    user = WTF::move(upcast);
                }
                Value nothing;
                nothing.position = position;
                nothing.inlineFrame = outerFrame;
                nothing.type = m_program.types.undefined;
                nothing.rep = repOf(m_program.at(nothing.type));
                m_caller.at(call) = WTF::move(nothing);
                m_caller.at(after).values.insert(0, call);
            } else if (result.type == m_caller.at(call).type) {
                m_caller.at(call) = WTF::move(result);
                m_caller.at(after).parameters.append(call);
            } else {
                Value upcast;
                upcast.opcode = Opcode::Cast;
                upcast.position = position;
                upcast.inlineFrame = outerFrame;
                upcast.type = m_caller.at(call).type;
                upcast.copyPredictionFrom(m_caller.at(call));
                upcast.rep = m_caller.at(call).rep;
                upcast.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
                upcast.children[0] = addValue(WTF::move(result));
                m_caller.at(after).parameters.append(upcast.children[0]);
                m_caller.at(call) = WTF::move(upcast);
                m_caller.at(after).values.insert(0, call);
            }
        }

        m_inlinedCopies.append(Copy { calleeID, m_inlinedFrom[block.index()] });
        uint32_t inlinedFrom = m_inlinedCopies.size() - 1;
        unsigned first = m_caller.blocks.size();
        auto& calleeNoReturnBlocks = noReturnBlocksOf(calleeID);
        auto isInlinedBlock = [&](unsigned i) {
            return !head || head->get(i);
        };
        Vector<BlockID, 16> madeFor(callee.blocks.size());
        for (unsigned i = 0; i < callee.blocks.size(); ++i) {
            if (!isInlinedBlock(i))
                continue;
            madeFor[i] = addBlock(inlinedFrom);
            if (calleeNoReturnBlocks.get(i))
                m_neverReturns.set(madeFor[i].index());
        }
        BlockID rest;
        if (head) {
            rest = addBlock(inlinedFrom);
            restCall.type = resultTypeOf(callee);
            restCall.rep = repOf(m_program.at(restCall.type));
            restCall.immediates[1] = afterHead;
            ValueID result = addValue(WTF::move(restCall));
            m_caller.at(rest).values.append(result);
            m_caller.at(rest).terminator.kind = TerminatorKind::Jump;
            m_caller.at(rest).terminator.targets.append(Target { after, Arguments { result } });
        }
        Vector<ValueID> map(callee.values.size());
        for (unsigned i = 0; i < arguments.size(); ++i) {
            ValueID parameter = callee.blocks[0].parameters[i];
            ValueID argument = arguments[i];
            TypeID type = callee.at(parameter).type;
            if (type != m_caller.at(argument).type || callee.at(parameter).hasDifferentPredictionThan(m_caller.at(argument))) {
                Value upcast;
                upcast.opcode = Opcode::Cast;
                upcast.position = position;
                upcast.inlineFrame = outerFrame;
                upcast.type = type;
                upcast.copyPredictionFrom(callee.at(parameter));
                upcast.rep = callee.at(parameter).rep;
                upcast.children[0] = argument;
                upcast.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
                argument = addValue(WTF::move(upcast));
                m_caller.at(block).values.append(argument);
            }
            map[parameter.index()] = argument;
        }
        auto copy = [&](ValueID value) {
            Value copied(callee.at(value));
            copied.inlineFrame += inlineFrame;
            map[value.index()] = addValue(WTF::move(copied));
        };
        for (unsigned i = 0; i < callee.blocks.size(); ++i) {
            if (!isInlinedBlock(i))
                continue;
            if (i) {
                for (ValueID parameter : callee.blocks[i].parameters)
                    copy(parameter);
            }
            for (ValueID value : callee.blocks[i].values)
                copy(value);
        }
        auto mapped = [&](ValueID& value) {
            value = map[value.index()];
        };
        for (unsigned i = 0; i < callee.blocks.size(); ++i) {
            if (!isInlinedBlock(i))
                continue;
            BlockID created = madeFor[i];
            if (i) {
                for (ValueID parameter : callee.blocks[i].parameters) {
                    m_caller.at(map[parameter.index()]).immediates[0] = created.index();
                    m_caller.at(created).parameters.append(map[parameter.index()]);
                }
            }
            for (ValueID value : callee.blocks[i].values) {
                auto& copied = m_caller.at(map[value.index()]);
                if (copied.opcode == Opcode::Results) {
                    copied = Value { };
                    copied.type = m_program.types.undefined;
                    copied.rep = repOf(m_program.at(copied.type));
                    continue;
                }
                copied.forEachChild(mapped);
                if (head && isUncheckedFieldRead(callee, callee.at(value))) {
                    Value test;
                    test.opcode = Opcode::HasShapeOf;
                    test.position = copied.position;
                    test.inlineFrame = copied.inlineFrame;
                    test.type = m_program.types.boolean;
                    test.rep = repOf(m_program.at(test.type));
                    test.children[0] = copied.child(0);
                    test.immediates = { m_program.typeOf(callee, callee.at(value).child(0)).singleLayout().index(), m_program.mayRunInMultipleInstances ? shapeTestIsForThisInstance : 0u };
                    ValueID isSubtype = addValue(WTF::move(test));
                    BlockID afterTest = addBlock(inlinedFrom);
                    if (calleeNoReturnBlocks.get(i))
                        m_neverReturns.set(afterTest.index());
                    m_caller.at(created).values.append(isSubtype);
                    auto& branch = m_caller.at(created).terminator;
                    branch.kind = TerminatorKind::Branch;
                    branch.value = isSubtype;
                    branch.targets.append(Target { afterTest, { } });
                    branch.targets.append(Target { rest, { } });
                    created = afterTest;
                    m_caller.at(created).values.append(map[value.index()]);
                    auto access = fieldOf(m_program, m_program.typeOf(callee, callee.at(value).child(0)), callee.at(value).string());
                    auto& read = m_caller.at(map[value.index()]);
                    if (TypeID declared = m_program.at(access->layout).fields[access->field].type; declared && declared != read.type && m_program.at(declared).isSubsetOf(m_program.at(read.type), m_program) && subtypesStoreOnlyDeclaredType(m_program, access->layout, access->field)) {
                        read.type = declared;
                        read.rep = repOf(m_program.at(declared));
                        read.copyPredictionFrom(Value { });
                    }
                    continue;
                }
                if (copied.opcode == Opcode::NewObjectWithShape)
                    copied.immediates[1] = m_caller.allocationOf(Allocation(callee.allocations[copied.allocation()]));
                else if (copied.opcode == Opcode::CallDirect || copied.opcode == Opcode::CallMethod || copied.opcode == Opcode::Call || copied.opcode == Opcode::Operation)
                    copied.immediates[1] = depth;
                m_caller.at(created).values.append(map[value.index()]);
            }
            Terminator terminator = callee.blocks[i].terminator;
            if (terminator.kind == TerminatorKind::Return) {
                ValueID result;
                if (terminator.value)
                    result = map[terminator.value.index()];
                else {
                    Value undefined;
                    undefined.position = position;
                    undefined.inlineFrame = outerFrame;
                    undefined.type = m_program.types.undefined;
                    undefined.rep = repOf(m_program.at(undefined.type));
                    result = addValue(WTF::move(undefined));
                    m_caller.at(created).values.append(result);
                }
                terminator = Terminator { };
                terminator.kind = TerminatorKind::Jump;
                terminator.targets.append(Target { after, Arguments { result } });
                if (!callee.resultTypes.isEmpty()) {
                    auto& arguments = terminator.targets[0].arguments;
                    arguments.clear();
                    for (ValueID returnedValue : callee.at(callee.blocks[i].terminator.value).arguments)
                        arguments.append(map[returnedValue.index()]);
                }
            } else {
                for (auto& target : terminator.targets) {
                    if (isInlinedBlock(target.block.index()))
                        target.block = madeFor[target.block.index()];
                    else
                        target = Target { rest, { } };
                }
                terminator.forEachChild(mapped);
                if (terminator.kind == TerminatorKind::Throw && !terminator.cases.isEmpty()) {
                    auto frameInCallee = terminator.cases.size() > 1 ? terminator.cases[1] : 0;
                    terminator.cases.shrink(1);
                    terminator.cases.append(inlineFrame + frameInCallee);
                }
            }
            m_caller.at(created).terminator = WTF::move(terminator);
        }
        auto& jump = m_caller.at(block).terminator;
        jump.kind = TerminatorKind::Jump;
        jump.targets.append(Target { BlockID(first), { } });
        if (isWatched)
            watch(first, m_caller.blocks.size(), inlinedFrom, handler);
    }

    void watch(unsigned begin, unsigned end, uint32_t inlinedFrom, const Target& handler)
    {
        for (unsigned b = begin; b < end; ++b) {
            BlockID block(b);
            if (m_caller.at(block).terminator.kind == TerminatorKind::Throw && m_caller.at(block).terminator.targets.isEmpty())
                m_caller.at(block).terminator.targets.append(handler);
            for (unsigned i = 0; i < m_caller.at(block).values.size(); ++i) {
                if (i + 1 == m_caller.at(block).values.size() && m_caller.at(block).terminator.kind == TerminatorKind::Invoke)
                    break;
                auto& value = m_caller.at(m_caller.at(block).values[i]);
                if (!mayThrow(m_program, m_caller, value) || isInitializingStore(m_program, m_caller, m_caller.at(block), i) || (value.opcode == Opcode::PutById && value.isInitialStore()))
                    continue;
                BlockID rest = addBlock(inlinedFrom);
                auto& from = m_caller.at(block);
                m_caller.at(rest).values.append(from.values.subspan(i + 1));
                from.values.shrink(i + 1);
                m_caller.at(rest).terminator = std::exchange(from.terminator, Terminator { });
                from.terminator.kind = TerminatorKind::Invoke;
                from.terminator.targets.append(Target { rest, { } });
                from.terminator.targets.append(handler);
                block = rest;
                i = UINT_MAX;
            }
        }
    }

    const Program& m_program;
    FunctionID m_id;
    Function& m_caller;
    Vector<Copy> m_inlinedCopies;
    Vector<uint32_t> m_inlinedFrom;
    BitVector m_neverReturns;
    BitVector m_isInLoop;
    BitVector m_movedFrames;
    uint32_t m_lastInlineFrame { 0 };
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_calledParameters;
    bool m_currentBlockIsInLoop { false };
    BitVector m_isInCallerLoop;
    bool m_currentBlockIsInCallerLoop { false };
    bool m_isInliningOwnBody { false };
    unsigned m_queriesSinceCallerChanged { 0 };
    Vector<uint32_t> m_firstUser;
    Vector<ValueID> m_users;
    BitVector m_isUsedByTerminator;
    unsigned m_resultSearchValueCount { 0 };
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_resultReads;
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_noReturnBlocksCache;
    UncheckedKeyHashMap<uint32_t, Head, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_heads;
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_parametersCalledInLoops;
    mutable UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_isAccessor;
    mutable UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_returnsOwnAllocation;
};

} // anonymous namespace

unsigned inlineCalls(const Program& program, FunctionID id, Function& function, CleanUpTiming timing)
{
    if (program.isEntry(id))
        return 0;
    if (function.isGenericEntrypoint() && function.blocks.size() == 1 && function.blocks[0].values.containsIf([&](ValueID value) { return function.at(value).opcode == Opcode::CreateRest; })) {
        Function withBody = function;
        bool allocatesArrayOnEntry = !Inlining(program, id, withBody).inlineOwnBody() || withBody.blocks[0].values.containsIf([&](ValueID value) {
            return withBody.at(value).opcode == Opcode::CreateRest;
        });
        if (!allocatesArrayOnEntry) {
            function = WTF::move(withBody);
            return 1 + Inlining(program, id, function).run(timing);
        }
    }
    return Inlining(program, id, function).run(timing);
}

void prepareForInlining(Program& program)
{
    Inlining::prepare(program);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
