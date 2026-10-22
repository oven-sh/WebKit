/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBindGlobalNames.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTPropertyAccessMembers.h"

namespace JSC { namespace AOT { namespace Typed {

// Binding drops the global object operand and keeps the type and the position of the access. None of the four opcodes is a cast, a test or a constant.
static constexpr AnalysisSet analysesUnaffectedByBinding { FunctionAnalysis::Identities, FunctionAnalysis::Frequencies, FunctionAnalysis::FailedTests };

static constexpr bool isNeitherCastNorTest(Opcode opcode)
{
    return !isCastOrConversion(opcode) && provesOf(opcode) == Proof::None;
}

static_assert(isNeitherCastNorTest(Opcode::PropGet) && isNeitherCastNorTest(Opcode::PropSet) && isNeitherCastNorTest(Opcode::GetGlobalBinding) && isNeitherCastNorTest(Opcode::SetGlobalBinding));

static bool isGlobalObject(const Program& program, const Type& type)
{
    const BuiltinTable& table = program.builtins();
    BuiltinID builtin = singleBuiltinOf(type);
    return builtin && table.canonicalID(builtin) == table.canonicalID(Builtins::GlobalThis);
}

static bool shouldBind(const FunctionPassContext& context, const Value& access)
{
    const Program& program = context.program;
    if (!isGlobalObject(program, program.typeOf(context.function, access.objectOperand())))
        return false;
    if (keyLookupOf(program, access.key()) != KeyLookup::Ordinary)
        return false;
    return freeNameAnswerOf({ program, context.inputs.flow }, access.key(), access.isFreeName()).kind == FreeNameAnswerKind::Bound;
}

static void convertToGlobalBinding(Value& access)
{
    PropertyKeyID key = access.key();
    AccessSiteID site = access.accessSite();
    bool seesScriptBindings = access.isFreeName();

    if (access.opcode == Opcode::PropSet) {
        ECMAMode mode = access.mode();
        ValueID stored = access.valueOperand();
        access.become(Opcode::SetGlobalBinding);
        access.setMode(mode);
        access.setValueOperand(stored);
    } else {
        bool isForTypeOf = access.isForTypeOf();
        access.become(Opcode::GetGlobalBinding);
        access.setIsForTypeOf(isForTypeOf);
    }

    access.setKey(key);
    access.setLevel(KnowledgeLevel::Expected);
    access.setAccessSite(site);
    access.setSeesScriptBindings(seesScriptBindings);
}

void convertGlobalBindingToNamedAccess(IRBuilder& builder, ProgramPoint point, ValueID id)
{
    Function& function = builder.function();
    RELEASE_ASSERT(globalBindingAccesses.contains(function.at(id).opcode));

    builder.setOrigin(function.at(id).origin());
    Value globalObject = IRBuilder::make(Opcode::Builtin);
    globalObject.setBuiltin(Builtins::GlobalThis);
    ValueID object = builder.insertBefore(point, WTF::move(globalObject));

    Value& access = function.edit(id);
    PropertyKeyID key = access.key();
    AccessSiteID site = access.accessSite();
    bool isFreeName = access.seesScriptBindings();

    if (access.opcode == Opcode::SetGlobalBinding) {
        ECMAMode mode = access.mode();
        ValueID stored = access.valueOperand();
        access.become(Opcode::PropSet);
        access.setMode(mode);
        access.setValueOperand(stored);
    } else {
        bool isForTypeOf = access.isForTypeOf();
        access.become(Opcode::PropGet);
        access.setIsForTypeOf(isForTypeOf);
    }

    access.setObjectOperand(object);
    access.setKey(key);
    access.setAccessSite(site);
    access.setIsFreeName(isFreeName);
}

PassOutcome bindGlobalNames(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<ValueID, 8> accesses;
    for (const ValueSite& site : context.sitesIn(SiteList::GlobalNames)) {
        if (shouldBind(context, function.at(site.value)))
            accesses.append(site.value);
    }
    if (accesses.isEmpty())
        return PassOutcome::Unchanged;
    for (ValueID access : accesses)
        convertToGlobalBinding(function.edit(access));

    context.analyses.invalidate(valueAnalyses - analysesUnaffectedByBinding);
    context.keptAnalyses = analysesUnaffectedByBinding;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
