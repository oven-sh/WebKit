/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTFlowShapeResults.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ShapeProofs {
    Vector<LayoutID> losableLayoutByType;

    LayoutID findLosableLayout(TypeID type) const
    {
        if (!type || type.index() >= losableLayoutByType.size())
            return { };
        return losableLayoutByType[type.index()];
    }
};

struct FunctionCheck {
    const Program& program;
    const FlowResults& results;
    BuildContext& build;
    FunctionID id;
    const Function& function;
    Vector<InternalError>& errors;

    void report(InternalErrorKind kind, BlockID block, ValueID value, LayoutID layout = { }) const
    {
        errors.append(InternalError {
            .kind = kind,
            .function = id,
            .block = block,
            .value = value,
            .layout = layout,
            .location = function.locationOf(value),
        });
    }

    bool isOutside(ValueID value, TypeID required) const
    {
        return required && !program.typeOf(function, value).isSubsetOf(program.at(required), program);
    }
};

} // anonymous namespace

static bool isValidMember(const Program& program, LayoutID member)
{
    if (member.index() >= program.layouts.size())
        return false;
    const Layout& layout = program.at(member);
    return !layout.isEnvironment() && layout.flow.allocation != AllocationState::NeverAllocated;
}

static bool hasValidMembers(const Program& program, LayoutID id)
{
    const Vector<LayoutID>& members = program.at(id).flow.members;
    if (std::ranges::adjacent_find(members, std::ranges::greater_equal { }) != members.end())
        return false;
    return std::ranges::all_of(members, [&](LayoutID member) { return isValidMember(program, member); });
}

static bool hasAllocationClasses(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
        return true;
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeProven(const Program& program, const FlowResults& results, LayoutID id)
{
    const Layout& layout = program.at(id);
    // A namespace object is immutable from its creation, so its result rule names the layout without a proof of flow.
    if (!hasAllocationClasses(layout.kind))
        return true;
    return std::ranges::any_of(layout.flow.allocationClasses, [&](AllocationClassID allocationClass) {
        return shapePermanenceOf(results.at(allocationClass)) == ShapePermanence::CannotBeLost;
    });
}

static LayoutID findLosableMember(const Program& program, const BitVector& provableLayouts, const Type& type)
{
    for (LayoutID member : membersOf(program, type)) {
        if (!provableLayouts.get(member.index()))
            return member;
    }
    return { };
}

static ShapeProofs computeShapeProofs(const Program& program, const FlowResults& results, const ParallelLoops& loops)
{
    Vector<uint32_t> provable = findIndicesInParallel(loops, program.layouts.size(), [&](unsigned index) {
        return canBeProven(program, results, LayoutID(index));
    });
    BitVector provableLayouts(program.layouts.size());
    for (uint32_t index : provable)
        provableLayouts.quickSet(index);

    ShapeProofs proofs { Vector<LayoutID>(program.types.size()) };
    forEachIndexInParallel(loops, program.types.size(), [&](unsigned index) {
        proofs.losableLayoutByType[index] = findLosableMember(program, provableLayouts, program.at(TypeID(index)));
    });
    return proofs;
}

static void checkProgramPosition(const ShapeProofs& shapes, const DeclaredType& declared, Vector<InternalError>& errors)
{
    if (LayoutID layout = shapes.findLosableLayout(declared.type))
        errors.append(InternalError { .kind = InternalErrorKind::FlowProofOverLosableShape, .layout = layout });
}

static void checkLayout(const Program& program, const ShapeProofs& shapes, LayoutID id, Vector<InternalError>& errors)
{
    if (!hasValidMembers(program, id))
        errors.append(InternalError { .kind = InternalErrorKind::FlowMembersInvalid, .layout = id });
    for (const Field& field : program.at(id).fields)
        checkProgramPosition(shapes, field.declared, errors);
}

static void checkSignaturePosition(const FunctionCheck& check, const ShapeProofs& shapes, const DeclaredType& declared)
{
    if (LayoutID layout = shapes.findLosableLayout(declared.type))
        check.errors.append(InternalError { .kind = InternalErrorKind::FlowProofOverLosableShape, .function = check.id, .layout = layout });
}

static void checkSignature(const FunctionCheck& check, const ShapeProofs& shapes)
{
    const Function& function = check.function;
    for (const DeclaredType& capture : function.captures)
        checkSignaturePosition(check, shapes, capture);
    checkSignaturePosition(check, shapes, function.thisType);
    for (const DeclaredType& parameter : function.parameters)
        checkSignaturePosition(check, shapes, parameter);
    for (const DeclaredType& result : function.results)
        checkSignaturePosition(check, shapes, result);
}

static void checkProvenLayouts(const FunctionCheck& check, const ShapeProofs& shapes, BlockID block, ValueID id)
{
    const Value& value = check.function.at(id);
    LayoutID layout = shapes.findLosableLayout(value.type);
    if (!layout && value.opcode == Opcode::AssumeType)
        layout = shapes.findLosableLayout(value.castType());
    if (layout)
        check.report(InternalErrorKind::FlowProofOverLosableShape, block, id, layout);
}

static const AllocationClass* findStampedClass(const FlowResults& results, const Value& value)
{
    if (PayloadWord::allocationClass(value.opcode)) {
        if (AllocationClassID allocationClass = value.allocationClass())
            return &results.at(allocationClass);
    }
    if (PayloadWord::allocationSite(value.opcode)) {
        if (AllocationSiteID site = value.allocationSite())
            return &results.classOf(site);
    }
    return nullptr;
}

static void checkKeptShape(const FunctionCheck& check, BlockID block, ValueID id)
{
    const Value& value = check.function.at(id);
    if (!FlagMask::keepsShape(value.opcode) || !value.keepsShape())
        return;
    const AllocationClass* allocationClass = findStampedClass(check.results, value);
    if (allocationClass && shapePermanenceOf(*allocationClass) != ShapePermanence::CannotBeLost)
        check.report(InternalErrorKind::FlowProofOverLosableShape, block, id, allocationClass->layout);
}

static void checkShapeProofs(const FunctionCheck& check, const ShapeProofs& shapes, BlockID id, const Block& block)
{
    for (ValueID parameter : block.parameters)
        checkProvenLayouts(check, shapes, id, parameter);
    for (ValueID value : block.values) {
        checkProvenLayouts(check, shapes, id, value);
        checkKeptShape(check, id, value);
    }
}

static void checkOperands(const FunctionCheck& check, const TypingContext& typing, BlockID block, ValueID user)
{
    const Value& value = check.function.at(user);
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (check.isOutside(operand, requiredTypeOfOperand(typing, value, operandIndex)))
            check.report(InternalErrorKind::FlowWriterOutsideDeclaredType, block, user);
    });
}

static void checkReturn(const FunctionCheck& check, const TypingContext& typing, BlockID block, const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Return || !terminator.valueOperand())
        return;
    if (check.isOutside(terminator.valueOperand(), requiredTypeOfOperand(typing, terminator, OperandIndex::Return::value)))
        check.report(InternalErrorKind::FlowWriterOutsideDeclaredType, block, terminator.valueOperand());
}

static void checkWriters(const FunctionCheck& check, const TypingContext& typing, BlockID id, const Block& block)
{
    for (ValueID value : block.values)
        checkOperands(check, typing, id, value);
    checkReturn(check, typing, id, block.terminator);
}

static bool isDeclaredSubset(const Program& program, TypeID narrow, TypeID wide)
{
    return !narrow || !wide || program.at(narrow).isSubsetOf(program.at(wide), program);
}

static bool hasEntryTypesInsideVersion(const Program& program, const Function& function, const Function& version)
{
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        const DeclaredType* declared = function.declaredEntryParameter(index);
        const DeclaredType* declaredByVersion = version.declaredEntryParameter(index);
        if (!declared != !declaredByVersion)
            return false;
        if (declared && !isDeclaredSubset(program, declared->type, declaredByVersion->type))
            return false;
    }
    return true;
}

static bool hasResultTypesInsideVersion(const Program& program, const Function& function, const Function& version)
{
    for (unsigned index = 0; index < function.results.size(); ++index) {
        if (!isDeclaredSubset(program, function.results[index].type, version.results[index].type))
            return false;
    }
    return true;
}

static bool isValidGenericVersion(const FunctionCheck& check, FunctionID versionID)
{
    const Program& program = check.program;
    if (versionID.index() >= program.functions.size())
        return false;
    const Function& function = check.function;
    const Function& version = program.at(versionID);
    const auto* creator = version.creator.tryAs<CreatorRecord::Version>();
    if (version.role != FunctionRole::GenericVersion || !creator || creator->provenVersion != check.id || version.derivation.source != check.id)
        return false;
    if (function.kind != version.kind || function.flags != version.flags || function.length != version.length || function.module != version.module)
        return false;
    if (function.numberOfEntryParameters() != version.numberOfEntryParameters() || function.results.size() != version.results.size())
        return false;
    if (!hasEntryTypesInsideVersion(program, function, version))
        return false;
    // A function whose callers check its result returns whatever flows in, which the result type as read does not bound.
    return check.results.function(check.id).resultCheck == ResultCheck::ByCallers || hasResultTypesInsideVersion(program, function, version);
}

static bool isLinkedVersion(const FunctionCheck& check)
{
    const auto* creator = check.function.creator.tryAs<CreatorRecord::Version>();
    if (!creator || creator->provenVersion.index() >= check.results.functionCountAsRead())
        return false;
    return check.results.function(creator->provenVersion).genericVersion == check.id;
}

static bool hasValidVersionLinks(const FunctionCheck& check)
{
    if (check.function.role == FunctionRole::GenericVersion && !isLinkedVersion(check))
        return false;
    if (check.id.index() >= check.results.functionCountAsRead())
        return true;
    FunctionID linked = check.results.function(check.id).genericVersion;
    return !linked || isValidGenericVersion(check, linked);
}

static bool mayHaveFlowProof(const FunctionCheck& check)
{
    return check.results.hasResults()
        && check.build.isEnabled(FlowResultKind::NarrowedTypes, findFunctionAsRead(&check.program, check.id).index());
}

static void checkAssumedTailCasts(const FunctionCheck& check, const TypingContext& typing, BlockID block, const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return;
    ValueID call = terminator.callOperand();
    Type returned = check.program.typeOf(check.function, call);
    for (TailCast cast : terminator.casts()) {
        const Type& castType = check.program.at(cast.declared.type);
        if (cast.resolution == CastResolution::Assume && !returned.isSubsetOf(castType, check.program))
            check.report(InternalErrorKind::FlowRemovedClaimNotProven, block, call);
        returned = returned.castTo(castType, typing.types);
    }
}

static bool endsAtCastToNever(const FunctionCheck& check, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Unreachable || block.values.isEmpty())
        return false;
    const Value& last = check.function.at(block.values.last());
    return last.opcode == Opcode::AssumeType && check.program.at(last.type).isNever();
}

// The switch of a position decides whether it is narrowed, so a function whose own switch is off still casts what it stores there.
static BitVector findCastsIntoPositions(const FunctionCheck& check, const TypingContext& typing, const CFG& cfg)
{
    const Function& function = check.function;
    BitVector casts(function.values.size());
    for (BlockID id : cfg.reversePostOrder()) {
        if (endsAtCastToNever(check, function.at(id)))
            casts.quickSet(function.at(id).values.last().index());
        for (ValueID user : function.at(id).values) {
            const Value& value = function.at(user);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                const Value& cast = function.at(operand);
                if (cast.opcode == Opcode::AssumeType && cast.castType() == requiredTypeOfOperand(typing, value, operandIndex))
                    casts.quickSet(operand.index());
            });
        }
    }
    return casts;
}

static void checkAssumedTypes(const FunctionCheck& check, const TypingContext& typing, const CFG& cfg)
{
    BitVector castsIntoPositions;
    if (check.results.hasResults())
        castsIntoPositions = findCastsIntoPositions(check, typing, cfg);

    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = check.function.at(id);
        for (ValueID valueID : block.values) {
            const Value& value = check.function.at(valueID);
            if (value.opcode != Opcode::AssumeType || castsIntoPositions.get(valueID.index()))
                continue;
            if (check.isOutside(value.valueOperand(), value.castType()))
                check.report(InternalErrorKind::FlowRemovedClaimNotProven, id, valueID);
        }
        checkAssumedTailCasts(check, typing, id, block.terminator);
    }
}

static void checkBody(const FunctionCheck& check, const ShapeProofs* shapes)
{
    if (!check.function.hasBody())
        return;

    ScratchTypeArena types(check.program.types);
    TypingContext typing { check.program, check.function, types.arena(), TypeLevel::Proven };
    CFG cfg(check.function);
    if (shapes) {
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = check.function.at(id);
            checkShapeProofs(check, *shapes, id, block);
            checkWriters(check, typing, id, block);
        }
    }
    if (!mayHaveFlowProof(check))
        checkAssumedTypes(check, typing, cfg);
}

static void checkFunction(const FunctionCheck& check, const ShapeProofs* shapes)
{
    if (shapes) {
        checkSignature(check, *shapes);
        if (!hasValidVersionLinks(check))
            check.errors.append(InternalError { .kind = InternalErrorKind::FlowVersionMismatch, .function = check.id });
    }
    checkBody(check, shapes);
}

void validateFlowResults(const Program& program, const FlowResults& results, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<InternalError> errors;
    std::optional<ShapeProofs> shapes;
    if (results.hasResults()) {
        shapes = computeShapeProofs(program, results, loops);
        errors = collectInParallel<InternalError>(loops, program.layouts.size(), [&](unsigned index, Vector<InternalError>& found) {
            checkLayout(program, *shapes, LayoutID(index), found);
        });
        errors.appendVector(collectInParallel<InternalError>(loops, program.variables.size(), [&](unsigned index, Vector<InternalError>& found) {
            checkProgramPosition(*shapes, program.variables[index].declared, found);
        }));
    }

    std::optional<TypeInterningPhase> scratch;
    if (!program.types.hasOpenPhase())
        scratch.emplace(program.types, 0u, TypeInterningPhase::Scratch);
    errors.appendVector(collectInParallel<InternalError>(loops, program.functions.size(), [&](unsigned index, Vector<InternalError>& found) {
        checkFunction(FunctionCheck { program, results, build, FunctionID(index), program.functions[index], found }, shapes ? &*shapes : nullptr);
    }));
    if (scratch)
        scratch->discard();

    build.diagnostics().append(WTF::move(errors));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
