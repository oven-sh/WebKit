/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTDeclaredFunctionsInScopes.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLayout.h"
#include "AOTResultRules.h"
#include "AOTSchemaResultRules.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesOfTailCallsInBodies { Opcode::Call, Opcode::CallList, Opcode::CallDirect };


template<typename IDType>
static bool requireRecordOfVerifiedOperation(BodyVerifier& verifier, const BodyBlock& block, ValueID value, IDType id)
{
    if (id) [[likely]]
        return true;
    verifier.fail(InternalErrorKind::IdOutOfRange, block.id(), value, verifier.id().index());
    return false;
}

static BodyValue definitionThroughUpcastsInVerifiedBody(const BodyIndex& index, ValueID operand)
{
    BodyValue definition = index.value(operand);
    while (definition.opcode() == Opcode::Upcast)
        definition = index.value(definition.as<Opcode::Upcast>().valueOperand());
    return definition;
}

static std::optional<uint32_t> numberOfResultsOfVerifiedDefinition(const Program& program, const BodyValue& definition)
{
    Opcode opcode = definition.opcode();
    if (!definitionsThatMayGiveSeveralResultsInVerifiedBodies.contains(opcode))
        return std::nullopt;
    if (opcode == Opcode::Results)
        return static_cast<uint32_t>(definition.as<Opcode::Results>().valuesOperands().size());
    if (opcode == Opcode::CallDirect) {
        FunctionID callee = definition.as<Opcode::CallDirect>().function();
        if (!callee || program.at(callee).results.size() == 1)
            return std::nullopt;
        return static_cast<uint32_t>(program.at(callee).results.size());
    }
    return numberOfDeclaredResultsOf(opcode);
}

template<typename T>
static bool repeatsEntryInVerifiedList(BodyList<T> list)
{
    Vector<T, 16> sorted;
    sorted.reserveInitialCapacity(list.size());
    for (T entry : list)
        sorted.append(entry);
    AOT::sort(sorted);
    return std::ranges::adjacent_find(sorted) != sorted.end();
}

static bool hasValidHolesInVerifiedArray(const BodyOperation<Opcode::NewArray>& array)
{
    std::span<const uint32_t> holes = array.holes();
    bool isStrictlyAscending = std::ranges::adjacent_find(holes, std::ranges::greater_equal { }) == holes.end();
    return isStrictlyAscending && (holes.empty() || holes.back() < array.elementsOperands().size() + holes.size());
}

static bool mayThrowOrCallAtVerifiedPosition(Opcode opcode)
{
    static constexpr OperandUses usesThatCall { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::PassesToUserCode };
    static constexpr OperandUses usesThatCallNothing {
        OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents,
        OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
        OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
    };
    static_assert(everyOperandUseIsInExactlyOneOf({ usesThatCall, usesThatCallNothing }));

    Effects bound = declaredBoundOf(opcode);
    if (bound.mayThrowByRule() || bound.mayRunOtherCode())
        return true;
    uint32_t numberOfOperandInfos = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperandInfos; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        if (!info.protocols.isEmpty() || info.uses.containsAny(usesThatCall))
            return true;
    }
    return false;
}

static bool verifiedFunctionReportsLineNumbers(const Program& program, const Function& function)
{
    if (!function.module || function.isHiddenInStackTraces())
        return false;
    return !std::ranges::empty(program.at(function.module).positions.lineStarts);
}

static void verifyTerminatorOperandsAndEdgeArgumentsOfVerifiedBlock(BodyVerifier& verifier, const BodyBlock& block, const BodyTerminator& terminator)
{
    const BodyIndex& index = verifier.index();
    auto hasSeveralResults = [&](ValueID id) {
        return numberOfResultsOfVerifiedDefinition(verifier.program(), index.value(id)).has_value();
    };

    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (verifier.hasFailed())
            return;
        if (mustBeDefinition(terminator.operandKind(operandIndex)) && index.isParameter(operand))
            verifier.fail(InternalErrorKind::OperandMustBeDefinition, block.id(), { }, operand.index());
        else if (terminator.kind() != TerminatorKind::Return && hasSeveralResults(operand))
            verifier.fail(InternalErrorKind::OperandTypeMismatch, block.id(), { }, operand.index());
        else if (opcodesThatOpenIterationsInVerifiedBodies.contains(index.opcodeOf(operand)))
            verifier.fail(InternalErrorKind::OperandTypeMismatch, block.id(), { }, operand.index());
    });
    block.forEachOutgoingEdge([&](BodyTarget target, bool) {
        for (ValueID argument : target.arguments()) {
            if (!verifier.hasFailed() && hasSeveralResults(argument))
                verifier.fail(InternalErrorKind::OperandTypeMismatch, block.id(), { }, argument.index());
        }
    });
}

void BodyVerifier::verifyOperations()
{
    for (uint32_t blockIndex = 0; blockIndex < index().numberOfBlocks() && !m_operationsAreVerified; ++blockIndex) {
        BodyBlock block = index().block(BlockID(blockIndex));
        uint32_t indexInBlock = 0;
        for (BodyValue value : block.values()) {
            verifyOperation(block, indexInBlock++, value);
            if (hasFailed())
                return;
        }
        verifyTerminatorOperation(block, block.terminator());
        if (hasFailed())
            return;
    }
    RELEASE_ASSERT(m_operationsAreVerified);

    if (!m_methodDefinitions.isEmpty())
        verifyMethodIndexes();
    if (!hasFailed() && function().has(FunctionFlags::IsGenerator))
        verifyGeneratorStart();
    if (m_moduleEvaluations.isEmpty())
        return;
    if (!hasFailed())
        verifyBareEvaluations();
    if (!hasFailed())
        verifyRequestsOfEvaluations();
}

void BodyVerifier::verifyOperation(const BodyBlock& block, uint32_t indexInBlock, const BodyValue& value)
{
    verifyFieldRules(*this, block, value);
    if (!hasFailed())
        verifyOperandDefinitions(block, value);
    if (!hasFailed())
        verifyOperationByOpcode(block, indexInBlock, value);
}

void BodyVerifier::verifyOperationByOpcode(const BodyBlock& block, uint32_t indexInBlock, const BodyValue& value)
{
    verifyPositions(block, value);
    if (!hasFailed() && opcodesWithRuleOfTheirOwnInVerifiedBodies.contains(value.opcode()))
        verifyRuleOfOpcode(block, indexInBlock, value);
}

void BodyVerifier::verifyRuleOfOpcode(const BodyBlock& block, uint32_t indexInBlock, const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::CatchValue:
    case Opcode::CatchException:
        verifyCatch(block, indexInBlock, value);
        return;

    case Opcode::NarrowByTest:
    case Opcode::Convert:
        verifyCast(block, value);
        return;

    case Opcode::NewObject:
    case Opcode::NewArray:
        verifyAllocation(block, value);
        return;

    case Opcode::FinishObject:
    case Opcode::GetClassPrototype:
    case Opcode::DefineMethod:
    case Opcode::FinishClass:
        verifyObjectUnderConstruction(block, value);
        return;

    case Opcode::NewClass:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        verifyClassOperation(block, value);
        return;

    case Opcode::NewClosure:
        verifyNumberOfVariadicOperands(block, value);
        if (!hasFailed())
            verifyClosureOperation(block, value);
        return;

    case Opcode::GetCapture:
    case Opcode::IsClosureOf:
        verifyClosureOperation(block, value);
        return;

    case Opcode::CallDirect:
        verifyNumberOfVariadicOperands(block, value);
        if (!hasFailed())
            verifyDirectCall(block, value);
        return;

    case Opcode::Results:
        if (index().isUnused(value.id()))
            fail(InternalErrorKind::OperandTypeMismatch, block.id(), value.id());
        return;

    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments:
    case Opcode::GetMappedArgument:
    case Opcode::SetMappedArgument:
        verifyArgumentsOperation(block, value);
        return;

    case Opcode::PrivateGet:
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine:
    case Opcode::PrivateHas:
        verifyPrivateAccess(block, value);
        return;

    case Opcode::Await:
    case Opcode::Yield:
    case Opcode::InitialYield:
        verifySuspension(block, value);
        return;

    case Opcode::Builtin:
    case Opcode::ModuleBuiltin:
        verifyBuiltinConstant(block, value);
        return;

    case Opcode::NativeBinding: {
        BuiltinID binding = value.as<Opcode::NativeBinding>().builtin();
        if (!requireRecordOfVerifiedOperation(*this, block, value.id(), binding))
            return;
        const Builtin& entry = program().builtins().at(binding);
        if (entry.kind != BuiltinKind::NativeBinding || !entry.isNamedByText)
            fail(InternalErrorKind::InvalidBuiltinCall, block.id(), value.id());
        return;
    }

    case Opcode::DirectEval:
        verifyDirectEval(block, value);
        return;

    case Opcode::EvaluateModule:
    case Opcode::RecordRequire:
        verifyModuleEvaluation(block, value);
        return;

    case Opcode::GetEnvironmentField:
    case Opcode::PutEnvironmentField: {
        LayoutID layout = value.layout();
        if (!requireRecordOfVerifiedOperation(*this, block, value.id(), layout))
            return;
        if (program().at(layout).kind != LayoutKind::Environment || (value.opcode() == Opcode::PutEnvironmentField && !storeKeepsDeclaredFunctionOverItsOwnScope(program(), index(), value)))
            fail(InternalErrorKind::InvalidEnvironmentUse, block.id(), value.id());
        return;
    }

    case Opcode::CountExecution: {
        auto counter = value.as<Opcode::CountExecution>();
        if (requireRecordOfVerifiedOperation(*this, block, value.id(), counter.module()))
            requireRecordOfVerifiedOperation(*this, block, value.id(), counter.description());
        return;
    }

    case Opcode::Upcast: case Opcode::NarrowByField: case Opcode::Check: case Opcode::CheckInitialized: case Opcode::RequireObjectCoercible: case Opcode::RequireObject: case Opcode::TypeTest: case Opcode::IsUnlockedInstance:
    case Opcode::Undefined: case Opcode::Null: case Opcode::Absent: case Opcode::Boolean: case Opcode::Integer: case Opcode::Double: case Opcode::String: case Opcode::BigInt: case Opcode::Data:
    case Opcode::GetModuleVariable: case Opcode::PutModuleVariable: case Opcode::ScopeHasBinding:
    case Opcode::NewDictionary: case Opcode::AppendElement: case Opcode::AppendSpread: case Opcode::AppendHole: case Opcode::NewRegExp: case Opcode::NewTemplateObject:
    case Opcode::NewRuntimeError: case Opcode::NewSuppressedError:
    case Opcode::PropGet: case Opcode::PropSet: case Opcode::PropDefine: case Opcode::PropHas: case Opcode::PropDelete:
    case Opcode::ElemGet: case Opcode::ElemSet: case Opcode::ElemHas: case Opcode::ElemDelete: case Opcode::ElemDefine:
    case Opcode::DefineGetter: case Opcode::DefineSetter: case Opcode::TryDefineLiveBinding: case Opcode::TryReExportFields: case Opcode::SetFunctionName: case Opcode::SetLiteralPrototype:
    case Opcode::CopyDataProperties: case Opcode::CopyObject: case Opcode::GetSuperBase: case Opcode::SuperGet: case Opcode::SuperSet:
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div: case Opcode::Mod: case Opcode::Pow:
    case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor: case Opcode::LShift: case Opcode::RShift: case Opcode::URShift:
    case Opcode::Negate: case Opcode::ToNumeric: case Opcode::Increment: case Opcode::Decrement: case Opcode::BitNot: case Opcode::ToNumber:
    case Opcode::Less: case Opcode::LessEq: case Opcode::Greater: case Opcode::GreaterEq: case Opcode::Eq: case Opcode::StrictEq: case Opcode::InstanceOf:
    case Opcode::ToBoolean: case Opcode::LogicalNot: case Opcode::TypeOf: case Opcode::ToString: case Opcode::Strcat: case Opcode::ToPropertyKey: case Opcode::ToObject: case Opcode::ToThis:
    case Opcode::Call: case Opcode::Construct: case Opcode::CallList: case Opcode::ConstructList: case Opcode::ResultAt:
    case Opcode::LoopHint:
    case Opcode::ResolveHeritage: case Opcode::NewClassToken: case Opcode::GetConstructPrototype:
    case Opcode::IteratorOpen: case Opcode::IteratorResume: case Opcode::IteratorClose: case Opcode::ForInOpen: case Opcode::ForInStep: case Opcode::ReadScope:
    case Opcode::ModuleNamespace: case Opcode::ImportModule: case Opcode::RequireModule: case Opcode::WasLoadedSynchronously: case Opcode::SeesMockOf: case Opcode::IsLinkedBelow: case Opcode::MockSeenBy: case Opcode::NamespaceSeenBy:
    case Opcode::AreLoaderFunctionsStored: case Opcode::ResolveModule: case Opcode::ThrowModuleFailure: case Opcode::ImportDynamic: case Opcode::RequireDynamic: case Opcode::ResolveDynamic:
    case Opcode::IsConstructor: case Opcode::ReadsIteratorOfListUnseen: case Opcode::ArraySpeciesCreate: case Opcode::GetByValWithThis: case Opcode::GetOwnProperty: case Opcode::ToLength:
    case Opcode::ThrowNewError: case Opcode::ResumeGenerator: case Opcode::StepGenerator: case Opcode::IsGeneratorSuspendedAtStart: case Opcode::IsUnmodifiedGenerator:
    case Opcode::CreateIteratorResult: case Opcode::StepArrayIterator: case Opcode::StepMapIterator: case Opcode::StepSetIterator:
    case Opcode::RequireBuiltinModule: case Opcode::LinkTimeConstant: case Opcode::HasJSType: case Opcode::FindsDeclaredMember: case Opcode::FindsNoMember: case Opcode::IsPlainInstanceOf: case Opcode::IsPlainHeirOf: case Opcode::IsAsBorn: case Opcode::IsAsBornExceptForAddedNames: case Opcode::IsPrototypeChainPlain: case Opcode::GetInternalField: case Opcode::PutInternalField:
    case Opcode::NewPromise: case Opcode::CreatePromise: case Opcode::ResolvePromise: case Opcode::RejectPromise:
    case Opcode::IsPristineArray: case Opcode::ArrayStorageKind: case Opcode::HasContiguousIndex: case Opcode::GetContiguousIndex: case Opcode::GetDoubleIndex:
    case Opcode::PutContiguousIndex: case Opcode::PutDoubleIndex: case Opcode::GetOwnContiguousIndex: case Opcode::GetOwnDoubleIndex: case Opcode::GetOwnInt32Index:
    case Opcode::PutOwnContiguousIndex: case Opcode::PutOwnDoubleIndex: case Opcode::PutOwnInt32Index:
    case Opcode::NewContiguousArray: case Opcode::NewArrayLike:
    case Opcode::TryPutByValDirect: case Opcode::TryPutIndex: case Opcode::TryCopyFilledElements: case Opcode::TryPutElements: case Opcode::SortNumbers: case Opcode::TryFillNewArray:
    case Opcode::TruncateNewArray:
    case Opcode::CollectionTable: case Opcode::CollectionIteratorClosedMark: case Opcode::GetCollectionIteratorTable: case Opcode::PutCollectionIteratorTable:
    case Opcode::IsCollectionIteratorClosedMark: case Opcode::MapNext: case Opcode::SetNext:
    case Opcode::KeyConstant: case Opcode::Unspecified: case Opcode::EmptyArgumentList: case Opcode::Parameter:
    case Opcode::CopyObjectAndDefine: case Opcode::CopyTwoObjects:
    case Opcode::Expect: case Opcode::CallTable: case Opcode::BeginMemberGuards: case Opcode::ForInOpenCountingNames:
    case Opcode::ShapeTest: case Opcode::IsSlotFilled: case Opcode::HasNoExtraProperties: case Opcode::HasNoFieldOutsideSlot: case Opcode::IsNotFrozen:
    case Opcode::HasLayoutPrototype: case Opcode::IsPristine: case Opcode::HasStructureOfClass: case Opcode::HasArrayStorage: case Opcode::IsElementPresent:
    case Opcode::IsNotThenable: case Opcode::AreMembersUnchanged: case Opcode::IsMemberAsBuilt: case Opcode::HasNumberOfBoundArguments: case Opcode::GetWordOfBoundFunction: case Opcode::IsHolderWithoutAddedNames: case Opcode::HasKeyOrder: case Opcode::AreHeldFieldsFilled:
    case Opcode::IsEnumeratorCurrent:
    case Opcode::GetSlot: case Opcode::GetSlotOrUndefined: case Opcode::GetSlotGetter: case Opcode::GetSlotSetter: case Opcode::GetDeferredClosureSlot: case Opcode::GetDeferredClosureSlotOrUndefined:
    case Opcode::PutSlot: case Opcode::PutSlotOrAdd: case Opcode::FinishBirth: case Opcode::TryFinishBirth: case Opcode::PropHasOwn:
    case Opcode::TryCopyFields: case Opcode::GetGlobalBinding: case Opcode::SetGlobalBinding: case Opcode::GetCachedGetter: case Opcode::ElemGetOrOwnAccessor: case Opcode::GetGetterOfAccessorOrUndefined: case Opcode::GetValueAtSiteOrAbsent: case Opcode::GetOwnValueUnderKeyOrAbsent:
    case Opcode::GetStaticFieldOrAbsent: case Opcode::PutStaticField: case Opcode::GetPropertyOfHeir: case Opcode::PutPropertyOfHeir:
    case Opcode::ElemGetByKeyParts:
    case Opcode::ReserveArrayCapacity: case Opcode::ConvertToDoubleStorage:
    case Opcode::GetArrayCapacity: case Opcode::ExtendArrayLength: case Opcode::SetArrayLength:
    case Opcode::GetArrayLength: case Opcode::GetArrayStorage: case Opcode::GetArrayElement: case Opcode::PutArrayElement: case Opcode::HasArrayElement: case Opcode::AppendElements: case Opcode::AppendMembers: case Opcode::AppendValuesOfMap:
    case Opcode::GetTypedArrayLength: case Opcode::GetTypedArrayVector: case Opcode::GetTypedArrayElement: case Opcode::PutTypedArrayElement:
    case Opcode::GetEnumeratorIndexedLength: case Opcode::IsElementInStorage: case Opcode::GetElementInStorageOrAbsent: case Opcode::GetStringLength: case Opcode::GetStringCodeUnit:
    case Opcode::GetArgument: case Opcode::GetRestLength: case Opcode::CreateRest:
    case Opcode::MarkEnvironmentEscaped: case Opcode::ReleaseEnvironment:
    case Opcode::CallBuiltin: case Opcode::DivideIntegers: case Opcode::RemainderOfIntegers: case Opcode::ReciprocalOfDivisor: case Opcode::RemainderByReciprocal: case Opcode::WholeDivisor: case Opcode::RemainderByWholeDivisor: case Opcode::ToInt32: case Opcode::ToUint32: case Opcode::ToIntegerOrInfinity: case Opcode::NewSetFromData: case Opcode::NewMapFromData:
    case Opcode::NewFormatterWithPattern: case Opcode::SetUpFormatterWithOptions: case Opcode::FormatDateWithPattern: case Opcode::FormatNumberWithPattern: case Opcode::NewArgumentList: case Opcode::NewArrayOfGiven:
    case Opcode::SharedRegExpLiteral: case Opcode::GetBuiltinValue: case Opcode::GetWatchedBuiltinValue: case Opcode::GetWatchedBuiltinValueOrAbsent: case Opcode::InitializeBuiltin:
    case Opcode::DestructureIterable: case Opcode::AppendIterable: case Opcode::EnumeratorGetByVal: case Opcode::RequireAttachedTypedArray: case Opcode::RequireLengthFitsList: case Opcode::CreateAsyncFromSyncIterator:
    case Opcode::ThrowNotIterable:
    case Opcode::NewGenerator: case Opcode::NewGeneratorOfLibrary: case Opcode::NewAsyncGenerator: case Opcode::TakeGeneratorPrototype:
    case Opcode::StepAsyncGenerator: case Opcode::AwaitMayContinue: case Opcode::AwaitBySavingFrame:
    case Opcode::AwaitCallBySavingFrame: case Opcode::YieldBySavingFrame: case Opcode::GetHeapFrameWord: case Opcode::PutHeapFrameWord: case Opcode::NewHeapWords:
    case Opcode::AsyncGeneratorYield: case Opcode::ReturnAsyncResult: case Opcode::PromiseAllOfElements:
    case Opcode::HasInPrototypeChain: case Opcode::TryFindInPrototypeChain: case Opcode::HasDefaultHasInstance:
    case Opcode::BeginModuleEvaluation: case Opcode::FinishModuleEvaluation: case Opcode::EvaluateRequiredModule: case Opcode::EnterBodyThatRunsOnce:
    case Opcode::NumberOrNaN:
    case Opcode::DefineOwnProperty: case Opcode::DefineAccessors: case Opcode::IntegerBound: case Opcode::FulfillFreshPromise:
    case Opcode::Substring: case Opcode::FlattenString: case Opcode::StringRange: case Opcode::StringOrder: case Opcode::StringOrderToCharacter:
    case Opcode::StringEqualsLiteral: case Opcode::ValueEqualsLiteral: case Opcode::StringStartsWithLiteral: case Opcode::StringEndsWithLiteral:
    case Opcode::StringIndexOfLiteral: case Opcode::StringIndexOfCharacter: case Opcode::StringLastIndexOfLiteral:
    case Opcode::StringRangeEqualsLiteral: case Opcode::StringRangeToNumber: case Opcode::StringRangeParseInt:
    case Opcode::FindStringRangeInMap: case Opcode::FindKeyInMap: case Opcode::GetValueAtPlaceInMap: case Opcode::MovesOfEntriesInCollections: case Opcode::TryPutValueAtPlaceInMap:
    case Opcode::StringBuilderBegin: case Opcode::StringBuilderAppend: case Opcode::StringBuilderLength:
    case Opcode::StringBuilderToString: case Opcode::StringBuilderFinish: case Opcode::StringBuilderDiscard: case Opcode::StringBuilderCheckpoint: case Opcode::StringBuilderRollBack:
    case Opcode::StackStringBuilderBegin: case Opcode::StackStringBuilderStart: case Opcode::StackStringBuilderCursor: case Opcode::StackStringBuilderLength:
    case Opcode::StackStringBuilderToString: case Opcode::StackStringBuilderFinish: case Opcode::StackStringBuilderDiscard:
    case Opcode::StringBuilderAppendConstant: case Opcode::StringBuilderAppendInteger: case Opcode::StringBuilderAppendNumber: case Opcode::StringBuilderAppendString:
    case Opcode::StringBuilderAppendStringRange: case Opcode::StringBuilderAppendBoolean: case Opcode::StringBuilderAppendCodeUnit: case Opcode::StringBuilderAppendStringOrNullish: case Opcode::StringBuilderAppendValue:
    case Opcode::StackStringBuilderAppendConstant: case Opcode::StackStringBuilderAppendInteger: case Opcode::StackStringBuilderAppendNumber: case Opcode::StackStringBuilderAppendString:
    case Opcode::StackStringBuilderAppendStringRange: case Opcode::StackStringBuilderAppendBoolean: case Opcode::StackStringBuilderAppendCodeUnit: case Opcode::StackStringBuilderAppendStringOrNullish: case Opcode::StackStringBuilderAppendValue:
    case Opcode::ResolveCallTarget: case Opcode::CallResolved: case Opcode::RegExpMatchLiteral: case Opcode::CallInline:
    case Opcode::RegExpMatchLiteralInParts: case Opcode::BoundsOfRegExpMatch: case Opcode::NewRegExpMatchArray: case Opcode::SubstringOrUndefined:
    case Opcode::BoundsOfStringSplit: case Opcode::NewStringSplitArray:
    case Opcode::Box: case Opcode::Unbox: case Opcode::IntegerToNumber: case Opcode::NumberToInteger: case Opcode::WidenInteger: case Opcode::NarrowInteger: case Opcode::Decode: case Opcode::Compress:
    case Opcode::Narrowed: case Opcode::EndCellLifetime: case Opcode::CheckProof: case Opcode::GuessFailsUnasked: case Opcode::AskObjectAgain: case Opcode::BeginEffectsCheck: case Opcode::CheckIterationRestart: case Opcode::CheckInlineeFallback: case Opcode::CheckReloadBypass:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BodyVerifier::verifyTerminatorOperation(const BodyBlock& block, const BodyTerminator& terminator)
{
    verifyFieldRules(*this, block, terminator);
    if (!hasFailed())
        verifyTerminatorOperandsAndEdgeArgumentsOfVerifiedBlock(*this, block, terminator);
    if (hasFailed() || !terminatorsWithRuleOfTheirOwnInVerifiedBodies.contains(terminator.kind()))
        return;

    switch (terminator.kind()) {
    case TerminatorKind::Switch: {
        BodyList<int64_t> cases = terminator.as<TerminatorKind::Switch>().cases();
        for (int64_t caseValue : cases) {
            if (!Range::full().contains(caseValue)) {
                fail(InternalErrorKind::PayloadOutOfRange, block.id());
                return;
            }
        }
        if (repeatsEntryInVerifiedList(cases))
            fail(InternalErrorKind::DuplicateListEntry, block.id(), { }, id().index());
        return;
    }
    case TerminatorKind::SwitchString:
        if (repeatsEntryInVerifiedList(terminator.as<TerminatorKind::SwitchString>().strings()))
            fail(InternalErrorKind::DuplicateListEntry, block.id(), { }, id().index());
        return;
    case TerminatorKind::Return:
        verifyReturn(block, terminator);
        return;
    case TerminatorKind::ReturnCall:
        verifyReturnCall(block, terminator);
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BodyVerifier::verifyOperandDefinitions(const BodyBlock& block, const BodyValue& value)
{
    if (opcodesThatTakeAnyValueInVerifiedBodies.contains(value.opcode())) [[likely]] {
        bool namesWhatFewOperandsTake = false;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            namesWhatFewOperandsTake |= definitionsThatFewOperandsTakeInVerifiedBodies.contains(index().opcodeOf(operand));
        });
        if (!namesWhatFewOperandsTake) [[likely]]
            return;
    }
    verifyOperandDefinitionsOneByOne(block, value);
}

bool BodyVerifier::isTakenByFewOperands(const BodyValue& definition) const
{
    return opcodesThatOpenIterationsInVerifiedBodies.contains(definition.opcode()) || numberOfResultsOfVerifiedDefinition(program(), definition).has_value();
}

void BodyVerifier::verifyOperandDefinitionsOneByOne(const BodyBlock& block, const BodyValue& value)
{
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (hasFailed())
            return;

        OperandKind kind = value.operandKind(operandIndex);
        BodyValue definition = index().value(operand);
        if (mustBeDefinition(kind) && definition.isParameter()) {
            fail(InternalErrorKind::OperandMustBeDefinition, block.id(), value.id(), operand.index());
            return;
        }

        std::optional<uint32_t> numberOfResults = numberOfResultsOfVerifiedDefinition(program(), definition);
        bool isOfKind = true;
        if (kind == OperandKind::Results)
            isOfKind = numberOfResults && definition.opcode() != Opcode::Results;
        else if (numberOfResults)
            isOfKind = false;
        else if (kind == OperandKind::IteratorState)
            isOfKind = definition.opcode() == Opcode::IteratorOpen;
        else if (kind == OperandKind::Enumerator)
            isOfKind = definition.opcode() == Opcode::ForInOpen;
        else if (kind != OperandKind::Convertible)
            isOfKind = !opcodesThatOpenIterationsInVerifiedBodies.contains(definition.opcode());
        if (!isOfKind) {
            fail(InternalErrorKind::OperandTypeMismatch, block.id(), value.id(), operand.index());
            return;
        }

        if (value.opcode() != Opcode::ResultAt)
            return;
        uint32_t resultIndex = value.as<Opcode::ResultAt>().index();
        if (resultIndex >= *numberOfResults)
            fail(InternalErrorKind::ResultIndexOutOfRange, block.id(), value.id(), resultIndex);
    });
}

ALWAYS_INLINE_INTO_OTHER_FILES void BodyVerifier::verifyNumberOfVariadicOperands(const BodyBlock& block, const BodyValue& value)
{
    size_t expectedNumber = 0;
    if (value.opcode() == Opcode::NewObject)
        expectedNumber = value.as<Opcode::NewObject>().fields().size();
    else {
        bool makesClosure = value.opcode() == Opcode::NewClosure;
        FunctionID named = makesClosure ? value.as<Opcode::NewClosure>().function() : value.as<Opcode::CallDirect>().function();
        if (!requireRecordOfVerifiedOperation(*this, block, value.id(), named))
            return;
        const Function& callee = program().at(named);
        expectedNumber = makesClosure ? callee.captures.size() : callee.numberOfEntryParameters();
    }

    size_t number = value.variadicOperands().size();
    if (number != expectedNumber)
        fail(InternalErrorKind::ArgumentCountMismatch, block.id(), value.id(), static_cast<uint32_t>(number));
}

void BodyVerifier::verifyCatch(const BodyBlock& block, uint32_t indexInBlock, const BodyValue& value)
{
    bool followsCatchException = value.opcode() == Opcode::CatchValue && indexInBlock == 1
        && (*block.values().begin()).opcode() == Opcode::CatchException;
    bool startsBlock = !indexInBlock || followsCatchException;
    if (!startsBlock || !block.isHandlerBlock())
        fail(InternalErrorKind::InvalidCatch, block.id(), value.id());
}

void BodyVerifier::verifyCast(const BodyBlock& block, const BodyValue& value)
{
    if (value.opcode() == Opcode::Convert) {
        verifyConvertedAllocation(block, value);
        return;
    }

    ASSERT(value.opcode() == Opcode::NarrowByTest);
    auto behindTests = [&](ValueID named) {
        named = index().stripCasts(named);
        while (ValueID tested = index().valueNarrowedInto(named))
            named = index().stripCasts(tested);
        return named;
    };
    ValueID left = value.as<Opcode::NarrowByTest>().leftOperand();
    ValueID behindLeft = left ? behindTests(left) : ValueID();
    bool isJoin = behindLeft && index().isParameter(behindLeft) && index().blockOf(behindLeft) != entryBlock && index().numberOfIncomingEdges(index().blockOf(behindLeft)) > 1;
    if (!behindLeft || (!isJoin && behindLeft != behindTests(value.as<Opcode::NarrowByTest>().valueOperand())))
        fail(InternalErrorKind::InvalidProof, block.id(), value.id());
}

void BodyVerifier::verifyConvertedAllocation(const BodyBlock& block, const BodyValue& value)
{
    auto conversion = value.as<Opcode::Convert>();
    BodyValue converted = index().value(conversion.valueOperand());
    if (converted.opcode() != Opcode::NewObject || conversion.illTypedCause() != IllTypedCause::None)
        return;
    DeclaredType declared = conversion.declared();
    if (!declared.isEstablished())
        return;

    LayoutID allocated = converted.as<Opcode::NewObject>().layout();
    if (!requireRecordOfVerifiedOperation(*this, block, value.id(), allocated))
        return;

    TypeReader types(program());
    Type type = types.at(declared.written());
    if (type.anyObject || type.environment == allocated || types.at(type.layouts).contains(allocated))
        return;
    if (mayBeInsideSomeClassPart(types, allocated, classPartsOf(types, type)))
        return;

    fail(InternalErrorKind::FamilyNotClosed, block.id(), value.id());
    m_errors.last().layout = allocated;
}

void BodyVerifier::verifyAllocation(const BodyBlock& block, const BodyValue& value)
{
    if (value.opcode() == Opcode::NewArray) {
        auto array = value.as<Opcode::NewArray>();
        if (requireRecordOfVerifiedOperation(*this, block, value.id(), array.contents()) && !hasValidHolesInVerifiedArray(array))
            fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id());
        return;
    }

    auto allocation = value.as<Opcode::NewObject>();
    if (!requireRecordOfVerifiedOperation(*this, block, value.id(), allocation.layout()))
        return;
    verifyNumberOfVariadicOperands(block, value);
    if (hasFailed())
        return;

    const auto& fields = program().at(allocation.layout()).fields;
    for (uint32_t listed : allocation.fields()) {
        if (listed >= fields.size() || !fields[listed].form.contains(FieldForm::IsAccessor))
            continue;
        fail(InternalErrorKind::AccessorSlotMismatch, block.id(), value.id());
        m_errors.last().layout = allocation.layout();
        return;
    }
}

void BodyVerifier::verifyObjectUnderConstruction(const BodyBlock& block, const BodyValue& value)
{
    bool finishesObject = value.opcode() == Opcode::FinishObject;
    ValueID operand = finishesObject ? value.as<Opcode::FinishObject>().objectOperand() : value.classObjectOperand();
    BodyValue head = definitionThroughUpcastsInVerifiedBody(index(), operand);
    bool isHead = finishesObject ? head.opcode() == Opcode::NewObject : head.opcode() == Opcode::NewClass && head.as<Opcode::NewClass>().layout() == value.layout();
    if (!isHead) {
        fail(InternalErrorKind::OperandTypeMismatch, block.id(), value.id(), operand.index());
        return;
    }
    if (value.opcode() == Opcode::DefineMethod)
        m_methodDefinitions.append({ head.id(), value.as<Opcode::DefineMethod>().index(), block.id(), value.id() });
}

void BodyVerifier::verifyMethodIndexes()
{
    AOT::sort(m_methodDefinitions, { }, &MethodDefinitionInBody::classObject);
    ValueID classObject;
    uint32_t nextIndex = 0;
    for (const MethodDefinitionInBody& definition : m_methodDefinitions) {
        if (definition.classObject != classObject) {
            classObject = definition.classObject;
            nextIndex = 0;
        }
        if (definition.index != nextIndex++) {
            fail(InternalErrorKind::PayloadOutOfRange, definition.block, definition.value, definition.index);
            return;
        }
    }
}

void BodyVerifier::verifyReturn(const BodyBlock& block, const BodyTerminator& terminator)
{
    ValueID returned = terminator.as<TerminatorKind::Return>().valueOperand();
    std::optional<uint32_t> numberOfReturnedResults = returned ? numberOfResultsOfVerifiedDefinition(program(), index().value(returned)) : std::nullopt;

    bool returnsInnerValue = function().isCoroutine();
    if (!returnsInnerValue && function().results.size() != 1) {
        if (!numberOfReturnedResults || *numberOfReturnedResults != function().results.size())
            fail(InternalErrorKind::ReturnTypeMismatch, block.id());
        return;
    }
    if (numberOfReturnedResults) {
        fail(InternalErrorKind::ReturnTypeMismatch, block.id());
        return;
    }
    if (returned || returnsInnerValue)
        return;

    const DeclaredType& declared = function().result();
    if (!declared.isExpected() && !(program().at(declared.written()).primitives & Primitive::Undefined))
        fail(InternalErrorKind::ReturnTypeMismatch, block.id());
}

void BodyVerifier::verifyReturnCall(const BodyBlock& block, const BodyTerminator& terminator)
{
    auto returnCall = terminator.as<TerminatorKind::ReturnCall>();
    ValueID call = returnCall.callOperand();

    bool isCallOfBlock = block.holds(call) && opcodesOfTailCallsInBodies.contains(index().opcodeOf(call));
    bool isInTailPosition = !block.hasHandler() && function().results.size() == 1 && !function().isCoroutine();
    std::optional<BodyUser> user = index().onlyUser(call);
    bool isOnlyReturned = user && user->kind == BodyUseKind::TerminatorOperand && user->block == block.id();
    if (!isCallOfBlock || !isInTailPosition || !isOnlyReturned) {
        fail(InternalErrorKind::InvalidReturnCall, block.id(), call);
        return;
    }

    for (uint32_t later = call.index() + 1; later < block.endOfValues().index(); ++later) {
        if (!declaredBoundOf(index().opcodeOf(ValueID(later))).isUnobservable()) {
            fail(InternalErrorKind::InvalidReturnCall, block.id(), call);
            return;
        }
    }

    BodyList<TailCast> casts = returnCall.casts();
    for (TailCast cast : casts) {
        if (!requireRecordOfVerifiedOperation(*this, block, call, cast.declared.written()))
            return;
    }

    const DeclaredType& result = function().result();
    if (result.isExpected())
        return;
    TypeID last;
    if (!casts.isEmpty())
        last = casts[casts.size() - 1].declared.written();
    else if (index().opcodeOf(call) == Opcode::CallDirect)
        last = program().at(index().value(call).as<Opcode::CallDirect>().function()).result().written();

    Type declared = program().at(result.written());
    bool fitsResult = declared.isAnything() || last == result.written() || (last && isSubset(program(), program().at(last), declared));
    if (!fitsResult)
        fail(InternalErrorKind::InvalidReturnCall, block.id(), call);
}

void BodyVerifier::verifyPositions(const BodyBlock& block, const BodyValue& value)
{
    ASCIILiteral problem;
    switch (value.writtenPositionRole()) {
    case WrittenPositionRole::None:
        if (!value.hasPosition() && mayThrowOrCallAtVerifiedPosition(value.opcode()) && verifiedFunctionReportsLineNumbers(program(), function()))
            problem = "an operation that may throw or call has no position"_s;
        break;
    case WrittenPositionRole::WhereWritten:
        if (!value.hasPosition() || *value.writtenPosition() == BodyValue::noPosition)
            problem = "one of the two positions of the operation is absent"_s;
        break;
    case WrittenPositionRole::StartOfConstruction:
    case WrittenPositionRole::StartForCallers:
    case WrittenPositionRole::WhereListIsWritten: {
        std::optional<ConstructionStart> start = value.constructionStart();
        if (!start || start->reported == BodyValue::noPosition || start->start == BodyValue::noPosition)
            problem = "one of the two positions of the operation is absent"_s;
        break;
    }
    }
    if (problem.isNull())
        return;
    fail(InternalErrorKind::PayloadOutOfRange, block.id(), value.id());
    m_errors.last().detail = problem;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
