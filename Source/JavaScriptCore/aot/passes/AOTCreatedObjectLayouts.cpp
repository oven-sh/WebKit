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
#include "AOTCopyClosureFactories.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LayoutCreation {
    FunctionID function;
    ValueID call;
    LayoutID layout;
};

} // anonymous namespace

static void becomeLiterals(const AnalysisContext& context, Function& function, TypeArena& types, std::span<const LayoutCreation> creations)
{
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID call = function.at(block).values[index];
            auto creation = std::ranges::find(creations, call, &LayoutCreation::call);
            if (creation == creations.end())
                continue;
            Origin origin = function.at(call).origin();
            builder.setOrigin(origin);

            Value allocation = IRBuilder::make(Opcode::NewObject);
            allocation.setLayout(creation->layout);
            ValueID object = builder.insertBefore({ block, index }, WTF::move(allocation));

            Value link = IRBuilder::make(Opcode::SetLiteralPrototype, { object, function.at(call).variadicOperands()[0], function.at(call).calleeOperand(), function.at(call).thisValueOperand() });
            link.setRefusesOtherValues(true);
            builder.insertBefore({ block, index }, WTF::move(link));

            Value& finished = function.edit(call);
            finished = IRBuilder::make(Opcode::FinishObject, { object });
            finished.setOrigin(origin);
        }
    }
    builder.applyInsertions();
}

void giveLayoutsToCreatedObjects(Program& program, const BitVector& copiesOfMakers, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    const BuiltinTable& builtins = program.builtins();
    const Program::BodiesAsRead* bodies = program.bodiesAsRead();
    // Where the reader has noted which functions name Object.create, no other function as read is looked at. The functions that were added since follow them.
    Vector<uint32_t> candidates;
    if (bodies) {
        for (FunctionID id : bodies->functionsThatNameObjectCreate)
            candidates.append(id.index());
    }
    for (uint32_t index = bodies ? program.numberOfFunctionsAsRead() : 0; index < program.functions.size(); ++index)
        candidates.append(index);
    // They come in the order of the functions, so a layout gets the same number on any number of threads.
    Vector<LayoutCreation> creations = collectInParallel<LayoutCreation>(build.parallelLoops(), candidates.size(), [&](unsigned position, Vector<LayoutCreation>& found) {
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
    // The copies of a function make the same objects.
    UncheckedKeyHashMap<uint64_t, LayoutID> layoutsByPlace;
    for (LayoutCreation& creation : creations) {
        const Function& function = program.at(creation.function);
        if (functions.isEmpty() || functions.last() != creation.function)
            functions.append(creation.function);
        uint64_t place = (static_cast<uint64_t>(function.module.index()) + 1) << 32 | function.at(creation.call).position;
        // What each copy of a maker makes has a prototype object of its own.
        bool hasPlace = function.at(creation.call).hasPosition() && !copiesOfMakers.get(creation.function.index());
        if (LayoutID known = hasPlace ? layoutsByPlace.get(place) : LayoutID()) {
            creation.layout = known;
            continue;
        }
        Layout layout;
        layout.kind = LayoutKind::Instance;
        layout.module = function.module;
        layout.position = function.at(creation.call).position;
        layout.flags = { LayoutFlags::IsOfCreatedObject };
        creation.layout = editor.addLayout(WTF::move(layout));
        if (hasPlace)
            layoutsByPlace.add(place, creation.layout);
    }
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        auto ofFunction = std::ranges::equal_range(creations, id, { }, &LayoutCreation::function);
        becomeLiterals({ build, program, nullptr, nullptr, id }, function, arenas.types, creations.subspan(ofFunction.begin() - creations.begin(), ofFunction.size()));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
