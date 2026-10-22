/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreatedObjectLayouts.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Creation {
    FunctionID function;
    ValueID call;
    LayoutID layout;
};

} // anonymous namespace

static bool createsObjectBelowItsArgument(const BuiltinTable& builtins, const Function& function, const Value& operation)
{
    if (operation.opcode != Opcode::Call || operation.expectedResult() || operation.variadicOperands().size() != 1)
        return false;
    const Value& callee = function.at(definitionThroughUpcasts(function, operation.calleeOperand()));
    return callee.opcode == Opcode::Builtin && builtins.canonicalID(callee.builtin()) == Builtins::ObjectCreate;
}

static void becomeLiterals(const Program& program, BuildContext& build, FunctionID id, Function& function, TypeArena& types, std::span<const Creation> creations)
{
    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID call = function.at(block).values[index];
            auto creation = std::ranges::find(creations, call, &Creation::call);
            if (creation == creations.end())
                continue;
            ValueID prototype = function.at(call).variadicOperands()[0];
            builder.setOrigin(function.at(call).origin());

            Value allocation = IRBuilder::make(Opcode::NewObject);
            allocation.setLayout(creation->layout);
            ValueID object = builder.insertBefore({ block, index }, WTF::move(allocation));

            Value link = IRBuilder::make(Opcode::SetLiteralPrototype, { object, prototype, function.at(call).calleeOperand(), function.at(call).thisValueOperand() });
            link.setRefusesOtherValues(true);
            builder.insertBefore({ block, index }, WTF::move(link));

            Value& finished = function.edit(call);
            Origin origin = finished.origin();
            finished = IRBuilder::make(Opcode::FinishObject, { object });
            finished.setOrigin(origin);
        }
    }
    builder.applyInsertions();
}

void giveLayoutsToCreatedObjects(Program& program, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    // They come in the order of the functions, so a layout gets the same number on any number of threads.
    const BuiltinTable& builtins = program.builtins();
    // Where the reader has noted which functions name Object.create, no other function as read is looked at. The functions that were added since follow them.
    Vector<uint32_t> candidates;
    if (const Program::BodiesAsRead* bodies = program.bodiesAsRead()) {
        for (FunctionID id : bodies->functionsThatNameObjectCreate)
            candidates.append(id.index());
        for (uint32_t index = program.numberOfFunctionsAsRead(); index < program.functions.size(); ++index)
            candidates.append(index);
    } else {
        candidates = Vector<uint32_t>(program.functions.size(), [](size_t index) {
            return static_cast<uint32_t>(index);
        });
    }
    Vector<Creation> creations = collectInParallel<Creation>(build.parallelLoops(), candidates.size(), [&](unsigned position, Vector<Creation>& found) {
        uint32_t index = candidates[position];
        const Function& function = program.functions[index];
        // The model of a built-in says what the built-in does and is never run: what it answers is made by other code, with no layout of the model.
        if (!function.module || function.role == FunctionRole::Model)
            return;
        for (const Block& block : function.blocks) {
            // A tail call stays a call.
            ValueID returnedCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.operand(0) : ValueID();
            for (ValueID id : block.values) {
                if (id != returnedCall && createsObjectBelowItsArgument(builtins, function, function.at(id)))
                    found.append({ FunctionID(index), id, { } });
            }
        }
    });
    // From here on the bodies are no longer as they were read.
    program.forgetBodiesAsRead();
    if (creations.isEmpty())
        return;

    ProgramEditor editor(program);
    Vector<FunctionID> functions;
    for (Creation& creation : creations) {
        const Function& function = program.at(creation.function);
        Layout layout;
        layout.kind = LayoutKind::Record;
        layout.module = function.module;
        layout.position = function.at(creation.call).position;
        layout.flags = { LayoutFlags::IsOfCreatedObject };
        creation.layout = editor.addLayout(WTF::move(layout));
        if (functions.isEmpty() || functions.last() != creation.function)
            functions.append(creation.function);
    }
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        auto ofFunction = std::ranges::equal_range(creations, id, { }, &Creation::function);
        becomeLiterals(program, build, id, function, arenas.types, creations.subspan(ofFunction.begin() - creations.begin(), ofFunction.size()));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
