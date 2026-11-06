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
#include "BytecodeGeneratorInlines.h"
#include "BytecodeLivenessAnalysisInlines.h"
#include "BytecodeRewriter.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "JSGenerator.h"
#include "Label.h"
#include "PreciseJumpTargetsInlines.h"
#include "PyBytecode.h"
#include "StrongInlines.h"
#include "UnlinkedCodeBlockGenerator.h"
#include "UnlinkedMetadataTableInlines.h"

namespace JSC {

struct YieldData {
    JSInstructionStream::Offset point { 0 };
    JSInstructionStream::Offset instructionBefore { 0 };
    VirtualRegister argument { 0 };
    FastBitVector liveness;
    bool found { false }; // The bytecode optimizer may have removed an unreachable op_yield, leaving a hole in the numbering.
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
        JSInstructionStream::Offset previous = 0;
        for (const auto& instruction : m_instructions) {
            JSInstructionStream::Offset instructionBefore = std::exchange(previous, instruction.offset());
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
                data.instructionBefore = instructionBefore;
                data.argument = bytecode.m_argument;
                data.found = true;
                break;
            }

            case op_py_generator_registers: {
                // The one that is there already takes the values of the parameters from the first slots.
                auto bytecode = instruction->as<OpPyGeneratorRegisters>();
                ASSERT(!bytecode.m_offset && !bytecode.m_isSave);
                m_firstPythonParameter = bytecode.m_first;
                m_pythonParameterCount = bytecode.m_count;
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
        }
    }

    struct Storage {
        Identifier identifier;
        unsigned identifierIndex;
        ScopeOffset scopeOffset;
    };

    // Registers to give a value, any value, at a place in the code: see GeneratorLivenessAnalysis::findRegistersWithoutValue().
    struct Reset {
        JSInstructionStream::Offset point;
        bool isForJumpsToo; // Otherwise only for what comes from the instruction before.
        Vector<VirtualRegister, 4> registers;
    };

    void run();

    BytecodeGraph& LEGACY_NODELETE graph() { return m_graph; }

    const Yields& LEGACY_NODELETE yields() const
    {
        return m_yields;
    }

    Yields& LEGACY_NODELETE yields()
    {
        return m_yields;
    }

    Vector<Reset>& LEGACY_NODELETE resets()
    {
        return m_resets;
    }

    JSInstructionStream::Ref LEGACY_NODELETE enterPoint() const
    {
        return m_instructions.at(m_enterPoint);
    }

    std::optional<GeneratorFrameData> LEGACY_NODELETE generatorFrameData() const
    {
        return m_generatorFrameData;
    }

    const JSInstructionStream& LEGACY_NODELETE instructions() const
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
        m_generatorFrameSymbolTable->add(NoLockingNecessary, identifier.impl(), SymbolTableEntry(VarOffset(scopeOffset)));

        Storage storage = {
            identifier,
            identifierIndex,
            scopeOffset
        };
        m_storages[index] = storage;
        return storage;
    }

    BytecodeGenerator& m_bytecodeGenerator;
    VirtualRegister m_firstPythonParameter;
    unsigned m_pythonParameterCount { 0 };
    JSInstructionStream::Offset m_enterPoint;
    std::optional<GeneratorFrameData> m_generatorFrameData;
    UnlinkedCodeBlockGenerator* m_codeBlock;
    JSInstructionStreamWriter& m_instructions;
    BytecodeGraph m_graph;
    Vector<std::optional<Storage>> m_storages;
    Yields m_yields;
    Vector<Reset> m_resets;
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

        for (YieldData& data : m_generatorification.yields()) {
            if (!data.found)
                continue;
            data.liveness = getLivenessInfoAtInstruction(codeBlock, instructions, m_generatorification.graph(), BytecodeIndex(m_generatorification.instructions().at(data.point).next().offset()));
        }

        if (codeBlock->isPython())
            findRegistersWithoutValue(codeBlock, instructions);
    }

private:
    // The first of the locals that a call leaves something in that is no value: the frame of the callee begins there.
    static std::optional<unsigned> firstLocalOverwrittenBy(const JSInstruction* instruction)
    {
        switch (instruction->opcodeID()) {
        case op_call:
            return instruction->as<OpCall>().m_argv - CallFrame::headerSizeInRegisters;
        case op_call_ignore_result:
            return instruction->as<OpCallIgnoreResult>().m_argv - CallFrame::headerSizeInRegisters;
        case op_py_call_spread:
            return instruction->as<OpPyCallSpread>().m_firstFree.toLocal();
        case op_py_for_iter:
            return instruction->as<OpPyForIter>().m_stackOffset - CallFrame::headerSizeInRegisters;
        case op_py_send:
            return instruction->as<OpPySend>().m_stackOffset - CallFrame::headerSizeInRegisters;
        case op_call_varargs:
        case op_tail_call:
        case op_tail_call_varargs:
        case op_call_direct_eval:
        case op_construct:
        case op_construct_varargs:
        case op_super_construct:
        case op_super_construct_varargs:
        case op_iterator_open:
        case op_iterator_next:
        case op_async_iterator_open:
        case op_async_iterator_next:
            RELEASE_ASSERT_NOT_REACHED(); // Python code has none of these.
        default:
            return std::nullopt;
        }
    }

    // What is in a register that is live at a yield goes into an object, where the collector takes it for a value. But the
    // analysis errs on the side of "live": it follows paths that are never taken, such as from an instruction that cannot
    // throw to a handler. So it may find a use for a register that is not in use at the yield at all, and what is in such a
    // register is whatever was last on that part of the stack: the header of the frame of a callee, say.
    //
    // A register that is really in use is never without a value, because the generator of the code does not put the frame of
    // a callee on top of it. So a register that is live where it has just lost its value, or never had one, is not in use
    // there, and may be given a value: at the start of the code, after a call, and where an exception that a call may have thrown
    // is caught. (A register that is live after a yield gets its value back. One that is not is written before the next
    // yield at which it is live, or it would be live after this one as well.)
    void findRegistersWithoutValue(UnlinkedCodeBlockGenerator* codeBlock, JSInstructionStreamWriter& instructions)
    {
        BytecodeGraph& graph = m_generatorification.graph();
        auto resetLiveRegisters = [&](JSInstructionStream::Offset point, bool isForJumpsToo, unsigned firstLocal, std::initializer_list<VirtualRegister> written) {
            BytecodeGeneratorification::Reset reset { point, isForJumpsToo, { } };
            getLivenessInfoAtInstruction(codeBlock, instructions, graph, BytecodeIndex(point)).forEachSetBit([&](size_t index) {
                VirtualRegister local = virtualRegisterForLocal(index);
                if (index >= firstLocal && std::ranges::find(written, local) == written.end())
                    reset.registers.append(local);
            });
            if (!reset.registers.isEmpty())
                m_generatorification.resets().append(WTF::move(reset));
        };

        // op_enter gives the variables a value.
        resetLiveRegisters(m_generatorification.enterPoint().next().offset(), true, codeBlock->numVars(), { });

        Vector<std::pair<JSInstructionStream::Offset, unsigned>> calls;
        for (const auto& instruction : instructions) {
            std::optional<unsigned> firstLocal = firstLocalOverwrittenBy(instruction.ptr());
            if (!firstLocal)
                continue;
            calls.append({ instruction.offset(), *firstLocal });
            VirtualRegister result;
            if (instruction->is<OpCall>())
                result = instruction->as<OpCall>().m_dst;
            else if (instruction->is<OpPyCallSpread>())
                result = instruction->as<OpPyCallSpread>().m_dst;
            else if (instruction->is<OpPyForIter>())
                result = instruction->as<OpPyForIter>().m_dst;
            else if (instruction->is<OpPySend>())
                result = instruction->as<OpPySend>().m_dst;
            resetLiveRegisters(instruction.next().offset(), false, *firstLocal, { result });
            // These two have made their call when they jump, too. What is above the registers in use here is not in use there
            // either, whoever else jumps there.
            if (instruction->is<OpPyForIter>() || instruction->is<OpPySend>()) {
                extractStoredJumpTargetsForInstruction(codeBlock, instruction, [&](int32_t relativeOffset) {
                    resetLiveRegisters(instruction.offset() + relativeOffset, true, *firstLocal, { result });
                });
            }
        }

        for (size_t i = 0; i < codeBlock->numberOfExceptionHandlers(); ++i) {
            UnlinkedHandlerInfo& handler = codeBlock->exceptionHandler(i);
            unsigned firstLocal = std::numeric_limits<unsigned>::max();
            for (auto it = std::ranges::lower_bound(calls, handler.start, { }, [](auto& call) { return call.first; }); it != calls.end() && it->first < handler.end; ++it)
                firstLocal = std::min(firstLocal, it->second);
            if (firstLocal == std::numeric_limits<unsigned>::max())
                continue;
            auto target = instructions.at(handler.target);
            auto bytecode = target->as<OpPyCatch>();
            // Where the handler goes on, which only the jump behind op_py_catch leads to. Nothing comes between these two:
            // Python::whereHandlerBegins().
            extractStoredJumpTargetsForInstruction(codeBlock, target.next(), [&](int32_t relativeOffset) {
                resetLiveRegisters(target.next().offset() + relativeOffset, true, firstLocal, { bytecode.m_exception, bytecode.m_previous });
            });
        }
    }

    BytecodeGeneratorification& m_generatorification;
};

void BytecodeGeneratorification::run()
{
    // We calculate the liveness at each merge point. This gives us the information which registers should be saved and resumed conservatively.

    VM& vm = m_bytecodeGenerator.vm();
    bool isPython = m_codeBlock->isPython();
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
        // In Python the frame is set up first. Only a generator that starts gets as far as the switch, for op_py_generator_enter goes
        // straight to where one is resumed. What comes next is for throw(), which passes a state that there is not.
        JSInstructionStream::Offset start = nextToEnterPoint.offset();
        JSInstructionStream::Offset other = start;
        if (isPython) {
            auto startPoint = m_instructions.at(m_bytecodeGenerator.pythonGeneratorStartPoint());
            RELEASE_ASSERT(startPoint->is<OpPyJfalse>());
            extractStoredJumpTargetsForInstruction(m_codeBlock, startPoint, [&](int32_t relative) {
                start = startPoint.offset() + relative;
            });
            other = startPoint.next().offset();
        }
        jumpTable.add(0, start);
        // A yield the bytecode optimizer removed as unreachable can never be resumed at; give its state the default
        // target so the table has no holes (a 0 entry would be rebased into a jump to op_enter).
        for (unsigned i = 0; i < m_yields.size(); ++i)
            jumpTable.add(i + 1, m_yields[i].found ? m_yields[i].point : other);
        jumpTable.m_defaultOffset = other;

        auto emitSwitch = [&] (BytecodeRewriter::Fragment& fragment) {
            fragment.appendInstruction<OpSwitchImm>(switchTableIndex, state);
        };
        if (isPython) {
            rewriter.insertFragmentBefore(nextToEnterPoint, [&] (BytecodeRewriter::Fragment& fragment) {
                fragment.appendInstruction<OpPyGeneratorEnter>();
            });
            rewriter.replaceBytecodeWithFragment(m_instructions.at(m_bytecodeGenerator.pythonGeneratorStartPoint()), emitSwitch);
        } else
            rewriter.insertFragmentBefore(nextToEnterPoint, emitSwitch);
    }

    // Before what follows: where one of these is right before a yield, it has to come before the registers are saved.
    VirtualRegister anyValue = m_resets.isEmpty() ? VirtualRegister() : m_bytecodeGenerator.pythonConstant(jsUndefined())->virtualRegister();
    for (const Reset& reset : m_resets) {
        auto emitReset = [&] (BytecodeRewriter::Fragment& fragment) {
            for (VirtualRegister local : reset.registers)
                fragment.appendInstruction<OpMov>(local, anyValue);
        };
        auto instruction = m_instructions.at(reset.point);
        if (reset.isForJumpsToo)
            rewriter.insertFragmentAfter(instruction, emitReset);
        else
            rewriter.insertFragmentBefore(instruction, emitReset);
    }

    // A Python generator has a slot for each register that is ever saved, the parameters first. Registers that follow one another
    // are saved and restored with one instruction, so their slots follow one another as well.
    Vector<int> slotOfLocal;
    if (isPython) {
        unsigned slotCount = 0;
        auto allot = [&](size_t local) {
            while (slotOfLocal.size() <= local)
                slotOfLocal.append(-1);
            if (slotOfLocal[local] < 0)
                slotOfLocal[local] = slotCount++;
        };
        for (unsigned i = 0; i < m_pythonParameterCount; ++i)
            allot(m_firstPythonParameter.toLocal() + i);
        auto blockPieces = Python::blockPiecesIn(m_codeBlock->pythonMarks());
        for (YieldData& data : m_yields) {
            if (!data.found)
                continue;
            for (auto& piece : blockPieces) {
                if (!piece.contains(data.point))
                    continue;
                piece.forEachRegister([&](VirtualRegister held) {
                    data.liveness[held.toLocal()] = true;
                });
            }
            // All the variables are kept, needed or not: gi_frame.f_locals shows them. And a generator that is resumed
            // counts on it (llint_python_generator_resume).
            for (unsigned local = m_bytecodeGenerator.firstPythonVariable(); local < m_codeBlock->numVars(); ++local)
                data.liveness[local] = true;
            data.liveness.forEachSetBit(allot);
        }
        // In the order of the registers, whichever yield they were first seen at.
        unsigned next = m_pythonParameterCount;
        for (size_t local = 0; local < slotOfLocal.size(); ++local) {
            if (slotOfLocal[local] >= static_cast<int>(m_pythonParameterCount))
                slotOfLocal[local] = next++;
        }
        ASSERT(next == slotCount);
        m_bytecodeGenerator.setPythonGeneratorSlotCount(slotCount);
    }
    auto forEachRunOfLiveRegisters = [&](const YieldData& data, const auto& function) {
        size_t first = 0;
        unsigned count = 0;
        data.liveness.forEachSetBit([&](size_t index) {
            if (count && index == first + count && slotOfLocal[index] == slotOfLocal[first] + static_cast<int>(count)) {
                ++count;
                return;
            }
            if (count)
                function(virtualRegisterForLocal(first), count, static_cast<unsigned>(slotOfLocal[first]));
            first = index;
            count = 1;
        });
        if (count)
            function(virtualRegisterForLocal(first), count, static_cast<unsigned>(slotOfLocal[first]));
    };

    for (const YieldData& data : m_yields) {
        if (!data.found)
            continue;
        VirtualRegister scope = virtualRegisterForArgumentIncludingThis(static_cast<int32_t>(JSGenerator::Argument::Frame));

        auto instruction = m_instructions.at(data.point);
        if (isPython) {
            // See BytecodeGenerator::emitPythonYieldPoint().
            auto placeholder = m_instructions.at(data.instructionBefore);
            RELEASE_ASSERT(placeholder->opcodeID() == op_nop);
            rewriter.replaceBytecodeWithFragment(placeholder, [&] (BytecodeRewriter::Fragment& fragment) {
                struct Run {
                    VirtualRegister first;
                    unsigned count { 0 };
                    unsigned offset { 0 };
                };
                Run last { data.argument };
                forEachRunOfLiveRegisters(data, [&](VirtualRegister first, unsigned count, unsigned offset) {
                    if (last.count)
                        fragment.appendInstruction<OpPyGeneratorRegisters>(last.first, last.count, last.offset, true);
                    last = { first, count, offset };
                });
                fragment.appendInstruction<OpPyGeneratorSuspend>(data.argument, last.first, last.count, last.offset);
            });
            rewriter.replaceBytecodeWithFragment(instruction, [&] (BytecodeRewriter::Fragment& fragment) {
                forEachRunOfLiveRegisters(data, [&](VirtualRegister first, unsigned count, unsigned offset) {
                    fragment.appendInstruction<OpPyGeneratorRegisters>(first, count, offset, false);
                });
            });
            continue;
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
