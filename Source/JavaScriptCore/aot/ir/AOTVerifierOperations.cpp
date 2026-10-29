/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinPristine.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTInductionVariables.h"
#include "AOTJSONVerification.h"
#include "AOTLimits.h"
#include "AOTRegExpFlags.h"
#include "AOTSort.h"
#include "AOTSortedSet.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "JSGlobalObjectFunctions.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesWithConstructionRules { Opcode::FinishObject, Opcode::FinishClass, Opcode::DefineMethod, Opcode::GetClassPrototype };
static constexpr OpcodeSet opcodesWithClassRules { Opcode::NewClass, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr OpcodeSet tailCallOpcodes { Opcode::Call, Opcode::CallList, Opcode::CallDirect, Opcode::CallForwardVarargs };

static bool isPrivateKind(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<InternalErrorKind> findDescriptorError(const Value& definition)
{
    using Flag = PropertyDescriptorFlags;
    static constexpr std::pair<Flag, uint32_t> operands[] = {
        { Flag::HasValue, OperandIndex::DefineOwnProperty::value },
        { Flag::HasGet, OperandIndex::DefineOwnProperty::getter },
        { Flag::HasSet, OperandIndex::DefineOwnProperty::setter },
    };
    static constexpr std::pair<Flag, Flag> attributes[] = {
        { Flag::HasWritable, Flag::IsWritable },
        { Flag::HasEnumerable, Flag::IsEnumerable },
        { Flag::HasConfigurable, Flag::IsConfigurable },
    };

    OptionSet<Flag> flags = definition.descriptorFlags();
    for (auto [isPresent, operandIndex] : operands) {
        bool hasOperand = !!definition.operand(operandIndex);
        if (flags.contains(isPresent) != hasOperand)
            return hasOperand ? InternalErrorKind::UnexpectedOperand : InternalErrorKind::OperandMissing;
    }
    if (flags.containsAny({ Flag::HasValue, Flag::HasWritable }) && flags.containsAny({ Flag::HasGet, Flag::HasSet }))
        return InternalErrorKind::PayloadOutOfRange;
    for (auto [isPresent, isSet] : attributes) {
        if (flags.contains(isSet) && !flags.contains(isPresent))
            return InternalErrorKind::PayloadOutOfRange;
    }
    return std::nullopt;
}

static bool hasClassProofs(const Type& type)
{
    return !type.classes.isEmpty() && type != type.withoutClassProofs();
}

static bool hasUntestableBuiltins(const Type& type)
{
    return !type.builtins.isEmpty() && type != type.withUntestableBuiltinsWidened();
}

template<typename T>
static bool hasDuplicates(std::span<const T> list)
{
    return SortedSet<T, 16>(list).size() != list.size();
}

static size_t numberOfComputedElements(const Program& program, const Function& function, const Value& array)
{
    return std::ranges::count_if(array.elementsOperands(), [&](ValueID element) {
        while (familyOf(function.at(element).opcode) == OperationFamily::Representation)
            element = function.at(element).valueOperand();
        return !isRematerializable(function.at(element)) && !program.typeOf(function, element).isNumber();
    });
}

static bool isTerminatorUse(const Use& use, BlockID block)
{
    switch (use.kind) {
    case Use::Kind::TerminatorOperand:
        return use.block == block;
    case Use::Kind::Operand:
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasOnlyTerminatorUses(std::span<const Use> uses, BlockID block)
{
    return std::ranges::all_of(uses, [&](const Use& use) {
        return isTerminatorUse(use, block);
    });
}

static bool haveNoEffects(FunctionAnalyses& analyses, std::span<const ValueID> values)
{
    return std::ranges::all_of(values, [&](ValueID id) {
        return analyses.hasNoEffects(id);
    });
}

static bool isLegalIn(CastResolution resolution, IRForm form)
{
    switch (resolution) {
    case CastResolution::Unresolved:
        return false;
    case CastResolution::Check:
        return form == IRForm::AsRead;
    case CastResolution::Nothing:
    case CastResolution::Upcast:
    case CastResolution::Assume:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canNarrow(CastResolution resolution)
{
    switch (resolution) {
    case CastResolution::Check:
    case CastResolution::Assume:
        return true;
    case CastResolution::Unresolved:
    case CastResolution::Nothing:
    case CastResolution::Upcast:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasPayloadOutOfRange(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::IntegerBound)
        return !relationalOperators.contains(value.comparison());
    if (value.opcode == Opcode::ValueLooselyEqualsLiteral)
        return !std::isnan(jsToNumber(program.strings.toWTFString(value.literal())));
    return false;
}

void FunctionVerifier::verifyOperations()
{
    for (BlockID blockID : analyses().cfg().reversePostOrder()) {
        const Block& block = m_function.at(blockID);
        for (uint32_t indexInBlock = 0; indexInBlock < block.values.size(); ++indexInBlock) {
            ValueID id = block.values[indexInBlock];
            const Value& value = m_function.at(id);
            m_hasFrameAllocation |= hasFrameStorage(value);
            m_hasTypedDataOperations |= hasTypedDataRules(value.opcode) || referencesFunctionWithCursor(m_program, value);
            verifyOperation(blockID, indexInBlock, id, value);
            if (hasFailed())
                return;
        }
        verifyTerminatorOperation(blockID, block.terminator);
        if (hasFailed())
            return;
        verifyFormRules(blockID, block);
        if (hasFailed())
            return;
    }
    verifyMethodIndexes();
    verifyGeneratorStart();
    verifyBareEvaluations();
    verifyRequestsOfEvaluations();
    runSubsystemCheckers();
}

static bool asksForWhatNoCodeAnswers(const Value& value)
{
    if (value.opcode == Opcode::IsPristine)
        return value.aspects().containsAny(aspectsWithoutTest);
    return value.opcode == Opcode::RegExpMatchLiteral && !value.regExpOperand() && value.regExpFlags().containsAny(flagsOfMatchesUsingObject);
}

// Whoever adds a read or a store keeps the record of the place true: a later pass takes the stores of what nothing reads, and the room of what nothing stores to.
static bool agreesWithRecord(const LocationFlow& flow, bool isRead)
{
    return isRead ? flow.reads != ReadState::NeverRead : flow.writers != WriterState::AllKnown || flow.stores != StoreCount::Never;
}

void FunctionVerifier::verifyOperation(BlockID block, uint32_t indexInBlock, ValueID id, const Value& value)
{
    Opcode opcode = value.opcode;
    if (asksForWhatNoCodeAnswers(value))
        fail(InternalErrorKind::PayloadOutOfRange, block, id);
    switch (familyOf(opcode)) {
    case OperationFamily::Exception:
        verifyCatch(block, indexInBlock, id, value);
        return;
    case OperationFamily::Cast:
        verifyCast(block, id, value);
        return;
    case OperationFamily::Allocation:
    case OperationFamily::Access:
    case OperationFamily::Class:
        if (opcodesWithConstructionRules.contains(opcode))
            verifyObjectUnderConstruction(block, id, value);
        else if (opcodesWithClassRules.contains(opcode))
            verifyClassOperation(block, id, value);
        else if (opcode == Opcode::NewArray || Value::opcodesWithKeyIsIntegral.contains(opcode))
            verifyElementOperation(block, id, value);
        else if (opcode == Opcode::NewObject) {
            verifyFormsOfListedFields(block, id, value);
            if (m_program.at(value.layout()).flow.allocation == AllocationState::NeverAllocated)
                fail(InternalErrorKind::InvalidLayout, block, id, value.layout().index());
        }
        return;
    case OperationFamily::Constant:
        if (opcode == Opcode::NewClosure)
            verifyClosureOperation(block, id, value);
        else if (opcode == Opcode::Builtin || opcode == Opcode::ModuleBuiltin)
            verifyBuiltinOperation(block, id, value);
        else if (opcode == Opcode::Data)
            verifyElementOperation(block, id, value);
        else if (opcode == Opcode::KeyConstant && isPrivateKind(m_program.at(value.key()).kind))
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    case OperationFamily::Call:
        if (opcode == Opcode::CallDirect || opcode == Opcode::CallTable)
            verifyDirectCall(block, id, value);
        return;
    case OperationFamily::PrivateAccess:
        verifyPrivateAccess(block, id, value);
        return;
    case OperationFamily::Coroutine:
    case OperationFamily::SavedFrame:
        verifySuspension(block, indexInBlock, id, value);
        return;
    case OperationFamily::Arguments:
        verifyArgumentsOperation(block, id, value);
        return;
    case OperationFamily::Environment:
        if (opcode == Opcode::GetCapture)
            verifyClosureOperation(block, id, value);
        return;
    case OperationFamily::BuiltinCall:
    case OperationFamily::InlineBuiltin:
        verifyBuiltinOperation(block, id, value);
        return;
    case OperationFamily::RuntimeIntrinsic:
        if (opcode == Opcode::NativeBinding)
            verifyBuiltinOperation(block, id, value);
        return;
    case OperationFamily::Eval:
        verifyDirectEval(block, id, value);
        return;
    case OperationFamily::ResolvedCall:
        if (opcode == Opcode::CallResolved)
            verifyResolvedCall(block, id, value);
        return;
    case OperationFamily::SlotAccess:
    case OperationFamily::SlotRun:
        verifySlotOperation(block, id, value);
        return;
    case OperationFamily::FusedAccess:
    case OperationFamily::ObjectCreation:
        if (Value::opcodesWithKeys.contains(opcode))
            verifyKeyedTail(block, id, value, value.keys().size());
        else if (opcode == Opcode::DefineOwnProperty) {
            if (std::optional<InternalErrorKind> error = findDescriptorError(value))
                fail(*error, block, id);
        }
        return;
    case OperationFamily::ElementAccess:
    case OperationFamily::ArrayIntrinsic:
        verifyElementOperation(block, id, value);
        return;
    case OperationFamily::StringBuilder:
    case OperationFamily::StringBuilderPart:
        verifyStringBuilderOperation(block, id, value);
        return;
    case OperationFamily::SelectedOperator:
    case OperationFamily::String:
        if (hasPayloadOutOfRange(m_program, value))
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    case OperationFamily::Guard:
        if (opcode == Opcode::IsSlotFilled)
            verifySlotOperation(block, id, value);
        else if (Value::opcodesWithKeyIsIntegral.contains(opcode))
            verifyElementOperation(block, id, value);
        return;
    case OperationFamily::Module:
        if (opcode == Opcode::EvaluateModule || opcode == Opcode::RecordRequire)
            verifyModuleEvaluation(block, id, value);
        // A module whose namespace object nothing makes has no word to keep it in.
        else if ((opcode == Opcode::ModuleNamespace || opcode == Opcode::ImportModule) && m_inputs.flow && m_inputs.flow->hasResults() && m_inputs.flow->module(value.module()).namespaceUse == NamespaceUse::NeverCreated)
            fail(InternalErrorKind::InvalidModule, block, id, value.module().index());
        return;
    case OperationFamily::Binding:
        if (Value::opcodesWithVariable.contains(opcode) && !agreesWithRecord(m_program.at(value.variable()).flow, opcode != Opcode::PutModuleVariable))
            fail(InternalErrorKind::InvalidVariable, block, id, value.variable().index());
        else if (opcode == Opcode::GetModuleVariable || opcode == Opcode::GetEncodedModuleVariable)
            verifyVariableRead(block, id, value);
        else if ((opcode == Opcode::GetEnvironmentField || opcode == Opcode::PutEnvironmentField) && value.field() < m_program.at(value.layout()).fields.size()
            && !agreesWithRecord(flowOf(m_program, { value.layout(), value.field() }).location, opcode == Opcode::GetEnvironmentField))
            fail(InternalErrorKind::InvalidEnvironmentUse, block, id);
        return;
    case OperationFamily::Operator: case OperationFamily::Hint: case OperationFamily::Control:
    case OperationFamily::Iteration: case OperationFamily::ForIn: case OperationFamily::EngineIntrinsic:
    case OperationFamily::PromiseIntrinsic: case OperationFamily::CollectionIntrinsic:
    case OperationFamily::GuardValue: case OperationFamily::OwnAccess: case OperationFamily::ExpandedIteration: case OperationFamily::JSONParse:
    case OperationFamily::ExpectedRows: case OperationFamily::RuntimeValidation: case OperationFamily::JSONWrite: case OperationFamily::StringRangeRead: case OperationFamily::Representation:
    case OperationFamily::KeyPartsLookup:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A variable that nothing fills stays as it began, and its type says so.
void FunctionVerifier::verifyVariableRead(BlockID block, ValueID id, const Value& value)
{
    const Variable& variable = m_program.at(value.variable());
    if (variable.flow.writers != WriterState::AllKnown || variable.flow.stores != StoreCount::Never || variable.flow.initialization == InitializationState::SetAtCreation)
        return;
    if (variable.initialization == VariableInitialization::Function || variable.declared.isExpected())
        return;
    // Every load of a module that cannot be linked throws, so no read of what the module would have filled is ever come to.
    if (std::ranges::contains(m_program.moduleFailures, variable.module, &ModuleFailure::importer))
        return;
    uint16_t initialValue = variable.initialization == VariableInitialization::Undefined ? Primitive::Undefined : Primitive::Absent;
    if (!(m_program.at(variable.declared.written()).primitives & initialValue))
        fail(InternalErrorKind::InvalidVariable, block, id, value.variable().index());
}

void FunctionVerifier::verifyCatch(BlockID block, uint32_t indexInBlock, ValueID id, const Value& value)
{
    const Block& contents = m_function.at(block);
    bool followsCatchException = value.opcode == Opcode::CatchValue && indexInBlock == 1
        && m_function.at(contents.values[0]).opcode == Opcode::CatchException;
    bool startsBlock = !indexInBlock || followsCatchException;
    if (!startsBlock || !contents.isHandlerBlock())
        fail(InternalErrorKind::InvalidCatch, block, id);
}

void FunctionVerifier::verifyCast(BlockID block, ValueID id, const Value& value)
{
    switch (value.opcode) {
    case Opcode::Upcast:
        if (typeOf(value.valueOperand()).mayBeUnspecified())
            need(InternalErrorKind::UpcastNotSubset, block, id, value.valueOperand(), m_program.at(value.castType()).withPrimitives(Primitive::Unspecified));
        else
            need(InternalErrorKind::UpcastNotSubset, block, id, value.valueOperand(), m_program.at(value.castType()));
        return;
    case Opcode::Expect:
        if (hasClassProofs(m_program.at(value.expectedType().predicted())))
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    case Opcode::Check:
    case Opcode::TypeTest: {
        bool isCheck = value.opcode == Opcode::Check;
        const Type& tested = m_program.at(isCheck ? value.expectedType().predicted() : value.testedType());
        if (hasClassProofs(tested) || (isCheck && hasUntestableBuiltins(tested)))
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    case Opcode::ReportMismatch: {
        const Type& expected = m_program.at(value.expectedType().predicted());
        if (hasClassProofs(expected) || hasUntestableBuiltins(expected))
            fail(InternalErrorKind::PayloadOutOfRange, block, id);
        return;
    }
    case Opcode::AssumeType: {
        // Nobody tests storage for numbers alone where a type proves it. A literal is born with such storage only where it names it.
        const ClassPart* part = m_program.at(value.provenType()).singleBuiltinClass();
        if (!part || !part->contentsAreProven || !holdsOnlyNumbers(part->arrayStorage))
            return;
        const Value& array = m_function.at(analyses().sameObjectThroughCasts(value.valueOperand()).valueInFrontOfFacts());
        if (array.opcode == Opcode::NewArray && array.arrayStorage() != part->arrayStorage)
            fail(InternalErrorKind::StoredTypeMismatch, block, id, value.valueOperand().index());
        return;
    }
    case Opcode::Convert:
    case Opcode::NarrowByField:
    case Opcode::CheckInitialized:
    case Opcode::RequireObjectCoercible:
    case Opcode::RequireObject:
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void FunctionVerifier::verifyObjectUnderConstruction(BlockID block, ValueID id, const Value& value)
{
    if (!isAsRead(*this))
        return;

    bool finishesObject = value.opcode == Opcode::FinishObject;
    ValueID operand = finishesObject ? value.objectOperand() : value.classObjectOperand();
    const Value& head = m_function.at(analyses().definitionThroughUpcasts(operand));
    bool isHead = finishesObject ? head.opcode == Opcode::NewObject : (head.opcode == Opcode::NewClass && head.layout() == value.layout());
    if (!isHead) {
        fail(InternalErrorKind::OperandTypeMismatch, block, id, operand.index());
        return;
    }
    if (value.opcode == Opcode::DefineMethod)
        m_methodDefinitions.append({ analyses().definitionThroughUpcasts(operand), value.index(), block, id });
}

void FunctionVerifier::verifyMethodIndexes()
{
    auto classAndIndexOf = [](const MethodDefinitionSite& site) {
        return std::pair { site.classObject, site.index };
    };
    AOT::sort(m_methodDefinitions, { }, classAndIndexOf);
    auto repeated = std::ranges::adjacent_find(m_methodDefinitions, { }, classAndIndexOf);
    if (repeated != m_methodDefinitions.end())
        fail(InternalErrorKind::PayloadOutOfRange, repeated->block, repeated->value, repeated->index);
}

void FunctionVerifier::verifyTerminatorOperation(BlockID block, const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::ReturnCall:
        verifyReturnCall(block, terminator);
        return;
    case TerminatorKind::TestAndBranch:
        verifyTestAndBranch(block, terminator);
        return;
    case TerminatorKind::Switch: {
        Vector<int64_t, 16> cases;
        for (int64_t caseValue : terminator.cases()) {
            if (!Range::full().contains(caseValue)) {
                fail(InternalErrorKind::PayloadOutOfRange, block);
                return;
            }
            cases.append(caseValue);
        }
        if (hasDuplicates(cases.span()))
            fail(InternalErrorKind::DuplicateListEntry, block);
        return;
    }
    case TerminatorKind::SwitchString:
        if (hasDuplicates(terminator.strings()))
            fail(InternalErrorKind::DuplicateListEntry, block);
        return;
    case TerminatorKind::SwitchOnShape: {
        Vector<LayoutID, 16> members;
        for (LayoutSetID id : terminator.layoutSets()) {
            const LayoutSet& set = m_program.at(id);
            if (!set.all() || set.written().isEmpty()) {
                fail(InternalErrorKind::PayloadOutOfRange, block);
                return;
            }
            members.appendVector(set.written());
        }
        if (hasDuplicates(members.span()))
            fail(InternalErrorKind::DuplicateListEntry, block);
        return;
    }
    case TerminatorKind::TryCallInline:
        if (!hasValidInlineParameters(terminator.inlineBuiltin(), terminator.inlineParameters()))
            fail(InternalErrorKind::PayloadOutOfRange, block);
        else if (!overloadTakesOperands(typing(), terminator))
            fail(InternalErrorKind::InvalidBuiltinCall, block);
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FunctionVerifier::verifyReturnCall(BlockID block, const Terminator& terminator)
{
    ValueID call = terminator.callOperand();
    const Block& contents = m_function.at(block);
    const UseLists& useLists = analyses().useLists();
    ValueLocation location = useLists.locationOf(call);

    bool isCallOfBlock = location.placement == ValuePlacement::InBlock && location.block == block && tailCallOpcodes.contains(m_function.at(call).opcode);
    bool isInTailPosition = !contents.handler && m_function.results.size() == 1 && !m_function.isUnsplitCoroutine();
    if (!isCallOfBlock || !isInTailPosition || !hasOnlyTerminatorUses(useLists.usesOf(call), block)) {
        fail(InternalErrorKind::InvalidReturnCall, block, call);
        return;
    }

    std::span<const ValueID> valuesAfterCall = contents.values.subspan(location.index + 1);
    bool endsBlock = valuesAfterCall.empty() || (m_form < IRForm::Selected && haveNoEffects(analyses(), valuesAfterCall));
    if (!endsBlock) {
        fail(InternalErrorKind::InvalidReturnCall, block, call);
        return;
    }

    TypingContext context = typing();
    Type returned = typeOf(call);
    for (TailCast cast : terminator.casts()) {
        DeclaredType target = cast.declared;
        target.changeType(Proven::byPosition(targetOfConvert(context, target)));
        const Type& declared = context.typeOf(target);
        bool narrowsWithoutProof = !canNarrow(cast.resolution) && !returned.isSubsetOf(declared, m_program);
        if (!isLegalIn(cast.resolution, m_form) || narrowsWithoutProof) {
            fail(InternalErrorKind::InvalidReturnCall, block, call);
            return;
        }
        returned = returned.castTo(declared, context.types);
    }
    if (!returned.isSubsetOf(context.typeOf(m_function.result()), m_program))
        fail(InternalErrorKind::InvalidReturnCall, block, call);
}

void FunctionVerifier::verifyTestAndBranch(BlockID block, const Terminator& terminator)
{
    ValueID condition = terminator.conditionOperand();
    const BlockValues& values = m_function.at(block).values;
    bool isFused = !values.isEmpty() && values.last() == condition && branchesOf(m_function.at(condition).opcode) != Branches::No
        && hasOnlyTerminatorUses(analyses().useLists().usesOf(condition), block);
    if (!isFused)
        fail(InternalErrorKind::InvalidTestAndBranch, block, condition);
}

void FunctionVerifier::verifyFormRules(BlockID blockID, const Block& block)
{
    switch (m_form) {
    case IRForm::AsRead:
        if (isOptimizing())
            return;
        for (ValueID id : block.values) {
            if (removedByOf(m_function.at(id).opcode) == PassID::TypeValues) {
                fail(InternalErrorKind::RequestAfterTyping, blockID, id);
                return;
            }
        }
        return;
    case IRForm::Prepared:
        return;
    case IRForm::Selected:
    case IRForm::Placed:
        break;
    }

    for (ValueID parameter : block.parameters) {
        if (m_function.at(parameter).expected().predicted()) {
            fail(InternalErrorKind::ExpectedTypeAfterSelection, blockID, parameter);
            return;
        }
    }
    for (ValueID id : block.values) {
        const Value& value = m_function.at(id);
        if (value.expected().predicted()) {
            fail(InternalErrorKind::ExpectedTypeAfterSelection, blockID, id);
            return;
        }
        bool isFusedCondition = block.terminator.kind == TerminatorKind::TestAndBranch && block.terminator.conditionOperand() == id;
        if (branchesOf(value.opcode) == Branches::Only && !isFusedCondition) {
            fail(InternalErrorKind::InvalidTestAndBranch, blockID, id);
            return;
        }
        if (value.opcode == Opcode::ForInStep && !value.positionOperand()) {
            fail(InternalErrorKind::RequiredPassLeftOperation, blockID, id);
            return;
        }
        if (value.opcode != Opcode::NewArray)
            continue;
        size_t batchSize = numberOfComputedElements(m_program, m_function, value);
        if (batchSize > m_build.limit(LimitID::MaxArrayLiteralBatch)) {
            fail(InternalErrorKind::ArgumentCountMismatch, blockID, id, static_cast<uint32_t>(batchSize));
            m_errors.last().limit = LimitID::MaxArrayLiteralBatch;
            return;
        }
    }
}

void FunctionVerifier::runSubsystemCheckers()
{
    if (hasFailed())
        return;
    if ((m_form >= IRForm::Prepared || isOptimizing()) && m_hasFrameAllocation)
        verifyFramePointers(analyses(), m_errors);
    if (m_hasTypedDataOperations || hasCursorInSignature(m_program, m_function))
        verifyTypedDataRules(m_program, m_id, m_function, analyses(), m_form, m_errors);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
