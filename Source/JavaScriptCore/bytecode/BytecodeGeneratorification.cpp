/*
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>
 * Copyright (C) 2016-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BytecodeGeneratorification.h"

#include "BytecodeDumper.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "BytecodeLivenessAnalysisInlines.h"
#include "BytecodeRewriter.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "JSGenerator.h"
#include "Label.h"
#include "StrongInlines.h"
#include "UnlinkedCodeBlockGenerator.h"
#include "UnlinkedMetadataTableInlines.h"

namespace JSC {

struct YieldData {
    JSInstructionStream::Offset point { 0 };
    VirtualRegister argument { 0 };
    FastBitVector liveness;
    // SPEC-ungil §N.5: offset of the op_put_internal_field(State, SuspendedX)
    // emitted by BytecodeGenerator::emitGeneratorStateChange immediately
    // before this op_yield, when it could be identified. gilOffProcess, that
    // store is the resume-claim UNCLAIM and must be the LAST publication of
    // the suspension — run() moves it after the frame-save sequence (the
    // landed order stores the suspend state BEFORE the OpPutToScope saves, so
    // a rival claimant could resume into a half-saved frame).
    std::optional<JSInstructionStream::Offset> stateStorePoint;
};

class BytecodeGeneratorification {
public:
    typedef Vector<YieldData> Yields;

    struct GeneratorFrameData {
        JSInstructionStream::Offset m_point;
        VirtualRegister m_dst;
        VirtualRegister m_scope;
        VirtualRegister m_symbolTable;
        VirtualRegister m_initialValue;
    };

    BytecodeGeneratorification(BytecodeGenerator& bytecodeGenerator, UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions, SymbolTable* generatorFrameSymbolTable, int generatorFrameSymbolTableIndex)
        : m_bytecodeGenerator(bytecodeGenerator)
        , m_codeBlock(codeBlock)
        , m_instructions(instructions)
        , m_graph(m_codeBlock, m_instructions)
        , m_generatorFrameSymbolTable(codeBlock->vm(), generatorFrameSymbolTable)
        , m_generatorFrameSymbolTableIndex(generatorFrameSymbolTableIndex)
    {
        std::optional<JSInstructionStream::Offset> previousPutInternalFieldPoint;
        for (const auto& instruction : m_instructions) {
            switch (instruction->opcodeID()) {
            case op_enter: {
                m_enterPoint = instruction.offset();
                break;
            }

            case op_yield: {
                auto bytecode = instruction->as<OpYield>();
                unsigned liveCalleeLocalsIndex = bytecode.m_yieldPoint;
                if (liveCalleeLocalsIndex >= m_yields.size())
                    m_yields.grow(liveCalleeLocalsIndex + 1);
                YieldData& data = m_yields[liveCalleeLocalsIndex];
                data.point = instruction.offset();
                data.argument = bytecode.m_argument;
                // emitYieldPoint emits the suspend-state store immediately
                // before op_yield (only alignment nops can intervene, on
                // NEEDS_ALIGNED_ACCESS targets — 32-bit-only, where gilOff
                // fail-stops at VM entry; run() fail-stops gilOff if this
                // stays nullopt). Validated again in run().
                data.stateStorePoint = previousPutInternalFieldPoint;
                break;
            }

            case op_create_generator_frame_environment: {
                auto bytecode = instruction->as<OpCreateGeneratorFrameEnvironment>();
                GeneratorFrameData data;
                data.m_point = instruction.offset();
                data.m_dst = bytecode.m_dst;
                data.m_scope = bytecode.m_scope;
                data.m_symbolTable = bytecode.m_symbolTable;
                data.m_initialValue = bytecode.m_initialValue;
                m_generatorFrameData = WTF::move(data);
                break;
            }

            default:
                break;
            }
            previousPutInternalFieldPoint = instruction->opcodeID() == op_put_internal_field ? std::optional<JSInstructionStream::Offset>(instruction.offset()) : std::nullopt;
        }
    }

    struct Storage {
        Identifier identifier;
        unsigned identifierIndex;
        ScopeOffset scopeOffset;
    };

    void run();

    BytecodeGraph& NODELETE graph() { return m_graph; }

    const Yields& NODELETE yields() const
    {
        return m_yields;
    }

    Yields& NODELETE yields()
    {
        return m_yields;
    }

    JSInstructionStream::Ref NODELETE enterPoint() const
    {
        return m_instructions.at(m_enterPoint);
    }

    std::optional<GeneratorFrameData> NODELETE generatorFrameData() const
    {
        return m_generatorFrameData;
    }

    const JSInstructionStream& NODELETE instructions() const
    {
        return m_instructions;
    }

private:
    Storage storageForGeneratorLocal(VM& vm, unsigned index)
    {
        // We assign a symbol to a register. There is one-on-one corresponding between a register and a symbol.
        // By doing so, we allocate the specific storage to save the given register.
        // This allow us not to save all the live registers even if the registers are not overwritten from the previous resuming time.
        // It means that, the register can be retrieved even if the immediate previous op_save does not save it.

        if (m_storages.size() <= index)
            m_storages.grow(index + 1);
        if (std::optional<Storage> storage = m_storages[index])
            return *storage;

        Identifier identifier = Identifier::from(vm, index);
        unsigned identifierIndex = m_codeBlock->numberOfIdentifiers();
        m_codeBlock->addIdentifier(identifier);
        ScopeOffset scopeOffset = m_generatorFrameSymbolTable->takeNextScopeOffset(NoLockingNecessary);
        m_generatorFrameSymbolTable->set(NoLockingNecessary, identifier.impl(), SymbolTableEntry(VarOffset(scopeOffset)));

        Storage storage = {
            identifier,
            identifierIndex,
            scopeOffset
        };
        m_storages[index] = storage;
        return storage;
    }

    BytecodeGenerator& m_bytecodeGenerator;
    JSInstructionStream::Offset m_enterPoint;
    std::optional<GeneratorFrameData> m_generatorFrameData;
    UnlinkedCodeBlockGenerator* m_codeBlock;
    JSInstructionStreamWriter& m_instructions;
    BytecodeGraph m_graph;
    Vector<std::optional<Storage>> m_storages;
    Yields m_yields;
    Strong<SymbolTable> m_generatorFrameSymbolTable;
    int m_generatorFrameSymbolTableIndex;
};

class GeneratorLivenessAnalysis : public BytecodeLivenessPropagation {
public:
    GeneratorLivenessAnalysis(BytecodeGeneratorification& generatorification)
        : m_generatorification(generatorification)
    {
    }

    void run(UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions)
    {
        // Perform modified liveness analysis to determine which locals are live at the merge points.
        // This produces the conservative results for the question, "which variables should be saved and resumed?".

        runLivenessFixpoint(codeBlock, instructions, m_generatorification.graph());

        for (YieldData& data : m_generatorification.yields())
            data.liveness = getLivenessInfoAtInstruction(codeBlock, instructions, m_generatorification.graph(), BytecodeIndex(m_generatorification.instructions().at(data.point).next().offset()));
    }

private:
    BytecodeGeneratorification& m_generatorification;
};

void BytecodeGeneratorification::run()
{
    // We calculate the liveness at each merge point. This gives us the information which registers should be saved and resumed conservatively.

    VM& vm = m_bytecodeGenerator.vm();
    {
        GeneratorLivenessAnalysis pass(*this);
        pass.run(m_codeBlock, m_instructions);
    }

    BytecodeRewriter rewriter(m_bytecodeGenerator, m_graph, m_codeBlock, m_instructions);

    // Setup the global switch for the generator.
    {
        auto nextToEnterPoint = enterPoint().next();
        unsigned switchTableIndex = m_codeBlock->numberOfUnlinkedSwitchJumpTables();
        VirtualRegister state = virtualRegisterForArgumentIncludingThis(static_cast<int32_t>(JSGenerator::Argument::State));
        auto& jumpTable = m_codeBlock->addUnlinkedSwitchJumpTable();
        jumpTable.m_min = 0;
        jumpTable.m_branchOffsets = FixedVector<int32_t>(m_yields.size() + 1);
        std::ranges::fill(jumpTable.m_branchOffsets, 0);
        jumpTable.add(0, nextToEnterPoint.offset());
        for (unsigned i = 0; i < m_yields.size(); ++i)
            jumpTable.add(i + 1, m_yields[i].point);
        jumpTable.m_defaultOffset = nextToEnterPoint.offset();

        rewriter.insertFragmentBefore(nextToEnterPoint, [&] (BytecodeRewriter::Fragment& fragment) {
            fragment.appendInstruction<OpSwitchImm>(switchTableIndex, state);
        });
    }

    // SPEC-ungil §N.5 (gilOffProcess only; derivation identical to
    // JSCConfig.h Config::finalize() — Options are finalized long before any
    // code is generatorified, and the byte itself may not be latched yet
    // during the first VM's construction): the suspend-state store is the
    // resume-claim UNCLAIM. The landed emission order (state store, THEN the
    // OpPutToScope frame saves inserted below) lets a rival thread claim the
    // generator and resume into a half-saved frame. Move the store after the
    // saves, directly before the op_ret, so the unclaim is the suspension's
    // LAST publication. GIL-on/flag-off keeps the landed order verbatim.
    //
    // Program order alone is NOT the ruled happens-before: the r15 F1
    // amendment (UNGIL-HANDOUT §N.5) requires the Running->SuspendedX unclaim
    // to be a store-RELEASE in ALL tiers, pairing with claimGeneratorResume's
    // acquire CAS. That release half lives in the op_put_internal_field
    // implementations themselves: gilOffProcess, every tier emits a
    // store-store fence before the internal-field store (LLInt
    // LowLevelInterpreter64.asm, Baseline JITPropertyAccess.cpp, DFG
    // SpeculativeJIT::compilePutInternalField, FTL compilePutInternalField),
    // so the relocated store below publishes the frame saves on arm64 and is
    // a compiler barrier in the optimizing tiers.
    bool gilOffProcess = Options::useJSThreads() && !Options::useThreadGIL()
        && Options::useVMLite() && Options::useSharedAtomStringTable() && Options::useSharedGCHeap();

    for (const YieldData& data : m_yields) {
        VirtualRegister scope = virtualRegisterForArgumentIncludingThis(static_cast<int32_t>(JSGenerator::Argument::Frame));

        auto instruction = m_instructions.at(data.point);

        std::optional<OpPutInternalField> stateStoreToMove;
        if (gilOffProcess) {
            // FAIL CLOSED (no silent fallback to the known-vulnerable
            // pre-save unclaim order): gilOffProcess only runs on 64-bit
            // targets (32-bit fail-stops at VM entry, AB-1), and on 64-bit
            // emitYieldPoint emits the state store immediately before
            // op_yield (the NEEDS_ALIGNED_ACCESS nop interleaving is
            // 32-bit-only). If a future BytecodeGenerator change inserts an
            // instruction into that window, this deterministic compile-time
            // crash is the diagnostic — not a silently re-opened race.
            RELEASE_ASSERT(data.stateStorePoint, data.point);
            auto stateStoreInstruction = m_instructions.at(*data.stateStorePoint);
            auto put = stateStoreInstruction->as<OpPutInternalField>();
            // Validate this really is emitGeneratorStateChange's store: the
            // generator register (the Generator ARGUMENT for generator /
            // async-generator / module body modes, a LOCAL VAR for wrapper-
            // less async functions — BytecodeGenerator.cpp AsyncFunctionMode
            // arm), field State (index 0 for JSGenerator, JSAsyncGenerator
            // AND AbstractModuleRecord — static_asserts in
            // emitGeneratorStateChange/JSGenerator.h). Any mismatch is a
            // structural change to emitYieldPoint and fail-stops above's
            // rationale applies.
            static_assert(!static_cast<unsigned>(JSGenerator::Field::State));
            RELEASE_ASSERT(put.m_base == m_bytecodeGenerator.generatorRegister()->virtualRegister(), put.m_base.offset());
            RELEASE_ASSERT(!put.m_index, put.m_index);
            stateStoreToMove = put;
            // Remove the early store; an identical one is appended after
            // the saves below. (A jump targeting the removed instruction
            // re-resolves to the save fragment — same suspension, so the
            // behavior is unchanged.)
            rewriter.replaceBytecodeWithFragment(stateStoreInstruction, [&] (BytecodeRewriter::Fragment&) { });
        }

        // Emit save sequence.
        rewriter.insertFragmentBefore(instruction, [&] (BytecodeRewriter::Fragment& fragment) {
            data.liveness.forEachSetBit([&](size_t index) {
                VirtualRegister operand = virtualRegisterForLocal(index);
                Storage storage = storageForGeneratorLocal(vm, index);

                fragment.appendInstruction<OpPutToScope>(
                    scope, // scope
                    storage.identifierIndex, // identifier
                    operand, // value
                    GetPutInfo(DoNotThrowIfNotFound, ResolvedClosureVar, InitializationMode::NotInitialization, m_bytecodeGenerator.ecmaMode()), // info
                    SymbolTableOrScopeDepth::symbolTable(VirtualRegister { m_generatorFrameSymbolTableIndex }), // symbol table constant index
                    storage.scopeOffset.offset() // scope offset
                );
            });

            // §N.5 unclaim: the relocated suspend-state store, ordered after
            // every frame save (gilOffProcess only; see above).
            if (stateStoreToMove)
                fragment.appendInstruction<OpPutInternalField>(stateStoreToMove->m_base, stateStoreToMove->m_index, stateStoreToMove->m_value);

            // Insert op_ret just after save sequence.
            fragment.appendInstruction<OpRet>(data.argument);
        });

        // Emit resume sequence.
        rewriter.replaceBytecodeWithFragment(instruction, [&] (BytecodeRewriter::Fragment& fragment) {
            data.liveness.forEachSetBit([&](size_t index) {
                VirtualRegister operand = virtualRegisterForLocal(index);
                Storage storage = storageForGeneratorLocal(vm, index);

                fragment.appendInstruction<OpGetFromScope>(
                    operand, // dst
                    scope, // scope
                    storage.identifierIndex, // identifier
                    GetPutInfo(DoNotThrowIfNotFound, ResolvedClosureVar, InitializationMode::NotInitialization, m_bytecodeGenerator.ecmaMode()), // info
                    0, // local scope depth
                    storage.scopeOffset.offset(), // scope offset
                    m_bytecodeGenerator.nextValueProfileIndex()
                );
            });
        });
    }

    if (m_generatorFrameData) {
        auto instruction = m_instructions.at(m_generatorFrameData->m_point);
        rewriter.replaceBytecodeWithFragment(instruction, [&] (BytecodeRewriter::Fragment& fragment) {
            if (!m_generatorFrameSymbolTable->scopeSize()) {
                // This will cause us to put jsUndefined() into the generator frame's scope value.
                fragment.appendInstruction<OpMov>(m_generatorFrameData->m_dst, m_generatorFrameData->m_initialValue);
            } else
                fragment.appendInstruction<OpCreateLexicalEnvironment>(m_generatorFrameData->m_dst, m_generatorFrameData->m_scope, m_generatorFrameData->m_symbolTable, m_generatorFrameData->m_initialValue);
        });
    }

    rewriter.execute();
}

void performGeneratorification(BytecodeGenerator& bytecodeGenerator, UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions, SymbolTable* generatorFrameSymbolTable, int generatorFrameSymbolTableIndex)
{
    if (Options::dumpBytecodesBeforeGeneratorification()) [[unlikely]] {
        dataLogLn("Bytecodes before generatorification");
        CodeBlockBytecodeDumper<UnlinkedCodeBlockGenerator>::dumpBlock(codeBlock, instructions, WTF::dataFile());
    }

    BytecodeGeneratorification pass(bytecodeGenerator, codeBlock, instructions, generatorFrameSymbolTable, generatorFrameSymbolTableIndex);
    pass.run();

    if (Options::dumpBytecodesBeforeGeneratorification()) [[unlikely]] {
        dataLogLn("Bytecodes after generatorification");
        CodeBlockBytecodeDumper<UnlinkedCodeBlockGenerator>::dumpBlock(codeBlock, instructions, WTF::dataFile());
    }
}

} // namespace JSC
