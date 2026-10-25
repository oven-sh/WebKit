/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTIRBuilder.h"
#include "AOTRuleSupport.h"
#include "AOTSplitUnions.h"
#include "AOTVector.h"
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

// The row says rangeOf: the call makes a view of the class of its receiver over a part of the same memory.
static bool makesRangeOfView(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Value& call = context.function.at(site.value);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || context.function.at(site.block).frequency != BlockFrequency::Normal)
        return false;
    if (program.builtins().at(call.overload()).result.rangeOf != BuiltinOperand::receiver() || call.isDemotedTailCall())
        return false;
    if (!isTypedView(typedArrayTypeOf(program.builtins(), program.typeOf(context.function, call.receiverOperand()))))
        return false;
    return call.argumentsOperands().size() <= 2 && std::ranges::all_of(call.argumentsOperands(), [&](ValueID bound) {
        return program.typeOf(context.function, bound).overlaps(program.at(TypeTable::number), program);
    });
}

// The bounds are clamped here, and what is left of the call is an allocation without conditions, which SinkAllocations moves to whoever needs the view, or removes.
// A view whose length can change, or that has lost its memory, takes the call as it was, and so does a bound that is no number.
static void expand(SiteExpansion& expansion, const ValueSite& site)
{
    IRBuilder& builder = expansion.builder();
    expansion.begin(site);
    const Value call = expansion.original();
    ValueID view = call.receiverOperand();
    Vector<ValueID, 2> bounds(call.argumentsOperands());
    Vector<BlockID, 3> blocksWithCall;

    for (ValueID& bound : bounds) {
        if (expansion.context().program.typeOf(expansion.function(), bound).isNumber())
            continue;
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(bound, TypeTable::number), OperandIndex::TypeTest::value, expansion.frequency(), BlockFrequency::Unlikely);
        blocksWithCall.append(branch.notTaken);
        bound = branch.provenOnTaken;
    }
    ValueID hasFixedBytes = expansion.append(IRBuilder::make(Opcode::HasFixedBytes, { view }));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(hasFixedBytes, expansion.frequency(), BlockFrequency::Unlikely);
    blocksWithCall.append(blocks.notTaken);

    builder.moveToEndOf(blocks.taken);
    Value length = IRBuilder::make(Opcode::GetTypedArrayLength, { view });
    length.setTypedArrayType(typedArrayTypeOf(expansion.context().program.builtins(), expansion.context().program.typeOf(expansion.function(), view)));
    Value clamped = IRBuilder::make(Opcode::StringRange, { expansion.append(WTF::move(length)), bounds.isEmpty() ? builder.appendInteger(0) : bounds[0], bounds.size() < 2 ? ValueID() : bounds[1] });
    clamped.setMethod(StringRangeMethod::Slice);
    ValueID range = expansion.append(WTF::move(clamped));
    Value made = IRBuilder::make(Opcode::TypedArrayRange, { view, expansion.append(makeResultAt(range, 0)), expansion.append(makeResultAt(range, 1)) });
    made.setValueType(call.type);
    made.setAllocationSite(call.allocationSite());
    expansion.finishArm(expansion.append(WTF::move(made)));

    for (BlockID block : blocksWithCall) {
        builder.moveToEndOf(block);
        expansion.finishArm(expansion.appendOriginal());
    }
    expansion.end();
}

unsigned expandRangesOfViews(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    Vector<ValueSite, 2> sites;
    for (const ValueSite& site : calls) {
        if (makesRangeOfView(context, site))
            sites.append(site);
    }
    if (sites.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const ValueSite& site : sites | std::views::reverse)
        expand(expansion, site);
    return sites.size();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
