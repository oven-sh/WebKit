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

// Puts the body of a function in the place of a call of it. What is copied is the body as the program has it, whatever has become of
// that function's own copy by now: so the outcome does not depend on which function is worked on first. The calls that come along are
// looked at in turn, down to a depth. A call that closes a cycle stays a call.
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
        Vector<Program::FactsForInlining> facts(count);
        forEachIndexInParallel(count, [&](unsigned i) {
            Function none;
            Inlining inlining(program, FunctionID(i), none);
            sizes[i] = inlining.sizeOf(std::as_const(program).functions[i], UINT_MAX);
            neverReturn[i] = inlining.neverReturnsIn(FunctionID(i));
            if (!std::as_const(program).functions[i].blocks.isEmpty())
                facts[i] = { inlining.isAccessor(FunctionID(i)), inlining.returnsMethodTable(FunctionID(i)), inlining.parametersCalledBy(FunctionID(i)), inlining.parametersCalledInLoopsBy(FunctionID(i)) };
        });
        program.factsForInlining = WTF::move(facts);
        program.sizesForInlining = WTF::move(sizes);
        program.blocksThatNeverReturn = WTF::move(neverReturn);
    }

    unsigned run(CleanUpTiming timing)
    {
        bool hasCallToLookAt = m_caller.blocks.containsIf([&](auto& block) {
            return block.values.containsIf([&](ValueID id) {
                auto& value = m_caller.at(id);
                return value.opcode == Opcode::CallDirect && value.immediates[1] != behindItsHead && !value.isCallWhoseCopyWasDeclined();
            });
        });
        if (!hasCallToLookAt)
            return 0;
        unsigned replaced = 0;
        unsigned size = sizeOf(m_caller, UINT_MAX);
        m_copiedFrom.fill(none, m_caller.blocks.size());
        m_neverReturns = blocksThatNeverReturn(m_caller, [&](FunctionID callee) {
            return neverReturnsIn(callee).get(0);
        });
        if (m_caller.kind == FunctionKind::Module || m_program.isResumeFunction(m_caller) || m_program.isHotOnlyInItsLoops(m_id))
            m_isInLoop = blocksInLoops(m_caller);
        m_isInLoopOfCaller = blocksInLoops(m_caller);
        BitVector takesMisfits;
        if ((m_caller.has(Format::FunctionIsHiddenInStackTraces) && !m_caller.isGenericEntrypoint()) || m_caller.has(FunctionLogsMisfits)) {
            for (auto& block : m_caller.blocks) {
                if (block.terminator.kind == TerminatorKind::Invoke)
                    takesMisfits.set(block.terminator.targets[1].block.index());
            }
        }
        Vector<BlockID> worklist;
        for (unsigned b = m_caller.blocks.size(); b--;)
            worklist.append(BlockID(b));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (m_neverReturns.get(block.index()) || takesMisfits.get(block.index()))
                continue;
            m_currentBlockIsInLoop = m_isInLoop.get(block.index());
            m_currentBlockIsInLoopOfCaller = m_isInLoopOfCaller.get(block.index());
            for (unsigned k = 0; k < m_caller.at(block).values.size(); ++k) {
                ValueID call = m_caller.at(block).values[k];
                if (m_caller.at(call).opcode != Opcode::CallDirect || m_caller.at(call).immediates[1] == behindItsHead || m_caller.at(call).isCallWhoseCopyWasDeclined())
                    continue;
                FunctionID callee = m_caller.at(call).function();
                // One level more where the copy makes a call of a value a call of a function.
                bool isTooDeep = depthOf(m_caller.at(call)) >= maxDepth + (isHandedFunctionItCalls(callee, m_caller.at(call)) || isHandedGeneratorItIterates(m_program.at(callee), m_caller.at(call)));
                // The call is one of the worker from here on: it is looked at again.
                if (m_program.at(callee).nilOfResults && takeApartResultOrNil(m_program, m_caller, call)) {
                    didChangeCaller();
                    m_numberOfValuesSearchedForResults = 0;
                    m_readsOfResults.clear();
                    ++replaced;
                    --k;
                    continue;
                }
                if (m_program.at(callee).resumeFunction || (m_caller.isGenericEntrypoint() && m_program.genericEntrypoints.get(callee.index()) == m_id) || m_caller.functionOfValueEntry == callee)
                    continue;
                if (m_caller.resumeFunction == callee && !(m_caller.declaredAs & functionIsGenerator))
                    continue;
                if (closesCycle(callee, block)) {
                    m_caller.at(call).immediates[1] = maxDepth;
                    continue;
                }
                if (!mayBeCopied(m_program.at(callee), m_caller.at(call)) || consumesFactoryResult(callee))
                    continue;
                bool callerIsHot = m_program.isHotByText(m_id) && (m_currentBlockIsInLoop || !m_program.isHotOnlyInItsLoops(m_id));
                if (m_program.at(callee).worker && !isMerelyRead(call))
                    continue;
                if (isOwnWorker(callee))
                    continue;
                // Its first stores are lowered together, in one block. What is copied in the place of a call that a handler watches is split at whatever may throw.
                if (k + 1 == m_caller.at(block).values.size() && m_caller.at(block).terminator.kind == TerminatorKind::Invoke && !initialStoresOf(m_program.at(callee)).empty() && !initialStoresCannotThrowAt(block, k, m_program.at(callee)))
                    continue;
                // A wrapper is a call and a birth, where a call stood.
                bool isBuiltin = m_program.at(callee).worker || isOwnWorker(callee) || (callerIsHot && m_program.at(callee).has(Format::FunctionIsBuiltin));
                bool isMoved = !isTooDeep && isOnlyCall(callee, m_caller.at(call));
                bool takesNothingInCopies = !m_currentBlockIsInLoopOfCaller && !constructsForDerivedClass(callee, m_caller.at(call));
                unsigned limit = depthOf(m_caller.at(call)) ? (takesNothingInCopies ? 0 : smallInCopy) : smallSize;
                unsigned limitWithoutLicence = limit;
                char licence = depthOf(m_caller.at(call)) ? 'c' : 's';
                bool isBornAsCast = handsBackWhatIsCast(callee, block, k);
                bool isFactory = !isBornAsCast && !isTooDeep && returnsMethodTable(callee) && isUsedOnlyAsReceiver(m_caller, call);
                if (isBornAsCast) {
                    limit = smallWhereMoreIsGained;
                    licence = 'b';
                } else if (onlyStoresToThis(m_program.at(callee)) || isAccessor(callee) || isHandedFunctionItCalls(callee, m_caller.at(call)) || isHandedGeneratorItIterates(m_program.at(callee), m_caller.at(call))) {
                    limit = smallWhereMoreIsGained;
                    licence = isAccessor(callee) ? 'a' : onlyStoresToThis(m_program.at(callee)) ? 't' : 'h';
                } else if (m_currentBlockIsInLoopOfCaller && !depthOf(m_caller.at(call)) && m_program.numberOfDirectCalls[callee.index()] <= fewCalls) {
                    limit = smallWhereMoreIsGained;
                    licence = 'f';
                }
                if (!callerIsHot && !onlyStoresToThis(m_program.at(callee)) && !isBornAsCast && !isFactory && !isHandedFunctionItCalls(callee, m_caller.at(call), true) && !isHandedGeneratorItIterates(m_program.at(callee), m_caller.at(call)))
                    limit = limitWithoutLicence = takesNothingInCopies && depthOf(m_caller.at(call)) ? 0 : tinyWhereNotHot;
                if (!isTooDeep && limit < maxHandedLiteral && isHandedArrayLiteral(m_caller.at(call))) {
                    limit = maxHandedLiteral;
                    licence = 'h';
                }
                if (!isTooDeep && limit < smallWhereMoreIsGained && callerSuspends() && isHandedObjectItOnlyReads(m_caller.at(call))) {
                    limit = smallWhereMoreIsGained;
                    licence = 'h';
                }
                if (isFactory) {
                    limit = maxFactorySize;
                    licence = 'r';
                }
                unsigned growsBy = sizeOf(m_program.at(callee), isBuiltin || isMoved ? UINT_MAX : limit + 1);
                if (growsBy <= limitWithoutLicence)
                    licence = depthOf(m_caller.at(call)) ? 'c' : 's';
                // Over the limit as a whole. What is left of it at this call counts where that is no more than half: a copy that the
                // constants of the call make another function of, not one that is a little too large.
                if (!isTooDeep && !isBuiltin && !isMoved && growsBy > limit) {
                    unsigned left = sizeAtCall(m_program.at(callee), m_caller.at(call), smallWhereMoreIsGained + 1);
                    if (left <= (m_currentBlockIsInLoopOfCaller ? smallWhereMoreIsGained : mostLeftByConstants) && sizeOf(m_program.at(callee), 2 * left) >= 2 * left) {
                        growsBy = left;
                        limit = smallWhereMoreIsGained;
                        licence = 'k';
                    }
                }
                const BitVector* head = nullptr;
                if (isTooDeep || (!isBuiltin && !isMoved && growsBy > limit)) {
                    if (!callerIsHot || runsOnceOrWaits())
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
                uint8_t given = licenceForMap(head ? 'e' : m_program.at(callee).worker ? 'w' : isOwnWorker(callee) ? 'o' : isBuiltin ? 'p' : isMoved ? 'm' : licence);
                if (m_program.takesNoCopy(m_id, given) && m_program.at(m_id).startFunction != callee) {
                    m_caller.at(call).immediates[1] = Value::copyWasDeclined - given;
                    continue;
                }
                unsigned first = m_caller.blocks.size();
                copyIn(block, k, callee, head);
                didChangeCaller();
                m_caller.inlineFrames[m_frameOfLastCopy].licence = given;
                if (isMoved)
                    m_movedFrames.set(m_frameOfLastCopy);
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

    // The caller is a general entry that has done nothing yet but call its function.
    bool takeOwnBody()
    {
        auto& values = m_caller.blocks[0].values;
        size_t k = values.findIf([&](ValueID id) {
            return m_caller.at(id).opcode == Opcode::CallDirect;
        });
        if (k == notFound || m_caller.at(values[k]).inlineFrame)
            return false;
        FunctionID callee = m_caller.at(values[k]).function();
        auto& function = m_program.at(callee);
        m_isTakingOwnBody = true;
        if (m_program.genericEntrypoints.get(callee.index()) != m_id || function.resumeFunction || (function.kind == FunctionKind::Constructor && !handsRestParameterToSuperclass(function)) || !mayBeCopied(function, m_caller.at(values[k])))
            return false;
        m_copiedFrom.fill(none, m_caller.blocks.size());
        copyIn(BlockID(0u), k, callee);
        m_caller.inlineFrames[m_frameOfLastCopy].licence = licenceForMap('o');
        cleanUp(m_program, m_caller);
        // `f.apply(thisArg, arguments)` is told from a call that keeps what it is passed.
        if (resolveCalls(m_program, m_caller, m_program.isWarmByText(m_id)))
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

    bool handsRestParameterToSuperclass(const Function& constructor) const
    {
        if (!constructor.has(Format::FunctionHasRestParameter) || constructor.blocks.isEmpty() || constructor.blocks[0].parameters.isEmpty())
            return false;
        if (constructor.constructedClass && m_program.at(constructor.constructedClass).isDeclaredToExtendGlobal)
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

    // A function this small is copied to every call that stands in the caller as it was written.
    static constexpr unsigned smallSize = 13;
    // And one this small to a call that came along with a copy.
    static constexpr unsigned smallInCopy = 12;
    // In a caller that the text does not say is hot nothing larger than this is copied, but to its only call: a copy buys time only where time is spent.
    static constexpr unsigned tinyWhereNotHot = 7;
    // One this small wherever the copy does away with more than the call: a constructor of stores (the object is born as what it is cast to,
    // with its fields), a function that calls a function made at the call (no closure is made, and the call of it is a direct one).
    static constexpr unsigned smallWhereMoreIsGained = 40;
    static constexpr unsigned mostLeftByConstants = 10;
    // What only tests what it is handed, and what reads memory besides, is copied wherever its call stands, up to these: see isAccessor().
    static constexpr unsigned maxPredicate = 40;
    static constexpr unsigned maxAccessor = 20;
    // As much at a call as written of a function that this few calls in the whole program name: there is little to copy to.
    static constexpr unsigned fewCalls = 3;
    // A function this small is no larger than the call of it.
    static constexpr unsigned maxForwarder = 2;
    // The head of a function is at most this: see headOf().
    static constexpr unsigned maxHead = 6;
    // In the place of the depth of a call: its head stands in front of it, and it stays a call, in this round and in those to come.
    static constexpr uint32_t behindItsHead = Format::none - 1;
    // One this small goes to its only call.
    static constexpr unsigned maxMoved = 200;
    static constexpr unsigned maxFactorySize = 400;
    static constexpr unsigned maxHandedLiteral = 120;
    static constexpr unsigned minFactoryMethods = 8;
    // A function this large takes in no more.
    static constexpr unsigned maxGrownTo = 4000;
    static constexpr uint32_t none = UINT32_MAX;
    // A call that came along with a body that came along with a body, and so on this many times, stays a call.
    // More than that makes the code larger and no faster.
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

    // Behind casts, and behind block parameters: what a loop uses comes through its header, and through the block where its ways back meet. The one value that gets into the web of block parameters from outside, if it is one.
    static ValueID originOf(const Function& function, ValueID value, EdgesIntoBlocks& edges)
    {
        auto behindCasts = [&](ValueID id) {
            while (function.at(id).opcode == Opcode::Cast)
                id = function.at(id).child(0);
            return id;
        };
        auto isBlockParameter = [&](ValueID id) {
            return function.at(id).opcode == Opcode::Parameter && function.at(id).immediates[0];
        };
        value = behindCasts(value);
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
                ValueID handed = behindCasts(target->arguments[parameter.immediates[1]]);
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

    // By parameter, as the entry block takes them: the function calls it.
    const Program::FactsForInlining* factsOf(FunctionID id) const
    {
        return m_program.factsForInlining.isEmpty() || m_program.at(id).blocks.isEmpty() ? nullptr : &m_program.factsForInlining[id.index()];
    }

    const BitVector& parametersCalledBy(FunctionID id)
    {
        if (auto* facts = factsOf(id))
            return facts->parametersCalled;
        return m_parametersCalledBy.ensure(id.index(), [&] {
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

    // Likewise: it calls it in a loop.
    const BitVector& parametersCalledInLoopsBy(FunctionID id)
    {
        if (auto* facts = factsOf(id))
            return facts->parametersCalledInLoops;
        return m_parametersCalledInLoopsBy.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            BitVector result;
            EdgesIntoBlocks edges;
            Vector<unsigned> cycles;
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                for (ValueID value : function.blocks[b].values) {
                    if (function.at(value).opcode != Opcode::Call)
                        continue;
                    if (cycles.isEmpty())
                        cycles = cycleOfEachBlock(function);
                    if (cycles[b] == isInNoCycle)
                        break;
                    if (size_t index = function.blocks[0].parameters.find(originOf(function, function.at(value).child(0), edges)); index != notFound)
                        result.set(index);
                }
            }
            return result;
        }).iterator->value;
    }

    bool isHandedGeneratorItIterates(const Function& callee, const Value& call) const
    {
        BitVector generators;
        for (unsigned index = 0; index < call.arguments.size(); ++index) {
            ValueID argument = call.arguments[index];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            auto& made = m_caller.at(argument);
            if (made.opcode == Opcode::CallDirect && !made.isJumpToContinuation() && m_program.at(made.function()).resumeFunction && (m_program.at(made.function()).declaredAs & (functionIsAsync | functionIsGenerator)) == functionIsGenerator)
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

    bool isHandedFunctionItCalls(FunctionID callee, const Value& call, bool inLoop = false)
    {
        for (unsigned index : inLoop ? parametersCalledInLoopsBy(callee) : parametersCalledBy(callee)) {
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
                    bool yieldsIt = (value.opcode == Opcode::Cast && isAlias.get(value.child(0).index()))
                        || (value.opcode == Opcode::GetField && heldIn.contains(static_cast<uint64_t>(value.layout().index()) << 32 | value.field()));
                    if (yieldsIt) {
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

    bool isHandedArrayLiteral(const Value& call) const
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
        return m_caller.savesItsFrame || m_caller.declaredAs & (functionIsAsync | functionIsGenerator) || m_program.isResumeFunction(m_caller);
    }

    bool isUsedBehindSuspension(ValueID object) const
    {
        std::optional<StringID> nameOfAwait = m_program.find(asBytes("AwaitBySavingFrame"_s.span8()));
        auto suspends = [&](const Value& value) {
            if (value.opcode == Opcode::Await || value.opcode == Opcode::Yield)
                return true;
            if (value.opcode == Opcode::Operation)
                return nameOfAwait && value.string() == *nameOfAwait;
            if (value.opcode != Opcode::CallBuiltin)
                return false;
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::AwaitBySavingFrame;
        };
        unsigned numberOfBlocks = m_caller.blocks.size();
        unsigned birthBlock = numberOfBlocks;
        BitVector isAlias(m_caller.values.size());
        isAlias.set(object.index());
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (ValueID id : m_caller.blocks[b].values) {
                if (id == object)
                    birthBlock = b;
                else if (m_caller.at(id).opcode == Opcode::Cast && isAlias.get(m_caller.at(id).child(0).index()))
                    isAlias.set(id.index());
            }
        }
        if (birthBlock == numberOfBlocks)
            return false;
        BitVector isReached(numberOfBlocks);
        BitVector isReachedBehindSuspension(numberOfBlocks);
        Vector<std::pair<unsigned, bool>, 16> worklist;
        worklist.append({ birthBlock, false });
        while (!worklist.isEmpty()) {
            auto [b, isBehind] = worklist.takeLast();
            auto& block = m_caller.blocks[b];
            bool isBorn = b != birthBlock;
            for (ValueID id : block.values) {
                if (!isBorn) {
                    isBorn = id == object;
                    continue;
                }
                auto& value = m_caller.at(id);
                if (isBehind) {
                    bool uses = false;
                    value.forEachChild([&](ValueID child) {
                        uses |= isAlias.get(child.index());
                    });
                    if (uses)
                        return true;
                }
                isBehind |= suspends(value);
            }
            if (isBehind && block.terminator.value && isAlias.get(block.terminator.value.index()))
                return true;
            for (auto& target : block.terminator.targets) {
                if (isBehind) {
                    for (ValueID argument : target.arguments) {
                        if (isAlias.get(argument.index()))
                            return true;
                    }
                }
                unsigned next = target.block.index();
                if (next != birthBlock && !(isBehind ? isReachedBehindSuspension : isReached).set(next))
                    worklist.append({ next, isBehind });
            }
        }
        return false;
    }

    bool isHandedObjectItOnlyReads(const Value& call) const
    {
        auto& callee = m_program.at(call.function());
        if (callee.blocks.isEmpty() || callee.blocks[0].parameters.size() != call.arguments.size())
            return false;
        for (unsigned i = 0; i < call.arguments.size(); ++i) {
            ValueID argument = call.arguments[i];
            while (m_caller.at(argument).opcode == Opcode::Cast)
                argument = m_caller.at(argument).child(0);
            if (m_caller.at(argument).opcode == Opcode::NewObjectWithShape && isUsedBehindSuspension(argument) && onlyReadsFieldsOfParameter(callee, i))
                return true;
        }
        return false;
    }

    bool returnsMethodTable(FunctionID id) const
    {
        if (auto* facts = factsOf(id))
            return facts->returnsMethodTable;
        auto& function = m_program.at(id);
        LayoutID resultLayout = m_program.at(resultTypeOf(function)).onlyView();
        if (!resultLayout || m_program.at(resultLayout).kind != LayoutKind::Record || m_program.at(resultLayout).fields.size() < minFactoryMethods)
            return false;
        ValueID made;
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
            if (!value || function.at(value).opcode != Opcode::NewObjectWithShape || (made && made != value))
                return false;
            made = value;
        }
        if (!made)
            return false;
        unsigned methods = 0;
        for (auto& value : function.values)
            methods += value.opcode == Opcode::PutField && value.child(0) == made && function.at(value.child(1)).opcode == Opcode::NewClosure;
        return methods >= minFactoryMethods;
    }

    enum class UseOfObject : uint8_t { Reads, IsAlias, IsAnother };

    static UseOfObject useOfObject(const Function& caller, const Value& value, ValueID object)
    {
        switch (value.opcode) {
        case Opcode::GetField:
        case Opcode::IsUndefinedOrNull:
        case Opcode::IteratorOpen:
            return UseOfObject::Reads;
        case Opcode::GetById:
            return value.readsMethodFor() ? UseOfObject::IsAnother : UseOfObject::Reads;
        case Opcode::GetByVal:
            return value.child(0) != object || value.child(1) == object || caller.at(value.child(1)).opcode != Opcode::Integer ? UseOfObject::IsAnother : UseOfObject::Reads;
        case Opcode::Cast:
            return value.castReason() != CastReason::Upcast && value.castReason() != CastReason::Flow && value.castReason() != CastReason::NonNull ? UseOfObject::IsAnother : UseOfObject::IsAlias;
        default:
            return UseOfObject::IsAnother;
        }
    }

    void didChangeCaller()
    {
        m_timesAskedSinceCallerChanged = 0;
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

    bool isMerelyRead(ValueID result)
    {
        static constexpr unsigned timesAskedWithoutUsers = 2;
        bool knowsUsers = m_timesAskedSinceCallerChanged++ >= timesAskedWithoutUsers;
        if (knowsUsers && m_firstUser.isEmpty())
            findUsers();
        Vector<ValueID, 4> aliases { result };
        for (unsigned next = 0; next < aliases.size(); ++next) {
            ValueID object = aliases[next];
            if (knowsUsers) {
                if (m_isUsedByTerminator.quickGet(object.index()))
                    return false;
                for (ValueID user : m_users.span().subspan(m_firstUser[object.index()], m_firstUser[object.index() + 1] - m_firstUser[object.index()])) {
                    UseOfObject use = useOfObject(m_caller, m_caller.at(user), object);
                    if (use == UseOfObject::IsAnother)
                        return false;
                    if (use == UseOfObject::IsAlias)
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
                    UseOfObject use = useOfObject(m_caller, value, object);
                    if (use == UseOfObject::IsAnother)
                        return false;
                    if (use == UseOfObject::IsAlias)
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

    // Nothing else calls the function, and it is no value. The caller is too large to be copied anywhere itself. So the body is not copied: it
    // moves, see dropFunctionsWithoutCalls(). A call that resolveCalls() finds later makes it a copy after all.
    bool isOnlyCall(FunctionID callee, const Value& call) const
    {
        if (m_caller.isGenericEntrypoint() || (call.inlineFrame && !m_movedFrames.get(call.inlineFrame)))
            return false;
        if (m_program.numberOfDirectCalls[callee.index()] != 1 || m_program.genericEntrypoints.contains(callee.index()))
            return false;
        // A ramp is copied nowhere either.
        return (m_program.at(m_id).resumeFunction || m_program.at(m_id).startFunction || sizeOf(m_program.at(m_id), smallSize + 1) > smallSize) && sizeOf(m_program.at(callee), maxMoved + 1) <= maxMoved;
    }

    // Whatever it returns is an object that it has made, or that a function it calls has.
    bool handsBackWhatItMakes(FunctionID id, unsigned depth = 0) const
    {
        static constexpr unsigned maxDepth = 3;
        if (depth > maxDepth)
            return false;
        if (auto found = m_handsBackWhatItMakes.find(id.index()); found != m_handsBackWhatItMakes.end())
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
                result = handsBackWhatItMakes(called, depth + 1);
            if (!result)
                break;
        }
        // What was cut short by the depth is not remembered: from another function it may be reached.
        if (result || !depth)
            m_handsBackWhatItMakes.add(id.index(), result);
        return result;
    }

    // The callee hands back an object it has made, and the first thing the caller does with it is to cast it to a layout that it could have been born as: copied, it is: CastSimplification::specializeAllocationFor().
    bool handsBackWhatIsCast(FunctionID callee, BlockID block, unsigned k) const
    {
        auto& values = m_caller.at(block).values;
        LayoutID made = m_program.at(m_caller.at(values[k]).type).onlyView();
        if (!made || m_program.at(made).specializations.isEmpty())
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
            LayoutID target = m_program.at(user.castReason() == CastReason::Coerce ? user.coercedType() : user.type).onlyView();
            return target && std::ranges::binary_search(m_program.at(made).specializations, target) && handsBackWhatItMakes(callee);
        }
        return false;
    }

    // How a call got here is forgotten from one round to the next, and a call by name becomes a direct call in between. So the depth
    // is kept in the second immediate of CallDirect, CallMethod and Call, which nothing else uses. resolveCalls() hands it on.
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

    // A copy pays by being run often. The top-level code of a module runs once, and a coroutine waits between its parts.
    bool runsOnceOrWaits() const
    {
        return !m_currentBlockIsInLoop && (m_caller.kind == FunctionKind::Module || m_program.isResumeFunction(m_caller));
    }

    // Plain stores into an object that was made right before the call and that nobody has seen.
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
        unsigned indexOfThis = callee.captureTypes.size();
        ValueID object = withoutUpcasts(m_caller.at(values[k]).arguments[indexOfThis]);
        if (m_caller.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        size_t birth = values.find(object);
        if (birth == notFound || birth >= k)
            return false;
        for (size_t i = birth + 1; i < k; ++i) {
            if (withoutUpcasts(values[i]) == object)
                continue;
            bool usesIt = false;
            m_caller.at(values[i]).forEachChild([&](ValueID child) {
                usesIt |= withoutUpcasts(child) == object;
            });
            if (usesIt)
                return false;
        }
        LayoutID made = m_caller.at(object).layout();
        auto& layout = m_program.at(made);
        if (!isObjectLayout(layout.kind) || (layout.kind == LayoutKind::Record && layout.mayHaveOtherPrototype))
            return false;
        ValueID thisValue = callee.blocks[0].parameters[indexOfThis];
        for (ValueID id : initialStoresOf(callee)) {
            auto& store = callee.at(id);
            auto field = layout.fieldNamed(store.string());
            if (store.child(0) != thisValue || store.child(1) == thisValue || !field)
                return false;
            if ((m_program.prototypePropertyNames.get(store.string().index()) && layout.kind != LayoutKind::Record) || inheritsByItsLayout(m_program, made, store.string()))
                return false;
            TypeID type = layout.fields[*field].type;
            if (type && !m_program.typeOf(callee, store.child(1)).isWithin(m_program.at(type), m_program))
                return false;
        }
        return true;
    }

    // It reads and tests, and that is all.
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

    // What it comes to once the accessors it calls are copied into it.
    std::optional<unsigned> sizeAsAccessor(const Function& function, bool& reads, unsigned nesting) const
    {
        unsigned size = 0;
        BitVector isInChainOfIntegerTests;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (runsInitializer(function, block))
                continue;
            size += takeChainOfIntegerTests(function, b, isInChainOfIntegerTests);
            bool isInChain = isInChainOfIntegerTests.get(b);
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (isInChain && id == block.terminator.value)
                    continue;
                if (value.opcode == Opcode::CallDirect) {
                    if (nesting == 2)
                        return std::nullopt;
                    auto sizeOfCallee = sizeAsAccessor(m_program.at(value.function()), reads, nesting + 1);
                    if (!sizeOfCallee)
                        return std::nullopt;
                    size += *sizeOfCallee;
                } else if (readsMemory(value.opcode))
                    reads = true;
                else if (!isTest(value.opcode))
                    return std::nullopt;
                if (value.opcode != Opcode::CallDirect)
                    size += countsForSize(value.opcode);
            }
            if (!endsInTest(block))
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

    static bool endsInTest(const Block& block)
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

    // Which target of a Branch is not expected to be taken, where the test itself says so: `===` holds, undefined or null is found, a bit
    // that is tested for is set, what was read or handed is falsy.
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

    bool isOpenReadOfField(const Function& function, const Value& value) const
    {
        if (value.opcode != Opcode::GetById)
            return false;
        auto& receiver = m_program.typeOf(function, value.child(0));
        LayoutID view = receiver.onlyView();
        if (!view || !m_program.at(view).admitsForeignObjects || m_program.at(view).members.isEmpty())
            return false;
        auto access = fieldOf(m_program, receiver, value.string());
        if (!access || !onlyFieldsSupplyToMembers(m_program, access->layout, value.string()))
            return false;
        return std::ranges::all_of(m_program.at(view).members, [&](LayoutID member) {
            return !!m_program.at(member).fieldNamed(value.string());
        });
    }

    // The blocks next to the entry that test and read and may come to return without leaving them. They are copied in the place of a call
    // of a function that is not: what leaves them calls it, and it starts over. Nobody can tell that such a value was carried out twice.
    // Without a size there is none.
    const Head& headOf(FunctionID id)
    {
        return m_heads.ensure(id.index(), [&] {
            auto& function = m_program.at(id);
            if (!function.resultTypes.isEmpty())
                return Head { };
            auto sizeOfBlock = [&](const Block& block) -> std::optional<unsigned> {
                if (!endsInTest(block) || block.terminator.kind == TerminatorKind::Unreachable)
                    return std::nullopt;
                unsigned size = block.terminator.kind == TerminatorKind::Branch || block.terminator.kind == TerminatorKind::Switch;
                for (size_t i = 0; i < block.values.size(); ++i) {
                    auto& value = function.at(block.values[i]);
                    if (!isTest(value.opcode) && !readsMemory(value.opcode))
                        return std::nullopt;
                    bool isOpenRead = isOpenReadOfField(function, value);
                    if (!isOpenRead && (writesOf(m_program, function, block, i) > Writes::RuntimeState || (value.opcode == Opcode::Cast && castWritesShapeWords(m_program, function, value))))
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
                auto size = sizeOfBlock(function.blocks[b]);
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
                    head.size -= *sizeOfBlock(function.blocks[b]);
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

    // A constructor that does nothing but store to `this` is smaller than the call of it, wherever the call stands. So is an accessor.
    bool isOwnWorker(FunctionID callee) const { return m_program.at(m_id).worker == callee; }

    uint32_t depthOf(const Value& call) const
    {
        // A wrapper is taken in wherever its call stands. How deep that is, the frames say to the call of the worker.
        if (m_program.at(call.function()).worker || isOwnWorker(call.function()))
            return 0;
        if (onlyStoresToThis(m_program.at(call.function())))
            return 0;
        if (runsOnceOrWaits())
            return maxDepth;
        if (isAccessor(call.function()) || sizeOf(m_program.at(call.function()), maxForwarder + 1) <= maxForwarder)
            return 0;
        uint32_t depth = call.immediates[1] == Format::none ? 0 : call.immediates[1];
        uint32_t frames = 0;
        for (uint32_t frame = call.inlineFrame; frame; frame = m_caller.inlineFrames[frame].parent)
            frames += !m_program.at(m_caller.inlineFrames[frame].callee).worker;
        // The body of its own function is not a copy that came along with anything.
        if (frames && m_caller.isGenericEntrypoint())
            --frames;
        return std::max(depth, frames);
    }

    template<typename NeverReturns>
    static BitVector blocksThatNeverReturn(const Function& function, NOESCAPE const NeverReturns& neverReturns)
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

    const BitVector& neverReturnsIn(FunctionID id)
    {
        if (!m_program.blocksThatNeverReturn.isEmpty())
            return m_program.blocksThatNeverReturn[id.index()];
        return m_neverReturnsIn.ensure(id.index(), [&] {
            return blocksThatNeverReturn(m_program.at(id), [&](FunctionID callee) {
                return !m_program.at(callee).blocks.containsIf([](auto& block) {
                    return block.terminator.kind == TerminatorKind::Return;
                });
            });
        }).iterator->value;
    }

    // A function that was copied in, and what the call of it was copied in with.
    struct Copy {
        FunctionID function;
        uint32_t outer;
    };

    static std::optional<std::pair<ValueID, int64_t>> integerTestAtEndOf(const Function& function, const Block& block)
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

    static unsigned takeChainOfIntegerTests(const Function& function, unsigned first, BitVector& isInChain)
    {
        static constexpr unsigned fewestTests = 4;
        static constexpr unsigned mostPlaces = 3;
        static constexpr int64_t valuesInWindow = 64;
        static constexpr unsigned instructionsInUnit = 3;
        if (isInChain.get(first))
            return 0;
        auto tested = integerTestAtEndOf(function, function.blocks[first]);
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
        auto isSamePlace = [&](const Target* a, const Target* b) {
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
        Vector<const Target*, mostPlaces> places;
        for (unsigned at = first; !isInChain.get(at) && !links.contains(at);) {
            auto& block = function.blocks[at];
            auto test = integerTestAtEndOf(function, block);
            if (!test || test->first != tested->first)
                break;
            bool onlyTests = at == first || std::ranges::all_of(block.values, [&](ValueID id) {
                return id == block.terminator.value || function.at(id).opcode == Opcode::Integer;
            });
            if (!onlyTests || (at != first && !block.parameters.isEmpty()))
                break;
            const Target* place = placeOf(block.terminator.targets[0]);
            if (!places.containsIf([&](const Target* known) { return isSamePlace(known, place); })) {
                if (places.size() == mostPlaces)
                    break;
                places.append(place);
            }
            links.append(at);
            integers.append(test->second);
            if (!block.terminator.targets[1].arguments.isEmpty())
                break;
            at = block.terminator.targets[1].block.index();
        }
        if (links.size() < fewestTests)
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

    // It calls the initializer of a lazy variable, or the body of a module: it runs once, and is cold code.
    bool runsInitializer(const Function& function, const Block& block) const
    {
        return block.values.containsIf([&](ValueID id) {
            return function.at(id).opcode == Opcode::CallDirect && m_program.at(function.at(id).function()).kind == FunctionKind::Module;
        });
    }

    // The test that leads there and the branch on it are one operation on the way that is taken.
    bool isGuardOfInitializer(const Function& function, const Block& block) const
    {
        return block.terminator.kind == TerminatorKind::Branch && (runsInitializer(function, function.at(block.terminator.targets[0].block)) || runsInitializer(function, function.at(block.terminator.targets[1].block)));
    }

    // No further than the limit.
    unsigned sizeOf(const Function& function, unsigned limit) const
    {
        if (!m_program.sizesForInlining.isEmpty() && &function >= m_program.functions.begin() && &function < m_program.functions.end())
            return std::min(m_program.sizesForInlining[&function - m_program.functions.begin()], std::max(limit, 1u));
        unsigned size = 0;
        BitVector isInChainOfIntegerTests;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (runsInitializer(function, block))
                continue;
            size += takeChainOfIntegerTests(function, b, isInChainOfIntegerTests);
            if (size >= limit)
                return size;
            bool isInChain = isInChainOfIntegerTests.get(b);
            ValueID tested = isInChain || isGuardOfInitializer(function, block) ? block.terminator.value : ValueID();
            for (ValueID id : block.values) {
                if (countsForSize(function.at(id).opcode) && id != tested && ++size >= limit)
                    return size;
            }
            if (!isInChain && (block.terminator.kind == TerminatorKind::Branch || block.terminator.kind == TerminatorKind::Switch) && ++size >= limit)
                return size;
        }
        return size;
    }

    // What a value of the callee is at the call, where that is a constant: a constant, a parameter that is passed one, the length of a
    // parameter that is passed an array literal (the rest parameter).
    struct ConstantsAtCall {
        const Inlining& inlining;
        const Function& callee;
        const Value& call;
        StringID length;
        Vector<std::unique_ptr<Value>, 2> lengths { };
        // By the number of a block parameter that is only ever given itself and one other value: that value. The front end hands every
        // local that lives across a loop through the header, those that are never assigned too. Found when first asked for.
        Vector<ValueID> onlyGiven { };

        // As Cleanup::removeTrivialParameters() finds them once the body is copied.
        void findTrivialParameters()
        {
            onlyGiven.fill(ValueID(), callee.values.size());
            auto resolve = [&](ValueID value) {
                while (onlyGiven[value.index()])
                    value = onlyGiven[value.index()];
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
                        if (onlyGiven[parameter.index()])
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
                            onlyGiven[parameter.index()] = only;
                            changed = true;
                        }
                    }
                }
            } while (changed);
        }

        ValueID definitionOf(ValueID id, bool throughTests)
        {
            for (;;) {
                auto& value = callee.at(id);
                if (value.opcode == Opcode::Cast && (throughTests ? value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce : value.castReason() == CastReason::Upcast)) {
                    id = value.child(0);
                    continue;
                }
                if (value.opcode != Opcode::Parameter || callee.blocks[0].parameters.contains(id))
                    return id;
                if (onlyGiven.isEmpty())
                    findTrivialParameters();
                if (!onlyGiven[id.index()])
                    return id;
                id = onlyGiven[id.index()];
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
            // Of an object made at the call: what its layout has not got and does not inherit is `undefined`, and whether it has a property is known.
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

        // The value itself comes to a constant or to an argument.
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

    // The size of what is left of the callee at this call: what a branch on constants of the call does not lead to goes once the body is
    // copied (Cleanup::foldConstantConditions(), ArrayEscapeAnalysis), so it is not counted. `emit("x", 1)`: `type === "error"`, and all arms
    // but one of `switch (args.length)`.
    unsigned sizeAtCall(const Function& callee, const Value& call, unsigned limit) const
    {
        if (call.arguments.size() != callee.blocks[0].parameters.size())
            return sizeOf(callee, limit);
        ConstantsAtCall constants { *this, callee, call, m_program.find(asBytes("length"_s.span8())).value_or(StringID()) };
        unsigned size = 0;
        BitVector seen(callee.blocks.size());
        seen.set(0);
        Vector<unsigned, 16> worklist { 0u };
        BitVector isInChainOfIntegerTests;
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            auto& block = callee.blocks[b];
            bool isCounted = !runsInitializer(callee, block);
            if (isCounted && !(block.terminator.kind == TerminatorKind::Branch && constants.decide(block.terminator.value))) {
                size += takeChainOfIntegerTests(callee, b, isInChainOfIntegerTests);
                if (size >= limit)
                    return size;
            }
            bool isInChain = isInChainOfIntegerTests.get(b);
            ValueID tested = isInChain || isGuardOfInitializer(callee, block) ? block.terminator.value : ValueID();
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
        for (uint32_t at = m_copiedFrom[block.index()]; at != none; at = m_copies[at].outer) {
            if (m_copies[at].function == callee)
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

    bool saysCallNoInline(FunctionID id) const
    {
        return m_saysCallNoInline.ensure(id.index(), [&] {
            static const BuiltinID callNoInline = Builtins::singleton().find(BuiltinKind::Operation, { }, Builtins::singleton().nameOf(asBytes("CallNoInline"_s.span8())));
            for (auto& value : m_program.at(id).values) {
                if (value.opcode == Opcode::Operation ? Builtins::singleton().find(BuiltinKind::Operation, { }, m_program.builtinName(value.string())) == callNoInline : value.opcode == Opcode::CallBuiltin && value.builtin() == callNoInline)
                    return true;
            }
            return false;
        }).iterator->value;
    }
    mutable UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_saysCallNoInline;

    bool mayBeCopied(const Function& callee, const Value& call) const
    {
        if (callee.blocks.isEmpty() || callee.kind == FunctionKind::Module || callee.has(Format::FunctionIsAsync | Format::FunctionIsGenerator | FunctionIsGenericEntrypoint | FunctionUsesArgumentBuffer | FunctionLogsMisfits))
            return false;
        if (m_program.logsTypeErrors && callee.has(Format::FunctionIsHiddenInStackTraces) && !callee.has(Format::FunctionIsBuiltin) && hasHandlers(callee))
            return false;
        if (m_caller.inlineFrames.size() > maxInlineFrames || callee.readsMemberOfHostObject || callee.readsAsyncLocalStorage)
            return false;
        if (callee.savesItsFrame && m_program.at(m_id).startFunction != call.function())
            return false;
        if (m_caller.has(Format::FunctionIsGenerator) && (callee.resumeFunction || m_program.isResumeFunction(callee)))
            return false;
        if (callee.has(Format::FunctionIsBuiltin) && !m_program.standsForBuiltin(callee) && saysCallNoInline(call.function()))
            return false;
        // Whatever is live into a handler is kept in memory, and that is every variable of a loop the handler goes back into.
        if (!m_isTakingOwnBody && !isOwnWorker(call.function()) && m_program.at(m_id).startFunction != call.function() && hasHandlers(callee) && !hasHandlers(m_caller) && !isHandedGeneratorItIterates(callee, call))
            return false;
        if (call.arguments.size() != callee.blocks[0].parameters.size())
            return false;
        auto& result = m_program.at(resultTypeOf(callee));
        if (!result.isWithin(m_program.at(call.type), m_program))
            return false;
        bool yieldsUndefined = m_program.at(m_program.types.undefined).isWithin(result, m_program);
        for (auto& block : callee.blocks) {
            if (block.terminator.kind == TerminatorKind::Return && !block.terminator.value && !yieldsUndefined)
                return false;
        }
        return true;
    }

    BlockID addBlock(uint32_t copiedFrom)
    {
        m_caller.blocks.append(Block { });
        m_copiedFrom.append(copiedFrom);
        // It takes the place of part of the block that is being looked at.
        if (m_currentBlockIsInLoop)
            m_isInLoop.set(m_caller.blocks.size() - 1);
        if (m_currentBlockIsInLoopOfCaller)
            m_isInLoopOfCaller.set(m_caller.blocks.size() - 1);
        return BlockID(m_caller.blocks.size() - 1);
    }

    ValueID addValue(Value&& value)
    {
        m_caller.values.append(WTF::move(value));
        return ValueID(m_caller.values.size() - 1);
    }

    // Replaces the value at `k` of the block, a direct call, with the body of the function, or with its head and a call for the rest.
    void copyIn(BlockID block, unsigned k, FunctionID calleeID, const BitVector* head = nullptr)
    {
        auto& callee = m_program.at(calleeID);
        ValueID call = m_caller.at(block).values[k];
        Value callOfRest(m_caller.at(call));
        Arguments arguments = m_caller.at(call).arguments;
        uint32_t position = m_caller.at(call).position;
        uint32_t depth = m_isTakingOwnBody ? 0 : callee.worker ? (m_caller.at(call).immediates[1] == Format::none ? 0 : m_caller.at(call).immediates[1]) : depthOf(m_caller.at(call)) + 1;
        uint32_t outerFrame = m_caller.at(call).inlineFrame;
        if (m_caller.inlineFrames.isEmpty())
            m_caller.inlineFrames.append(InlineFrame { });
        uint32_t inlineFrame = m_caller.inlineFrames.size();
        m_caller.inlineFrames.append(InlineFrame { calleeID, position, outerFrame });
        m_caller.inlineFrames.last().isTailCall = m_caller.at(call).isInTailPosition;
        m_caller.inlineFrames.last().isConstruction = m_caller.at(call).constructs;
        m_frameOfLastCopy = inlineFrame;
        for (unsigned i = 1; i < callee.inlineFrames.size(); ++i) {
            InlineFrame frame = callee.inlineFrames[i];
            frame.parent += inlineFrame;
            m_caller.inlineFrames.append(frame);
        }
        bool isWatched = m_caller.at(block).terminator.kind == TerminatorKind::Invoke && k + 1 == m_caller.at(block).values.size();

        BlockID after = addBlock(m_copiedFrom[block.index()]);
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
        // What the call yielded is what the function is declared to yield. Its users were written, and held to their types, for a value of the type of the call, which in untyped text is anything. Whether they may as well go by the
        // type of the function, which says more, is for simplifyCasts() to find out.
        {
            Value result;
            result.opcode = Opcode::Parameter;
            result.position = position;
            result.inlineFrame = outerFrame;
            result.type = resultTypeOf(callee);
            result.takePredictionOf(m_caller.at(call));
            result.rep = repOf(m_program.at(result.type));
            result.immediates = { after.index(), 0 };
            if (!callee.resultTypes.isEmpty()) {
                // One parameter a result; whatever looked at a result of the call looks at that.
                Arguments yielded;
                for (unsigned i = 0; i < callee.resultTypes.size(); ++i) {
                    Value parameter(result);
                    parameter.takePredictionOf(Value { });
                    parameter.type = callee.resultTypes[i];
                    parameter.rep = repOf(m_program.at(parameter.type));
                    parameter.immediates = { after.index(), i };
                    yielded.append(addValue(WTF::move(parameter)));
                    m_caller.at(after).parameters.append(yielded.last());
                }
                for (; m_numberOfValuesSearchedForResults < m_caller.values.size(); ++m_numberOfValuesSearchedForResults) {
                    auto& user = m_caller.values[m_numberOfValuesSearchedForResults];
                    if (user.opcode == Opcode::ResultAt)
                        m_readsOfResults.add(user.child(0).index(), Vector<uint32_t, 2> { }).iterator->value.append(m_numberOfValuesSearchedForResults);
                }
                for (uint32_t index : m_readsOfResults.take(call.index())) {
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
                    upcast.children[0] = yielded[user.resultIndex()];
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
                upcast.takePredictionOf(m_caller.at(call));
                upcast.rep = m_caller.at(call).rep;
                upcast.immediates[1] = static_cast<uint32_t>(CastReason::Upcast);
                upcast.children[0] = addValue(WTF::move(result));
                m_caller.at(after).parameters.append(upcast.children[0]);
                m_caller.at(call) = WTF::move(upcast);
                m_caller.at(after).values.insert(0, call);
            }
        }

        m_copies.append(Copy { calleeID, m_copiedFrom[block.index()] });
        uint32_t copiedFrom = m_copies.size() - 1;
        unsigned first = m_caller.blocks.size();
        auto& neverReturnsInCallee = neverReturnsIn(calleeID);
        auto isCopied = [&](unsigned i) {
            return !head || head->get(i);
        };
        Vector<BlockID, 16> madeFor(callee.blocks.size());
        for (unsigned i = 0; i < callee.blocks.size(); ++i) {
            if (!isCopied(i))
                continue;
            madeFor[i] = addBlock(copiedFrom);
            if (neverReturnsInCallee.get(i))
                m_neverReturns.set(madeFor[i].index());
        }
        BlockID rest;
        if (head) {
            rest = addBlock(copiedFrom);
            callOfRest.type = resultTypeOf(callee);
            callOfRest.rep = repOf(m_program.at(callOfRest.type));
            callOfRest.immediates[1] = behindItsHead;
            ValueID result = addValue(WTF::move(callOfRest));
            m_caller.at(rest).values.append(result);
            m_caller.at(rest).terminator.kind = TerminatorKind::Jump;
            m_caller.at(rest).terminator.targets.append(Target { after, Arguments { result } });
        }
        Vector<ValueID> map(callee.values.size());
        for (unsigned i = 0; i < arguments.size(); ++i) {
            ValueID parameter = callee.blocks[0].parameters[i];
            ValueID argument = arguments[i];
            // The body was written, and held to its types, for a value of the type of the parameter. Whether it may as well go by the type
            // of the argument, which says more, is for simplifyCasts() to find out.
            TypeID type = callee.at(parameter).type;
            if (type != m_caller.at(argument).type || callee.at(parameter).hasPredictionUnlike(m_caller.at(argument))) {
                Value upcast;
                upcast.opcode = Opcode::Cast;
                upcast.position = position;
                upcast.inlineFrame = outerFrame;
                upcast.type = type;
                upcast.takePredictionOf(callee.at(parameter));
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
            if (!isCopied(i))
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
            if (!isCopied(i))
                continue;
            BlockID made = madeFor[i];
            if (i) {
                for (ValueID parameter : callee.blocks[i].parameters) {
                    m_caller.at(map[parameter.index()]).immediates[0] = made.index();
                    m_caller.at(made).parameters.append(map[parameter.index()]);
                }
            }
            for (ValueID value : callee.blocks[i].values) {
                auto& copied = m_caller.at(map[value.index()]);
                // What it yields goes to the block behind the call.
                if (copied.opcode == Opcode::Results) {
                    copied = Value { };
                    copied.type = m_program.types.undefined;
                    copied.rep = repOf(m_program.at(copied.type));
                    continue;
                }
                copied.forEachChild(mapped);
                if (head && isOpenReadOfField(callee, callee.at(value))) {
                    Value test;
                    test.opcode = Opcode::HasShapeOf;
                    test.position = copied.position;
                    test.inlineFrame = copied.inlineFrame;
                    test.type = m_program.types.boolean;
                    test.rep = repOf(m_program.at(test.type));
                    test.children[0] = copied.child(0);
                    test.immediates = { m_program.typeOf(callee, callee.at(value).child(0)).onlyView().index(), m_program.mayRunInSeveralInstances ? shapeTestIsForThisInstance : 0u };
                    ValueID isMember = addValue(WTF::move(test));
                    BlockID behindTest = addBlock(copiedFrom);
                    if (neverReturnsInCallee.get(i))
                        m_neverReturns.set(behindTest.index());
                    m_caller.at(made).values.append(isMember);
                    auto& branch = m_caller.at(made).terminator;
                    branch.kind = TerminatorKind::Branch;
                    branch.value = isMember;
                    branch.targets.append(Target { behindTest, { } });
                    branch.targets.append(Target { rest, { } });
                    made = behindTest;
                    m_caller.at(made).values.append(map[value.index()]);
                    auto access = fieldOf(m_program, m_program.typeOf(callee, callee.at(value).child(0)), callee.at(value).string());
                    auto& read = m_caller.at(map[value.index()]);
                    if (TypeID declared = m_program.at(access->layout).fields[access->field].type; declared && declared != read.type && m_program.at(declared).isWithin(m_program.at(read.type), m_program) && membersHoldOnlyDeclaredType(m_program, access->layout, access->field)) {
                        read.type = declared;
                        read.rep = repOf(m_program.at(declared));
                        read.takePredictionOf(Value { });
                    }
                    continue;
                }
                if (copied.opcode == Opcode::NewObjectWithShape)
                    copied.immediates[1] = m_caller.allocationOf(Allocation(callee.allocations[copied.allocation()]));
                else if (copied.opcode == Opcode::CallDirect || copied.opcode == Opcode::CallMethod || copied.opcode == Opcode::Call || copied.opcode == Opcode::Operation)
                    copied.immediates[1] = depth;
                m_caller.at(made).values.append(map[value.index()]);
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
                    m_caller.at(made).values.append(result);
                }
                terminator = Terminator { };
                terminator.kind = TerminatorKind::Jump;
                terminator.targets.append(Target { after, Arguments { result } });
                if (!callee.resultTypes.isEmpty()) {
                    auto& arguments = terminator.targets[0].arguments;
                    arguments.clear();
                    for (ValueID yielded : callee.at(callee.blocks[i].terminator.value).arguments)
                        arguments.append(map[yielded.index()]);
                }
            } else {
                for (auto& target : terminator.targets) {
                    if (isCopied(target.block.index()))
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
            m_caller.at(made).terminator = WTF::move(terminator);
        }
        auto& jump = m_caller.at(block).terminator;
        jump.kind = TerminatorKind::Jump;
        jump.targets.append(Target { BlockID(first), { } });
        if (isWatched)
            watch(first, m_caller.blocks.size(), copiedFrom, handler);
    }

    // What was copied in the place of a call that a handler watches is watched by it: whatever may throw ends its block, and what is
    // thrown and not caught goes there.
    void watch(unsigned begin, unsigned end, uint32_t copiedFrom, const Target& handler)
    {
        for (unsigned b = begin; b < end; ++b) {
            BlockID block(b);
            if (m_caller.at(block).terminator.kind == TerminatorKind::Throw && m_caller.at(block).terminator.targets.isEmpty())
                m_caller.at(block).terminator.targets.append(handler);
            for (unsigned i = 0; i < m_caller.at(block).values.size(); ++i) {
                // Its last value is watched by a handler of what was copied.
                if (i + 1 == m_caller.at(block).values.size() && m_caller.at(block).terminator.kind == TerminatorKind::Invoke)
                    break;
                // A store to an object nobody has seen stays with the allocation. The first stores of a constructor are such: it would not have been copied in otherwise.
                auto& value = m_caller.at(m_caller.at(block).values[i]);
                if (!mayThrow(m_program, m_caller, value) || isInitializingStore(m_program, m_caller, m_caller.at(block), i) || (value.opcode == Opcode::PutById && value.isInitialStore()))
                    continue;
                BlockID rest = addBlock(copiedFrom);
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
    Vector<Copy> m_copies;
    // By block: the copy it is part of, among m_copies.
    Vector<uint32_t> m_copiedFrom;
    BitVector m_neverReturns;
    // Of the top-level code of a module and of a resume function.
    BitVector m_isInLoop;
    // By inline frame: the body was moved in.
    BitVector m_movedFrames;
    uint32_t m_frameOfLastCopy { 0 };
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_parametersCalledBy;
    bool m_currentBlockIsInLoop { false };
    BitVector m_isInLoopOfCaller;
    bool m_currentBlockIsInLoopOfCaller { false };
    bool m_isTakingOwnBody { false };
    unsigned m_timesAskedSinceCallerChanged { 0 };
    Vector<uint32_t> m_firstUser;
    Vector<ValueID> m_users;
    BitVector m_isUsedByTerminator;
    unsigned m_numberOfValuesSearchedForResults { 0 };
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_readsOfResults;
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_neverReturnsIn;
    UncheckedKeyHashMap<uint32_t, Head, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_heads;
    UncheckedKeyHashMap<uint32_t, BitVector, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_parametersCalledInLoopsBy;
    mutable UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_isAccessor;
    mutable UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_handsBackWhatItMakes;
};

} // anonymous namespace

unsigned inlineCalls(const Program& program, FunctionID id, Function& function, CleanUpTiming timing)
{
    // An entry is compiled in the place of a module's code, where inline frames name functions in another way. It runs once.
    if (program.isEntry(id))
        return 0;
    // The general entry of a function with a rest parameter has the arguments in memory. With the body of the function in it, whatever its
    // size, what the body reads from the array it reads from there: see ArrayEscapeAnalysis. That is tried, and kept if the array is no
    // longer made on the way in.
    if (function.isGenericEntrypoint() && function.blocks.size() == 1 && function.blocks[0].values.containsIf([&](ValueID value) { return function.at(value).opcode == Opcode::CreateRest; })) {
        Function withBody = function;
        bool makesArrayOnEntry = !Inlining(program, id, withBody).takeOwnBody() || withBody.blocks[0].values.containsIf([&](ValueID value) {
            return withBody.at(value).opcode == Opcode::CreateRest;
        });
        if (!makesArrayOnEntry) {
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
