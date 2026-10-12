/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include "Options.h"

namespace JSC { namespace AOT { namespace Typed {

// An environment lies in the frame of the function that makes it if the pointer to it only ever goes down the stack: all that is done with it, in the whole program as it is compiled, is to
// read and write its variables and to hand it to a function that is called directly. Then it is in no cell, in no closure and in no block parameter, so nothing has it once the function that
// made it has returned or was unwound, nor once the same place makes the next one. What is live across a suspension of a coroutine is stored to the coroutine's frame, which is a cell: so no
// such environment is.
//
// It is the layout that says so, since a function that is handed one has nothing else to go by: Layout::isInFrame. There is no cell: no header, no write barrier. The collector finds what the
// variables refer to as it finds what a local variable refers to, by looking at every word of the stack: so references are kept whole, see Placement::findTopLevelActivations().
//
// An operation that is not listed here keeps the environments it sees on the heap.
void findEnvironmentsInFrames(Program& program)
{
    // These look at the header of whatever is a cell.
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
        return;
    static constexpr unsigned maxFields = 32;
    enum : uint8_t { Unknown, No, Yes };
    Vector<uint8_t> admitsEnvironment;
    admitsEnvironment.fill(Unknown, program.types.size());
    Vector<uint8_t> isMade(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> leavesStack(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));

    auto scan = [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = program.functions[i];
        auto keepOnHeap = [&](ValueID id, ASCIILiteral operation, const Value& at) {
            TypeID type = function.at(id).type;
            if (!type)
                return;
            uint8_t known = WTF::atomicLoad(&admitsEnvironment[type.index()], std::memory_order_relaxed);
            if (known == Unknown) {
                known = std::ranges::any_of(program.at(type).views, [&](LayoutID view) {
                    return program.at(view).kind == LayoutKind::Environment;
                }) ? Yes : No;
                WTF::atomicStore(&admitsEnvironment[type.index()], known, std::memory_order_relaxed);
            }
            if (known == No)
                return;
            for (LayoutID view : program.at(type).views) {
                if (!WTF::atomicLoad(&leavesStack[view.index()], std::memory_order_relaxed) && !WTF::atomicExchange(&leavesStack[view.index()], static_cast<uint8_t>(1), std::memory_order_relaxed) && program.notesAllocations)
                    program.whyEnvironmentsAreOnHeap.add(view.index(), Program::UseOfEnvironment { operation, FunctionID(i), at.inlineFrame, at.position });
            }
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            if (b) {
                for (ValueID id : block.parameters)
                    keepOnHeap(id, "a block parameter"_s, function.at(id));
            }
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                auto keep = [&](ValueID kept) {
                    keepOnHeap(kept, nameOf(value.opcode), value);
                };
                switch (value.opcode) {
                case Opcode::NewObjectWithShape:
                    WTF::atomicStore(&isMade[value.layout().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
                    value.forEachChild(keep);
                    break;
                case Opcode::GetField:
                    keep(id);
                    break;
                case Opcode::PutField:
                    keep(value.child(1));
                    break;
                case Opcode::CallDirect:
                    keep(id);
                    break;
                case Opcode::AssertNotAllocated:
                    break;
                default:
                    keep(id);
                    value.forEachChild(keep);
                    break;
                }
            }
            if (block.terminator.value)
                keepOnHeap(block.terminator.value, "Return"_s, function.at(block.terminator.value));
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    keepOnHeap(argument, "a block parameter"_s, function.at(argument));
            }
        }
    };
    if (program.notesAllocations) {
        for (unsigned i = 0; i < program.functions.size(); ++i)
            scan(i);
    } else
        forEachIndexInParallel(program.functions.size(), scan);

    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        layout.isInFrame = layout.kind == LayoutKind::Environment && isMade[i] && !leavesStack[i] && layout.fields.size() <= maxFields;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
