/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if OS(UNIX) && HAVE(MACHINE_CONTEXT)
#define RUN_BUN_TRAP_POSITION_TESTS() RUN_ALONE(testFoldedAccessFaultsAtFirstInstruction())
#else
#define RUN_BUN_TRAP_POSITION_TESTS() do { } while (false)
#endif

#define RUN_BUN_TRAP_TESTS() do { \
        RUN(testTrapFoldsIntoAccess()); \
        RUN(testTrapStaysWithoutFoldableAccess()); \
        RUN(testConsecutiveTrapsFoldSeparately()); \
        RUN(testTrapThroughBaseAndIndexFoldsIntoAccessThroughBoth()); \
        RUN(testTrapReachOfZeroRunsNoStep()); \
        RUN(testTrapStaysWithoutKeptValues()); \
        RUN_BUN_TRAP_POSITION_TESTS(); \
    } while (false)

static constexpr int32_t testTrapReach = 128;
static constexpr int32_t offsetOfTrap = 5;
static constexpr int32_t offsetOfAccess = 16;

struct TrapGraph {
    explicit TrapGraph(uint32_t trapReach = testTrapReach)
        : TrapGraph(policyWithTrapReach(trapReach))
    {
    }

    explicit TrapGraph(const B3::Policy& policy)
        : proc(policy)
        , code(proc.code())
        , root(code.addBlock())
        , base(code.newTmp(GP))
        , result(code.newTmp(GP))
    {
        proc.setOptLevel(2);
        root->append(Move, nullptr, argumentTmp(0), base);
        root->append(Move, nullptr, argumentTmp(1), result);
    }

    static B3::Policy policyWithTrapReach(uint32_t trapReach)
    {
        B3::Policy policy;
        policy.allocation.keepsValuesAfterLowering = true;
        policy.air.peepholes.trapReach = trapReach;
        return policy;
    }

    Tmp appendTrap(BasicBlock* block, Tmp pointer, int32_t offset = offsetOfTrap)
    {
        Tmp loaded = code.newTmp(GP);
        trapOrigin = appendTrappingLoad(proc, block, Arg::addr(pointer, offset), loaded);
        return loaded;
    }

    void appendReturnOfResult(BasicBlock* block)
    {
        block->append(Move, nullptr, result, returnTmp());
        appendReturn(block);
    }

    unsigned numberOfFoldedTraps() const { return proc.report().countOf(B3::Counter::TrapsFoldedIntoAccesses); }

    B3::Procedure proc;
    Code& code;
    BasicBlock* root;
    Tmp base;
    Tmp result;
    B3::Value* trapOrigin { nullptr };
};

enum class TrapAccess : uint8_t {
    Load64,
    Load32,
    Load16,
    Load8,
    LoadSignExtended32,
    LoadDouble,
    Store64,
    Store32,
    Store8,
    StoreDouble,
    StoreImmediate,
    AddFromMemory,
    AddToMemory,
    AddDoubleFromMemory,
    CompareMemory,
    TestMemory,
};

static constexpr TrapAccess allTrapAccesses[] = {
    TrapAccess::Load64, TrapAccess::Load32, TrapAccess::Load16, TrapAccess::Load8, TrapAccess::LoadSignExtended32, TrapAccess::LoadDouble,
    TrapAccess::Store64, TrapAccess::Store32, TrapAccess::Store8, TrapAccess::StoreDouble, TrapAccess::StoreImmediate,
    TrapAccess::AddFromMemory, TrapAccess::AddToMemory, TrapAccess::AddDoubleFromMemory, TrapAccess::CompareMemory, TrapAccess::TestMemory,
};

template<typename... Arguments>
static bool tryAppend(BasicBlock* block, B3::Air::Opcode opcode, Arguments... arguments)
{
    Inst inst(opcode, nullptr, arguments...);
    if (!inst.isValidForm(CPUFeatureSet::host()))
        return false;
    block->appendInst(WTF::move(inst));
    return true;
}

static bool tryAppendBranchAndReturns(TrapGraph& graph, bool didAppendBranch)
{
    if (!didAppendBranch)
        return false;
    BasicBlock* taken = graph.code.addBlock();
    BasicBlock* notTaken = graph.code.addBlock();
    graph.root->setSuccessors(taken, notTaken);
    appendReturn(taken, 1);
    appendReturn(notTaken, 0);
    return true;
}

static bool tryAppendAccessAndReturn(TrapGraph& graph, TrapAccess access)
{
    BasicBlock* root = graph.root;
    Tmp result = graph.result;
    Tmp number = graph.code.newTmp(FP);
    Tmp sum = graph.code.newTmp(FP);
    Arg address = Arg::addr(graph.base, offsetOfAccess);
    bool isValid = false;
    switch (access) {
    case TrapAccess::Load64:
        isValid = tryAppend(root, Move, address, result);
        break;
    case TrapAccess::Load32:
        isValid = tryAppend(root, Move32, address, result);
        break;
    case TrapAccess::Load16:
        isValid = tryAppend(root, Load16, address, result);
        break;
    case TrapAccess::Load8:
        isValid = tryAppend(root, Load8, address, result);
        break;
    case TrapAccess::LoadSignExtended32:
        isValid = tryAppend(root, SignExtend32To64, address, result);
        break;
    case TrapAccess::LoadDouble:
        isValid = tryAppend(root, MoveDouble, address, number) && tryAppend(root, MoveDoubleTo64, number, result);
        break;
    case TrapAccess::Store64:
        isValid = tryAppend(root, Move, result, address);
        break;
    case TrapAccess::Store32:
        isValid = tryAppend(root, Move32, result, address);
        break;
    case TrapAccess::Store8:
        isValid = tryAppend(root, Store8, result, address);
        break;
    case TrapAccess::StoreDouble:
        isValid = tryAppend(root, Move64ToDouble, result, number) && tryAppend(root, MoveDouble, number, address);
        break;
    case TrapAccess::StoreImmediate:
        isValid = tryAppend(root, Move, Arg::imm(7), address);
        break;
    case TrapAccess::AddFromMemory:
        isValid = tryAppend(root, Add64, address, result);
        break;
    case TrapAccess::AddToMemory:
        isValid = tryAppend(root, Add64, result, address);
        break;
    case TrapAccess::AddDoubleFromMemory:
        isValid = tryAppend(root, Move64ToDouble, result, number) && tryAppend(root, AddDouble, address, number, sum) && tryAppend(root, MoveDoubleTo64, sum, result);
        break;
    case TrapAccess::CompareMemory:
        return tryAppendBranchAndReturns(graph, tryAppend(root, Branch32, Arg::relCond(MacroAssembler::Equal), address, Arg::imm(7)));
    case TrapAccess::TestMemory:
        return tryAppendBranchAndReturns(graph, tryAppend(root, BranchTest32, Arg::resCond(MacroAssembler::NonZero), address, Arg::bitImm(1)));
    }
    if (isValid)
        graph.appendReturnOfResult(root);
    return isValid;
}

static Vector<Inst*> instsWithEffects(Code& code)
{
    Vector<Inst*> result;
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            if (inst.kind.effects)
                result.append(&inst);
        }
    }
    return result;
}

static bool accesses(const Inst& inst, Tmp base, int32_t offset)
{
    return std::ranges::find(inst.args(), Arg::addr(base, offset)) != inst.args().end();
}

struct TrapRun {
    int64_t returned;
    std::array<int64_t, 4> memory;

    friend bool operator==(const TrapRun&, const TrapRun&) = default;
};

static TrapRun runOnMemory(const Compilation& compilation, int64_t word, int64_t operand)
{
    TrapRun run { 0, { 1, 2, word, 4 } };
    static_assert(offsetOfAccess == 2 * sizeof(int64_t));
    run.returned = invoke<int64_t>(compilation, run.memory.data(), operand);
    return run;
}

void testTrapFoldsIntoAccess()
{
    unsigned numberOfAccessesTried = 0;
    for (TrapAccess access : allTrapAccesses) {
        TrapGraph folded;
        folded.appendTrap(folded.root, folded.base);
        if (!tryAppendAccessAndReturn(folded, access))
            continue;
        ++numberOfAccessesTried;
        checkPassChangesCode(folded.code, foldTrapsIntoAccesses);

        Vector<Inst*> insts = instsWithEffects(folded.code);
        CHECK(insts.size() == 1);
        CHECK(accesses(*insts[0], folded.base, offsetOfAccess));
        CHECK(insts[0]->origin == folded.trapOrigin);
        CHECK(folded.numberOfFoldedTraps() == 1);

        TrapGraph unfolded(0);
        unfolded.appendTrap(unfolded.root, unfolded.base);
        CHECK(tryAppendAccessAndReturn(unfolded, access));
        auto foldedCode = compile(folded.proc);
        auto unfoldedCode = compile(unfolded.proc);
        for (int64_t word : { 7ll, 8ll, -1ll, 0x4010000080008081ll })
            CHECK(runOnMemory(*foldedCode, word, 0x4008000000000003ll) == runOnMemory(*unfoldedCode, word, 0x4008000000000003ll));
        CHECK(!unfolded.numberOfFoldedTraps());
    }
    CHECK(numberOfAccessesTried >= 10);
}

enum class TrapObstacle : uint8_t {
    None,
    RegisterInstructionsBetween,
    ControlIndependentLoadBetween,
    AccessEndsBeyondReach,
    AccessBelowBase,
    AccessThroughOtherBase,
    IndexedAccess,
    AddressIsOnlyComputed,
    TwoMemoryOperands,
    AccessHasEffects,
    AccessInNextBlock,
    TrapBeyondReach,
    LoadedValueIsUsed,
    LoadBetween,
    ControlDependentLoadBetween,
    StoreBetween,
    PatchBetween,
    BaseChangesBetween,
    DivisionBetween,
};

static constexpr TrapObstacle allTrapObstacles[] = {
    TrapObstacle::None, TrapObstacle::RegisterInstructionsBetween, TrapObstacle::ControlIndependentLoadBetween, TrapObstacle::AccessEndsBeyondReach, TrapObstacle::AccessBelowBase, TrapObstacle::AccessThroughOtherBase,
    TrapObstacle::IndexedAccess, TrapObstacle::AddressIsOnlyComputed, TrapObstacle::TwoMemoryOperands, TrapObstacle::AccessHasEffects, TrapObstacle::AccessInNextBlock,
    TrapObstacle::TrapBeyondReach, TrapObstacle::LoadedValueIsUsed, TrapObstacle::LoadBetween, TrapObstacle::ControlDependentLoadBetween, TrapObstacle::StoreBetween, TrapObstacle::PatchBetween,
    TrapObstacle::BaseChangesBetween, TrapObstacle::DivisionBetween,
};

static bool tryAppendBetweenTrapAndAccess(TrapGraph& graph, TrapObstacle obstacle)
{
    BasicBlock* root = graph.root;
    Tmp other = graph.code.newTmp(GP);
    switch (obstacle) {
    case TrapObstacle::RegisterInstructionsBetween:
        return tryAppend(root, Move, Arg::imm(3), other) && tryAppend(root, Add64, other, graph.result);
    case TrapObstacle::LoadBetween:
        return tryAppend(root, Move, Arg::addr(graph.result), other);
    case TrapObstacle::ControlIndependentLoadBetween:
    case TrapObstacle::ControlDependentLoadBetween: {
        auto* origin = graph.proc.add<B3::MemoryValue>(B3::Load, B3::Int64, B3::Origin(), graph.proc.addConstant(B3::Origin(), B3::Int64, 0));
        origin->setControlDependent(obstacle == TrapObstacle::ControlDependentLoadBetween);
        root->append(Move, origin, Arg::addr(graph.result), other);
        return true;
    }
    case TrapObstacle::StoreBetween:
        return tryAppend(root, Move, graph.base, Arg::addr(graph.result));
    case TrapObstacle::PatchBetween:
        appendCheckedPatch(root, addEmptyPatchpoint(graph.proc), patchpointSpecialOf(graph.code));
        return true;
    case TrapObstacle::BaseChangesBetween:
        return tryAppend(root, Add64, Arg::imm(8), graph.base);
    case TrapObstacle::DivisionBetween:
#if CPU(X86_64)
        return tryAppend(root, Move, graph.result, Tmp(X86Registers::eax))
            && tryAppend(root, X86ConvertToQuadWord64, Tmp(X86Registers::eax), Tmp(X86Registers::edx))
            && tryAppend(root, X86Div64, Tmp(X86Registers::eax), Tmp(X86Registers::edx), graph.result);
#else
        return false;
#endif
    default:
        return true;
    }
}

static bool tryAppendObstructedAccess(TrapGraph& graph, TrapObstacle obstacle, BasicBlock* block)
{
    Tmp loaded = graph.code.newTmp(GP);
    switch (obstacle) {
    case TrapObstacle::AccessEndsBeyondReach:
        return tryAppend(block, Move, Arg::addr(graph.base, testTrapReach - 4), loaded);
    case TrapObstacle::AccessBelowBase:
        return tryAppend(block, Move, Arg::addr(graph.base, -8), loaded);
    case TrapObstacle::AccessThroughOtherBase:
        return tryAppend(block, Move, Arg::addr(graph.result, offsetOfAccess), loaded);
    case TrapObstacle::IndexedAccess:
        return tryAppend(block, Move, Arg::index(graph.base, graph.result, 8, 0), loaded);
    case TrapObstacle::AddressIsOnlyComputed:
        return tryAppend(block, Lea64, Arg::addr(graph.base, offsetOfAccess), loaded);
    case TrapObstacle::TwoMemoryOperands:
        return tryAppend(block, Move, Arg::addr(graph.base, offsetOfAccess), Arg::addr(graph.base, offsetOfAccess + 8), loaded);
    case TrapObstacle::AccessHasEffects:
        appendTrappingLoad(graph.proc, block, Arg::addr(graph.base, offsetOfAccess), loaded);
        return tryAppend(block, Add64, loaded, graph.result);
    default:
        return tryAppend(block, Move, Arg::addr(graph.base, offsetOfAccess), loaded);
    }
}

void testTrapStaysWithoutFoldableAccess()
{
    unsigned numberOfObstaclesTried = 0;
    for (TrapObstacle obstacle : allTrapObstacles) {
        TrapGraph graph;
        Tmp loaded = graph.appendTrap(graph.root, graph.base, obstacle == TrapObstacle::TrapBeyondReach ? testTrapReach : offsetOfTrap);
        if (!tryAppendBetweenTrapAndAccess(graph, obstacle))
            continue;
        BasicBlock* block = graph.root;
        if (obstacle == TrapObstacle::AccessInNextBlock) {
            block = graph.code.addBlock();
            appendJump(graph.root, block);
        }
        if (!tryAppendObstructedAccess(graph, obstacle, block))
            continue;
        if (obstacle == TrapObstacle::LoadedValueIsUsed)
            block->append(Add64, nullptr, loaded, graph.result);
        graph.appendReturnOfResult(block);
        ++numberOfObstaclesTried;

        // A jump chooses nothing, so the search goes on in the block that it leads to.
        bool folds = obstacle == TrapObstacle::None || obstacle == TrapObstacle::RegisterInstructionsBetween || obstacle == TrapObstacle::ControlIndependentLoadBetween || obstacle == TrapObstacle::AccessInNextBlock;
        if (folds)
            checkPassChangesCode(graph.code, foldTrapsIntoAccesses);
        else
            checkPassLeavesCodeUnchanged(graph.code, foldTrapsIntoAccesses);
        CHECK(graph.numberOfFoldedTraps() == folds);
        CHECK(accesses(*instsWithEffects(graph.code)[0], graph.base, offsetOfAccess) == folds);
    }
    CHECK(numberOfObstaclesTried >= 17);
}

void testConsecutiveTrapsFoldSeparately()
{
    TrapGraph graph;
    Tmp next = graph.code.newTmp(GP);
    graph.appendTrap(graph.root, graph.base);
    B3::Value* firstOrigin = graph.trapOrigin;
    graph.root->append(Move, nullptr, Arg::addr(graph.base, offsetOfAccess), next);
    graph.appendTrap(graph.root, next);
    B3::Value* secondOrigin = graph.trapOrigin;
    graph.root->append(Move, nullptr, Arg::addr(next, 8), graph.result);
    graph.appendTrap(graph.root, next);
    graph.appendReturnOfResult(graph.root);
    CHECK(firstOrigin != secondOrigin);

    checkPassChangesCode(graph.code, foldTrapsIntoAccesses);
    Vector<Inst*> insts = instsWithEffects(graph.code);
    CHECK(insts.size() == 3);
    CHECK(accesses(*insts[0], graph.base, offsetOfAccess) && insts[0]->origin == firstOrigin);
    CHECK(accesses(*insts[1], next, 8) && insts[1]->origin == secondOrigin);
    CHECK(accesses(*insts[2], next, offsetOfTrap) && insts[2]->origin == graph.trapOrigin);
    CHECK(graph.numberOfFoldedTraps() == 2);

    std::array<int64_t, 3> second { 0, 42, 0 };
    std::array<int64_t, 3> first { 0, 0, std::bit_cast<int64_t>(second.data()) };
    auto compilation = compile(graph.proc);
    CHECK(invoke<int64_t>(*compilation, first.data(), 0) == 42);
}

enum class IndexedAccess : uint8_t {
    SameBaseAndIndex,
    OtherIndex,
    OtherScale,
    OtherBase,
    WithoutIndex,
    IndexChangesBetween,
    BaseChangesBetween,
};

void testTrapThroughBaseAndIndexFoldsIntoAccessThroughBoth()
{
    for (IndexedAccess access : { IndexedAccess::SameBaseAndIndex, IndexedAccess::OtherIndex, IndexedAccess::OtherScale, IndexedAccess::OtherBase, IndexedAccess::WithoutIndex, IndexedAccess::IndexChangesBetween, IndexedAccess::BaseChangesBetween }) {
        TrapGraph graph;
        BasicBlock* root = graph.root;
        Tmp index = graph.result;
        Tmp other = graph.code.newTmp(GP);
        Tmp checked = graph.code.newTmp(GP);
        Tmp loaded = graph.code.newTmp(GP);
        Arg trap = Arg::index(graph.base, index, 8, offsetOfTrap);
        if (!Inst(Load8, nullptr, trap, checked).isValidForm(CPUFeatureSet::host()))
            return;

        root->append(Move, nullptr, index, other);
        graph.trapOrigin = appendTrappingLoad(graph.proc, root, trap, checked);
        if (access == IndexedAccess::IndexChangesBetween)
            root->append(Add64, nullptr, Arg::imm(1), index);
        if (access == IndexedAccess::BaseChangesBetween)
            root->append(Add64, nullptr, Arg::imm(8), graph.base);
        Arg address = Arg::index(graph.base, index, 8, offsetOfAccess);
        if (access == IndexedAccess::OtherIndex)
            address = Arg::index(graph.base, other, 8, offsetOfAccess);
        if (access == IndexedAccess::OtherScale)
            address = Arg::index(graph.base, index, 4, offsetOfAccess);
        if (access == IndexedAccess::OtherBase)
            address = Arg::index(other, index, 8, offsetOfAccess);
        if (access == IndexedAccess::WithoutIndex)
            address = Arg::addr(graph.base, offsetOfAccess);
        root->append(Move, nullptr, address, loaded);
        root->append(Move, nullptr, loaded, returnTmp());
        appendReturn(root);

        bool folds = access == IndexedAccess::SameBaseAndIndex;
        if (folds)
            checkPassChangesCode(graph.code, foldTrapsIntoAccesses);
        else
            checkPassLeavesCodeUnchanged(graph.code, foldTrapsIntoAccesses);
        CHECK(graph.numberOfFoldedTraps() == folds);
        Vector<Inst*> insts = instsWithEffects(graph.code);
        CHECK(insts.size() == 1);
        CHECK(insts[0]->origin == graph.trapOrigin);
        CHECK((insts[0]->kind.opcode == Move) == folds);
        if (!folds)
            continue;

        std::array<int64_t, 5> memory { 10, 11, 12, 13, 14 };
        static_assert(offsetOfAccess == 2 * sizeof(int64_t));
        auto compilation = compile(graph.proc);
        CHECK(invoke<int64_t>(*compilation, memory.data(), 0) == 12);
        CHECK(invoke<int64_t>(*compilation, memory.data(), 2) == 14);
    }
}

void testTrapReachOfZeroRunsNoStep()
{
    for (uint32_t trapReach : { 0, testTrapReach }) {
        TrapGraph graph(trapReach);
        graph.appendTrap(graph.root, graph.base);
        CHECK(tryAppendAccessAndReturn(graph, TrapAccess::Load64));
        prepareForGeneration(graph.code);
        CHECK(graph.numberOfFoldedTraps() == !!trapReach);
        CHECK(numberOfInsts(graph.code, Load8) == !trapReach);
    }
}

void testTrapStaysWithoutKeptValues()
{
    B3::Policy policy = TrapGraph::policyWithTrapReach(testTrapReach);
    policy.allocation.keepsValuesAfterLowering = false;
    TrapGraph graph(policy);
    if (graph.code.shouldPreserveB3Origins())
        return;
    graph.appendTrap(graph.root, graph.base);
    CHECK(tryAppendAccessAndReturn(graph, TrapAccess::Load64));
    checkPassLeavesCodeUnchanged(graph.code, foldTrapsIntoAccesses);
    CHECK(!graph.numberOfFoldedTraps());
}

#if OS(UNIX) && HAVE(MACHINE_CONTEXT)

class EffectPositionObserver final : public InstObserver {
public:
    void didGenerate(const Inst& inst, MacroAssembler::Label begin, MacroAssembler::Label) final
    {
        if (inst.kind.effects)
            m_positions.append(begin);
    }

    const Vector<MacroAssembler::Label>& positions() const { return m_positions; }

private:
    Vector<MacroAssembler::Label> m_positions;
};

static sigjmp_buf s_stateBeforeFault;
static const void* s_faultingInstruction;

static void recordFaultingInstruction(int, siginfo_t*, void* context)
{
    PlatformRegisters& registers = WTF::registersFromUContext(static_cast<ucontext_t*>(context));
    s_faultingInstruction = MachineContext::instructionPointer(registers)->untaggedPtr();
    siglongjmp(s_stateBeforeFault, 1);
}

static const void* findFaultingInstruction(const Compilation& compilation, intptr_t base)
{
    struct sigaction action { };
    action.sa_sigaction = recordFaultingInstruction;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    struct sigaction previousSegmentationAction;
    struct sigaction previousBusAction;
    CHECK(!sigaction(SIGSEGV, &action, &previousSegmentationAction));
    CHECK(!sigaction(SIGBUS, &action, &previousBusAction));

    s_faultingInstruction = nullptr;
    if (!sigsetjmp(s_stateBeforeFault, 1))
        invoke<int64_t>(compilation, base, static_cast<int64_t>(0));

    CHECK(!sigaction(SIGSEGV, &previousSegmentationAction, nullptr));
    CHECK(!sigaction(SIGBUS, &previousBusAction, nullptr));
    return s_faultingInstruction;
}

void testFoldedAccessFaultsAtFirstInstruction()
{
    constexpr intptr_t unmappedBase = 2;
    for (TrapAccess access : allTrapAccesses) {
        TrapGraph graph;
        graph.appendTrap(graph.root, graph.base);
        if (!tryAppendAccessAndReturn(graph, access))
            continue;
        prepareForGeneration(graph.code);
        CHECK(graph.numberOfFoldedTraps() == 1);

        EffectPositionObserver observer;
        graph.code.setInstObserver(&observer);
        CCallHelpers jit;
        generate(graph.code, jit);
        graph.code.setInstObserver(nullptr);
        CHECK(observer.positions().size() == 1);

        LinkBuffer linkBuffer(jit, nullptr);
        const void* startOfAccess = linkBuffer.locationOf<JITCompilationPtrTag>(observer.positions()[0]).untaggedPtr();
        Compilation compilation(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "testair compilation"), graph.proc.releaseByproducts());
        CHECK(findFaultingInstruction(compilation, unmappedBase) == startOfAccess);
    }
}

#endif // OS(UNIX) && HAVE(MACHINE_CONTEXT)
