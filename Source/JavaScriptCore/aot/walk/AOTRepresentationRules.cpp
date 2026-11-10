/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTTypeRelations.h"
#include "AOTWalk.h"
#include <array>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class RepresentationRule : uint8_t {
    BySchema,
    OwnForm,
    Narrowed,
    Forwarding,
    AsItIs,
    Operator,
    Truncation,
    ElementAccess,
    KeyedAccess,
    NewElement,
    Concatenation,
    AppendedNumber,
    TestOfCellOrValue,
    BuiltinCall,
    BoxedResult,
    SlotRead,
    AccessorRead,
    SlotStore,
    NewObject,
    PrivateDefinition,
    EnvironmentField,
    Variable,
    Capture,
    NewClosure,
    DirectCall,
    TableCall,
    Results,
    ResultAt,
};

using RepresentationRuleSet = EnumeratorBitSet<RepresentationRule, static_cast<unsigned>(RepresentationRule::ResultAt) + 1>;

} // anonymous namespace

static constexpr OpcodeSet readsAtPositionThatIsAnyNumber {
    Opcode::IsElementInStorage, Opcode::GetElementInStorageOrAbsent
};

static constexpr RepresentationRule representationRuleOf(Opcode opcode)
{
    if (readsAtPositionThatIsAnyNumber.contains(opcode))
        return RepresentationRule::ElementAccess;

    switch (opcode) {
    case Opcode::Undefined: case Opcode::Null: case Opcode::Absent: case Opcode::Boolean: case Opcode::Integer: case Opcode::Double: case Opcode::String: case Opcode::KeyConstant:
    case Opcode::Parameter:
    case Opcode::Box: case Opcode::Unbox: case Opcode::IntegerToNumber: case Opcode::NumberToInteger: case Opcode::WidenInteger: case Opcode::NarrowInteger: case Opcode::Decode: case Opcode::Compress:
        return RepresentationRule::OwnForm;
    case Opcode::Narrowed:
        return RepresentationRule::Narrowed;
    case Opcode::CheckInitialized: case Opcode::RequireObjectCoercible: case Opcode::RequireObject: case Opcode::FinishObject: case Opcode::FinishClass:
    case Opcode::Convert: case Opcode::Check: case Opcode::Expect: case Opcode::Upcast: case Opcode::NarrowByField: case Opcode::NarrowByTest:
        return RepresentationRule::Forwarding;
    case Opcode::TypeTest: case Opcode::EndCellLifetime: case Opcode::CheckProof: case Opcode::AskObjectAgain: case Opcode::CheckIterationRestart: case Opcode::CheckInlineeFallback: case Opcode::CheckReloadBypass:
        return RepresentationRule::AsItIs;
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div: case Opcode::Mod: case Opcode::Pow: case Opcode::Negate: case Opcode::Increment: case Opcode::Decrement:
    case Opcode::BitAnd: case Opcode::BitOr: case Opcode::BitXor: case Opcode::LShift: case Opcode::RShift: case Opcode::URShift: case Opcode::BitNot:
    case Opcode::Less: case Opcode::LessEq: case Opcode::Greater: case Opcode::GreaterEq: case Opcode::Eq: case Opcode::StrictEq:
    case Opcode::ToNumeric: case Opcode::ToNumber: case Opcode::ToBoolean: case Opcode::ToString:
        return RepresentationRule::Operator;
    case Opcode::ToInt32: case Opcode::ToUint32: case Opcode::StringRange:
        return RepresentationRule::Truncation;
    case Opcode::GetArrayElement: case Opcode::PutArrayElement: case Opcode::HasArrayElement: case Opcode::IsElementPresent: case Opcode::TryPutByValDirect:
    case Opcode::GetTypedArrayElement: case Opcode::PutTypedArrayElement:
        return RepresentationRule::ElementAccess;
    case Opcode::ElemGet: case Opcode::ElemSet:
        return RepresentationRule::KeyedAccess;
    case Opcode::NewArray: case Opcode::AppendElement:
        return RepresentationRule::NewElement;
    case Opcode::Strcat:
        return RepresentationRule::Concatenation;
    case Opcode::StringBuilderAppendNumber: case Opcode::StackStringBuilderAppendNumber:
        return RepresentationRule::AppendedNumber;
    case Opcode::ShapeTest: case Opcode::IsPristine: case Opcode::HasStructureOfClass: case Opcode::IsClosureOf:
        return RepresentationRule::TestOfCellOrValue;
    case Opcode::CallBuiltin: case Opcode::CallInline:
        return RepresentationRule::BuiltinCall;
    case Opcode::RegExpMatchLiteral: case Opcode::GetBuiltinValue: case Opcode::GetWatchedBuiltinValue: case Opcode::GetWatchedBuiltinValueOrAbsent: case Opcode::GetSuperBase:
        return RepresentationRule::BoxedResult;
    case Opcode::GetSlot: case Opcode::GetSlotOrUndefined: case Opcode::GetDeferredClosureSlot: case Opcode::GetDeferredClosureSlotOrUndefined:
        return RepresentationRule::SlotRead;
    case Opcode::GetSlotGetter: case Opcode::GetSlotSetter:
        return RepresentationRule::AccessorRead;
    case Opcode::PutSlot: case Opcode::PutSlotOrAdd:
        return RepresentationRule::SlotStore;
    case Opcode::NewObject:
        return RepresentationRule::NewObject;
    case Opcode::PrivateDefine:
        return RepresentationRule::PrivateDefinition;
    case Opcode::GetEnvironmentField: case Opcode::PutEnvironmentField:
        return RepresentationRule::EnvironmentField;
    case Opcode::GetModuleVariable: case Opcode::PutModuleVariable:
        return RepresentationRule::Variable;
    case Opcode::GetCapture:
        return RepresentationRule::Capture;
    case Opcode::NewClosure:
        return RepresentationRule::NewClosure;
    case Opcode::CallDirect: case Opcode::AwaitCallBySavingFrame:
        return RepresentationRule::DirectCall;
    case Opcode::CallTable:
        return RepresentationRule::TableCall;
    case Opcode::Results:
        return RepresentationRule::Results;
    case Opcode::ResultAt:
        return RepresentationRule::ResultAt;

    case Opcode::BigInt: case Opcode::Builtin: case Opcode::ModuleBuiltin: case Opcode::Unspecified: case Opcode::EmptyArgumentList: case Opcode::NewTemplateObject: case Opcode::Data:
    case Opcode::DirectEval: case Opcode::ScopeHasBinding: case Opcode::NewDictionary: case Opcode::FinishBirth: case Opcode::TryFinishBirth: case Opcode::AppendHole:
    case Opcode::HasLayoutPrototype: case Opcode::IsNotFrozen: case Opcode::HasNoExtraProperties: case Opcode::IsSlotFilled: case Opcode::AreMembersUnchanged: case Opcode::IsMemberAsBuilt: case Opcode::HasNumberOfBoundArguments: case Opcode::GetWordOfBoundFunction: case Opcode::IsHolderWithoutAddedNames: case Opcode::BeginMemberGuards:
    case Opcode::HasKeyOrder: case Opcode::AreHeldFieldsFilled: case Opcode::HasNoFieldOutsideSlot:
    case Opcode::PropGet: case Opcode::PropSet: case Opcode::PropDefine: case Opcode::GetGlobalBinding: case Opcode::SetGlobalBinding:
    case Opcode::PropHas: case Opcode::ElemHas: case Opcode::PropHasOwn: case Opcode::PropDelete: case Opcode::ElemDelete: case Opcode::ElemGetByKeyParts:
    case Opcode::DefineGetter: case Opcode::DefineSetter: case Opcode::DefineOwnProperty: case Opcode::DefineAccessors: case Opcode::SetFunctionName: case Opcode::SetLiteralPrototype: case Opcode::ElemDefine:
    case Opcode::ToObject: case Opcode::ToThis: case Opcode::GetArrayLength: case Opcode::GetArrayStorage: case Opcode::HasArrayStorage: case Opcode::DivideIntegers: case Opcode::RemainderOfIntegers: case Opcode::ReciprocalOfDivisor: case Opcode::RemainderByReciprocal: case Opcode::ToIntegerOrInfinity:
    case Opcode::IntegerBound: case Opcode::StringOrder: case Opcode::StringOrderToCharacter: case Opcode::StringEqualsLiteral: case Opcode::ValueEqualsLiteral: case Opcode::LogicalNot:
    case Opcode::NumberOrNaN:
    case Opcode::TypeOf: case Opcode::InstanceOf: case Opcode::HasInPrototypeChain: case Opcode::TryFindInPrototypeChain: case Opcode::HasDefaultHasInstance: case Opcode::ToPropertyKey:
    case Opcode::Call: case Opcode::ResolveCallTarget: case Opcode::CallResolved: case Opcode::Construct: case Opcode::GetConstructPrototype: case Opcode::CallList: case Opcode::ConstructList: case Opcode::NewArgumentList: case Opcode::NewArrayOfGiven:
    case Opcode::CreateUnmappedArguments: case Opcode::CreateMappedArguments: case Opcode::GetMappedArgument: case Opcode::SetMappedArgument: case Opcode::GetArgument: case Opcode::GetRestLength: case Opcode::CreateRest:
    case Opcode::CatchValue: case Opcode::CatchException: case Opcode::NewRuntimeError: case Opcode::NewSuppressedError: case Opcode::LoopHint:
    case Opcode::ResolveHeritage: case Opcode::NewClass: case Opcode::GetClassPrototype: case Opcode::DefineMethod: case Opcode::NewClassToken:
    case Opcode::SuperConstruct: case Opcode::SuperConstructList: case Opcode::SuperGet: case Opcode::SuperSet:
    case Opcode::PrivateGet: case Opcode::PrivateSet: case Opcode::PrivateHas:

    case Opcode::InitializeBuiltin: case Opcode::NativeBinding: case Opcode::LinkTimeConstant: case Opcode::RequireBuiltinModule:
    case Opcode::IsConstructor: case Opcode::ReadsIteratorOfListUnseen: case Opcode::ArraySpeciesCreate: case Opcode::GetByValWithThis: case Opcode::GetOwnProperty: case Opcode::ToLength:
    case Opcode::HasJSType: case Opcode::FindsDeclaredMember: case Opcode::FindsNoMember: case Opcode::IsPlainInstanceOf: case Opcode::IsPlainHeirOf: case Opcode::IsAsBorn: case Opcode::IsAsBornExceptForAddedNames: case Opcode::IsPrototypeChainPlain: case Opcode::GetInternalField: case Opcode::PutInternalField: case Opcode::ThrowNewError:
    case Opcode::IsPristineArray: case Opcode::ArrayStorageKind: case Opcode::HasContiguousIndex: case Opcode::GetContiguousIndex: case Opcode::GetDoubleIndex: case Opcode::PutContiguousIndex: case Opcode::PutDoubleIndex:
    case Opcode::GetOwnContiguousIndex: case Opcode::GetOwnDoubleIndex: case Opcode::GetOwnInt32Index: case Opcode::PutOwnContiguousIndex: case Opcode::PutOwnDoubleIndex: case Opcode::PutOwnInt32Index:
    case Opcode::TryPutIndex: case Opcode::TryCopyFilledElements: case Opcode::TryPutElements: case Opcode::TryFillNewArray:
    case Opcode::NewContiguousArray: case Opcode::NewArrayLike: case Opcode::TruncateNewArray: case Opcode::AppendElements: case Opcode::AppendMembers: case Opcode::AppendValuesOfMap: case Opcode::ReserveArrayCapacity: case Opcode::ConvertToDoubleStorage:
    case Opcode::GetTypedArrayLength: case Opcode::GetTypedArrayVector: case Opcode::RequireAttachedTypedArray: case Opcode::RequireLengthFitsList: case Opcode::GetStringLength: case Opcode::GetStringCodeUnit: case Opcode::Substring: case Opcode::FlattenString: case Opcode::StringStartsWithLiteral: case Opcode::StringEndsWithLiteral:
    case Opcode::StringIndexOfLiteral: case Opcode::StringIndexOfCharacter: case Opcode::StringLastIndexOfLiteral:
    case Opcode::StringRangeEqualsLiteral: case Opcode::StringRangeToNumber: case Opcode::StringRangeParseInt: case Opcode::GuessFailsUnasked: case Opcode::BeginEffectsCheck:
    case Opcode::FindStringRangeInMap: case Opcode::FindKeyInMap: case Opcode::GetValueAtPlaceInMap: case Opcode::MovesOfEntriesInCollections: case Opcode::TryPutValueAtPlaceInMap:
    case Opcode::StringBuilderBegin: case Opcode::StringBuilderAppend: case Opcode::StringBuilderLength: case Opcode::StringBuilderToString: case Opcode::StringBuilderFinish:
    case Opcode::StringBuilderDiscard: case Opcode::StringBuilderCheckpoint: case Opcode::StringBuilderRollBack:
    case Opcode::StringBuilderAppendConstant: case Opcode::StringBuilderAppendInteger: case Opcode::StringBuilderAppendString: case Opcode::StringBuilderAppendStringRange:
    case Opcode::StringBuilderAppendBoolean: case Opcode::StringBuilderAppendCodeUnit: case Opcode::StringBuilderAppendStringOrNullish: case Opcode::StringBuilderAppendValue:
    case Opcode::StackStringBuilderBegin: case Opcode::StackStringBuilderStart: case Opcode::StackStringBuilderCursor: case Opcode::StackStringBuilderLength: case Opcode::StackStringBuilderToString: case Opcode::StackStringBuilderFinish: case Opcode::StackStringBuilderDiscard:
    case Opcode::StackStringBuilderAppendConstant: case Opcode::StackStringBuilderAppendInteger: case Opcode::StackStringBuilderAppendString: case Opcode::StackStringBuilderAppendStringRange:
    case Opcode::StackStringBuilderAppendBoolean: case Opcode::StackStringBuilderAppendCodeUnit: case Opcode::StackStringBuilderAppendStringOrNullish: case Opcode::StackStringBuilderAppendValue:
    case Opcode::NewRegExp: case Opcode::SharedRegExpLiteral:
    case Opcode::CollectionTable: case Opcode::CollectionIteratorClosedMark: case Opcode::GetCollectionIteratorTable: case Opcode::PutCollectionIteratorTable: case Opcode::IsCollectionIteratorClosedMark:
    case Opcode::MapNext: case Opcode::SetNext: case Opcode::NewSetFromData: case Opcode::NewMapFromData:
    case Opcode::NewFormatterWithPattern: case Opcode::SetUpFormatterWithOptions: case Opcode::FormatDateWithPattern: case Opcode::FormatNumberWithPattern:
    case Opcode::CopyDataProperties: case Opcode::CopyObject: case Opcode::CopyObjectAndDefine: case Opcode::CopyTwoObjects: case Opcode::TryCopyFields: case Opcode::ForInOpen: case Opcode::ForInOpenCountingNames: case Opcode::ForInStep: case Opcode::IsEnumeratorCurrent: case Opcode::EnumeratorGetByVal: case Opcode::GetEnumeratorIndexedLength: case Opcode::IsElementInStorage:
    case Opcode::IteratorOpen: case Opcode::IteratorResume: case Opcode::IteratorClose: case Opcode::CreateAsyncFromSyncIterator: case Opcode::ThrowNotIterable: case Opcode::DestructureIterable: case Opcode::AppendIterable: case Opcode::AppendSpread:
    case Opcode::CreateIteratorResult: case Opcode::StepArrayIterator: case Opcode::StepMapIterator: case Opcode::StepSetIterator:
    case Opcode::Yield: case Opcode::InitialYield: case Opcode::YieldBySavingFrame: case Opcode::AsyncGeneratorYield: case Opcode::NewGenerator: case Opcode::NewGeneratorOfLibrary: case Opcode::NewAsyncGenerator: case Opcode::TakeGeneratorPrototype:
    case Opcode::ResumeGenerator: case Opcode::StepGenerator: case Opcode::StepAsyncGenerator: case Opcode::IsGeneratorSuspendedAtStart: case Opcode::IsUnmodifiedGenerator:
    case Opcode::Await: case Opcode::AwaitMayContinue: case Opcode::AwaitBySavingFrame: case Opcode::ReadScope:
    case Opcode::NewPromise: case Opcode::CreatePromise: case Opcode::ResolvePromise: case Opcode::RejectPromise: case Opcode::FulfillFreshPromise: case Opcode::IsNotThenable: case Opcode::ReturnAsyncResult: case Opcode::PromiseAllOfElements:
    case Opcode::NewHeapWords: case Opcode::GetHeapFrameWord: case Opcode::PutHeapFrameWord:
    case Opcode::MarkEnvironmentEscaped: case Opcode::ReleaseEnvironment:
    case Opcode::EvaluateModule: case Opcode::RecordRequire: case Opcode::RequireModule: case Opcode::ImportModule: case Opcode::ModuleNamespace:
    case Opcode::BeginModuleEvaluation: case Opcode::FinishModuleEvaluation: case Opcode::EvaluateRequiredModule: case Opcode::EnterBodyThatRunsOnce:
    case Opcode::WasLoadedSynchronously: case Opcode::SeesMockOf: case Opcode::IsLinkedBelow: case Opcode::MockSeenBy: case Opcode::NamespaceSeenBy: case Opcode::AreLoaderFunctionsStored: case Opcode::ResolveModule: case Opcode::ThrowModuleFailure:
    case Opcode::ImportDynamic: case Opcode::RequireDynamic: case Opcode::ResolveDynamic: case Opcode::TryDefineLiveBinding: case Opcode::TryReExportFields: case Opcode::CountExecution:
    case Opcode::GetStaticFieldOrAbsent: case Opcode::PutStaticField:
    case Opcode::GetCachedGetter: case Opcode::ElemGetOrOwnAccessor: case Opcode::GetGetterOfAccessorOrUndefined: case Opcode::GetValueAtSiteOrAbsent: case Opcode::GetOwnValueUnderKeyOrAbsent: case Opcode::GetElementInStorageOrAbsent: case Opcode::GetPropertyOfHeir: case Opcode::PutPropertyOfHeir:
    case Opcode::RegExpMatchLiteralInParts: case Opcode::BoundsOfRegExpMatch: case Opcode::NewRegExpMatchArray: case Opcode::SubstringOrUndefined:
    case Opcode::BoundsOfStringSplit: case Opcode::NewStringSplitArray:
    case Opcode::GetArrayCapacity: case Opcode::ExtendArrayLength: case Opcode::SetArrayLength:
        return RepresentationRule::BySchema;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::array<RepresentationRule, numberOfOpcodes> representationRulesByOpcode = []() consteval {
    std::array<RepresentationRule, numberOfOpcodes> rules { };
    for (Opcode opcode : allMembersOf<Opcode>)
        rules[static_cast<unsigned>(opcode)] = representationRuleOf(opcode);
    return rules;
}();

static ALWAYS_INLINE RepresentationRule representationRuleInTable(Opcode opcode)
{
    return representationRulesByOpcode[static_cast<unsigned>(opcode)];
}

static constexpr std::array<ResultRule, numberOfOpcodes> resultRulesByOpcodeForForms = []() consteval {
    std::array<ResultRule, numberOfOpcodes> rules { };
    for (Opcode opcode : allMembersOf<Opcode>)
        rules[static_cast<unsigned>(opcode)] = resultRuleOf(opcode);
    return rules;
}();

static ALWAYS_INLINE ResultRule resultRuleInTable(Opcode opcode)
{
    return resultRulesByOpcodeForForms[static_cast<unsigned>(opcode)];
}

static constexpr RepresentationRuleSet rulesThatTakeOperandsAsBuilt {
    RepresentationRule::OwnForm, RepresentationRule::Narrowed, RepresentationRule::Forwarding, RepresentationRule::AsItIs, RepresentationRule::Results
};
static constexpr RepresentationRuleSet rulesThatTakeOperandsByKind {
    RepresentationRule::BySchema, RepresentationRule::BoxedResult, RepresentationRule::SlotRead, RepresentationRule::AccessorRead,
    RepresentationRule::Capture, RepresentationRule::ResultAt
};
static constexpr RepresentationRuleSet rulesThatTakeOperandsByKindWhereNotDeclared {
    RepresentationRule::SlotStore, RepresentationRule::NewObject, RepresentationRule::PrivateDefinition, RepresentationRule::EnvironmentField,
    RepresentationRule::Variable, RepresentationRule::NewClosure
};
static constexpr OpcodeSet operatorsThatTestInForms = allRelations | allEqualities | OpcodeSet { Opcode::ToBoolean };

constinit const std::array<OperandsAreTaken, numberOfOpcodes> howOperandsAreTakenByOpcode = []() consteval {
    std::array<OperandsAreTaken, numberOfOpcodes> ways { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        RepresentationRule rule = representationRuleOf(opcode);
        OperandsAreTaken way = OperandsAreTaken::ByRule;
        if (rulesThatTakeOperandsAsBuilt.contains(rule) && !genericOperationOf(opcode))
            way = OperandsAreTaken::AsBuilt;
        else if (rulesThatTakeOperandsByKind.contains(rule))
            way = OperandsAreTaken::ByKind;
        else if (rule == RepresentationRule::TestOfCellOrValue)
            way = OperandsAreTaken::ByKindOrAsCell;
        else if (rulesThatTakeOperandsByKindWhereNotDeclared.contains(rule))
            way = OperandsAreTaken::ByKindWhereNotDeclared;
        else if (rule == RepresentationRule::Operator)
            way = OperandsAreTaken::ByFormOfOperator;
        ways[static_cast<unsigned>(opcode)] = way;
    }
    return ways;
}();

static Representation representationOfBuiltinParameter(TypeReader types, BuiltinOverloadID overload, BuiltinOperand operand)
{
    const BuiltinTable& table = types.builtins();
    const BuiltinParameter* parameter = table.parameterFor(table.at(overload), operand);
    if (!parameter)
        return Representation::JSValue;
    bool isConvertedInFrontOfCall = parameter->converted && !conversionOpcodesOf(parameter->conversion).empty();
    BuiltinTypeID written = isConvertedInFrontOfCall ? parameter->converted : parameter->accepted;
    if (!written || table.at(written).isAnything())
        return Representation::JSValue;

    const BuiltinType& type = table.at(written);
    uint16_t primitives = primitivesOfBuiltinType(type);
    for (const BuiltinTypePart& part : type.parts) {
        switch (part.kind) {
        case BuiltinTypePartKind::Operand:
        case BuiltinTypePartKind::Slot:
        case BuiltinTypePartKind::IteratedValues:
        case BuiltinTypePartKind::CallbackResult:
            return Representation::JSValue;
        case BuiltinTypePartKind::Prototype:
            primitives |= Primitive::Null;
            break;
        case BuiltinTypePartKind::Instance:
        case BuiltinTypePartKind::Builtin:
        case BuiltinTypePartKind::Layout:
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            break;
        }
    }
    if (!(primitives & Primitive::String) && !type.stringLiterals.empty())
        primitives |= Primitive::LiteralString;

    Representation cellOrValue = primitives & ~cellPrimitives ? Representation::JSValue : Representation::Cell;
    if (type.mayBeObject() || primitives & Primitive::LiteralString)
        return cellOrValue;

    Range integers = Range::empty();
    if (primitives & Primitive::Integer) {
        bool fitsInt32 = type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) && !type.integers.isEmpty() && Range { type.integers.minimum, type.integers.maximum }.isSubsetOf(Range::int32());
        integers = fitsInt32 ? Range::int32() : Range::full();
    }
    return representationOf(types, *tryMakeTypeOfPrimitives(primitives, integers));
}

namespace {

template<typename Subject>
class RepresentationRuleTable {
public:
    RepresentationRuleTable(const RepresentationContext& context, const Subject& subject, const OperandForms& forms, TypeID result)
        : m_context(context)
        , m_subject(subject)
        , m_forms(forms)
        , m_result(result)
    {
    }

    void setResult(TypeID result) { m_result = result; }
    void setVariadicOperand(uint32_t operandIndex, TypeID type, Representation representation)
    {
        m_variadicIndex = operandIndex;
        m_variadicType = type;
        m_variadicForm = representation;
    }

    ALWAYS_INLINE Representation wantedAt(uint32_t operandIndex) const
    {
        if (!typeIDAt(operandIndex))
            return Representation::Unchosen;
        Representation wanted;
        if constexpr (HoldsOpcode<Subject>)
            wanted = wantedByOperation(operandIndex);
        else
            wanted = wantedByTerminator(operandIndex);
        if (wanted == Representation::Unchosen && formAsBuiltAt(operandIndex) == Representation::Unchosen)
            return naturalFormAt(operandIndex);
        return wanted;
    }

    ALWAYS_INLINE Representation resultAt(uint32_t resultIndex) const
    {
        if constexpr (HoldsOpcode<Subject>)
            return resultOfOperation(resultIndex);
        else
            return terminatorKindOf(m_subject) == TerminatorKind::TryCallInline ? byType() : Representation::Void;
    }

    bool isEqualityOfNumbers() const
    {
        if (!allEqualities.contains(opcode()) || m_subject.form() != OperatorForm::Numbers)
            return false;
        return typeAt(leftIndex()).isNumber() && typeAt(rightIndex()).isNumber();
    }

    bool widthFollowsFromResult() const
    {
        Opcode opcode = this->opcode();
        if (representationRuleInTable(opcode) != RepresentationRule::Operator || allBitwiseOperators.contains(opcode) || testOperators.contains(opcode))
            return false;
        return m_subject.form() == OperatorForm::Integers || m_subject.form() == OperatorForm::CheckedIntegers;
    }

    ALWAYS_INLINE bool resultFollowsFromType() const
    {
        RepresentationRule rule = representationRuleInTable(opcode());
        ResultRule ruleOfSchema = resultRuleInTable(opcode());
        if (rule == RepresentationRule::OwnForm)
            return isConstantWithoutChosenForm();
        if (rulesThatGiveFormWithoutType.contains(rule) || ruleOfSchema == ResultRule::None)
            return false;
        if (rule == RepresentationRule::Operator)
            return widthFollowsFromResult() || binaryOperatorsOnInt32.contains(opcode());
        return ruleOfSchema != ResultRule::Unknown || rule == RepresentationRule::ElementAccess;
    }

    bool holdsNoneOfItsResults() const
    {
        if (!opcodesThatMayHaveSeveralResults.contains(opcode()))
            return false;
        if (numberOfResultsOf(m_context.program, m_subject))
            return true;
        return opcode() == Opcode::CallDirect && !returnsOneValue(m_context.plan.conventions().unitEnteredBy(m_subject));
    }

private:
    static constexpr OpcodeSet binaryOperatorsOnInt32 { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift };
    static constexpr OpcodeSet testOperators = operatorsThatTestInForms;

    static_assert(unknownResultsOf(Opcode::DestructureIterable) == UINT32_MAX && !unknownResultsOf(Opcode::ForInStep) && !unknownResultsOf(Opcode::Results));

    static constexpr RepresentationSet unboxedElementForms { Representation::Int32, Representation::Double, Representation::OptionalDouble };
    static constexpr RepresentationRuleSet rulesThatGiveFormWithoutType {
        RepresentationRule::Narrowed, RepresentationRule::Forwarding, RepresentationRule::BoxedResult, RepresentationRule::AccessorRead,
        RepresentationRule::EnvironmentField, RepresentationRule::Capture, RepresentationRule::DirectCall, RepresentationRule::TableCall, RepresentationRule::Results
    };
    static_assert(opcodesThatMayHaveSeveralResults.contains(Opcode::CallDirect));

    static constexpr uint32_t forwardedOperand = 0;
    static_assert(OperandIndex::Narrowed::value == forwardedOperand && OperandIndex::CheckInitialized::value == forwardedOperand && OperandIndex::RequireObjectCoercible::value == forwardedOperand);
    static_assert(OperandIndex::RequireObject::value == forwardedOperand && OperandIndex::FinishObject::object == forwardedOperand && OperandIndex::FinishClass::classObject == forwardedOperand);

    static constexpr uint32_t receiverOfBuiltinCall = 0;
    static_assert(OperandIndex::CallInline::receiver == receiverOfBuiltinCall && OperandIndex::CallBuiltin::receiver == receiverOfBuiltinCall);

    static Representation integerFormOf(const Type& type) { return type.integers.isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64; }
    static Representation unboxedNumberFormOf(const Type& type) { return type.isInteger() ? integerFormOf(type) : Representation::Double; }
    static Representation unboxedNumberOrJSValue(const Type& type) { return type.isNumber() ? unboxedNumberFormOf(type) : Representation::JSValue; }
    static Representation wantedByConcatenation(const Type& part) { return part.isAnyString() ? Representation::Cell : unboxedNumberOrJSValue(part); }

    static bool isTestOfNumber(std::optional<InlineBuiltin> builtin)
    {
        return builtin == InlineBuiltin::NumberIsNaN || builtin == InlineBuiltin::NumberIsFinite || builtin == InlineBuiltin::NumberIsInteger || builtin == InlineBuiltin::NumberIsSafeInteger;
    }

    Opcode opcode() const { return opcodeOf(m_subject); }
    bool isConstantWithoutChosenForm() const { return constantsInFormOfUses.contains(opcode()) && m_subject.representation == Representation::Unchosen; }
    uint32_t leftIndex() const { return declaredPosition(OperandPosition::left(opcode())); }
    uint32_t rightIndex() const { return declaredPosition(OperandPosition::right(opcode())); }

    TypeID typeIDAt(uint32_t operandIndex) const { return operandIndex == m_variadicIndex ? m_variadicType : m_forms.typeAt(operandIndex); }
    Type typeAt(uint32_t operandIndex) const { return m_context.types.at(typeIDAt(operandIndex)); }
    Representation formAsBuiltAt(uint32_t operandIndex) const { return operandIndex == m_variadicIndex ? m_variadicForm : m_forms.representationAt(operandIndex); }
    Representation naturalFormAt(uint32_t operandIndex) const { return representationOf(m_context.types, typeIDAt(operandIndex)); }
    Representation heldFormAt(uint32_t operandIndex) const
    {
        Representation asBuilt = formAsBuiltAt(operandIndex);
        return asBuilt == Representation::Unchosen ? naturalFormAt(operandIndex) : asBuilt;
    }

    std::optional<uint32_t> variadicIndexOf(uint32_t operandIndex) const
    {
        uint32_t numberOfNamedOperands = m_subject.numberOfNamedOperands();
        if (operandIndex < numberOfNamedOperands)
            return std::nullopt;
        return operandIndex - numberOfNamedOperands;
    }

    bool isDeclaredPosition(uint32_t operandIndex) const { return m_subject.operandKind(operandIndex) == OperandKind::Declared; }

    template<typename Predicate>
    bool everyOperand(const Predicate& holds) const
    {
        for (uint32_t operandIndex = 0; operandIndex < m_subject.numberOfOperands(); ++operandIndex) {
            if (typeIDAt(operandIndex) && !holds(operandIndex))
                return false;
        }
        return true;
    }

    Representation byType() const { return m_result ? representationOf(m_context.types, m_result) : Representation::Unchosen; }
    Representation legalOrByType(Representation form) const { return m_result && isLegalRepresentation(m_context.types.at(m_result), form) ? form : byType(); }
    Representation forwardedForm() const { return typeIDAt(forwardedOperand) ? heldFormAt(forwardedOperand) : byType(); }

    Representation wantedByKind(uint32_t operandIndex) const
    {
        return representationWantedByKind(m_context.types, m_subject.operandKind(operandIndex), typeIDAt(operandIndex));
    }

    Representation integerWidth() const
    {
        bool takesInt32Alone = everyOperand([&](uint32_t operandIndex) {
            return typeAt(operandIndex).isInt32();
        });
        if (!takesInt32Alone)
            return Representation::Int64;
        if (testOperators.contains(opcode()))
            return Representation::Int32;
        if (!m_result)
            return Representation::Int64;
        Type result = m_context.types.at(m_result);
        return !result.isInteger() || result.isInt32() ? Representation::Int32 : Representation::Int64;
    }

    Representation wantedByMixedOperands(uint32_t operandIndex, bool (Type::*hasUnboxedKind)() const, Representation unboxed) const
    {
        uint32_t unboxedIndex = (typeAt(leftIndex()).*hasUnboxedKind)() ? leftIndex() : rightIndex();
        return operandIndex == unboxedIndex ? unboxed : Representation::JSValue;
    }

    Representation wantedByNumberAndValue(uint32_t operandIndex) const
    {
        uint32_t numberIndex = typeAt(leftIndex()).isNumber() ? leftIndex() : rightIndex();
        if (operandIndex != numberIndex)
            return Representation::JSValue;
        uint32_t otherIndex = numberIndex == leftIndex() ? rightIndex() : leftIndex();
        bool comparesIntegers = allRelations.contains(opcode()) && typeAt(numberIndex).isInteger() && typeAt(otherIndex).primitives & Primitive::Integer;
        bool comparesBoxes = allRelations.contains(opcode()) && heldFormAt(numberIndex) == Representation::JSValue && typeAt(numberIndex).primitives & Primitive::Integer && typeAt(otherIndex).primitives & Primitive::Integer;
        if (!comparesIntegers)
            return comparesBoxes ? Representation::JSValue : Representation::Double;
        return typeAt(numberIndex).isInt32() ? Representation::Int32 : Representation::Int64;
    }

    Representation wantedByBits(uint32_t operandIndex) const
    {
        if (opcode() == Opcode::ToBoolean)
            return isLegalRepresentation(typeAt(operandIndex), Representation::Cell32) ? Representation::Unchosen : naturalFormAt(operandIndex);
        bool comparesCells = everyOperand([&](uint32_t index) {
            return naturalFormAt(index) == Representation::Cell;
        });
        return comparesCells ? Representation::Cell : Representation::JSValue;
    }

    bool takesNumberAsItIs(uint32_t operandIndex) const
    {
        return allRelations.contains(opcode()) || isEqualityOfNumbers() || (opcode() == Opcode::ToBoolean && typeAt(operandIndex).isNumberOrNullish());
    }

    bool isSumOfBoxedNumbers() const
    {
        if (!allSums.contains(opcode()) || m_subject.form() != OperatorForm::Numbers || m_subject.representation != Representation::JSValue)
            return false;
        bool takesBoxedNumber = false;
        bool takesNothingElse = everyOperand([&](uint32_t operandIndex) {
            Type type = typeAt(operandIndex);
            if (type.isInt32())
                return true;
            takesBoxedNumber = true;
            return type.primitives & Primitive::Integer && heldFormAt(operandIndex) == Representation::JSValue;
        });
        return takesBoxedNumber && takesNothingElse;
    }

    bool comparesIntegersThatMayBeNullish() const
    {
        if (opcode() != Opcode::StrictEq || isEqualityOfNumbers())
            return false;
        return everyOperand([&](uint32_t operandIndex) {
            return typeAt(operandIndex).isInteger() || naturalFormAt(operandIndex) == Representation::OptionalInt64;
        });
    }

    bool comparesBoxesThatHoldNumbersOrNothing() const
    {
        if (opcode() != Opcode::StrictEq || m_subject.form() != OperatorForm::Numbers)
            return false;
        return everyOperand([&](uint32_t operandIndex) {
            Type type = typeAt(operandIndex);
            return heldFormAt(operandIndex) == Representation::JSValue && type.isNumberOrNullish() && !type.isNumber();
        });
    }

    Representation wantedByNumbers(uint32_t operandIndex) const
    {
        Type type = typeAt(operandIndex);
        Representation held = heldFormAt(operandIndex);
        if (comparesIntegersThatMayBeNullish())
            return Representation::OptionalInt64;
        if (comparesBoxesThatHoldNumbersOrNothing())
            return Representation::Unchosen;
        if (allBitwiseOperators.contains(opcode()) && opcode() != Opcode::BitNot) {
            if (type.isInteger())
                return integerFormOf(type);
            if (naturalFormAt(operandIndex) == Representation::OptionalInt64)
                return Representation::OptionalInt64;
            if (type.isNumber() && held == Representation::JSValue)
                return Representation::Unchosen;
        }
        if (opcode() == Opcode::ToBoolean && held == Representation::OptionalInt64)
            return Representation::OptionalInt64;
        if (isSumOfBoxedNumbers())
            return type.isInt32() ? Representation::Int32 : Representation::JSValue;
        if (takesNumberAsItIs(operandIndex) && (held == Representation::JSValue || (held == Representation::Int32 && type.isInt32())))
            return Representation::Unchosen;
        if (allRelations.contains(opcode()) && held == Representation::Int64 && type.isInteger()) {
            uint32_t otherIndex = operandIndex == leftIndex() ? rightIndex() : leftIndex();
            if (heldFormAt(otherIndex) == Representation::JSValue && typeAt(otherIndex).isNumber() && typeAt(otherIndex).primitives & Primitive::Integer)
                return Representation::Unchosen;
        }
        if (type.isNumber())
            return Representation::Double;
        RELEASE_ASSERT_WITH_MESSAGE(isLegalRepresentation(type, Representation::OptionalDouble), "AOT: an operator in the form Numbers is handed an operand that may be something other than a number, undefined or null: whoever builds it converts a boolean in front (prepareOperandsForFormNumbers)");
        return Representation::OptionalDouble;
    }

    Representation wantedByOperator(uint32_t operandIndex) const
    {
        OperatorForm form = m_subject.form();
        RELEASE_ASSERT(isLegalOperatorForm(opcode(), form));

        switch (form) {
        case OperatorForm::Generic:
        case OperatorForm::NullableString:
        case OperatorForm::MayBeString:
        case OperatorForm::MayBeStringOrNumber:
        case OperatorForm::NullableStrings:
            return Representation::JSValue;
        case OperatorForm::Integers:
            return allBitwiseOperators.contains(opcode()) ? Representation::Int32 : integerWidth();
        case OperatorForm::CheckedIntegers:
            return integerWidth();
        case OperatorForm::Numbers:
            return wantedByNumbers(operandIndex);
        case OperatorForm::NumberAndValue:
            return wantedByNumberAndValue(operandIndex);
        case OperatorForm::Bits:
            return wantedByBits(operandIndex);
        case OperatorForm::Booleans:
            return Representation::Boolean;
        case OperatorForm::Strings:
        case OperatorForm::BigInts:
        case OperatorForm::String:
            return Representation::Cell;
        case OperatorForm::StringAndValue:
            return wantedByMixedOperands(operandIndex, &Type::isAnyString, Representation::Cell);
        case OperatorForm::Int32AndValue:
            return wantedByMixedOperands(operandIndex, &Type::isInt32, Representation::Int32);
        case OperatorForm::StringAndPrimitive:
            return wantedByConcatenation(typeAt(operandIndex));
        case OperatorForm::NullableStringAndPrimitive:
            return typeAt(operandIndex).isAnyString() ? Representation::Cell : Representation::JSValue;
        case OperatorForm::Number:
        case OperatorForm::NumberByOperation:
            return unboxedNumberFormOf(typeAt(operandIndex));
        case OperatorForm::NullableNumber:
            return opcode() != Opcode::ToString && typeAt(operandIndex).isBoolean() ? Representation::Boolean : Representation::JSValue;
        case OperatorForm::Unselected:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Representation resultOfOperator() const
    {
        Opcode opcode = this->opcode();
        OperatorForm form = m_subject.form();
        RELEASE_ASSERT(isLegalOperatorForm(opcode, form));

        if (testOperators.contains(opcode))
            return Representation::Boolean;
        if (opcode == Opcode::ToString)
            return Representation::Cell;
        if (opcode == Opcode::URShift)
            return Representation::Int64;

        bool isBitwise = allBitwiseOperators.contains(opcode);
        switch (form) {
        case OperatorForm::Generic:
            if (binaryOperatorsOnInt32.contains(opcode) && m_result && m_context.types.at(m_result).isInt32())
                return Representation::Int32;
            return opcode == Opcode::ToNumber ? Representation::Double : Representation::JSValue;
        case OperatorForm::Integers:
            return isBitwise ? Representation::Int32 : integerWidth();
        case OperatorForm::Numbers:
            if (isSumOfBoxedNumbers())
                return Representation::JSValue;
            return isBitwise ? Representation::Int32 : Representation::Double;
        case OperatorForm::CheckedIntegers:
        case OperatorForm::NumberAndValue:
        case OperatorForm::NullableNumber:
            return Representation::Double;
        case OperatorForm::BigInts:
        case OperatorForm::Strings:
        case OperatorForm::StringAndPrimitive:
        case OperatorForm::MayBeString:
            return Representation::Cell;
        case OperatorForm::NullableStrings:
        case OperatorForm::NullableStringAndPrimitive:
            return Representation::JSValue;
        case OperatorForm::Booleans:
            return Representation::Int32;
        case OperatorForm::Bits:
        case OperatorForm::StringAndValue:
        case OperatorForm::Int32AndValue:
        case OperatorForm::String:
        case OperatorForm::Number:
        case OperatorForm::NumberByOperation:
        case OperatorForm::NullableString:
        case OperatorForm::MayBeStringOrNumber:
        case OperatorForm::Unselected:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Representation wantedByElementAccess(uint32_t operandIndex) const
    {
        Opcode opcode = this->opcode();
        Type type = typeAt(operandIndex);
        if (opcode != Opcode::TryPutByValDirect && OperandPosition::position(opcode) == operandIndex)
            return unboxedNumberFormOf(type);
        if (OperandPosition::value(opcode) != operandIndex) {
            if ((opcode == Opcode::GetTypedArrayElement || opcode == Opcode::PutTypedArrayElement) && variadicIndexOf(operandIndex))
                return Representation::Int64;
            return wantedByKind(operandIndex);
        }
        if (opcode == Opcode::PutTypedArrayElement)
            return unboxedNumberOrJSValue(type);
        Representation element = representationOfArrayElement(m_subject.arrayStorage());
        if (opcode == Opcode::TryPutByValDirect && m_subject.arrayStorage() == ArrayStorage::Int32 && type.isNumber() && !type.isInt32())
            return unboxedNumberFormOf(type);
        return isLegalRepresentation(type, element) ? element : Representation::JSValue;
    }

    bool isNewElement(uint32_t operandIndex) const
    {
        if (opcode() == Opcode::AppendElement)
            return operandIndex == OperandIndex::AppendElement::value;
        return !!variadicIndexOf(operandIndex);
    }

    Representation wantedByNewElement(uint32_t operandIndex) const
    {
        return unboxedElementForms.contains(heldFormAt(operandIndex)) ? Representation::Unchosen : Representation::JSValue;
    }

    Representation wantedByElementAppendedTo(uint32_t arrayIndex, uint32_t operandIndex) const
    {
        Representation wanted = wantedByNewElement(operandIndex);
        if (wanted != Representation::JSValue || !typeAt(operandIndex).isNumber())
            return wanted;
        bool isAppendedToDoubles = arrayStorageDeclaredBy(m_context.program, m_context.types, typeIDAt(arrayIndex)).predicted() == ArrayStorage::Double;
        return isAppendedToDoubles ? Representation::Double : wanted;
    }

    Representation wantedByElementOfLiteral(uint32_t operandIndex) const
    {
        if (opcode() == Opcode::AppendElement)
            return wantedByElementAppendedTo(OperandIndex::AppendElement::array, operandIndex);
        Representation wanted = wantedByNewElement(operandIndex);
        if (wanted != Representation::JSValue || m_subject.storage() != AllocationStorage::Heap)
            return wanted;
        bool isStoredAsDouble = m_subject.arrayStorage() == ArrayStorage::Double;
        return isStoredAsDouble && heldFormAt(operandIndex) == Representation::Int64 ? Representation::Double : wanted;
    }

    std::optional<InlineBuiltin> inlineBuiltin() const
    {
        if constexpr (HoldsOpcode<Subject>)
            return opcode() == Opcode::CallInline ? std::optional(m_subject.inlineBuiltin()) : std::nullopt;
        else
            return terminatorKindOf(m_subject) == TerminatorKind::TryCallInline ? std::optional(m_subject.inlineBuiltin()) : std::nullopt;
    }

    Representation wantedByBuiltinCall(uint32_t operandIndex) const
    {
        std::optional<InlineBuiltin> builtin = inlineBuiltin();
        if (!builtin)
            return wantedByKind(operandIndex);

        std::optional<uint32_t> argumentIndex = variadicIndexOf(operandIndex);
        if (argumentIndex && builtin == InlineBuiltin::ArrayPush)
            return wantedByElementAppendedTo(receiverOfBuiltinCall, operandIndex);

        Type type = typeAt(operandIndex);
        BuiltinOperand operand = argumentIndex ? BuiltinOperand::argument(*argumentIndex) : BuiltinOperand::receiver();
        Representation wanted = representationOfBuiltinParameter(m_context.types, m_subject.overload(), operand);
        if (argumentIndex && type.isNumber() && isTestOfNumber(builtin))
            wanted = Representation::Double;
        if (wanted == Representation::Double && type.isInteger())
            return integerFormOf(type);
        return isLegalRepresentation(type, wanted) ? wanted : Representation::JSValue;
    }

    bool returnsUnboxedNumber() const
    {
        if (opcode() != Opcode::CallBuiltin || m_subject.builtinCallForm() != BuiltinCallForm::Whole)
            return false;
        std::optional<uint32_t> number = BuiltinBindings::directFunctionNumberOf(m_subject.overload(), m_subject.numberOfVariadicOperands());
        return number && BuiltinBindings::directFunctionAt(*number).returnsNumber;
    }

    bool isDeferredClosure(uint32_t operandIndex) const
    {
        return formAsBuiltAt(operandIndex) == Representation::Void && !isLegalRepresentation(typeAt(operandIndex), Representation::Void);
    }

    Representation storedFormIn(const PlacedSlot& slot, uint32_t operandIndex) const
    {
        if (!slot.hasStorage() || isDeferredClosure(operandIndex))
            return Representation::Unchosen;
        if (slot.representation == SlotRepresentation::DeferredClosure)
            return Representation::Cell32;
        return representationOf(slot.representation);
    }

    Representation storedFormForKey(PropertyKeyID key, uint32_t operandIndex) const
    {
        return isDeferredClosure(operandIndex) ? Representation::Unchosen : representationStoredInSlot(m_context, key, m_subject.receivers());
    }

    bool fitsSlotsOfReceivers(uint32_t operandIndex) const
    {
        const SlotPlan& slots = m_context.plan.slots();
        Type stored = typeAt(operandIndex);
        return m_context.types.at(m_subject.receivers()).allOf([&](LayoutID member) {
            std::optional<FieldIndex> field = slots.findField(m_context.program, member, m_subject.key());
            if (!field)
                return false;
            TypeID taken = slots.slot(member, *field).type;
            return !taken || isSubset(m_context.types, stored, m_context.types.at(taken));
        });
    }

    Representation wantedByNewObject(uint32_t operandIndex) const
    {
        Representation stored = storedFormIn(m_context.plan.slot(m_subject.layout(), m_subject.fields()[*variadicIndexOf(operandIndex)]), operandIndex);
        return stored == Representation::Cell32 && heldFormAt(operandIndex) == Representation::Cell ? Representation::Unchosen : stored;
    }

    CodeUnit unitOfSelector() const
    {
        const CallingConventions& conventions = m_context.plan.conventions();
        return conventions.tableLookupOf(conventions.methodSelector(m_subject.selector()));
    }

    bool isHeldAtSuspension(uint32_t operandIndex) const
    {
        return opcode() == Opcode::AwaitCallBySavingFrame && operandIndex + m_subject.numberOfHeld().value_or(0) >= m_subject.numberOfOperands();
    }

    bool returnsOneValue(CodeUnit unit) const
    {
        return m_context.plan.conventions().numberOfResults(unit) == 1;
    }

    Representation wantedByOperation(uint32_t operandIndex) const
    {
        Opcode opcode = this->opcode();
        if (testsItsOwnGuess(m_subject)) {
            bool isIndexOfProvenList = opcode == Opcode::GetArrayElement && operandIndex == OperandIndex::GetArrayElement::position
                && readTakesIndexOfProvenList(m_context.types, typeIDAt(OperandIndex::GetArrayElement::array), typeIDAt(operandIndex));
            if (opcode == Opcode::PutArrayElement && operandIndex != OperandIndex::PutArrayElement::array
                && storeTakesIndexOfProvenList(m_context.types, typeIDAt(OperandIndex::PutArrayElement::array), typeIDAt(OperandIndex::PutArrayElement::position))) {
                if (operandIndex == OperandIndex::PutArrayElement::position)
                    return integerFormOf(typeAt(operandIndex));
                if (operandIndex == OperandIndex::PutArrayElement::value)
                    return heldFormAt(operandIndex) == Representation::Int64 ? Representation::Unchosen : wantedByNewElement(operandIndex);
            }
            static_assert(OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);
            if ((opcode == Opcode::PutSlot || opcode == Opcode::PutSlotOrAdd) && operandIndex == OperandIndex::PutSlot::value && heldFormAt(operandIndex) != Representation::JSValue && fitsSlotsOfReceivers(operandIndex)) {
                Representation stored = storedFormForKey(m_subject.key(), operandIndex);
                return stored == Representation::Unchosen ? Representation::JSValue : stored;
            }
            static_assert(OperandIndex::GetArrayElement::position == OperandIndex::PutArrayElement::position && OperandIndex::GetArrayElement::array == OperandIndex::PutArrayElement::array);
            bool isKeyOfProvenList = (opcode == Opcode::GetArrayElement || opcode == Opcode::PutArrayElement) && operandIndex == OperandIndex::GetArrayElement::position && isProvenToBeList(m_context.types, typeIDAt(OperandIndex::GetArrayElement::array));
            if (isKeyOfProvenList && heldFormAt(operandIndex) == Representation::Double && typeAt(operandIndex).isNumber())
                return Representation::Double;
            if (isKeyOfProvenList && !isIndexOfProvenList && heldFormAt(operandIndex) == Representation::OptionalDouble && typeAt(operandIndex).isNumberOrNullish())
                return Representation::OptionalDouble;
            return isIndexOfProvenList ? integerFormOf(typeAt(operandIndex)) : Representation::JSValue;
        }

        if (opcode == Opcode::PutContiguousIndex && operandIndex == OperandIndex::PutContiguousIndex::value && m_subject.goesByKindOfStorage())
            return heldFormAt(operandIndex) == Representation::Int64 ? Representation::Unchosen : wantedByNewElement(operandIndex);
        switch (representationRuleInTable(opcode)) {
        case RepresentationRule::BySchema:
        case RepresentationRule::BoxedResult:
        case RepresentationRule::SlotRead:
        case RepresentationRule::AccessorRead:
        case RepresentationRule::Capture:
        case RepresentationRule::ResultAt:
            return wantedByKind(operandIndex);
        case RepresentationRule::OwnForm:
        case RepresentationRule::Narrowed:
        case RepresentationRule::Forwarding:
        case RepresentationRule::AsItIs:
            return Representation::Unchosen;
        case RepresentationRule::Results:
            return Representation::Unchosen;
        case RepresentationRule::Operator:
            return wantedByOperator(operandIndex);
        case RepresentationRule::Truncation: {
            Type type = typeAt(operandIndex);
            if (opcode != Opcode::StringRange && !type.isInteger() && heldFormAt(operandIndex) == Representation::JSValue)
                return Representation::Unchosen;
            return unboxedNumberFormOf(type);
        }
        case RepresentationRule::ElementAccess:
            return wantedByElementAccess(operandIndex);
        case RepresentationRule::KeyedAccess: {
            Type type = typeAt(operandIndex);
            bool isUntaggedIndex = OperandPosition::property(opcode) == operandIndex && m_subject.keyIsIntegral() && type.isInteger();
            return isUntaggedIndex ? integerFormOf(type) : wantedByKind(operandIndex);
        }
        case RepresentationRule::NewElement:
            return isNewElement(operandIndex) ? wantedByElementOfLiteral(operandIndex) : wantedByKind(operandIndex);
        case RepresentationRule::Concatenation:
            return m_subject.isLikelyKey() ? wantedByKind(operandIndex) : wantedByConcatenation(typeAt(operandIndex));
        case RepresentationRule::AppendedNumber:
            if (m_subject.operandKind(operandIndex) == OperandKind::Number && heldFormAt(operandIndex) == Representation::JSValue)
                return Representation::Unchosen;
            return wantedByKind(operandIndex);
        case RepresentationRule::TestOfCellOrValue:
            if (m_subject.operandKind(operandIndex) == OperandKind::Any && heldFormAt(operandIndex) == Representation::Cell)
                return Representation::Unchosen;
            return wantedByKind(operandIndex);
        case RepresentationRule::BuiltinCall:
            return wantedByBuiltinCall(operandIndex);
        case RepresentationRule::SlotStore:
            return isDeclaredPosition(operandIndex) ? storedFormForKey(m_subject.key(), operandIndex) : wantedByKind(operandIndex);
        case RepresentationRule::NewObject:
            return isDeclaredPosition(operandIndex) ? wantedByNewObject(operandIndex) : wantedByKind(operandIndex);
        case RepresentationRule::PrivateDefinition:
            return isDeclaredPosition(operandIndex) ? Representation::JSValue : wantedByKind(operandIndex);
        case RepresentationRule::EnvironmentField:
            return isDeclaredPosition(operandIndex) ? storedFormIn(m_context.plan.slot(m_subject.layout(), m_subject.field()), operandIndex) : wantedByKind(operandIndex);
        case RepresentationRule::Variable:
            if (!isDeclaredPosition(operandIndex))
                return wantedByKind(operandIndex);
            return m_context.plan.variable(m_subject.variable()).hasWordInStatics() ? representationOfVariable(m_context, m_subject.variable()) : Representation::Unchosen;
        case RepresentationRule::NewClosure:
            return isDeclaredPosition(operandIndex) ? representationOfCapture(m_context, m_subject.function(), *variadicIndexOf(operandIndex)) : wantedByKind(operandIndex);
        case RepresentationRule::DirectCall:
            if (!isDeclaredPosition(operandIndex) || isHeldAtSuspension(operandIndex))
                return wantedByKind(operandIndex);
            return representationOfArgument(m_context, m_context.plan.conventions().unitEnteredBy(m_subject), *variadicIndexOf(operandIndex));
        case RepresentationRule::TableCall:
            return representationOfArgument(m_context, unitOfSelector(), operandIndex);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Representation wantedByTerminator(uint32_t operandIndex) const
    {
        Type type = typeAt(operandIndex);
        Representation held = heldFormAt(operandIndex);
        switch (terminatorKindOf(m_subject)) {
        case TerminatorKind::Branch:
        case TerminatorKind::TestAndBranch:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
            return wantedByKind(operandIndex);
        case TerminatorKind::Return:
            return returnsOneValue(m_context.unit) ? representationOfResult(m_context, m_context.unit, 0) : Representation::Void;
        case TerminatorKind::Switch:
            if (type.isNumber() && !type.isInteger() && held == Representation::JSValue)
                return Representation::Unchosen;
            if (held == Representation::OptionalDouble)
                return Representation::Unchosen;
            return unboxedNumberOrJSValue(type);
        case TerminatorKind::SwitchString:
            return naturalFormAt(operandIndex) == Representation::Cell ? Representation::Cell : Representation::JSValue;
        case TerminatorKind::ReturnCall:
            return Representation::Unchosen;
        case TerminatorKind::TryCallInline:
            return wantedByBuiltinCall(operandIndex);
        case TerminatorKind::Jump:
        case TerminatorKind::Unreachable:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool givesBoxedResultAt(uint32_t resultIndex) const
    {
        return unknownResultsOf(opcode()) >> (resultIndex < 31 ? resultIndex : 31) & 1;
    }

    Representation resultBySchema(uint32_t resultIndex) const
    {
        switch (resultRuleInTable(opcode())) {
        case ResultRule::None:
            return Representation::Void;
        case ResultRule::Unknown:
            return Representation::JSValue;
        case ResultRule::Constant:
        case ResultRule::Natural:
        case ResultRule::Allocation:
        case ResultRule::Declared:
            return byType();
        case ResultRule::Cast:
            return forwardedForm();
        case ResultRule::Results:
            return givesBoxedResultAt(resultIndex) ? Representation::JSValue : byType();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Representation resultOfOperation(uint32_t resultIndex) const
    {
        Opcode opcode = this->opcode();
        if (testsItsOwnGuess(m_subject))
            return resultRuleInTable(opcode) == ResultRule::None ? Representation::Void : Representation::JSValue;
        if (opcode == Opcode::GetTypedArrayLength)
            return Representation::Int64;

        switch (representationRuleInTable(opcode)) {
        case RepresentationRule::BySchema:
        case RepresentationRule::AsItIs:
        case RepresentationRule::Truncation:
        case RepresentationRule::KeyedAccess:
        case RepresentationRule::NewElement:
        case RepresentationRule::Concatenation:
        case RepresentationRule::AppendedNumber:
        case RepresentationRule::TestOfCellOrValue:
        case RepresentationRule::SlotStore:
        case RepresentationRule::NewObject:
        case RepresentationRule::PrivateDefinition:
            return resultBySchema(resultIndex);
        case RepresentationRule::OwnForm:
            return isConstantWithoutChosenForm() ? byType() : m_subject.representation;
        case RepresentationRule::Narrowed:
            return m_subject.representation == Representation::Unchosen ? forwardedForm() : m_subject.representation;
        case RepresentationRule::Forwarding:
            return forwardedForm();
        case RepresentationRule::Operator:
            return resultOfOperator();
        case RepresentationRule::ElementAccess:
            if (opcode == Opcode::GetArrayElement)
                return legalOrByType(representationOfArrayElement(m_subject.arrayStorage()));
            if (opcode == Opcode::GetTypedArrayElement)
                return legalOrByType(m_subject.typedArrayType() == NotTypedArray ? Representation::Double : representationOf(m_context.types, elementTypeOf(m_subject.typedArrayType())));
            return resultBySchema(resultIndex);
        case RepresentationRule::BuiltinCall:
            if (returnsUnboxedNumber())
                return Representation::Double;
            if (opcode == Opcode::CallBuiltin)
                return Representation::JSValue;
            if (opcode == Opcode::CallInline && (m_subject.inlineBuiltin() == InlineBuiltin::MapGet || m_subject.inlineBuiltin() == InlineBuiltin::WeakMapGet) && RepresentationSet { Representation::Int32, Representation::Double, Representation::OptionalDouble }.contains(byType()))
                return Representation::JSValue;
            return byType();
        case RepresentationRule::BoxedResult:
            return Representation::JSValue;
        case RepresentationRule::SlotRead:
            return m_result ? representationLoadedFromSlot(m_context, m_subject.key(), m_subject.receivers(), m_result) : Representation::Unchosen;
        case RepresentationRule::AccessorRead:
            return Representation::Cell;
        case RepresentationRule::EnvironmentField:
            return opcode == Opcode::GetEnvironmentField ? representationOf(m_context.plan.slot(m_subject.layout(), m_subject.field()).representation) : Representation::Void;
        case RepresentationRule::Variable: {
            if (opcode == Opcode::PutModuleVariable)
                return Representation::Void;
            Representation planned = representationOfVariable(m_context, m_subject.variable());
            return planned == Representation::Unchosen ? byType() : planned;
        }
        case RepresentationRule::Capture:
            return representationOfCapture(m_context, m_subject.function(), m_subject.index());
        case RepresentationRule::NewClosure:
            return m_subject.closureForm() == ClosureForm::Deferred ? Representation::Void : byType();
        case RepresentationRule::DirectCall:
            return opcode == Opcode::CallDirect ? representationOfResult(m_context, m_context.plan.conventions().unitEnteredBy(m_subject), resultIndex) : Representation::JSValue;
        case RepresentationRule::TableCall:
            return representationOfResult(m_context, unitOfSelector(), resultIndex);
        case RepresentationRule::Results:
            return formAsBuiltAt(OperandIndex::Results::values + resultIndex);
        case RepresentationRule::ResultAt:
            if (m_subject.representation != Representation::Unchosen)
                return m_subject.representation;
            return hasRegister(formAsBuiltAt(OperandIndex::ResultAt::results)) ? formAsBuiltAt(OperandIndex::ResultAt::results) : byType();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const RepresentationContext& m_context;
    const Subject& m_subject;
    const OperandForms& m_forms;
    TypeID m_result;
    uint32_t m_variadicIndex { UINT32_MAX };
    TypeID m_variadicType;
    Representation m_variadicForm { Representation::Unchosen };
};

constexpr uint32_t mostNamedOperandsOfEntry = []() consteval {
    uint32_t most = 0;
    for (Opcode opcode : allMembersOf<Opcode>)
        most = std::max<uint32_t>(most, numberOfNamedOperandsOf(opcode));
    for (TerminatorKind kind : allMembersOf<TerminatorKind>)
        most = std::max<uint32_t>(most, numberOfNamedOperandsOf(kind));
    return most;
}();

class FormsOfNamedOperands {
public:
    ALWAYS_INLINE FormsOfNamedOperands(const Walker& walker, const DraftWords& draft)
    {
        uint32_t numberOfNamedOperands = draft.numberOfNamedOperands();
        std::span<const Reference> operands = draft.operands();
        RELEASE_ASSERT(numberOfNamedOperands <= mostNamedOperandsOfEntry && numberOfNamedOperands <= operands.size());
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperands; ++operandIndex) {
            Reference operand = operands[operandIndex];
            m_types[operandIndex] = operand ? walker.typeOf(operand) : TypeID();
            m_representations[operandIndex] = operand ? walker.representationOf(operand) : Representation::Unchosen;
        }
        m_forms = { std::span<const TypeID>(m_types.data(), numberOfNamedOperands), std::span<const Representation>(m_representations.data(), numberOfNamedOperands) };
    }

    const OperandForms& forms() const LIFETIME_BOUND { return m_forms; }

private:
    std::array<TypeID, mostNamedOperandsOfEntry> m_types;
    std::array<Representation, mostNamedOperandsOfEntry> m_representations;
    OperandForms m_forms;
};

} // anonymous namespace

template<typename Subject>
Representation wantedRepresentationOf(const RepresentationContext& context, const Subject& subject, const OperandForms& forms, uint32_t operandIndex)
{
    TypeID result;
    if constexpr (std::is_same_v<Subject, Operation>)
        result = subject.type;
    return RepresentationRuleTable<Subject>(context, subject, forms, result).wantedAt(operandIndex);
}

template<typename Subject>
Representation resultRepresentationOf(const RepresentationContext& context, const Subject& subject, const OperandForms& forms, TypeID result, uint32_t resultIndex)
{
    RepresentationRuleTable<Subject> table(context, subject, forms, result);
    if constexpr (HoldsOpcode<Subject>) {
        if (!result && table.holdsNoneOfItsResults())
            return Representation::Void;
    }
    return table.resultAt(resultIndex);
}

template<typename Subject>
bool isEqualityOfNumbers(const RepresentationContext& context, const Subject& subject, const OperandForms& forms)
{
    if constexpr (HoldsOpcode<Subject>)
        return RepresentationRuleTable<Subject>(context, subject, forms, TypeID()).isEqualityOfNumbers();
    else
        return false;
}

template Representation wantedRepresentationOf<Draft>(const RepresentationContext&, const Draft&, const OperandForms&, uint32_t);
template Representation wantedRepresentationOf<Operation>(const RepresentationContext&, const Operation&, const OperandForms&, uint32_t);
template Representation wantedRepresentationOf<TerminatorDraft>(const RepresentationContext&, const TerminatorDraft&, const OperandForms&, uint32_t);
template Representation wantedRepresentationOf<Terminator>(const RepresentationContext&, const Terminator&, const OperandForms&, uint32_t);
template Representation resultRepresentationOf<Draft>(const RepresentationContext&, const Draft&, const OperandForms&, TypeID, uint32_t);
template Representation resultRepresentationOf<Operation>(const RepresentationContext&, const Operation&, const OperandForms&, TypeID, uint32_t);
template Representation resultRepresentationOf<TerminatorDraft>(const RepresentationContext&, const TerminatorDraft&, const OperandForms&, TypeID, uint32_t);
template Representation resultRepresentationOf<Terminator>(const RepresentationContext&, const Terminator&, const OperandForms&, TypeID, uint32_t);
template bool isEqualityOfNumbers<Draft>(const RepresentationContext&, const Draft&, const OperandForms&);
template bool isEqualityOfNumbers<Operation>(const RepresentationContext&, const Operation&, const OperandForms&);
template bool isEqualityOfNumbers<TerminatorDraft>(const RepresentationContext&, const TerminatorDraft&, const OperandForms&);
template bool isEqualityOfNumbers<Terminator>(const RepresentationContext&, const Terminator&, const OperandForms&);

template<typename DraftType>
static WantedRepresentations wantedRepresentationsFromWalker(Walker& walker, const DraftType& draft)
{
    WantedRepresentations wanted;
    uint32_t numberOfOperands = draft.numberOfOperands();
    bool hasOperand = false;
    for (Reference operand : draft.operands())
        hasOperand |= !!operand;
    if (!hasOperand) {
        wanted.fill(Representation::Unchosen, numberOfOperands);
        return wanted;
    }

    RepresentationContext context { walker.program(), walker.plan(), walker.types(), walker.unit() };
    FormsOfNamedOperands named(walker, draft);
    RepresentationRuleTable<DraftType> table(context, draft, named.forms(), TypeID());
    bool resultIsSet = false;

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        if (!operand) {
            wanted.append(Representation::Unchosen);
            continue;
        }
        if (operandIndex >= draft.numberOfNamedOperands())
            table.setVariadicOperand(operandIndex, walker.typeOf(operand), walker.representationOf(operand));
        if constexpr (HoldsOpcode<DraftType>) {
            if (!resultIsSet && table.widthFollowsFromResult())
                table.setResult(walker.resultTypeOf(draft));
            resultIsSet = true;
        }
        wanted.append(table.wantedAt(operandIndex));
    }
    return wanted;
}

WantedRepresentations wantedRepresentationsOf(Walker& walker, const Draft& draft)
{
    return wantedRepresentationsFromWalker(walker, draft);
}

WantedRepresentations wantedRepresentationsOf(Walker& walker, const TerminatorDraft& draft)
{
    return wantedRepresentationsFromWalker(walker, draft);
}

static constexpr OpcodeSet opcodesWhoseResultFormLooksAtNoOperand = []() consteval {
    constexpr RepresentationRuleSet rulesThatLookAtOperands {
        RepresentationRule::Narrowed, RepresentationRule::Forwarding, RepresentationRule::Operator, RepresentationRule::Results, RepresentationRule::ResultAt
    };
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (!rulesThatLookAtOperands.contains(representationRuleOf(opcode)) && resultRuleOf(opcode) != ResultRule::Cast)
            opcodes.add(opcode);
    }
    return opcodes;
}();

constinit const std::array<Representation, numberOfOpcodes> formsOfResultsByOpcode = []() consteval {
    constexpr RepresentationRuleSet rulesThatGoBySchema {
        RepresentationRule::BySchema, RepresentationRule::AsItIs, RepresentationRule::Truncation, RepresentationRule::KeyedAccess,
        RepresentationRule::NewElement, RepresentationRule::Concatenation, RepresentationRule::AppendedNumber, RepresentationRule::TestOfCellOrValue, RepresentationRule::SlotStore,
        RepresentationRule::NewObject, RepresentationRule::PrivateDefinition
    };
    std::array<Representation, numberOfOpcodes> forms { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        Representation form = Representation::Unchosen;
        RepresentationRule rule = representationRuleOf(opcode);
        if (!opcodesThatMayHaveSeveralResults.contains(opcode) && !genericOperationOf(opcode)) {
            if (rulesThatGoBySchema.contains(rule) && resultRuleOf(opcode) == ResultRule::None)
                form = Representation::Void;
            else if (rulesThatGoBySchema.contains(rule) && resultRuleOf(opcode) == ResultRule::Unknown)
                form = Representation::JSValue;
            else if (rule == RepresentationRule::BoxedResult)
                form = Representation::JSValue;
            else if (rule == RepresentationRule::AccessorRead)
                form = Representation::Cell;
            else if (opcode == Opcode::GetTypedArrayLength)
                form = Representation::Int64;
        }
        forms[static_cast<unsigned>(opcode)] = form;
    }
    return forms;
}();

constinit const std::array<Representation, numberOfOpcodes> formsOfResultsOfOperatorsByOpcode = []() consteval {
    std::array<Representation, numberOfOpcodes> forms { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        Representation form = Representation::Unchosen;
        if (representationRuleOf(opcode) == RepresentationRule::Operator && !opcodesThatMayHaveSeveralResults.contains(opcode) && !genericOperationOf(opcode)) {
            if (operatorsThatTestInForms.contains(opcode))
                form = Representation::Boolean;
            else if (opcode == Opcode::ToString)
                form = Representation::Cell;
            else if (opcode == Opcode::URShift)
                form = Representation::Int64;
        }
        forms[static_cast<unsigned>(opcode)] = form;
    }
    return forms;
}();

constinit const OpcodeSet opcodesWhoseResultsHaveFormsOfTheirTypes = []() consteval {
    constexpr RepresentationRuleSet rulesThatGoBySchema {
        RepresentationRule::BySchema, RepresentationRule::AsItIs, RepresentationRule::Truncation, RepresentationRule::KeyedAccess,
        RepresentationRule::NewElement, RepresentationRule::Concatenation, RepresentationRule::AppendedNumber, RepresentationRule::TestOfCellOrValue, RepresentationRule::SlotStore,
        RepresentationRule::NewObject, RepresentationRule::PrivateDefinition
    };
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        ResultRule rule = resultRuleOf(opcode);
        bool goesByType = rule == ResultRule::Constant || rule == ResultRule::Natural || rule == ResultRule::Allocation;
        if (goesByType && opcode != Opcode::GetTypedArrayLength && rulesThatGoBySchema.contains(representationRuleOf(opcode)) && !opcodesThatMayHaveSeveralResults.contains(opcode) && !genericOperationOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

template<typename ProvenType>
static Representation resultRepresentationFromWalker(Walker& walker, const Draft& draft, const ProvenType& provenType)
{
    RepresentationContext context { walker.program(), walker.plan(), walker.types(), walker.unit() };
    std::optional<FormsOfNamedOperands> named;
    OperandForms noForms;
    if (!opcodesWhoseResultFormLooksAtNoOperand.contains(draft.opcode))
        named.emplace(walker, draft);
    RepresentationRuleTable<Draft> table(context, draft, named ? named->forms() : noForms, TypeID());
    if (table.holdsNoneOfItsResults())
        return Representation::Void;

    if (draft.opcode == Opcode::ResultAt && draft.representation == Representation::Unchosen) {
        if (Reference producer = draft.operand(OperandIndex::ResultAt::results); producer && producer.isRecord()) {
            const Code& code = walker.code();
            const Operation& record = code.at(producer.id());
            if (record.opcode == Opcode::Results) {
                std::span<const ValueID> values = record.valuesOperands();
                return draft.index() < values.size() ? code.at(values[draft.index()]).representation : Representation::Unchosen;
            }
            return resultRepresentationOf(context, record, OperandForms { }, provenType(), draft.index());
        }
    }

    if (table.resultFollowsFromType())
        table.setResult(provenType());
    return table.resultAt(0);
}

Representation resultRepresentationOf(Walker& walker, const Draft& draft)
{
    return resultRepresentationFromWalker(walker, draft, [&] {
        return walker.resultTypeOf(draft);
    });
}

Representation resultRepresentationOf(Walker& walker, const Draft& draft, TypeID proven)
{
    return resultRepresentationFromWalker(walker, draft, [&] {
        return proven;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
