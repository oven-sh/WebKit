/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkExceptions.h"
#include "AOTLocalesPassedAtCalls.h"
#include "AOTNameClass.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include "AOTUnionTaggedMakers.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint8_t farthestObservedForLinkCalls = 2;
static constexpr uint8_t notObservedForLinkCalls = farthestObservedForLinkCalls + 1;

static constexpr uint16_t primitivesWithClassesForLinkCalls[] { Primitive::AnyString, LinkType::closed(Primitive::AnySymbol), Primitive::BigInt, LinkType::closed(Primitive::Number), Primitive::Boolean };

static constexpr OperandUses usesThatRecordsSpellOutForLinkCalls {
    OperandUse::Retains, OperandUse::RetainsContents, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::PassesToUserCode,
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype,
};

static constexpr OperandUses usesThatChangeObjectsForLinkCalls {
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype,
};

static constexpr OperandUses usesThatChangeListsForLinkCalls {
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::WritesContents, OperandUse::MovesElements,
};

static constexpr OperandUses usesThatMayMakeHolesForLinkCalls {
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
};

static constexpr OperandUses usesThatOnlyWriteOnObjectsForLinkCalls {
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
};

static constexpr OperandUses usesThatNeedNoRecordForLinkCalls {
    OperandUse::Inspects, OperandUse::Returns, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::MovesElements,
    OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
};

static constexpr OperandUses usesThatLeaveObjectsUnchangedForLinkCalls {
    OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly,
    OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents, OperandUse::MovesElements, OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
};

static_assert(everyOperandUseIsInExactlyOneOf({ usesThatRecordsSpellOutForLinkCalls, usesThatNeedNoRecordForLinkCalls }));
static_assert(everyOperandUseIsInExactlyOneOf({ usesThatChangeObjectsForLinkCalls, usesThatLeaveObjectsUnchangedForLinkCalls }));

static constexpr BuiltinMark marksOfRowsThatSayTooLittleForLinkCalls[] {
    BuiltinMark::EvaluatesTextInGlobalScope, BuiltinMark::HoldsLoader, BuiltinMark::IsSpy, BuiltinMark::LoadsCodeWithoutJSAccess, BuiltinMark::IsResolverThatLoaderCalls,
    BuiltinMark::IsRequireThatLoaderCalls, BuiltinMark::MocksModule, BuiltinMark::ExtendsMatchers, BuiltinMark::FakesTimers,
    BuiltinMark::RunsCodeWithLoader, BuiltinMark::ExposesNativeRoots,
};

struct SourceInLinkCalls {
    FlowNodeID node;
    uint8_t distance { 0 };
    TypeID passes;
};

struct ValuesInLinkCalls {
    Vector<SourceInLinkCalls, 2> sources;
    LinkType made;
    bool builtinCameByEscapeAlone { false };

    bool isEmpty() const { return sources.isEmpty() && made.isNever(); }
};

struct PassedInLinkCalls {
    PassedInLinkCalls() { }

    PassedInLinkCalls withoutReceiver() const
    {
        PassedInLinkCalls passed;
        passed.arguments = arguments;
        passed.list = list;
        passed.isConstruction = isConstruction;
        passed.newTarget = newTarget;
        return passed;
    }

    std::optional<ValuesInLinkCalls> receiver;
    std::span<const ValuesInLinkCalls> arguments;
    const ValuesInLinkCalls* list { nullptr };
    bool listIsGathered { false };
    bool isConstruction { false };
    const ValuesInLinkCalls* newTarget { nullptr };
};

struct PassedMadeInLinkCalls : PassedInLinkCalls {
    WTF_MAKE_NONCOPYABLE(PassedMadeInLinkCalls);
    WTF_MAKE_NONMOVABLE(PassedMadeInLinkCalls);
public:
    PassedMadeInLinkCalls() { }

    void reserveArguments(size_t numberOfArguments)
    {
        if (numberOfArguments > m_arguments.capacity()) [[unlikely]]
            m_arguments.reserveCapacity(numberOfArguments);
    }
    ValuesInLinkCalls& appendArgument()
    {
        m_arguments.constructAndAppend();
        arguments = m_arguments.span();
        return m_arguments.last();
    }
    void appendArgument(ValuesInLinkCalls&& values)
    {
        m_arguments.append(WTF::move(values));
        arguments = m_arguments.span();
    }
    void appendArgument(const ValuesInLinkCalls& values)
    {
        m_arguments.append(values);
        arguments = m_arguments.span();
    }
    ValuesInLinkCalls& ensureList()
    {
        if (!m_list)
            m_list = ValuesInLinkCalls();
        list = &*m_list;
        return *m_list;
    }
    void setNewTarget(ValuesInLinkCalls&& values)
    {
        m_newTarget = WTF::move(values);
        newTarget = &*m_newTarget;
    }

private:
    Vector<ValuesInLinkCalls, 4> m_arguments;
    std::optional<ValuesInLinkCalls> m_list;
    std::optional<ValuesInLinkCalls> m_newTarget;
};

struct ValuesAtOperandInLinkCalls {
    const ValuesInLinkCalls* standing { nullptr };
    ValuesInLinkCalls made;

    operator const ValuesInLinkCalls&() const { return standing ? *standing : made; }
};

struct ResultInLinkCalls {
    FlowNodeID node;
    uint32_t index { 0 };
};

struct FitInLinkCalls {
    bool mayBeInside { false };
    bool mayBeOutside { false };
    bool mayBeInsideInSight { false };
};

struct QuestionsInLinkCalls {
    Vector<PropertyKeyID, 8> keys;
    Vector<PropertyKeyID, 4> keysAnsweredByProgramAlone;
    Vector<PropertyKeyID, 4> keysOfConversion;
    bool readEverything { false };
    bool readOwnValues { false };
    bool readPrivateFields { false };
    bool readThroughPrototypes { false };
    bool describeObjects { false };
    bool describesLiteralMadeForCall { false };
    bool convertToPrimitive { false };
    bool onlyBoxedPrimitivesAreConverted { false };
    bool callMembers { false };
    bool iterate { false };
    bool membersOfTableAnswerNothing { false };
    bool ownPropertiesAnswerNothing { false };
    bool areAskedByFormatter { false };
    bool onlyFollow { false };
    bool askConstructorsOfInstances { false };
    bool whatIsFoundIsOnlyInspected { false };
    bool listKeys { false };

    bool isEmpty() const { return keys.isEmpty() && keysAnsweredByProgramAlone.isEmpty() && !readEverything && !readOwnValues && !readPrivateFields && !describeObjects && !convertToPrimitive && !callMembers; }
    friend bool operator==(const QuestionsInLinkCalls&, const QuestionsInLinkCalls&) = default;
};

struct InspectionInLinkCalls {
    QuestionsInLinkCalls questions;
    FunctionID function;
    uint32_t call { 0 };
    uint32_t stepForValues { 0 };
    BitVector nodes;
};

struct StepOfInspectionInLinkCalls {
    uint32_t inspection { 0 };
    LinkType subject;
};

struct SharedCallInLinkCalls {
    enum class Subjects : uint8_t {
        Instances,
        PrototypeObjects,
        ClassObjects,
        CalledClassObjects,
    };

    Subjects subjects { Subjects::Instances };
    FamilyID family;
    uint32_t index { 0 };
    FunctionID firstCaller;
    uint32_t firstCall { 0 };
    SummaryCall call;
    ThinSummary summary;
    Vector<FlowNodeID, 6> nodes;
    bool hasList { false };
    FlowNodeID result;
    FlowNodeID thrown;
    bool someMembersSleep { false };
    bool someSleeperWasWoken { false };
    Vector<LayoutID> membersAwakeFromTheStart;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersWokenLater;
};

struct MemberOfSharedCallInLinkCalls {
    uint32_t sharedCall { 0 };
    LayoutID layout;
    uint32_t tellsThatBringNothing { 0 };
};

struct FirstStepToHandlerInLinkCalls {
    BuiltinOperand operand;
    BuiltinName name;
    bool mayBeThere { true };
};

struct MemberWrittenInLinkCalls {
    PropertyKeyID key;
    ValueID value;
    ValueID writer;
    bool nothingIsKnownOfWriter { false };
};

struct LiteralMadeForCallInLinkCalls {
    ValueID handedOn;
    bool isInSight { false };
    Vector<MemberWrittenInLinkCalls, 8> written;
};

struct PathIntoLiteralInLinkCalls {
    const BuiltinHandlerCall* handlerCall { nullptr };
    const BuiltinTransfer* transfer { nullptr };
    std::span<const BuiltinName> stepsLeft;
};

struct RetainedExportInLinkCalls {
    RuntimeModuleID module;
    BuiltinName name;
    std::span<const BuiltinName> memberPath;
};

struct NativeCellInLinkCalls {
    BuiltinName name;
    bool isCalled { false };
    bool isValueRead { false };
    FlowNodeID kept;
    Vector<RetainedExportInLinkCalls, 1> retainedExports { };
};

struct RowCallInLinkCalls {
    BuiltinID builtin;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    const PassedInLinkCalls& passed;
    bool isSeenByScan { false };
    bool isCalledByBuiltin { false };
    const BuiltinOverload* row { nullptr };
    bool resultIsUnknown { false };
    ValuesInLinkCalls result;
    Vector<FirstStepToHandlerInLinkCalls, 8> firstStepsToHandlers;
    mutable Vector<LiteralMadeForCallInLinkCalls, 1> literalsMadeForCall;
    Vector<BuiltinOperand, 1> literalsTakenApart;
    Vector<BuiltinOperand, 1> objectsTakenApart;
};

static LinkType primitivesForLinkCalls(uint16_t primitives)
{
    LinkType type;
    type.primitives = LinkType::closed(primitives);
    if (!type.hasNumbers())
        type.counts = LinkType::Counts::BelowTwoTo31;
    else if (!(type.primitives & Primitive::Number))
        type.counts = LinkType::Counts::Int32;
    return type;
}

static LinkType elementsOfViewForLinkCalls(TypedArrayType kind)
{
    switch (kind) {
    case TypeInt8:
    case TypeUint8:
    case TypeUint8Clamped:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
        return primitivesForLinkCalls(LinkType::primitivesOf(Primitive::Integer, Range::int32()));
    case TypeUint32:
        return primitivesForLinkCalls(Primitive::Integer);
    case TypeFloat16:
    case TypeFloat32:
    case TypeFloat64:
        return primitivesForLinkCalls(Primitive::Number);
    case TypeBigInt64:
    case TypeBigUint64:
        return primitivesForLinkCalls(Primitive::BigInt);
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    return primitivesForLinkCalls(Primitive::Number | Primitive::BigInt);
}

static LinkType primitivesWrittenForLinkCalls(const BuiltinType& written)
{
    return primitivesForLinkCalls(LinkType::primitivesOf(primitivesOfBuiltinType(written), integerRangeWrittenBy(written)));
}

static LinkType wholeNumbersForLinkCalls()
{
    return primitivesForLinkCalls(Primitive::NumberWithoutFraction);
}

static LinkType closuresForLinkCalls(FunctionID function)
{
    LinkType type;
    type.functions = LinkType::Functions::Closure;
    type.function = function;
    return type;
}

static LinkType instancesAmongForLinkCalls(const LinkType& type)
{
    return type.instancesAlone();
}

static bool holdsOnlyInstancesOfOneClassForLinkCalls(const LinkType& type)
{
    return type.builtinClass && type == instancesAmongForLinkCalls(type);
}

static LinkType builtinForLinkCalls(BuiltinID builtin)
{
    LinkType type;
    type.builtin = builtin;
    return type;
}

static LinkType objectsForLinkCalls(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Layout;
    type.layout = layout;
    return type;
}

static LinkType prototypeForLinkCalls(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototype;
    type.layout = layout;
    return type;
}

static LinkType prototypesForLinkCalls(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototypes;
    type.family = family;
    return type;
}

static LinkType unseenObjectsForLinkCalls()
{
    LinkType type;
    type.hasUnknownObjects = true;
    return type;
}

static bool isEnteredByCallsAloneForLinkCalls(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return true;
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinClassID classOfClosuresForLinkCalls(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    if (function.has(FunctionFlags::IsGenerator))
        return isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
}

static bool operandsOverlapForLinkCalls(BuiltinOperand first, BuiltinOperand second)
{
    if (first.isReceiver() || second.isReceiver())
        return first == second;
    if (first.kind() == BuiltinOperand::Kind::Argument)
        return second.coversArgument(first.argumentIndex());
    return second.kind() == BuiltinOperand::Kind::ArgumentsFrom || first.coversArgument(second.argumentIndex());
}

enum class SaidOfOperandInLinkCalls : uint8_t { Itself, WhatItHolds, Either };

static bool typeNamesOperandForLinkCalls(const BuiltinTable& builtins, BuiltinTypeID id, BuiltinOperand operand, SaidOfOperandInLinkCalls said, Vector<BuiltinTypeID, 8>& visitedTypes)
{
    if (!id || !visitedTypes.appendIfNotContains(id))
        return false;
    return std::ranges::any_of(builtins.at(id).parts, [&](const BuiltinTypePart& part) {
        bool saysIt = said == SaidOfOperandInLinkCalls::Either || (part.kind == BuiltinTypePartKind::Operand) == (said == SaidOfOperandInLinkCalls::Itself);
        return (saysIt && part.operand && operandsOverlapForLinkCalls(*part.operand, operand))
            || std::ranges::any_of(part.contents, [&](const BuiltinSlotContents& contents) {
                return typeNamesOperandForLinkCalls(builtins, contents.type, operand, said, visitedTypes);
            });
    });
}

static bool recordsSpellOutForLinkCalls(const BuiltinTable& builtins, const BuiltinOverload& row, BuiltinOperand operand, OperandUse use)
{
    using Said = SaidOfOperandInLinkCalls;
    auto isNamed = [&](std::optional<BuiltinOperand> named) {
        return named && operandsOverlapForLinkCalls(*named, operand);
    };
    auto isNamedAt = [&](const BuiltinPlace& place, Said said) {
        Vector<BuiltinTypeID, 8> visitedTypes;
        bool saysIt = said == Said::Either || (place.kind == BuiltinPlaceKind::Operand) == (said == Said::Itself);
        return (saysIt && isNamed(place.operand)) || typeNamesOperandForLinkCalls(builtins, place.newValueType, operand, said, visitedTypes);
    };
    auto isHandedToCallback = [&](Said said) {
        return std::ranges::any_of(row.flow.callbacks, [&](const BuiltinCallback& callback) {
            return isNamedAt(callback.newTarget, said) || isNamedAt(callback.thisValue, said) || isNamedAt(callback.spreadArguments, said)
                || std::ranges::any_of(callback.arguments, [&](const BuiltinPlace& argument) {
                    return isNamedAt(argument, said);
                });
        });
    };
    bool isNamedByHandlerCall = std::ranges::any_of(row.flow.handlerCalls, [&](const BuiltinHandlerCall& handlerCall) {
        return isNamedAt(handlerCall.callee, Said::Either) || isNamedAt(handlerCall.thisValue, Said::Either)
            || std::ranges::any_of(handlerCall.arguments, [&](const BuiltinPlace& argument) {
                return isNamedAt(argument, Said::Either);
            });
    });
    if (isNamedByHandlerCall)
        return true;
    auto goesSomewhere = [&](Said said) {
        Vector<BuiltinTypeID, 8> visitedTypes;
        return typeNamesOperandForLinkCalls(builtins, row.result.type, operand, said, visitedTypes)
            || std::ranges::any_of(row.flow.transfers, [&](const BuiltinTransfer& transfer) {
                return isNamedAt(transfer.from, said);
            })
            || isHandedToCallback(said);
    };
    auto isActedOn = [&](std::initializer_list<BuiltinPropertyActionKind> kinds) {
        return std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
            return isNamed(action.object) && std::ranges::find(kinds, action.kind) != kinds.end();
        });
    };
    using Action = BuiltinPropertyActionKind;

    switch (use) {
    case OperandUse::Retains:
        return goesSomewhere(Said::Itself) || isNamed(row.result.prototypeSource) || isActedOn({ Action::BecomesPrototype })
            || std::ranges::any_of(row.flow.callbacks, [&](const BuiltinCallback& callback) {
                return isNamed(callback.callee);
            })
            || std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return isNamedAt(action.value, Said::Itself);
            });
    case OperandUse::RetainsContents:
        return goesSomewhere(Said::WhatItHolds) || isNamed(row.result.iteratedOperand)
            || isActedOn({ Action::ReadsByKey, Action::ReadsGetter, Action::ReadsSetter, Action::DescribesOwnProperties, Action::DescribesResult, Action::StoresByKey, Action::DeletesByKey })
            || std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return isNamedAt(action.value, Said::Either);
            });
    case OperandUse::Calls:
    case OperandUse::CallsPerElement:
    case OperandUse::CallsRepeatedly:
        return std::ranges::any_of(row.flow.callbacks, [&](const BuiltinCallback& callback) {
            return isNamed(callback.callee);
        });
    case OperandUse::PassesToUserCode:
        return isHandedToCallback(Said::Either);
    case OperandUse::StoresByKey:
    case OperandUse::DeletesByKey:
    case OperandUse::AssignsFrom:
    case OperandUse::DefinesName:
    case OperandUse::TakesDescriptor:
    case OperandUse::ChangesIntegrity:
    case OperandUse::ChangesPrototype:
        return isActedOn({ Action::StoresByKey, Action::DeletesByKey, Action::AssignsFrom, Action::DefinesName, Action::TakesDescriptor, Action::TakesDescriptorMap, Action::ChangesIntegrity, Action::ChangesPrototype,
            Action::BecomesPrototype, Action::DefinesGetter, Action::DefinesSetter, Action::DefinesFrom });
    case OperandUse::BecomesPrototype:
        return isActedOn({ Action::BecomesPrototype })
            || std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return action.kind == Action::ChangesPrototype && isNamedAt(action.value, Said::Itself);
            });
    case OperandUse::Inspects:
    case OperandUse::Returns:
    case OperandUse::ReadsContents:
    case OperandUse::WritesContents:
    case OperandUse::MovesElements:
    case OperandUse::AppliesTransitively:
    case OperandUse::AppliesToContents:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool recordsSpellOutEveryUseForLinkCalls(const BuiltinTable& builtins, const BuiltinOverload& row, const BuiltinParameter& parameter)
{
    for (OperandUse use : parameter.uses & usesThatRecordsSpellOutForLinkCalls) {
        if (!recordsSpellOutForLinkCalls(builtins, row, parameter.operand, use))
            return false;
    }
    return true;
}

static bool fieldOfResultHoldsOnlyWhatIsStoredForLinkCalls(const BuiltinOverload& row, uint16_t fieldIndex, bool isOfElements)
{
    auto isField = [&](const BuiltinPlace& place) {
        return place.kind == BuiltinPlaceKind::ResultField && place.fieldIndex == fieldIndex && place.isOfElements == isOfElements && place.holdsOnlyWhatIsStored;
    };
    return std::ranges::any_of(row.flow.transfers, isField, &BuiltinTransfer::to)
        || std::ranges::any_of(row.flow.callbacks, isField, &BuiltinCallback::resultTo)
        || std::ranges::any_of(row.flow.propertyActions, isField, &BuiltinPropertyAction::value);
}

static bool someRecordFillsResultSlotForLinkCalls(const BuiltinOverload& row, BuiltinSlotID slot)
{
    auto isSlot = [&](const BuiltinPlace& place) {
        return place.kind == BuiltinPlaceKind::ResultSlot && place.slot == slot;
    };
    return std::ranges::any_of(row.flow.transfers, isSlot, &BuiltinTransfer::to)
        || std::ranges::any_of(row.flow.callbacks, isSlot, &BuiltinCallback::resultTo)
        || std::ranges::any_of(row.flow.propertyActions, isSlot, &BuiltinPropertyAction::value);
}

static bool rowSaysTooLittleForLinkCalls(const BuiltinOverload& row)
{
    constexpr OptionSet<BuiltinOperandRole> rolesOfLoaders { BuiltinOperandRole::EvaluatedAsText, BuiltinOperandRole::ModuleSpecifier, BuiltinOperandRole::WorkerEntryPath };
    if (row.isUndeclared())
        return true;
    if (row.receiver && row.receiver->roles.containsAny(rolesOfLoaders))
        return true;
    return std::ranges::any_of(row.parameters, [&](const BuiltinParameter& parameter) {
        return parameter.roles.containsAny(rolesOfLoaders);
    });
}

class StateOfLinkCalls {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkCalls);
    WTF_MAKE_NONCOPYABLE(StateOfLinkCalls);
public:
    explicit StateOfLinkCalls(const LinkFlow&);

    std::span<const ResultInLinkCalls> resultsOf(FunctionID caller, uint32_t call) const LIFETIME_BOUND;

    struct FirstObservedNodes {
        FlowNodeID first;
        FlowNodeID second;
    };
    bool addObserved(unsigned lane, uint32_t call, FlowNodeID node)
    {
        FirstObservedNodes& ofCall = firstObserved[call];
        if (ofCall.first == node || ofCall.second == node)
            return false;
        if (!ofCall.first) {
            ofCall.first = node;
            return true;
        }
        if (!ofCall.second) {
            ofCall.second = node;
            return true;
        }
        return observed[lane].add(static_cast<uint64_t>(call) << 32 | node.index()).isNewEntry;
    }

    Vector<FirstObservedNodes> firstObserved;
    Vector<HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> observed;
    Vector<HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> observedAsOperandsOnlyLookedAt;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> layoutsLaidDown;
    Vector<InspectionInLinkCalls> inspections;
    Vector<StepOfInspectionInLinkCalls> stepsOfInspections;
    HashSet<std::pair<uint64_t, uint32_t>, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> subjectsOfInspections;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> observedBySteps;
    Vector<uint32_t> tellsThatBringNothing;

    HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> indexesOfSharedCalls;
    Vector<std::unique_ptr<SharedCallInLinkCalls>> sharedCalls;
    Vector<MemberOfSharedCallInLinkCalls> membersOfSharedCalls;
    static constexpr uint32_t isPorterOfFamily = 1u << 31;
    struct SleepersOfFamily {
        Vector<uint32_t> sharedCalls;
        Vector<LayoutID> membersThatAreUp;
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> portersOfMembers;
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isUp;
        PropertyKeyID nameOfFirstCall;
        bool someMemberFindsTheUnknownAlone { false };
    };
    HashMap<uint32_t, std::unique_ptr<SleepersOfFamily>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> sleepersOfFamilies;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> observedByMembers;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> listedBySharedCalls;
    Vector<uint8_t> callsOfUnseen;
    Vector<uint8_t> callsThatEnterMembersOfTable;
    Vector<uint8_t> callsWithDroppedReturns;
    Vector<uint8_t> callsOnUnseenReceivers;

    BitVector callsThatBuilderOfFactsWaitsFor;
    BitVector sharedCallsOfUnseen;
    static constexpr uint8_t hasGivenUpInSight = 2;
    bool hasCalledUnseen(uint32_t number) const { return number < callsOfUnseen.size() ? callsOfUnseen[number] : sharedCallsOfUnseen.get(number - callsOfUnseen.size()); }
    uint8_t raiseCallOfUnseen(uint32_t number, uint8_t level)
    {
        if (number < callsOfUnseen.size()) {
            uint8_t before = callsOfUnseen[number];
            callsOfUnseen[number] = std::max(before, level);
            return before;
        }
        return sharedCallsOfUnseen.set(number - callsOfUnseen.size()) ? 2 : 0;
    }
    uint8_t raiseCallOnUnseenReceiver(uint32_t number, uint8_t level)
    {
        return std::exchange(callsOnUnseenReceivers[number], std::max(callsOnUnseenReceivers[number], level));
    }
    static constexpr uint8_t isCalledOutOfSightOnPrimitives = 1 << 2;
    static constexpr uint8_t isDoneButForWhatIsInSight = 2 | isCalledOutOfSightOnPrimitives;
    Vector<uint8_t> callsToResolve;
    BitVector membersToResolve;
    Vector<Vector<FlowObserver>> observersToResolve;
    Vector<FlowObserver> membersObservedToResolve;
    Vector<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> membersOfTable;
    struct WhatTableSaysOfParameter {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WhatTableSaysOfParameter);
        QuestionsInLinkCalls questions;
        bool recordsSpellOutEveryUse { false };
    };
    Vector<std::unique_ptr<WhatTableSaysOfParameter>> whatTableSaysOfParameters;
    Vector<const WhatTableSaysOfParameter*> whatTableSaysAtPlacesOfRows;
    size_t placesOfRow { 1 };
    BytesSharedByThreads builtinsWhoseRowsAreTakenAtTheirWord;
    BytesSharedByThreads placesThatEveryRowOnlyLooksAt;
    BitVector builtinsWithCodeWaitingForCall;
    BitVector builtinsWithCodeWaitingForOperand;
    BitVector rowsWhoseCallsAreListed;
    Vector<NativeCellInLinkCalls> nativeCells;

private:
    void listResults();

    const LinkFlow& m_flow;
    Vector<uint32_t> m_firstResultOfCall;
    Vector<uint32_t> m_endOfResultsOfCall;
    Vector<ResultInLinkCalls> m_results;
};

static BitVector builtinsWithCodeWaitingInLinkCalls(const BuiltinTable& table, const Invocable<bool(const BuiltinCondition&)> auto& waits)
{
    BitVector waiting(table.builtinCount());
    auto rowWaits = [&](const auto& row) {
        return row.condition && waits(*row.condition);
    };
    for (uint32_t index = 0; index < table.builtinCount(); ++index) {
        if (std::ranges::any_of(table.calledFunctionsOf(BuiltinID(index)), rowWaits) || std::ranges::any_of(table.moduleLoadsOf(BuiltinID(index)), rowWaits) || std::ranges::any_of(table.featureUsesOf(BuiltinID(index)), rowWaits))
            waiting.quickSet(table.canonicalID(BuiltinID(index)).index());
    }
    return waiting;
}

static Vector<NativeCellInLinkCalls> nativeCellsOfTableForLinkCalls(const BuiltinTable& table)
{
    Vector<NativeCellInLinkCalls> cells;
    auto cellAt = [&](const BuiltinPlace& place) -> NativeCellInLinkCalls* {
        if (place.kind != BuiltinPlaceKind::NativeCell)
            return nullptr;
        size_t position = cells.findIf([&](const NativeCellInLinkCalls& cell) {
            return cell.name == place.nativeCell;
        });
        if (position == notFound) {
            position = cells.size();
            cells.append({ place.nativeCell });
        }
        return &cells[position];
    };
    auto markValueRead = [&](const BuiltinPlace& place) {
        if (NativeCellInLinkCalls* cell = cellAt(place))
            cell->isValueRead = true;
    };
    for (uint32_t index = 0; index < table.builtinCount(); ++index) {
        for (const BuiltinOverload& row : table.overloadsOf(BuiltinID(index))) {
            for (const BuiltinTransfer& transfer : row.flow.transfers) {
                markValueRead(transfer.from);
                cellAt(transfer.to);
            }
            for (const BuiltinCallback& callback : row.flow.callbacks) {
                markValueRead(callback.newTarget);
                markValueRead(callback.thisValue);
                markValueRead(callback.spreadArguments);
                std::ranges::for_each(callback.arguments, markValueRead);
            }
            for (const BuiltinPropertyAction& action : row.flow.propertyActions)
                markValueRead(action.value);
            for (const BuiltinHandlerCall& handlerCall : row.flow.handlerCalls) {
                if (NativeCellInLinkCalls* cell = cellAt(handlerCall.callee))
                    cell->isCalled = true;
                markValueRead(handlerCall.thisValue);
                std::ranges::for_each(handlerCall.arguments, markValueRead);
            }
        }
    }
    auto collectRetainedExports = [&](const BuiltinModuleLoad& load) {
        for (size_t place = 0; place < load.usesOfExports.size(); ++place) {
            for (const BuiltinHostUse& use : load.usesOfExports[place].list) {
                size_t position = use.use == BuiltinHostReadUse::Kept ? cells.findIf([&](const NativeCellInLinkCalls& cell) { return cell.name == use.nativeCell; }) : notFound;
                bool isTrackedThroughNativeCell = std::ranges::contains(table.at(load.module).memberOnlyExports, load.exports[place]);
                RELEASE_ASSERT(!isTrackedThroughNativeCell || (position != notFound && cells[position].isCalled && !cells[position].isValueRead));
                if (position != notFound)
                    cells[position].retainedExports.append({ load.module, load.exports[place], use.memberPath });
            }
        }
    };
    for (uint32_t index = 0; index < table.builtinCount(); ++index)
        std::ranges::for_each(table.moduleLoadsOf(BuiltinID(index)), collectRetainedExports);
    std::ranges::for_each(table.startupLoads(), collectRetainedExports);
    return cells;
}

StateOfLinkCalls::StateOfLinkCalls(const LinkFlow& flow)
    : m_flow(flow)
{
    size_t numberOfCalls = flow.calleesOfCalls.size();
    tellsThatBringNothing.fill(0, numberOfCalls);
    callsOfUnseen.fill(0, numberOfCalls);
    callsThatEnterMembersOfTable.fill(0, numberOfCalls);
    callsWithDroppedReturns.fill(0, numberOfCalls);
    callsOnUnseenReceivers.fill(0, numberOfCalls);
    callsThatBuilderOfFactsWaitsFor.ensureSize(numberOfCalls);

    callsToResolve.fill(0, numberOfCalls);
    firstObserved.grow(numberOfCalls);
    observed.grow(flow.numberOfLanes());
    observedAsOperandsOnlyLookedAt.grow(flow.numberOfLanes());
    observersToResolve.grow(flow.numberOfLanes());
    membersOfTable.grow(flow.numberOfLanes());
    for (unsigned row = 0; row < flow.program.builtins().overloadCount(); ++row)
        placesOfRow = std::max(placesOfRow, 1 + flow.program.builtins().at(BuiltinOverloadID(row)).parameters.size());
    whatTableSaysAtPlacesOfRows.fill(nullptr, flow.program.builtins().overloadCount() * placesOfRow);
    builtinsWhoseRowsAreTakenAtTheirWord = BytesSharedByThreads(flow.program.builtins().builtinCount());
    placesThatEveryRowOnlyLooksAt = BytesSharedByThreads(flow.program.builtins().builtinCount());
    builtinsWithCodeWaitingForCall = builtinsWithCodeWaitingInLinkCalls(flow.program.builtins(), [](const BuiltinCondition& condition) {
        return condition.callRunsOtherCode;
    });
    builtinsWithCodeWaitingForOperand = builtinsWithCodeWaitingInLinkCalls(flow.program.builtins(), [](const BuiltinCondition& condition) {
        return !!condition.nameToldUnder();
    });
    const BuiltinTable& builtins = flow.program.builtins();
    rowsWhoseCallsAreListed = RowsThatTakeLocales(builtins).rows;
    nativeCells = nativeCellsOfTableForLinkCalls(builtins);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.canonicalID(BuiltinID(index)).index() == index && callOrReadOfBuiltinIsEvent(builtins, BuiltinID(index)))
            rowsWhoseCallsAreListed.quickSet(index);
    }
    for (const BuiltinLoaderOfMachineCode& loader : builtins.loadersOfMachineCode()) {
        for (BuiltinID builtin : { loader.callee, loader.named }) {
            if (builtin)
                rowsWhoseCallsAreListed.quickSet(builtins.canonicalID(builtin).index());
        }
    }
    m_firstResultOfCall.fill(0, numberOfCalls);
    m_endOfResultsOfCall.fill(0, numberOfCalls);
    listResults();
}

std::span<const ResultInLinkCalls> StateOfLinkCalls::resultsOf(FunctionID caller, uint32_t call) const
{
    uint32_t number = m_flow.firstCallOfFunction[caller.index()] + call;
    return m_results.span().subspan(m_firstResultOfCall[number], m_endOfResultsOfCall[number] - m_firstResultOfCall[number]);
}

void StateOfLinkCalls::listResults()
{
    const ParallelLoops& loops = m_flow.loops;
    size_t numberOfCallers = m_flow.summaries.size();
    uint32_t callsOfConversion = static_cast<uint32_t>(m_flow.program.builtins().keysReadBy(Protocol::ToPrimitive).size());
    auto numberOfCallsAnsweringAt = [&](const SummaryNode& node) -> uint32_t {
        if (node.kind == SummaryNode::Kind::ResultOfCall)
            return 1;
        return node.kind == SummaryNode::Kind::ResultOfConversion ? callsOfConversion : 0;
    };
    forEachRangeOfIndicesInParallel(loops, numberOfCallers, [&](size_t first, size_t end) {
        for (size_t caller = first; caller < end; ++caller) {
            const ThinSummary& summary = m_flow.summaries.at(FunctionID(caller));
            uint32_t firstCall = m_flow.firstCallOfFunction[caller];
            for (const SummaryNode& node : summary.nodes) {
                uint32_t numberOfCalls = numberOfCallsAnsweringAt(node);
                RELEASE_ASSERT(!numberOfCalls || node.first + numberOfCalls <= summary.calls.size());
                for (uint32_t call = node.first; call < node.first + numberOfCalls; ++call)
                    ++m_endOfResultsOfCall[firstCall + call];
            }
        }
    });

    uint32_t position = 0;
    for (size_t call = 0; call < m_endOfResultsOfCall.size(); ++call) {
        uint32_t numberOfResults = m_endOfResultsOfCall[call];
        m_firstResultOfCall[call] = position;
        m_endOfResultsOfCall[call] = position;
        position += numberOfResults;
    }
    m_results.grow(position);

    forEachRangeOfIndicesInParallel(loops, numberOfCallers, [&](size_t first, size_t end) {
        for (size_t caller = first; caller < end; ++caller) {
            const ThinSummary& summary = m_flow.summaries.at(FunctionID(caller));
            uint32_t firstCall = m_flow.firstCallOfFunction[caller];
            for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
                const SummaryNode& node = summary.nodes[index];
                uint32_t numberOfCalls = numberOfCallsAnsweringAt(node);
                for (uint32_t call = node.first; call < node.first + numberOfCalls; ++call)
                    m_results[m_endOfResultsOfCall[firstCall + call]++] = ResultInLinkCalls { m_flow.nodeOf(FunctionID(caller), index), node.second };
            }
        }
    });
}

void DeleterOfStateOfLinkCalls::operator()(StateOfLinkCalls* state) const
{
    delete state;
}

class CallInLinkCalls {
    WTF_MAKE_NONCOPYABLE(CallInLinkCalls);
public:
    CallInLinkCalls(LinkFlow&, StateOfLinkCalls&, FunctionID caller, uint32_t index);
    CallInLinkCalls(LinkFlow&, StateOfLinkCalls&, SharedCallInLinkCalls&);

    void resolve();
    void resolveMemberOfSharedCall(uint32_t member);
    void wakeMembersOnWhichFieldIsBornUnderName();
    void wakeEverySleeper();
    void wake(LayoutID);
    void layPortersAtMembersOf(FamilyID, Vector<LayoutID>& membersThatMayAnswer);
    void callUnseenForMembersThatFindTheUnknownAlone() { callUnseen(); }
    void takeStepOfInspection(uint32_t step, const LinkType& now);
    void showPrivateFieldsOf(LayoutID, uint32_t inspection);
    static QuestionsInLinkCalls questionsOfComparisonOfPrivateFields();
    void inspectWhatExpectedBuiltinCompares();
    void enter(FunctionID callee);
    void noteCustomInstanceOf();

private:
    using Source = SourceInLinkCalls;
    using Values = ValuesInLinkCalls;
    using Passed = PassedInLinkCalls;
    using Fit = FitInLinkCalls;
    using Questions = QuestionsInLinkCalls;
    using RowCall = RowCallInLinkCalls;
    using SharedCall = SharedCallInLinkCalls;

    enum class Entry : uint8_t { Refused, Unseen, Body };
    enum class Lookup : uint8_t { Unknown, Absent, Found, FoundWhereProgramMayWrite };
    using HoldersPassed = Vector<LinkFlow::StillNothing, 4>;
    bool tableListsMemberAbove(BuiltinID firstHolder);
    bool isNameUndefinedFrom(BuiltinID firstHolder);
    bool isNameUndefinedOn(BuiltinID);
    bool isNameUndefinedAbove(BuiltinClassID);
    std::optional<HoldersPassed> passHoldersUpToMemberFrom(BuiltinID firstHolder);
    enum class WhenAbsent : uint8_t { CallThrows, MayHaveBeenAdded };

    struct MemberOfTable {
        Lookup outcome { Lookup::Unknown };
        BuiltinID member;
    };

    FlowNodeID nodeAt(uint32_t inSummary) const { return m_shared ? m_shared->nodes[inSummary] : m_flow.nodeOf(m_caller, inSummary); }
    FlowNodeID thrownNode() const { return m_shared ? m_shared->thrown : nodeTakingWhatIsThrown(m_flow, m_caller, m_call.thrownTo); }
    ALWAYS_INLINE LinkType heldAt(FlowNodeID node) const { return node == m_flow.unseen() ? LinkType::anything() : m_flow.typeAt(node); }
    LinkFlow::CalleesOfCall& callees() { return m_shared ? m_flow.calleesOfSharedCalls[m_shared->index] : m_flow.calleesOfCalls[m_number]; }
    bool lists(FunctionID callee) { return m_shared ? m_state.listedBySharedCalls.contains(static_cast<uint64_t>(m_shared->index) << 32 | callee.index()) : callees().lists(callee); }
    void list(FunctionID);
    bool isSettledButForOwnCallees() const { return m_state.hasCalledUnseen(m_number); }
    bool hasGivenUpInSight() const { return m_shared ? m_state.hasCalledUnseen(m_number) : m_state.callsOfUnseen[m_number] == static_cast<uint8_t>(UnknownIs::InSight); }
    bool isDoneButForWhatIsInSight() const { return !m_shared && m_state.callsOnUnseenReceivers[m_number] == StateOfLinkCalls::isDoneButForWhatIsInSight; }
    const Vector<ResultInLinkCalls, 1>& results();
    const Passed& passedByCall();
    Passed passedWithReceiver(const LinkType&);
    const MembersUnderKey& membersUnderKey();
    static uint8_t beyond(const Source* source) { return source ? std::min<uint8_t>(source->distance + 1, notObservedForLinkCalls) : 1; }

    void add(Values&, const LinkType&, bool cameByEscapeAlone = false);
    void addBesideWhatCameByEscapeAlone(Values&, const LinkType&, bool cameByEscapeAlone);
    void add(Values&, FlowNodeID, uint8_t distance, TypeID passes = { });
    void add(Values&, const Values&);
    Values valuesOf(const LinkType&);
    void addValuesAtNode(Values&, uint32_t inSummary, TypeID passes = { });
    Values valuesAtNode(uint32_t inSummary, TypeID passes = { });
    Values valuesAtSource(const Source&);
    void flowInto(const Values&, FlowNodeID);
    void escape(const Values&);
    void escape(const LinkType&, const Source*);
    void escape(const Passed&);
    bool watch(FlowNodeID, uint8_t distance);
    bool watchOperandOnlyLookedAt(FlowNodeID);
    bool returnsOperandWithoutFraction(const RowCall&);

    uint8_t placesThatEveryRowOnlyLooksAt(const RowCall&);
    Vector<FlowNodeID, 4> operandsPassedAt(const RowCall&, uint8_t places);
    LinkType typeAt(const Source&);
    struct TypeReadAtSource {
        Source source;
        LinkType type;
    };
    static constexpr size_t mostTypesRemembered = 8;
    Vector<TypeReadAtSource, mostTypesRemembered> m_typesReadAtSources;
    void give(const Values&);
    void throwError();
    void throwAnything();

    template<typename Functor>
    void forEachType(const Values& values, const Functor& functor)
    {
        for (size_t index = 0; index < values.sources.size(); ++index) {
            Source source = values.sources[index];
            LinkType type = typeAt(source);
            if (!type.isNever())
                functor(type, &source);
        }
        if (!values.made.isNever()) {
            LinkType made = values.made;
            m_flow.noteUseOf(made);
            if (made.functions == LinkType::Functions::Set)
                m_flow.loseFunctionsOfSet(made);
            functor(made, static_cast<const Source*>(nullptr));
        }
    }

    template<typename Functor>
    void forEachClassAtOrAbove(BuiltinClassID builtinClass, const Functor& functor)
    {
        for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent)
            functor(current);
    }

    template<typename Functor>
    void forEachLayoutOf(FamilyID family, const Functor& functor)
    {
        Vector<LayoutID, 8> members;
        m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID member) {
            members.append(member);
        });
        for (LayoutID member : members)
            functor(member);
    }

    template<typename Functor>
    void forEachConstructorOf(FamilyID family, const Functor& functor)
    {
        forEachLayoutOf(family, [&](LayoutID member) {
            FunctionID constructor = m_program.at(member).constructor;
            if (constructor && m_flow.classOfConstructor(constructor) == member)
                functor(constructor);
        });
    }

    void callSubject();
    LinkType enterOtherFunctions(const LinkType& callee);
    void callName();
    void callBuiltinOfCall();
    enum class UnknownIs : uint8_t { ByEscapeAlone = 1, InSight = 2 };
    static_assert(static_cast<uint8_t>(UnknownIs::InSight) == StateOfLinkCalls::hasGivenUpInSight);
    static UnknownIs howUnknownIs(const LinkType& type) { return type.hasDirectUnknownObjects() ? UnknownIs::InSight : UnknownIs::ByEscapeAlone; }
    enum class ReceiverOfUnseenCode : uint8_t { AnyThatArrives, UnseenObjectItself };
    void callUnseen(UnknownIs = UnknownIs::InSight, ReceiverOfUnseenCode = ReceiverOfUnseenCode::AnyThatArrives);
    void addObjectsOfOwnClasses();
    bool receiverIsOfConstructorFunctions(const Passed&);
    void callValue(LinkType callee, const Source*, bool mayGoByRows, const Passed&);
    void callValueAt(FlowNodeID, const Passed&);
    bool callWhatGetterAtReturns(FlowNodeID member, const Passed&);
    Values callThroughSharedCall(SharedCall::Subjects, FamilyID, const Passed&);
    Values tieToSharedCall(SharedCall::Subjects, FamilyID, const Passed&, std::optional<uint32_t>& indexOfSharedCall);
    void callMemberOfInstance(LayoutID);
    void callMethodsDeclaredOnInstance(LayoutID);
    void callMemberFoundAbove(FlowNodeID above, const Passed&, unsigned stepsLeft);
    void callMemberOfPrototype(LayoutID);
    bool inheritsFromObjectAlone(LayoutID) const;
    bool isNameOfConversion() const;
    void callExportOf(const Layout&);
    void callMemberOfClosure(FunctionID);
    BuiltinID builtinConstructorAbove(LayoutID);
    void callMemberOfLostClosures(const LinkType&);
    void callMemberOfClass(const LinkType& instances);
    bool laysWhatItIsHandedIntoReceiver(BuiltinID) const;
    void callMemberOfTable(MemberOfTable, Values receiver, WhenAbsent, ReceiverOfUnseenCode = ReceiverOfUnseenCode::AnyThatArrives);
    MemberOfTable findAbove(BuiltinID firstHolder);
    MemberOfTable findOnInstancesOf(BuiltinClassID builtinClass) { return findOnInstancesOf(builtinClass, m_call.key); }
    MemberOfTable findOnInstancesOf(BuiltinClassID, PropertyKeyID);
    MemberOfTable findOnObjectsOfClassItself(BuiltinClassID);
    bool listsAloneHaveMemberCalledThatMakesList(bool& holdsWhatCanBeRead);
    MemberOfTable findOnObjectsOfClassItself(BuiltinClassID, PropertyKeyID);
    bool isOfClassItself(const LinkType& instances, uint8_t distance);
    bool areListsThatOnlyProgramNames(const LinkType& instances);
    MemberOfTable findAbove(BuiltinID firstHolder, PropertyKeyID, const MembersUnderKey&, std::span<const LinkFlow::StillNothing> forEachHolderPassed = { });
    MemberOfTable findOnInstancesOf(BuiltinClassID, PropertyKeyID, const MembersUnderKey&);
    MemberOfTable findOnce(uint64_t question, const Invocable<MemberOfTable()> auto& find);

    void enter(FunctionID callee, const Passed&);
    const LinkFlow::PrototypeHelper* findFollowedHelper(FunctionID callee, const Passed&);
    void enterByBinding(FunctionID callee);
    bool listsReturnsOf(FunctionID callee, const Passed&);
    void followWholeResults(FunctionID entered);
    const Values* operandPassedTo(FunctionID callee, uint32_t parameter, const Passed&) const;
    LinkType operandLeftOut() const;
    bool mayPassTests(FunctionID callee, uint32_t parameter, std::span<const SummaryParameterTest>, const Passed&);
    bool watchTestedOperand(const Source&);
    void flowReturnsOf(FunctionID callee, const Passed&);
    void passOn(const Values&, FunctionID callee, uint32_t parameter);
    bool handsOnOwnNewTarget(const Passed&);
    bool handsNewTargetUpRowOfClasses(FunctionID callee, const Passed&);
    Entry passTo(FunctionID callee, const Passed&, Values& constructed);
    void letHeldPrototypeEscape(FunctionID);
    Values objectsConstructed(const Passed&, FunctionID callee);
    LinkType objectsThatNoNodeHolds(LayoutID);
    Values resultOf(FunctionID callee, uint32_t index, const Passed&, const Values& constructed, uint8_t distance);
    void noteWhatIsMadeThrough(FunctionID callee, const Passed&);
    enum class NewTargets : uint8_t { OwnClasses, OwnOrEscapedClasses, OwnOrUnseenClasses, OtherClasses };
    NewTargets classifyNewTargets(LayoutID above, const Values& newTargets);
    bool addObjectsMadeForNewTarget(Values&, FunctionID callee, const Passed&, uint8_t distance);
    Values passToLostFunctions(const Passed&, const LinkType& callee, const Source*);
    std::optional<uint32_t> argumentWithAllObjectsTaken(const RowCall&, const BuiltinOverload&);
    Values primitivesAmong(const Values&);
    Values passToUnseen(const Passed&);
    void noteArgumentsEscape();
    void noteArgumentsEscape(const Passed&);
    Values callValues(const Values& callees, const Passed&, bool mayGoByRows);

    Values callBuiltin(BuiltinID, const Passed&, bool isCalledByBuiltin, bool mayGoByRows = true);
    void notePreparationRegisteredBy(BuiltinID, const Passed&);
    Values callOutOfSight(const RowCall&);
    void noteOptionThatRunsCodeAmong(const RowCall&);
    void noteConditionsThatOperandsMayMeet(const RowCall&);
    bool askOperandsOfCallOutOfSightForConditions(const RowCall&);
    void noteKeysOfPrinterThatRegistryMayGive(const RowCall&);
    void noteTrapsOfHandler(const RowCall&);
    bool mayMeet(const BuiltinCondition&, const RowCall&);
    bool mayBePassedOptionThatRunsCode(const RowCall&);
    bool mayHaveProperty(const LinkType&, const Source*, PropertyKeyID);
    ValueID valueWrittenAs(const RowCall&, BuiltinOperand) const;
    std::optional<StringID> textWrittenAs(const RowCall&, BuiltinOperand) const;
    std::optional<bool> literalMadeForCallHas(const RowCall&, BuiltinOperand, BuiltinName, PropertyKeyID) const;
    std::optional<StringID> textThatLiteralWritesUnder(const RowCall&, BuiltinOperand, PropertyKeyID) const;
    size_t literalHandedOn(const RowCall&, ValueID handedOn, ValueID user) const;
    bool gatherWhatLiteralWrites(ValueID handedOn, ValueID user, Vector<MemberWrittenInLinkCalls, 8>&) const;
    bool isAbsentAboveLiterals(BuiltinName, PropertyKeyID) const;
    void enterCodeBehindRow(RowCall&);
    bool listenerMayReceivePromises(const RowCall&);
    bool rowsSpeakOf(const RowCall&);
    bool rowsMayBeTakenAtTheirWord(BuiltinID);
    void describeWhatRowRefuses(const RowCall&, const BuiltinOverload&);
    bool someRowInFrontTakesAllThatRowTakes(const RowCall&, const BuiltinOverload&);
    void applyOperandsBesideRefused(RowCall&, const BuiltinOverload&);
    Fit fitOf(const RowCall&, const BuiltinOverload&);
    BuiltinClassID classBelowPrototype(BuiltinID canonicalPrototype);
    bool m_rowFitsByEscapeAlone { false };
    BuiltinID m_memberWhoseOperandsAreRead;
    bool m_unseenCodeProvidesInstances { false };
    Fit fitOf(const RowCall&, const BuiltinParameter&);
    TypeID typeOfIntegerConstantAt(const Source*) const;
    void addFit(const LinkType&, const Source*, const BuiltinType& accepted, Fit&);
    bool isAsClassMakesIt(const LinkType& instances, PristineAspectSet, const Source*);
    bool membersAboveInstancesAreWhatTableLists(BuiltinClassID);
    bool membersOfHolderAreWhatTableLists(BuiltinID canonicalHolder);
    bool indexesAreAbsentAbove(BuiltinClassID);
    void applyRow(RowCall&);
    void applyParameter(RowCall&, const BuiltinParameter&);
    void applyCallback(RowCall&, const BuiltinCallback&);
    void applyHandlerCall(RowCall&, const BuiltinHandlerCall&);
    bool handlerMayBeThere(RowCall&, const BuiltinPlace& callee);
    using PathIntoLiteral = PathIntoLiteralInLinkCalls;
    bool isTakenApart(const RowCall&, std::optional<BuiltinOperand>) const;
    uint32_t nodeOfWrittenParts(const RowCall&, ContentsSlot) const;
    bool mayTakeApartLiteralAt(RowCall&, const BuiltinParameter&, bool byFields = false);
    bool stepsAreFieldsOrAbsent(RowCall&, const BuiltinParameter&, std::span<const PathIntoLiteral>);
    bool findsMethodsAlone(LayoutID, PropertyKeyID) const;
    bool m_methodsOfClassesAnswerNothing { false };
    std::optional<LayoutID> layoutOfLiteralInSight(const RowCall&, ValueID& written, ValueID& writer) const;
    bool stepsAreWrittenOrAbsent(const RowCall&, ValueID written, ValueID writer, std::span<const PathIntoLiteral>) const;
    void handOverLiteralsTakenApart(RowCall&);
    void handOverMembersOfLiteral(RowCall&, ValueID written, ValueID writer, std::span<const PathIntoLiteral>, uint8_t distance);
    void handOverMember(RowCall&, FieldReference, const MemberWrittenInLinkCalls&, std::span<const PathIntoLiteral>, uint8_t distance);
    void handOverMember(RowCall&, const Values& heldAtMember, const MemberWrittenInLinkCalls&, std::span<const PathIntoLiteral>, uint8_t distance);
    void letGoOfMember(RowCall&, const MemberWrittenInLinkCalls&, const Values& held, std::span<const PathIntoLiteral>);
    bool answersArePrinted(const RowCall&, const BuiltinHandlerCall&) const;
    void callHandlers(RowCall&, const BuiltinHandlerCall&, const Values& handlers);
    const NativeCellInLinkCalls* findNativeCell(BuiltinName) const;
    FlowNodeID nodeOfFunctionsKeptIn(BuiltinName cell);
    void addExportsThatHostKeepsIn(BuiltinName cell, Values&);
    Values whatLiesUnder(BuiltinName, const Values& holders);
    void applyAction(RowCall&, const BuiltinPropertyAction&);
    bool noteWritesOf(RowCall&, const BuiltinPropertyAction&);
    bool appendSymbolsHeldAt(const Values&, Vector<PropertyKeyID>&);
    bool onlyWritesOnObject(RowCall&, const BuiltinPropertyAction&);
    void escapeAllButBuiltins(const Values&, bool assigns, bool knowsThatLinkIsLeftAlone = false);
    std::optional<PropertyKeyID> keyWrittenAs(const RowCall&, BuiltinOperand) const;
    void tellWhatIsFoundUnder(PropertyKeyID, const Values& objects, bool readsThroughPrototypes);
    void tellWhatReadFinds(const RowCall&, const BuiltinPropertyAction&);
    void tellWhatIterationCallsAbove(BuiltinClassID, bool followsWhatIsMade);
    void tellWhatIterationCalls(const LinkType& iterable);
    void addResult(RowCall&);

    ValuesAtOperandInLinkCalls valuesAt(const RowCall&, BuiltinOperand);
    Values valuesHandedOn(RowCall&, BuiltinOperand);
    Values valuesAtPlace(RowCall&, const BuiltinPlace&);
    Values valuesOfType(RowCall&, BuiltinTypeID);
    void addValuesOfType(RowCall&, BuiltinTypeID, bool isResult, Values&);
    void addValuesOfPart(RowCall&, const BuiltinTypePart&, BuiltinFreshness, bool isResult, Values&);
    void addInstanceOf(RowCall&, const BuiltinTypePart&, BuiltinClassID, std::span<const BuiltinSlotContents> listed, bool isResult, Values&, bool nobodyElseHoldsIt = false);
    uint32_t lengthWrittenForView(const RowCall&) const;
    uint32_t numberOfArgumentsWrittenFor(const RowCall&, BuiltinTypeID) const;
    void passOnPrimitivesThatFillMarkedWord(RowCall&, std::span<const BuiltinSlotContents>);
    void addRecordOf(RowCall&, const BuiltinTypePart&, bool isResult, Values&);
    SiteID siteOf(const BuiltinTypePart&, BuiltinClassID, bool isResultOfCall = false);
    void handHolesOn(RowCall&, SiteID);
    bool conversionsMayRunCode(RowCall&);
    void makeSiteOfClassOfReceiver(const BuiltinTypePart&, const LinkType& receiver);
    bool onlyWaitRightBehindSeesResult() const;
    SlotOfContents slotOfField(const BuiltinTypePart&, uint16_t fieldIndex);
    Values stepValuesOf(IterationKind, const Values& iterated);
    enum class NamesRead : uint8_t { All, ThoseThatMayBeIndexes };
    void addContentsOf(const LinkType& holder, const Source*, Values&, NamesRead = NamesRead::All);
    void addContentsOfSlot(RowCall&, const Values& holders, BuiltinSlotID, Values&);
    void addIteratedValues(const Values& iterables, Values&);
    Values valuesIteratedOf(const Values& iterables);
    void addResultsOfCallbacks(RowCall&, BuiltinOperand, bool isResultAsItIsAnswered, bool containsCallbackResults, Values&);
    void addPrototypes(const Values& objects, Values&);
    bool hasKnownPrototypeChain(LayoutID, ClassSide) const;
    LinkType prototypeOfObjectsOf(LayoutID) const;
    Values contentsOf(const Values& holders, NamesRead = NamesRead::All);
    void settle(Values&, bool rowHasJustMadeThem = false);
    void storeAt(RowCall&, const BuiltinPlace&, const Values&);
    bool isBoundAsReceiverThatNoRowSpeaksOf(const RowCall&, const BuiltinPlace&);
    bool areBuiltinFunctionsAlone(const Values&);
    bool callBuiltinFunctionBoundAt(const LinkType& callee, const Passed&);
    std::optional<LinkType> findFulfillingFunction(RowCall&);
    bool fulfillThrough(const LinkType& callee, const Passed&);
    void storeInContents(SiteID, SlotOfContents, const Values&, bool rowHasJustMadeThem = false);
    bool isValueOfPromise(BuiltinClassID holder, SlotOfContents) const;
    void storeInSlot(const Values& holders, BuiltinSlotID, const Values&);
    bool mayBeMissingAt(const RowCall&, const BuiltinPlace&);
    bool isViewOfKnownKind(const BuiltinSlot&, const LinkType& holder) const;
    bool isSlotOfViewsAlone(const BuiltinSlot&, const Values& holders);
    bool isFollowed(BuiltinClassID) const;
    bool mayBeListWithLayout(const LinkType&);

    template<typename Functor>
    bool forEachSiteOfResultField(RowCall& call, const BuiltinPlace& place, const Functor& functor)
    {
        if (!place.isOfElements)
            return forEachSiteOfResult(call, functor);
        bool everyElementIsFollowed = true;
        bool everyHolderIsFollowed = forEachSiteOfResult(call, [&](SiteID, const BuiltinTypePart& holder) {
            everyElementIsFollowed &= holder.contents.size() == 1;
            for (const BuiltinSlotContents& contents : holder.contents) {
                const BuiltinType& type = m_builtins.at(contents.type);
                everyElementIsFollowed &= type.freshness == BuiltinFreshness::Yes && !type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData });
                for (const BuiltinTypePart& part : type.parts) {
                    if (part.kind == BuiltinTypePartKind::Layout)
                        functor(siteOf(part, m_builtins.at(part.layout).isTuple ? BuiltinClasses::Array : BuiltinClasses::Object), part);
                    else
                        everyElementIsFollowed = false;
                }
            }
        });
        return everyHolderIsFollowed && everyElementIsFollowed;
    }

    template<typename Functor>
    bool forEachSiteOfResult(RowCall& call, const Functor& functor)
    {
        BuiltinTypeID id = call.row->result.type;
        if (call.resultIsUnknown || !id)
            return false;
        const BuiltinType& type = m_builtins.at(id);
        if (type.isAnything() || type.freshness == BuiltinFreshness::No)
            return false;

        bool everyObjectIsFollowed = type.freshness != BuiltinFreshness::UnlessSpecies && !type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData });
        for (const BuiltinTypePart& part : type.parts) {
            switch (part.kind) {
            case BuiltinTypePartKind::Instance:
                if (isFollowed(part.builtinClass))
                    functor(siteOf(part, part.builtinClass, !call.isCalledByBuiltin && type.freshness == BuiltinFreshness::Yes), part);
                else
                    everyObjectIsFollowed = false;
                break;
            case BuiltinTypePartKind::Layout:
                functor(siteOf(part, m_builtins.at(part.layout).isTuple ? BuiltinClasses::Array : BuiltinClasses::Object), part);
                break;
            case BuiltinTypePartKind::InstanceOfReceiverClass:
            case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
                forEachType(valuesAt(call, BuiltinOperand::receiver()), [&](const LinkType& receiver, const Source*) {
                    BuiltinClassID made = holdsOnlyInstancesOfOneClassForLinkCalls(receiver) ? receiver.builtinClass : BuiltinClassID();
                    if (made && part.kind == BuiltinTypePartKind::TypedArrayOfReceiverElementType)
                        made = m_builtins.typedArrayClassOf(made);
                    if (made)
                        functor(siteOf(part, made), part);
                    else
                        everyObjectIsFollowed = false;
                });
                break;
            case BuiltinTypePartKind::Builtin:
            case BuiltinTypePartKind::Operand:
            case BuiltinTypePartKind::Slot:
            case BuiltinTypePartKind::IteratedValues:
            case BuiltinTypePartKind::CallbackResult:
            case BuiltinTypePartKind::Prototype:
                everyObjectIsFollowed = false;
                break;
            }
        }
        return everyObjectIsFollowed;
    }

    Questions questionsOf(const BuiltinParameter&);
    const StateOfLinkCalls::WhatTableSaysOfParameter& whatTableSaysOf(const BuiltinOverload&, const BuiltinParameter&);
    void tellKeysBecomeTextsAmong(const Values&);
    void addKeys(Questions&, std::span<const BuiltinName>);
    void addKeysAnsweredByProgramAlone(Questions&, std::span<const BuiltinName>);
    void addConversion(Questions&);
    bool ask(const Values&, const Questions&);
    Vector<FlowNodeID, 2> m_listsConverted;
    unsigned m_depthOfConversions { 0 };
    enum class UsedByParts : uint8_t { No, NothingAnswers, SomethingAnswers };
    UsedByParts askByParts(const LinkType&, const Source*, const Questions&);
    bool callConversions(const LinkType&, const Source*, const Questions&);
    bool addConversionMethods(LayoutID, PropertyKeyID, uint8_t distance, Values&);
    bool isDataDescriptorInSight(const Source*) const;
    bool isMapOfAccessorsAsWritten(const Values&) const;
    bool mayAnswer(const LinkType&, const Source*, const Questions&, bool ownValuesAreFollowed);
    bool tableAloneAnswersFrom(BuiltinID, const Questions&);
    bool onlyNamesThatTableAnswersAreRead(BuiltinID, const Questions&);
    bool ownValuesAreListed(const LinkType&);
    bool mayFindUnderNumbersAbove(const LinkType&, const Source*);
    template<typename Functor> void forEachLayoutWithListedOwnValues(const LinkType&, const Functor&);
    void inspect(const Values&, const Questions&);
    void inspectAt(FlowNodeID, uint32_t inspection);
    void inspectType(const LinkType&, uint32_t inspection);
    void tellWhatFormatterIsHanded(const LinkType&, const Questions&);
    void tellWhichMembersNativeCodeCallsOffUnseenObjects(const LinkType&, const Questions&);
    void tellFormatterLosesSight(const Questions&);
    bool mayBePrintedWholeWhereItIsRefused(const LinkType&, bool isLiteralMadeForCall);
    bool isLiteralMadeForCall(const RowCall&, BuiltinOperand) const;
    void followWhatHandlersAnswerForFormatter(RowCall&, const BuiltinHandlerCall&);
    bool findsNoElements(const RowCall&, const BuiltinPropertyAction&);
    bool gatherClosuresWrittenAt(const RowCall&, ValueID written, ValueID writer, std::span<const BuiltinName> path, Vector<FunctionID, 4>&);
    bool makesInstanceWithNothingCalledUnder(ValueID callOrConstruction, BuiltinName step, PropertyKeyID);
    enum class SubjectOfInspection : uint8_t { Layout, Function, Site };
    bool askInspected(SubjectOfInspection, uint32_t number, const LinkType& subject, uint32_t inspection);
    void askAgain(uint32_t step);
    bool mayFind(const LinkType& objects, PropertyKeyID, uint8_t distance, bool membersOfTableAnswerNothing, bool primitivesAnswerNothing, bool ownPropertiesAnswerNothing = false);

    LinkFlow& m_flow;
    StateOfLinkCalls& m_state;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    FunctionID m_caller;
    uint32_t m_index { 0 };
    uint32_t m_number { 0 };
    bool m_makesWhatRowsMakeAtOneSiteOfProgram { false };
    SharedCall* m_shared { nullptr };
    uint32_t m_memberThatWatches { 0 };
    const ThinSummary& m_summary;
    const SummaryCall& m_call;

    Vector<ResultInLinkCalls, 1> m_results;
    bool m_hasLookedForResults { false };
    std::optional<uint32_t> m_stepThatWatches;
    bool m_isEnteredOnReach { false };
    Vector<FlowNodeID, 4> m_operandsOnlyLookedAt;
    std::optional<PassedMadeInLinkCalls> m_passed;
    std::optional<MembersUnderKey> m_membersUnderKey;
    bool m_throwsError { false };
    bool m_throwsAnything { false };
    Vector<BuiltinTypeID, 8> m_typesBeingRead;
};

CallInLinkCalls::CallInLinkCalls(LinkFlow& flow, StateOfLinkCalls& state, FunctionID caller, uint32_t index)
    : m_flow(flow)
    , m_state(state)
    , m_program(flow.program)
    , m_builtins(flow.program.builtins())
    , m_caller(caller)
    , m_index(index)
    , m_number(flow.firstCallOfFunction[caller.index()] + index)
    , m_summary(flow.summaries.at(caller))
    , m_call(m_summary.calls[index])
{
}

CallInLinkCalls::CallInLinkCalls(LinkFlow& flow, StateOfLinkCalls& state, SharedCall& shared)
    : m_flow(flow)
    , m_state(state)
    , m_program(flow.program)
    , m_builtins(flow.program.builtins())
    , m_caller(shared.firstCaller)
    , m_index(shared.firstCall)
    , m_number(static_cast<uint32_t>(flow.calleesOfCalls.size()) + shared.index)
    , m_shared(&shared)
    , m_summary(shared.summary)
    , m_call(shared.call)
{
}

const Vector<ResultInLinkCalls, 1>& CallInLinkCalls::results()
{
    if (std::exchange(m_hasLookedForResults, true))
        return m_results;
    if (m_shared)
        m_results.append(ResultInLinkCalls { m_shared->result, 0 });
    else
        m_results.append(m_state.resultsOf(m_caller, m_index));
    return m_results;
}

void CallInLinkCalls::list(FunctionID callee)
{
    if (m_shared ? m_state.listedBySharedCalls.add(static_cast<uint64_t>(m_shared->index) << 32 | callee.index()).isNewEntry : !callees().lists(callee))
        callees().list(callee);
}

const PassedInLinkCalls& CallInLinkCalls::passedByCall()
{
    if (m_passed)
        return *m_passed;

    PassedMadeInLinkCalls& passed = m_passed.emplace();
    if (m_shared) {
        if (m_call.receiver != noSummaryNode)
            passed.receiver = valuesAtNode(m_call.receiver);
        for (uint32_t argument = 0; argument + m_shared->hasList < m_call.numberOfArguments; ++argument)
            passed.appendArgument(valuesAtNode(m_summary.arguments[argument]));
        if (m_shared->hasList)
            passed.ensureList() = valuesAtNode(m_summary.arguments.last());
        passed.isConstruction = m_call.isConstruction;
        if (m_call.newTarget != noSummaryNode)
            passed.setNewTarget(valuesAtNode(m_call.newTarget));
        return passed;
    }
    if (m_call.receiver != noSummaryNode)
        addValuesAtNode(passed.receiver.emplace(), m_call.receiver, m_call.callee == SummaryCall::Callee::Name ? TypeID(TypeTable::object) : TypeID());
    if (!m_call.passesList)
        passed.reserveArguments(m_call.numberOfArguments);
    for (uint32_t argument = 0; argument < m_call.numberOfArguments; ++argument) {
        uint32_t node = m_summary.arguments[m_call.firstArgument + argument];
        if (m_call.passesList)
            add(passed.ensureList(), valuesAtNode(node));
        else
            addValuesAtNode(passed.appendArgument(), node);
    }
    if (m_call.passesList)
        passed.ensureList();
    passed.isConstruction = m_call.isConstruction;
    if (m_call.newTarget != noSummaryNode)
        passed.setNewTarget(valuesAtNode(m_call.newTarget));
    return passed;
}

PassedInLinkCalls CallInLinkCalls::passedWithReceiver(const LinkType& receiver)
{
    Passed passed = passedByCall().withoutReceiver();
    passed.receiver.emplace().made = receiver;
    return passed;
}

const MembersUnderKey& CallInLinkCalls::membersUnderKey()
{
    if (!m_membersUnderKey)
        m_membersUnderKey.emplace(m_program, m_call.key);
    return *m_membersUnderKey;
}

void CallInLinkCalls::add(Values& values, const LinkType& made, bool cameByEscapeAlone)
{
    if (made.isNever())
        return;
    if (!cameByEscapeAlone && !values.builtinCameByEscapeAlone) [[likely]] {
        values.made = values.made.isNever() ? made : m_flow.joinOf(values.made, made, LinkFlow::LossAtJoin::WaitsForUse);
        return;
    }
    addBesideWhatCameByEscapeAlone(values, made, cameByEscapeAlone);
}

NEVER_INLINE void CallInLinkCalls::addBesideWhatCameByEscapeAlone(Values& values, const LinkType& made, bool cameByEscapeAlone)
{
    if (values.made.isNever()) {
        values.made = made;
        values.builtinCameByEscapeAlone = cameByEscapeAlone;
        return;
    }
    LinkType arriving = made;
    bool builtinsMeet = values.made.builtin && arriving.builtin && values.made.builtin != arriving.builtin;
    auto loseWithoutWaiting = [&](LinkType& type) {
        type.holdsLostBuiltins |= m_flow.unseenObjectsBelow(type.builtin, m_caller).holdsLostBuiltins;
        type.builtin = { };
        type.hasUnknownObjects = true;
        type.isUnknownEscapeOnly = false;
    };
    if (cameByEscapeAlone && (builtinsMeet || values.made.hasUnknownObjects))
        loseWithoutWaiting(arriving);
    if (values.made.builtin && values.builtinCameByEscapeAlone && (builtinsMeet || arriving.hasUnknownObjects))
        loseWithoutWaiting(values.made);
    bool everyoneBroughtItByEscapeAlone = values.made.builtin ? values.builtinCameByEscapeAlone && (!arriving.builtin || cameByEscapeAlone) : cameByEscapeAlone;
    values.made = m_flow.joinOf(values.made, arriving, LinkFlow::LossAtJoin::WaitsForUse);
    values.builtinCameByEscapeAlone = values.made.builtin && everyoneBroughtItByEscapeAlone;
}

void CallInLinkCalls::add(Values& values, FlowNodeID node, uint8_t distance, TypeID passes)
{
    if (node == m_flow.unseen()) {
        add(values, m_flow.passedBy(LinkType::anything(), passes));
        return;
    }
    bool isListed = std::ranges::any_of(values.sources, [&](const Source& source) {
        return source.node == node && source.passes == passes;
    });
    if (!isListed)
        values.sources.append(Source { node, distance, passes });
}

void CallInLinkCalls::add(Values& values, const Values& added)
{
    for (const Source& source : added.sources)
        add(values, source.node, source.distance, source.passes);
    add(values, added.made, added.builtinCameByEscapeAlone);
}

ValuesInLinkCalls CallInLinkCalls::valuesOf(const LinkType& made)
{
    Values values;
    values.made = made;
    return values;
}

void CallInLinkCalls::addValuesAtNode(Values& values, uint32_t inSummary, TypeID passes)
{
    if (inSummary == noSummaryNode)
        add(values, primitivesForLinkCalls(Primitive::Undefined));
    else
        add(values, nodeAt(inSummary), 0, passes);
}

ValuesInLinkCalls CallInLinkCalls::valuesAtNode(uint32_t inSummary, TypeID passes)
{
    Values values;
    addValuesAtNode(values, inSummary, passes);
    return values;
}

ValuesInLinkCalls CallInLinkCalls::valuesAtSource(const Source& source)
{
    Values values;
    add(values, source.node, source.distance, source.passes);
    return values;
}

void CallInLinkCalls::flowInto(const Values& values, FlowNodeID to)
{
    for (const Source& source : values.sources)
        m_flow.addEdge(source.node, to, source.passes);

    LinkType made = values.made;
    if (to != m_flow.unseen()) {
        switch (made.objects) {
        case LinkType::Objects::None:
        case LinkType::Objects::Prototype:
        case LinkType::Objects::Prototypes:
        case LinkType::Objects::Lost:
            break;
        case LinkType::Objects::Layout:
            {
                LinkFlow::LockerOfWhatIsShared locker(m_flow);
                if (m_state.layoutsLaidDown.add(static_cast<uint64_t>(made.layout.index()) << 32 | to.index()).isNewEntry)
                    m_flow.arriveFromSource(to, made.layout);
            }
            made.objects = LinkType::Objects::None;
            made.layout = { };
            break;
        case LinkType::Objects::Set:
            {
                LinkFlow::LockerOfWhatIsShared locker(m_flow);
                for (LayoutID member : m_flow.layoutsOfSet(made)) {
                    if (m_state.layoutsLaidDown.add(static_cast<uint64_t>(member.index()) << 32 | to.index()).isNewEntry)
                        m_flow.arriveFromSource(to, member);
                }
            }
            made.objects = LinkType::Objects::None;
            made.layout = { };
            break;
        case LinkType::Objects::Family:
            m_flow.arriveFromSource(to, made.family);
            made.objects = LinkType::Objects::None;
            made.family = { };
            break;
        }
    }
    m_flow.arrive(to, made);
}

void CallInLinkCalls::escape(const Values& values)
{
    flowInto(values, m_flow.unseen());
}

void CallInLinkCalls::escape(const LinkType& type, const Source* source)
{
    if (source)
        m_flow.addEdge(source->node, m_flow.unseen(), source->passes);
    else
        m_flow.arrive(m_flow.unseen(), type);
}

void CallInLinkCalls::escape(const Passed& passed)
{
    if (passed.receiver)
        escape(*passed.receiver);
    for (const Values& argument : passed.arguments)
        escape(argument);
    if (passed.list)
        escape(*passed.list);
    if (passed.newTarget)
        escape(*passed.newTarget);
}

void LinkFlow::takeOverCallsWhoseRowsLinkTells()
{
    Vector<uint64_t> listed = builtinsInReach().takeCallsWhoseRowsLinkTells();
    if (listed.isEmpty())
        return;
    std::ranges::sort(listed);
    StateOfLinkCalls& state = stateOfLinkCalls();
    for (size_t first = 0, end = 0; first < listed.size(); first = end) {
        FunctionID caller(static_cast<uint32_t>(listed[first] >> 32));
        while (end < listed.size() && listed[end] >> 32 == listed[first] >> 32)
            ++end;
        std::span<const uint64_t> ofCaller = listed.span().subspan(first, end - first);
        const auto& calls = summaries.at(caller).calls;
        for (uint32_t place = 0; place < calls.size(); ++place) {
            if (!std::ranges::binary_search(ofCaller, static_cast<uint64_t>(caller.index()) << 32 | calls[place].site.index()))
                continue;
            uint32_t number = firstCallOfFunction[caller.index()] + place;
            if (state.callsOfUnseen[number] || state.callsOnUnseenReceivers[number])
                builtinsInReach().noteCallLeftToUnseen(caller, calls[place].site);
            else
                state.callsThatBuilderOfFactsWaitsFor.quickSet(number);
        }
    }
}

bool CallInLinkCalls::watch(FlowNodeID node, uint8_t distance)
{
    if (distance > farthestObservedForLinkCalls)
        return false;
    if (m_stepThatWatches) {
        if (node != m_flow.unseen() && m_state.observedBySteps.add(static_cast<uint64_t>(*m_stepThatWatches) << 32 | node.index()).isNewEntry)
            m_flow.observe(node, FlowObserver { FlowObserver::Kind::Inspected, m_caller, *m_stepThatWatches });
        return true;
    }
    if (m_shared) {
        if (node == m_flow.unseen() || !m_state.observedByMembers.add(static_cast<uint64_t>(m_memberThatWatches) << 32 | node.index()).isNewEntry)
            return true;
        if (!m_flow.typeAt(node).isNever())
            ++m_state.membersOfSharedCalls[m_memberThatWatches].tellsThatBringNothing;
        m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfSharedCall, m_caller, m_memberThatWatches });
        return true;
    }
    if (!m_operandsOnlyLookedAt.isEmpty()) [[unlikely]] {
        if (m_operandsOnlyLookedAt.contains(node))
            return watchOperandOnlyLookedAt(node);
    }
    if (node == m_flow.unseen() || !m_state.addObserved(m_flow.laneOfObserversOf(m_caller), m_number, node))
        return true;
    if (!m_flow.typeAt(node).isNever())
        ++m_state.tellsThatBringNothing[m_number];
    m_flow.observe(node, FlowObserver { FlowObserver::Kind::SubjectOfCall, m_caller, m_index });
    return true;
}

NEVER_INLINE bool CallInLinkCalls::watchOperandOnlyLookedAt(FlowNodeID node)
{
    if (node == m_flow.unseen() || !m_state.observedAsOperandsOnlyLookedAt[m_flow.laneOfObserversOf(m_caller)].add(static_cast<uint64_t>(m_number) << 32 | node.index()).isNewEntry)
        return true;
    if (!m_flow.typeAt(node).isNever())
        ++m_state.tellsThatBringNothing[m_number];
    m_flow.observe(node, FlowObserver { FlowObserver::Kind::OperandOnlyLookedAt, m_caller, m_index });
    return true;
}

static bool onlyLooksAtOperandForLinkCalls(const BuiltinParameter& parameter)
{
    return (parameter.uses - OperandUses { OperandUse::Inspects, OperandUse::PassesToUserCode }).isEmpty() && (parameter.protocols - Protocols { Protocol::OwnKeys }).isEmpty() && parameter.roles.isEmpty()
        && parameter.conversion == BuiltinConversion::None && parameter.namedMembers.empty() && parameter.calledMembers.empty() && parameter.convertedMembers.empty();
}

static constexpr uint8_t placesAreKnownForLinkCalls = 1 << 7;
static constexpr uint32_t argumentsAmongPlacesForLinkCalls = 6;

uint8_t CallInLinkCalls::placesThatEveryRowOnlyLooksAt(const RowCall& call)
{
    if (call.mode != BuiltinCallMode::Call)
        return 0;
    if (uint8_t known = m_state.placesThatEveryRowOnlyLooksAt.at(call.builtin.index()))
        return known & ~placesAreKnownForLinkCalls;
    auto everyRowOnlyLooksAt = [&](BuiltinOperand operand) {
        bool hasRow = false;
        for (const BuiltinOverload& row : m_builtins.overloadsOf(call.builtin)) {
            if (row.mode != BuiltinCallMode::Call)
                continue;
            hasRow = true;
            const BuiltinParameter* parameter = m_builtins.parameterFor(row, operand);
            if (!parameter || !onlyLooksAtOperandForLinkCalls(*parameter))
                return false;
        }
        return hasRow;
    };
    uint8_t places = everyRowOnlyLooksAt(BuiltinOperand::receiver());
    for (uint32_t argument = 0; argument < argumentsAmongPlacesForLinkCalls; ++argument)
        places |= everyRowOnlyLooksAt(BuiltinOperand::argument(argument)) << (argument + 1);
    m_state.placesThatEveryRowOnlyLooksAt.set(call.builtin.index(), places | placesAreKnownForLinkCalls);
    return places;
}

Vector<FlowNodeID, 4> CallInLinkCalls::operandsPassedAt(const RowCall& call, uint8_t places)
{
    Vector<FlowNodeID, 4> lookedAt;
    auto isAmongPlaces = [&](uint32_t argument) {
        return argument < argumentsAmongPlacesForLinkCalls && places & 2 << argument;
    };
    auto add = [&](const Values& values) {
        for (const Source& source : values.sources)
            lookedAt.append(source.node);
    };
    if (call.passed.receiver && places & 1)
        add(*call.passed.receiver);
    for (uint32_t argument = 0; argument < call.passed.arguments.size() && argument < argumentsAmongPlacesForLinkCalls; ++argument) {
        if (isAmongPlaces(argument))
            add(call.passed.arguments[argument]);
    }
    if (lookedAt.isEmpty())
        return lookedAt;
    auto remove = [&](const Values& values) {
        for (const Source& source : values.sources)
            lookedAt.removeAll(source.node);
    };
    if (call.passed.receiver && !(places & 1))
        remove(*call.passed.receiver);
    for (uint32_t argument = 0; argument < call.passed.arguments.size(); ++argument) {
        if (!isAmongPlaces(argument))
            remove(call.passed.arguments[argument]);
    }
    if (call.passed.list)
        remove(*call.passed.list);
    if (call.passed.newTarget)
        remove(*call.passed.newTarget);
    return lookedAt;
}

LinkType CallInLinkCalls::typeAt(const Source& source)
{
    bool remembers = !m_stepThatWatches && !m_shared && m_operandsOnlyLookedAt.isEmpty() && m_flow.isDeferringUpdates();
    if (remembers) {
        for (const TypeReadAtSource& read : m_typesReadAtSources) {
            if (read.source.node == source.node && read.source.distance == source.distance && read.source.passes == source.passes) {
                return read.type;
            }
        }
    }
    LinkType type = LinkType::anything();
    if (watch(source.node, source.distance))
        type = m_flow.passedBy(m_flow.typeAt(source.node), source.passes);
    else
        m_flow.addEdge(source.node, m_flow.unseen(), source.passes);
    if (remembers && m_typesReadAtSources.size() < mostTypesRemembered)
        m_typesReadAtSources.append({ source, type });
    return type;
}

bool CallInLinkCalls::isNameOfConversion() const
{
    return std::ranges::any_of(m_builtins.keysReadBy(Protocol::ToPrimitive), [&](BuiltinName name) {
        return m_program.keyOf(name) == m_call.key;
    });
}

void CallInLinkCalls::give(const Values& values)
{
    if (values.isEmpty())
        return;
    bool givesPrimitivesAlone = !m_shared && m_call.isMadeByProtocol && m_call.callee == SummaryCall::Callee::Name && isNameOfConversion();
    for (const ResultInLinkCalls& result : results()) {
        if (result.index)
            m_flow.arrive(result.node, LinkType::anything());
        else if (givesPrimitivesAlone) {
            for (const Source& source : values.sources) {
                if (source.passes != TypeTable::object)
                    m_flow.addEdge(source.node, result.node, source.passes ? source.passes : typeOfPrimitives(Primitive::All));
            }
            m_flow.arrive(result.node, primitivesForLinkCalls(values.made.primitives));
        } else
            flowInto(values, result.node);
    }
}

void CallInLinkCalls::throwError()
{
    if (!std::exchange(m_throwsError, true)) {
        FlowNodeID thrown = thrownNode();
        if (m_flow.arrivalOfUnseenObjectsInSightBringsNothingTo(thrown))
            return;
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrown, unseenObjectsForLinkCalls());
    }
}

void CallInLinkCalls::throwAnything()
{
    if (!std::exchange(m_throwsAnything, true)) {
        FlowNodeID thrown = thrownNode();
        if (m_flow.arrivalOfAnythingInSightBringsNothingTo(thrown))
            return;
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrown, LinkType::anything());
    }
}

void CallInLinkCalls::resolve()
{
    if (m_flow.testRuns().isTestRun() && !m_shared && !m_call.isMadeByProtocol && !m_call.passesList && m_call.numberOfArguments > 1 && !m_flow.isInLibraryCode() && m_summary.arguments[m_call.firstArgument] != noSummaryNode)
        m_flow.testRuns().noteCallHandedObjectAndName(m_caller, m_call.site, nodeAt(m_summary.arguments[m_call.firstArgument]));
    if (!m_shared && m_call.continuesConstruction) {
        LinkType receiver = heldAt(nodeAt(m_call.receiver));
        if (!receiver.isNever() && receiver != closuresForLinkCalls(parentConstructorOf(m_program, m_caller)))
            m_flow.noteConstructionLeavesParent(m_caller);
    }
    if (!m_shared && std::ranges::any_of(callees().listed(), [&](FunctionID callee) { return m_flow.listsReturns(callee); })) {
        bool listsReturns = false;
        for (FunctionID callee : Vector<FunctionID, 4>(callees().listed().span())) {
            if (!listsReturnsOf(callee, passedByCall()))
                continue;
            if (!std::exchange(listsReturns, true))
                m_state.callsWithDroppedReturns[m_number] = 0;
            flowReturnsOf(callee, passedByCall());
        }
    }
    switch (m_call.callee) {
    case SummaryCall::Callee::Binding:
        if (m_call.function)
            enter(m_call.function);
        else
            callUnseen();
        return;
    case SummaryCall::Callee::Name:
        callName();
        return;
    case SummaryCall::Callee::Value:
        callSubject();
        return;
    case SummaryCall::Callee::Builtin:
        callBuiltinOfCall();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CallInLinkCalls::noteArgumentsEscape()
{
    if (m_shared || !m_call.sharesArgumentsObject)
        return;
    m_flow.arrive(m_flow.argumentsGatheredAtAnyPlace(m_caller), LinkType::anything());
    m_flow.noteNumberOfArgumentsGathered(m_caller, 0);
}

void CallInLinkCalls::noteArgumentsEscape(const Passed& passed)
{
    if (!m_shared && m_call.sharesArgumentsObject && passed.arguments.data() == passedByCall().arguments.data()) [[unlikely]]
        noteArgumentsEscape();
}

void CallInLinkCalls::callUnseen(UnknownIs unknownIs, ReceiverOfUnseenCode receiverOfUnseenCode)
{
    noteArgumentsEscape();
    bool receiversInSightStay = receiverOfUnseenCode == ReceiverOfUnseenCode::UnseenObjectItself && !m_shared && (m_call.subject == noSummaryNode || m_call.subject == m_call.receiver) && !m_state.hasCalledUnseen(m_number);
    uint8_t level = m_shared ? 2 : static_cast<uint8_t>(unknownIs);
    uint8_t levelBefore = receiversInSightStay ? m_state.raiseCallOnUnseenReceiver(m_number, level) : m_state.raiseCallOfUnseen(m_number, level);
    if (!m_shared && m_call.isMadeByProtocol)
        m_state.callsThatEnterMembersOfTable[m_number] = 1;
    if (levelBefore >= level)
        return;
    noteCustomInstanceOf();
    if (!m_shared && m_call.continuesConstruction)
        m_flow.noteConstructionLeavesParent(m_caller);
    if (levelBefore) {
        for (const ResultInLinkCalls& result : results())
            m_flow.arrive(result.node, LinkType::anything());
        return;
    }
    if (!m_shared && m_state.callsThatBuilderOfFactsWaitsFor.quickGet(m_number))
        m_flow.note(LinkFlow::Note::CallLeftToUnseen, m_number);

    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(level == 1, LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
    callees().mayEnterUnseen = true;
    addObjectsOfOwnClasses();
    if (m_flow.testRuns().isTestRun() && m_call.numberOfArguments && m_summary.arguments[m_call.firstArgument] != noSummaryNode)
        m_flow.testRuns().noteCallOutOfSight(m_caller, m_call.site, nodeAt(m_summary.arguments[m_call.firstArgument]));
    if (m_call.callee == SummaryCall::Callee::Name && m_call.key && (m_shared || !m_flow.rowsSpeakOfKeyFor(m_caller, m_call.key)))
        m_flow.note(LinkFlow::Note::NameCalled, m_call.key.bits(), (m_flow.isInLibraryCode() ? 0 : LinkFlow::spelledByProgram) | (m_shared || m_call.numberOfArguments || m_call.passesList ? 0 : LinkFlow::passesNoOperand));
    if (m_call.callee == SummaryCall::Callee::Value && m_call.builtin)
        m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, m_builtins.canonicalID(m_call.builtin).index(), LinkFlow::withoutRow);
    if (BuiltinID bound = m_call.builtin ? m_call.builtin : m_call.expectsCalleeItself ? m_call.expectedBuiltin : BuiltinID(); bound && m_state.builtinsWithCodeWaitingForOperand.quickGet(m_builtins.canonicalID(bound).index()) && m_builtins.canonicalID(bound) != m_memberWhoseOperandsAreRead)
        m_flow.note(LinkFlow::Note::BuiltinCalledWithOperandsNobodyRead, m_builtins.canonicalID(bound).index());
    auto leave = [&](uint32_t node) {
        if (node != noSummaryNode)
            m_flow.addEdge(nodeAt(node), m_flow.unseen());
    };
    if (!receiversInSightStay) {
        leave(m_call.subject);
        leave(m_call.receiver);
    }
    leave(m_call.newTarget);
    bool shouldAssumeArgumentsNeverEscape = AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::AssumeArgumentsNeverEscape) && !m_flow.isInLibraryCode();
    bool laysFirstArgumentAbovePrototype = m_call.callee == SummaryCall::Callee::Value && m_call.builtin && m_builtins.has(BuiltinMark::LaysOperandAbovePrototype, m_builtins.canonicalID(m_call.builtin));
    for (uint32_t argument = 0; !shouldAssumeArgumentsNeverEscape && argument < m_call.numberOfArguments; ++argument) {
        uint32_t passed = m_summary.arguments[m_call.firstArgument + argument];
        if (!argument && laysFirstArgumentAbovePrototype && passed != noSummaryNode)
            m_flow.layAbovePrototype(nodeAt(passed));
        else
            leave(passed);
    }
    inspectWhatExpectedBuiltinCompares();
    bool shouldAssumeUndefinedResult = AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::AssumeUnknownCallsReturnUndefined);
    for (const ResultInLinkCalls& result : results())
        m_flow.arrive(result.node, shouldAssumeUndefinedResult ? primitivesForLinkCalls(Primitive::Undefined) : LinkType::anything());
    throwAnything();
}

void CallInLinkCalls::addObjectsOfOwnClasses()
{
    if (!m_call.isConstruction || m_shared || !handsOnOwnNewTarget(passedByCall()))
        return;
    FlowNodeID made = m_flow.objectsMadeBySuperCallsOf(m_program.at(m_caller).constructedClass);
    m_flow.addEdge(made, m_flow.unseen());
    for (const ResultInLinkCalls& result : results()) {
        if (!result.index)
            m_flow.addEdge(made, result.node);
    }
}

void CallInLinkCalls::notePreparationRegisteredBy(BuiltinID builtin, const Passed& passed)
{
    if (!m_builtins.has(BuiltinMark::RegistersPreparationOfHolders, m_builtins.canonicalID(builtin)))
        return;
    bool isKnown = !passed.arguments.empty() && passed.arguments[0].made.isNever() && !passed.arguments[0].sources.isEmpty();
    for (size_t source = 0; isKnown && source < passed.arguments[0].sources.size(); ++source)
        isKnown = !passed.arguments[0].sources[source].distance;
    isKnown = isKnown && passed.arguments.size() > 1 && passed.arguments[1].made.isNever() && !passed.arguments[1].sources.isEmpty();
    for (size_t source = 0; isKnown && source < passed.arguments[1].sources.size(); ++source)
        isKnown = !passed.arguments[1].sources[source].distance;
    if (!isKnown) {
        m_flow.noteUnknownSymbolHandedToPreparation();
        return;
    }
    for (const Source& symbol : passed.arguments[0].sources) {
        for (const Source& function : passed.arguments[1].sources)
            m_flow.noteSymbolHandedToPreparation(m_caller, symbol.node, function.node);
    }
}

void CallInLinkCalls::callSubject()
{
    if (m_call.builtin)
        notePreparationRegisteredBy(m_call.builtin, passedByCall());
    if (m_call.subject == noSummaryNode) {
        callUnseen();
        return;
    }
    Source subject { nodeAt(m_call.subject), 0, { } };
    LinkType callee = heldAt(subject.node);
    callee.primitives = 0;
    if (callee.functions == LinkType::Functions::Closure && callee == closuresForLinkCalls(callee.function) && m_call.newTarget == noSummaryNode
        && lists(callee.function) && callees().builtins().isEmpty() && !callees().mayEnterUnseen) {
        throwError();
        return;
    }
    LinkType called = heldAt(subject.node);
    if (m_call.function) [[unlikely]]
        called = enterOtherFunctions(called);
    callValue(called, &subject, true, passedByCall());
}

LinkType CallInLinkCalls::enterOtherFunctions(const LinkType& callee)
{
    LinkType others = callee;
    if (callee.functions == LinkType::Functions::Set) {
        throwError();
        for (FunctionID member : m_flow.functionsOfSet(callee)) {
            if (member != m_call.function)
                enter(member, passedByCall());
        }
    } else if (callee.functions != LinkType::Functions::Closure || callee.function != m_call.function)
        return callee;
    others.functions = LinkType::Functions::None;
    others.function = { };
    return others;
}

void CallInLinkCalls::callBuiltinOfCall()
{
    if (!m_call.builtin) {
        callUnseen();
        return;
    }
    if (m_call.isMadeByHost) {
        bool hostMayFindWhatItLooksFor = false;
        for (uint32_t read : m_summary.namesThatHostLooksFor) {
            FlowNodeID node = nodeAt(read);
            if (!watch(node, 0)) {
                hostMayFindWhatItLooksFor = true;
                continue;
            }
            LinkType found = heldAt(node);
            LinkType nothing;
            nothing.primitives = found.primitives & (Primitive::UndefinedOrNull | Primitive::Absent);
            hostMayFindWhatItLooksFor |= !found.holdsSameAs(nothing);
        }
        if (!hostMayFindWhatItLooksFor)
            return;
    }
    if (m_call.isMadeByHost && (!m_flow.takeForConstant(m_call.builtin) || !watch(m_flow.writtenAtMember(m_call.builtin), 0))) {
        m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, m_builtins.canonicalID(m_call.builtin).index(), LinkFlow::withoutRow);
        callUnseen();
        return;
    }
    if (keyOfSymbolMadeOnceBy(m_flow.program, m_call)) {
        for (BuiltinName key : m_builtins.namesOfHostMethodsCallingStartupLoads()) {
            if (m_program.keyOf(key) == m_call.key)
                m_flow.note(LinkFlow::Note::OperandMayMeetCondition, m_builtins.canonicalID(m_call.builtin).index(), key.index());
        }
        LinkType symbol;
        symbol.primitives = LinkType::oneSymbol;
        for (const ResultInLinkCalls& result : results())
            m_flow.arrive(result.node, result.index ? LinkType::anything() : symbol, m_call.key);
        return;
    }
    if (!isSettledButForOwnCallees())
        give(callBuiltin(m_call.builtin, passedByCall(), false));
}

static bool holdsOnlyObjectsThatCannotBeCalledForLinkCalls(const LinkFlow& flow, const LinkType& callee)
{
    auto cannotBeCalled = [&](LayoutID layout) {
        size_t stepsLeft = flow.program.layouts.size();
        for (LayoutID current = layout; current && stepsLeft--; current = flow.program.at(current).base) {
            const Layout& made = flow.program.at(current);
            bool isOrdinary = made.kind == LayoutKind::Record || made.kind == LayoutKind::Container || (made.kind == LayoutKind::Instance && (made.base || !made.has(LayoutFlags::HasHeritage)));
            if (!isOrdinary || made.writtenBuiltinBase)
                return false;
        }
        return true;
    };
    if (callee.objects == LinkType::Objects::Layout)
        return cannotBeCalled(callee.layout);
    return callee.objects == LinkType::Objects::Set && std::ranges::all_of(flow.layoutsOfSet(callee), cannotBeCalled);
}

static bool holdsOnlyUncallableInstancesForLinkCalls(const BuiltinTable& builtins, const LinkType& callee)
{
    if (callee.classes != LinkType::Classes::Site || callee.setOfClasses)
        return false;
    switch (builtins.at(callee.builtinClass).type) {
    case ArrayType:
    case ErrorInstanceType:
    case JSDateType:
    case JSMapType:
    case JSPromiseType:
    case JSSetType:
    case JSWeakMapType:
    case JSWeakSetType:
    case RegExpObjectType:
        return true;
    default:
        return false;
    }
}

bool CallInLinkCalls::receiverIsOfConstructorFunctions(const Passed& passed)
{
    if (m_shared || m_call.callee != SummaryCall::Callee::Name || !passed.receiver)
        return false;
    bool isOfConstructorFunctions = false;
    bool isOfOthers = false;
    auto lookAt = [&](const LinkType& type, const Source*) {
        isOfOthers |= type.hasUnknownObjects || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None;
        switch (type.objects) {
        case LinkType::Objects::None:
            return;
        case LinkType::Objects::Layout:
            isOfConstructorFunctions = true;
            isOfOthers |= !m_flow.isOfConstructorFunction(type.layout);
            return;
        case LinkType::Objects::Set:
            isOfConstructorFunctions = true;
            isOfOthers |= !std::ranges::all_of(m_flow.layoutsOfSet(type), [&](LayoutID layout) { return m_flow.isOfConstructorFunction(layout); });
            return;
        case LinkType::Objects::Family:
            isOfConstructorFunctions = true;
            isOfOthers |= !std::ranges::all_of(m_flow.layoutsOfFamily(type.family), [&](LayoutID layout) { return m_flow.isOfConstructorFunction(layout); });
            return;
        case LinkType::Objects::Prototype:
        case LinkType::Objects::Prototypes:
        case LinkType::Objects::Lost:
            isOfOthers = true;
            return;
        }
    };
    bool mayOnlyLook = passed.receiver->made.isNever() && std::ranges::all_of(passed.receiver->sources, [](const Source& source) { return source.distance <= farthestObservedForLinkCalls; });
    if (mayOnlyLook) {
        for (const Source& source : passed.receiver->sources)
            lookAt(m_flow.passedBy(m_flow.typeAt(source.node), source.passes), &source);
        if (isOfOthers)
            return false;
        isOfConstructorFunctions = false;
    }
    forEachType(*passed.receiver, lookAt);
    return isOfConstructorFunctions && !isOfOthers;
}

void CallInLinkCalls::callValue(LinkType callee, const Source* source, bool mayGoByRows, const Passed& passed)
{
    if (callee.isNever())
        return;
    throwError();

    switch (callee.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        enter(callee.function, passed);
        break;
    case LinkType::Functions::Constructors:
        give(callThroughSharedCall(SharedCall::Subjects::CalledClassObjects, callee.constructorsOf, passed));
        break;
    case LinkType::Functions::Set:
        if (source && (!source->distance || (source->distance <= farthestObservedForLinkCalls && receiverIsOfConstructorFunctions(passed)))) {
            for (FunctionID member : m_flow.functionsOfSet(callee))
                enter(member, passed);
            break;
        }
        [[fallthrough]];
    case LinkType::Functions::Lost:
        give(passToLostFunctions(passed, callee, source));
        break;
    }

    if (callee.builtin && (m_shared || m_call.callee != SummaryCall::Callee::Value || !m_flow.isSpelledInBody(m_caller, m_call.subject)))
        m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_builtins.canonicalID(callee.builtin).index());
    if (callee.builtin && !isSettledButForOwnCallees()) {
        if (mayGoByRows) {
            callees().add(m_builtins.canonicalID(callee.builtin));
            give(callBuiltin(callee.builtin, passed, false));
        } else
            callUnseen();
    }
    if (callee.classes == LinkType::Classes::Site && callee.builtinClass == BuiltinClasses::Function && mayGoByRows && fulfillThrough(callee, passed)) [[unlikely]]
        return;
    bool ridesBesideUnseenObject = callee.classes == LinkType::Classes::Site && m_builtins.at(callee.builtinClass).isFollowedBesideUnseenObject && callee.hasUnknownObjects;
    if (callee.objects == LinkType::Objects::None && !callee.hasUnknownObjects && mayGoByRows && callBuiltinFunctionBoundAt(callee, passed))
        return;
    if ((callee.objects != LinkType::Objects::None && !holdsOnlyObjectsThatCannotBeCalledForLinkCalls(m_flow, callee)) || (callee.classes != LinkType::Classes::None && !ridesBesideUnseenObject && !holdsOnlyUncallableInstancesForLinkCalls(m_builtins, callee)))
        callUnseen();
    else if (callee.hasUnknownObjects)
        callUnseen(howUnknownIs(callee));
}

bool CallInLinkCalls::areBuiltinFunctionsAlone(const Values& values)
{
    bool areAlone = !values.isEmpty();
    forEachType(values, [&](const LinkType& type, const Source*) {
        LinkType builtinAlone;
        builtinAlone.builtin = type.builtin;
        BuiltinKind kind = type.builtin ? m_builtins.at(m_builtins.canonicalID(type.builtin)).kind : BuiltinKind::Namespace;
        areAlone &= type.builtin && type == builtinAlone && (kind == BuiltinKind::Function || kind == BuiltinKind::Method);
    });
    return areAlone;
}

bool CallInLinkCalls::callBuiltinFunctionBoundAt(const LinkType& callee, const Passed& passed)
{
    if (callee.classes != LinkType::Classes::Site || callee.builtinClass != BuiltinClasses::BoundFunction || passed.isConstruction)
        return false;
    FlowNodeID target = m_flow.contents(callee.site, m_flow.slotOf(m_builtins.findSlot(BuiltinClasses::BoundFunction, ContentsSlot::Value)));
    FlowNodeID boundArguments = m_flow.contents(callee.site, m_flow.slotOf(m_builtins.findSlot(BuiltinClasses::BoundFunction, ContentsSlot::Element)));
    if (!watch(target, 1) || !watch(boundArguments, 1) || !heldAt(boundArguments).isNever())
        return false;
    if (heldAt(target).isNever())
        return true;
    if (!areBuiltinFunctionsAlone(valuesOf(heldAt(target))))
        return false;
    if (isSettledButForOwnCallees())
        return true;
    BuiltinID bound = heldAt(target).builtin;
    Passed toTarget = passed.withoutReceiver();
    toTarget.receiver = valuesOf(m_builtins.ignoresReceiver(bound) ? primitivesForLinkCalls(Primitive::Undefined) : LinkType::anything());
    callees().add(m_builtins.canonicalID(bound));
    give(callBuiltin(bound, toTarget, false));
    return true;
}

std::optional<LinkType> CallInLinkCalls::findFulfillingFunction(RowCall& call)
{
    if (m_shared || m_makesWhatRowsMakeAtOneSiteOfProgram || call.isCalledByBuiltin)
        return std::nullopt;
    SlotOfContents value = m_flow.slotOf(m_builtins.findSlot(BuiltinClasses::Promise, ContentsSlot::Value));
    SiteID function = m_flow.siteOf(LinkFlow::MakerOfSite::PartOfRow, m_number, 0, BuiltinClasses::Function);
    bool promiseIsFollowed = forEachSiteOfResult(call, [&](SiteID promise, const BuiltinTypePart&) {
        m_flow.arrive(m_flow.contents(function, value), m_flow.typeOf(promise));
    });
    if (!promiseIsFollowed)
        return std::nullopt;
    return m_flow.typeOf(function);
}

bool CallInLinkCalls::fulfillThrough(const LinkType& callee, const Passed& passed)
{
    if (callee.objects != LinkType::Objects::None || callee.hasUnknownObjects || passed.isConstruction)
        return false;
    SlotOfContents value = m_flow.slotOf(m_builtins.findSlot(BuiltinClasses::Promise, ContentsSlot::Value));
    FlowNodeID promises = m_flow.contents(callee.site, value);
    if (!watch(promises, 1))
        return false;
    LinkType promise = heldAt(promises);
    if (promise.isNever())
        return true;
    if (promise.classes != LinkType::Classes::Site || promise.builtinClass != BuiltinClasses::Promise)
        return false;
    Values given;
    if (!passed.arguments.empty())
        given = passed.arguments[0];
    else {
        if (passed.list)
            add(given, *passed.list);
        add(given, primitivesForLinkCalls(Primitive::Undefined));
    }
    storeInContents(promise.site, value, given);
    callees().mayEnterUnseen = true;
    for (const ResultInLinkCalls& result : results())
        m_flow.arrive(result.node, result.index ? LinkType::anything() : primitivesForLinkCalls(Primitive::Undefined));
    return true;
}

void CallInLinkCalls::callValueAt(FlowNodeID node, const Passed& passed)
{
    Source source { node, 1, { } };
    watch(node, source.distance);
    callValue(heldAt(node), &source, true, passed);
}

bool CallInLinkCalls::callWhatGetterAtReturns(FlowNodeID member, const Passed& passed)
{
    if (m_shared || !passed.receiver || passed.isConstruction)
        return false;
    watch(member, 1);
    LinkType held = heldAt(member);
    if (held.isNever())
        return true;
    if (held.functions != LinkType::Functions::Closure || held != closuresForLinkCalls(held.function))
        return false;
    FunctionID getter = held.function;
    const Function& function = m_program.at(getter);
    if (function.kind != FunctionKind::Getter || !function.hasBody() || function.isCoroutine() || function.results.size() != 1)
        return false;

    m_flow.reach(getter);
    m_flow.addEdge(m_flow.thrown(getter), thrownNode());
    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t numberOfPositional = function.numberOfPositionalParameters();
    for (uint32_t parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter) {
        FlowNodeID to = m_flow.parameter(getter, parameter);
        switch (function.entryParameterKind(parameter)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            flowInto(*passed.receiver, to);
            break;
        case EntryParameter::Callee:
            m_flow.arrive(to, closuresForLinkCalls(getter));
            break;
        case EntryParameter::NewTarget:
            m_flow.arrive(to, primitivesForLinkCalls(Primitive::Undefined));
            break;
        case EntryParameter::ArgumentCount:
            m_flow.arrive(to, primitivesForLinkCalls(Primitive::Integer));
            break;
        case EntryParameter::Parameter:
            m_flow.arrive(to, parameter - firstPositional >= numberOfPositional ? LinkType::anything() : primitivesForLinkCalls(Primitive::Undefined));
            break;
        case EntryParameter::HiddenRest:
            m_flow.arrive(to, LinkType::anything());
            break;
        }
    }
    callValueAt(m_flow.resultToFollow(getter, 0), passed);
    return true;
}

static bool holdsNothingInSightForLinkCalls(const LinkType& type)
{
    return type.objects == LinkType::Objects::None && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin && !type.holdsLostBuiltins;
}

ALWAYS_INLINE void CallInLinkCalls::callName()
{
    if (!m_call.key || m_call.receiver == noSummaryNode) {
        callUnseen();
        return;
    }
    LinkType receiver = heldAt(nodeAt(m_call.receiver));
    if (receiver.isNever())
        return;
    if (isDoneButForWhatIsInSight() && holdsNothingInSightForLinkCalls(receiver))
        return;
    throwError();

    switch (receiver.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        callMemberOfInstance(receiver.layout);
        break;
    case LinkType::Objects::Set: {
        std::span<const LayoutID> members = m_flow.layoutsOfSet(receiver);
        Vector<FamilyID, 6> familiesOfPairs;
        for (LayoutID member : members) {
            bool sharesFamily = false;
            for (LayoutID other : members) {
                FamilyID family = other == member ? FamilyID() : m_flow.families.nearestCommonFamily(member, other);
                if (!family)
                    continue;
                sharesFamily = true;
                if (!familiesOfPairs.contains(family))
                    familiesOfPairs.append(family);
            }
            if (!sharesFamily)
                callMemberOfInstance(member);
        }
        for (FamilyID family : familiesOfPairs)
            give(callThroughSharedCall(SharedCall::Subjects::Instances, family, passedByCall()));
        break;
    }
    case LinkType::Objects::Prototype:
        callMemberOfPrototype(receiver.layout);
        break;
    case LinkType::Objects::Prototypes:
        give(callThroughSharedCall(SharedCall::Subjects::PrototypeObjects, receiver.family, passedByCall()));
        break;
    case LinkType::Objects::Family:
        give(callThroughSharedCall(SharedCall::Subjects::Instances, receiver.family, passedByCall()));
        break;
    case LinkType::Objects::Lost:
        callValueAt(m_flow.fieldReadThroughLost(receiver.layout, m_call.key), passedByCall());
        break;
    }

    switch (receiver.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        callMemberOfClosure(receiver.function);
        break;
    case LinkType::Functions::Constructors:
        give(callThroughSharedCall(SharedCall::Subjects::ClassObjects, receiver.constructorsOf, passedByCall()));
        break;
    case LinkType::Functions::Set:
        for (FunctionID member : m_flow.functionsOfSet(receiver))
            callMemberOfClosure(member);
        break;
    case LinkType::Functions::Lost:
        callMemberOfLostClosures(receiver);
        break;
    }

    switch (receiver.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site:
        callMemberOfClass(instancesAmongForLinkCalls(receiver));
        break;
    case LinkType::Classes::Mixed:
        if (!m_flow.namesClassesOf(receiver)) {
            callUnseen();
            break;
        }
        m_flow.forEachClassOf(receiver, [&](const LinkType& instancesOfOneClass) {
            callMemberOfClass(instancesOfOneClass);
        });
        break;
    }

    for (uint16_t group : primitivesWithClassesForLinkCalls) {
        if (isSettledButForOwnCallees() || isDoneButForWhatIsInSight())
            break;
        uint16_t present = (receiver.hasUnknownObjects ? receiver.directPrimitives() : receiver.primitives) & group;
        if (!present)
            continue;
        BuiltinClassID classOfPresent = primitiveClassOf(LinkType::inNamesOfPrimitive(present));
        MemberOfTable found = findOnInstancesOf(classOfPresent);
        if (found.outcome == Lookup::Unknown && !receiver.hasUnknownObjects && isNameUndefinedAbove(classOfPresent))
            found.outcome = Lookup::Absent;
        callMemberOfTable(found, valuesOf(primitivesForLinkCalls(present)), WhenAbsent::CallThrows, ReceiverOfUnseenCode::UnseenObjectItself);
        if (found.outcome == Lookup::Unknown && !m_shared && m_state.callsOnUnseenReceivers[m_number] == 2)
            m_state.callsOnUnseenReceivers[m_number] = StateOfLinkCalls::isDoneButForWhatIsInSight;
    }
    const BuiltinCondition* light = receiver.builtin ? m_flow.lightFormOf(receiver.builtin) : nullptr;
    bool isServedByLightForm = light && std::ranges::contains(light->methodsServed, m_program.builtinNameOf(m_call.key));
    if (receiver.builtin && (m_shared || (!isServedByLightForm && (m_call.receiver == noSummaryNode || !m_flow.isSpelledInBody(m_caller, m_call.receiver)))))
        m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_builtins.canonicalID(receiver.builtin).index());
    if (isServedByLightForm) {
        give(passToUnseen(passedByCall().withoutReceiver()));
    } else if (receiver.builtin && !isSettledButForOwnCallees()) {
        MemberOfTable found = findAbove(receiver.builtin);
        bool isUndefined = found.outcome != Lookup::Found && isNameUndefinedOn(receiver.builtin);
        if (isUndefined)
            found.outcome = Lookup::Absent;
        callMemberOfTable(found, valuesOf(builtinForLinkCalls(receiver.builtin)), isUndefined ? WhenAbsent::CallThrows : WhenAbsent::MayHaveBeenAdded);
    }
    if (receiver.hasUnknownObjects) {
        UnknownIs how = howUnknownIs(receiver);
        bool holdsWhatCanBeRead = false;
        if (how == UnknownIs::InSight && !m_shared && !isSettledButForOwnCallees() && !m_unseenCodeProvidesInstances && !m_flow.rowsSpeakOfKeyFor(m_caller, m_call.key) && listsAloneHaveMemberCalledThatMakesList(holdsWhatCanBeRead)) {
            SetForScope makesListAtOneSite(m_makesWhatRowsMakeAtOneSiteOfProgram, !holdsWhatCanBeRead);
            callMemberOfTable(findOnObjectsOfClassItself(BuiltinClasses::Array), valuesOf(unseenObjectsForLinkCalls()), WhenAbsent::CallThrows, ReceiverOfUnseenCode::UnseenObjectItself);
            how = UnknownIs::ByEscapeAlone;
        }
        callUnseen(how, ReceiverOfUnseenCode::UnseenObjectItself);
    }
}

bool CallInLinkCalls::listsAloneHaveMemberCalledThatMakesList(bool& holdsWhatCanBeRead)
{
    if (!m_call.key || liesInThread(m_call.key))
        return false;
    MemberOfTable found = findOnObjectsOfClassItself(BuiltinClasses::Array);
    if (found.outcome != Lookup::Found)
        return false;
    bool makesList = false;
    bool makesListOfWhatCallbackGives = false;
    for (const BuiltinOverload& row : m_builtins.overloadsOf(m_builtins.canonicalID(found.member))) {
        if (!row.result.type)
            continue;
        for (const BuiltinTypePart& part : m_builtins.at(row.result.type).parts) {
            makesList |= part.kind == BuiltinTypePartKind::InstanceOfReceiverClass || (part.kind == BuiltinTypePartKind::Instance && part.builtinClass == BuiltinClasses::Array);
            makesListOfWhatCallbackGives |= std::ranges::any_of(part.contents, [&](const BuiltinSlotContents& contents) {
                return contents.type && std::ranges::contains(m_builtins.at(contents.type).parts, BuiltinTypePartKind::CallbackResult, &BuiltinTypePart::kind);
            });
        }
    }
    if (!makesList)
        return false;
    if (makesListOfWhatCallbackGives && m_call.numberOfArguments && m_summary.arguments[m_call.firstArgument] != noSummaryNode) {
        const LinkType& callback = m_flow.typeAt(nodeAt(m_summary.arguments[m_call.firstArgument]));
        if (callback.functions == LinkType::Functions::Closure) {
            const ThinSummary& ofCallback = m_flow.summaries.at(callback.function);
            holdsWhatCanBeRead = std::ranges::any_of(ofCallback.edges, [&](const SummaryEdge& edge) {
                if (ofCallback.nodes[edge.to].kind != SummaryNode::Kind::Result)
                    return false;
                SummaryNode::Kind given = ofCallback.nodes[edge.from].kind;
                return given == SummaryNode::Kind::ClassInstance || given == SummaryNode::Kind::Allocation || given == SummaryNode::Kind::ResultOfCall || given == SummaryNode::Kind::Join;
            });
        }
    }
    static constexpr std::array otherClassesWhoseMethodsAreFoundByName { BuiltinClasses::Map, BuiltinClasses::Set, BuiltinClasses::String, BuiltinClasses::WeakMap, BuiltinClasses::WeakSet, BuiltinClasses::RegExp, BuiltinClasses::Promise, BuiltinClasses::Function };
    return std::ranges::none_of(m_builtins.classesWithInstanceMember(m_program.builtinNameOf(m_call.key)), [&](BuiltinClassID other) {
        return std::ranges::contains(otherClassesWhoseMethodsAreFoundByName, other);
    });
}

ValuesInLinkCalls CallInLinkCalls::callThroughSharedCall(SharedCall::Subjects subjects, FamilyID family, const Passed& passed)
{
    std::optional<uint32_t> indexOfSharedCall;
    Values answers = tieToSharedCall(subjects, family, passed, indexOfSharedCall);
    if (indexOfSharedCall)
        callees().addShared(*indexOfSharedCall);
    return answers;
}

ValuesInLinkCalls CallInLinkCalls::tieToSharedCall(SharedCall::Subjects subjects, FamilyID family, const Passed& passed, std::optional<uint32_t>& indexOfSharedCall)
{
    noteArgumentsEscape(passed);
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    if (passed.newTarget && handsOnOwnNewTarget(passed)) {
        Values answers;
        RELEASE_ASSERT(subjects == SharedCall::Subjects::CalledClassObjects);
        forEachConstructorOf(family, [&](FunctionID constructor) {
            Values constructed;
            switch (passTo(constructor, passed, constructed)) {
            case Entry::Refused:
                break;
            case Entry::Unseen:
                add(answers, LinkType::anything());
                break;
            case Entry::Body:
                add(answers, resultOf(constructor, 0, passed, constructed, notObservedForLinkCalls));
                break;
            }
        });
        return answers;
    }

    bool isByName = subjects != SharedCall::Subjects::CalledClassObjects;
    uint32_t numberOfArguments = static_cast<uint32_t>(passed.arguments.size());
    std::pair<uint64_t, uint32_t> place {
        static_cast<uint64_t>(family.index()) << 32 | (isByName ? m_call.key.index() : 0),
        static_cast<uint32_t>(subjects) | !!passed.list << 2 | passed.isConstruction << 3 | !!passed.receiver << 4 | isLibraryCode(m_program, m_program.at(m_caller)) << 5 | !!passed.newTarget << 6 | numberOfArguments << 7,
    };
    auto found = m_state.indexesOfSharedCalls.add(place, static_cast<uint32_t>(m_state.sharedCalls.size()));
    uint32_t index = found.iterator->value;
    if (found.isNewEntry) {
        auto made = makeUniqueWithoutFastMallocCheck<SharedCall>();
        made->subjects = subjects;
        made->family = family;
        made->index = index;
        made->firstCaller = m_caller;
        made->firstCall = m_index;
        made->nodes.append(m_flow.unseen());
        made->nodes.append(m_flow.makeSharedNode());
        made->call.callee = isByName ? SummaryCall::Callee::Name : SummaryCall::Callee::Value;
        made->call.site = m_call.site;
        made->call.isConstruction = passed.isConstruction;
        made->hasList = !!passed.list;
        if (isByName)
            made->call.key = m_call.key;
        if (passed.receiver)
            made->call.receiver = 1;
        made->call.numberOfArguments = numberOfArguments + made->hasList;
        SummaryBeingMade summary;
        for (uint32_t argument = 0; argument < made->call.numberOfArguments; ++argument) {
            summary.arguments.append(static_cast<uint32_t>(made->nodes.size()));
            made->nodes.append(m_flow.makeSharedNode());
        }
        made->summary = ThinSummary(summary);
        if (passed.newTarget) {
            made->call.newTarget = static_cast<uint32_t>(made->nodes.size());
            made->nodes.append(m_flow.makeSharedNode());
        }
        made->result = m_flow.makeSharedNode();
        made->thrown = m_flow.makeSharedNode();
        m_state.sharedCalls.append(WTF::move(made));
        m_flow.calleesOfSharedCalls.append(LinkFlow::CalleesOfCall { });
        m_flow.subjectsOfSharedCalls.append(LinkFlow::SubjectOfSharedCall { family, isByName ? m_call.key : PropertyKeyID(), place.second });

        uint32_t firstMember = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
        Vector<LayoutID> membersThatMayAnswer;
        if (subjects == SharedCall::Subjects::Instances && m_flow.listMembersThatMayAnswerUnder(family, m_call.key, membersThatMayAnswer)) {
            m_flow.build.statistics().count(StatisticID::SharedCallsWithDormantMembers);
            layPortersAtMembersOf(family, membersThatMayAnswer);
            firstMember = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
            m_state.sleepersOfFamilies.get(family.index())->sharedCalls.append(index);
            for (LayoutID member : membersThatMayAnswer)
                m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { index, member, 0 });
            m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { index, LayoutID(), 0 });
            m_state.sharedCalls[index]->someMembersSleep = true;
            m_state.sharedCalls[index]->membersAwakeFromTheStart = WTF::move(membersThatMayAnswer);
        } else {
            forEachLayoutOf(family, [&](LayoutID member) {
                FunctionID constructor = m_program.at(member).constructor;
                bool hasClassObject = constructor && m_flow.classOfConstructor(constructor) == member;
                if (hasClassObject || subjects == SharedCall::Subjects::Instances || subjects == SharedCall::Subjects::PrototypeObjects)
                    m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { index, member, 0 });
            });
        }
        uint32_t endOfMembers = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
        for (uint32_t member = firstMember; member < endOfMembers; ++member)
            CallInLinkCalls(m_flow, m_state, *m_state.sharedCalls[index]).resolveMemberOfSharedCall(member);
    }

    const SharedCall& shared = *m_state.sharedCalls[index];
    indexOfSharedCall = index;
    if (passed.receiver)
        flowInto(*passed.receiver, shared.nodes[1]);
    for (uint32_t argument = 0; argument < numberOfArguments; ++argument)
        flowInto(passed.arguments[argument], shared.nodes[2 + argument]);
    if (passed.list)
        flowInto(*passed.list, shared.nodes[2 + numberOfArguments]);
    if (passed.newTarget)
        flowInto(*passed.newTarget, shared.nodes[shared.call.newTarget]);
    m_flow.addEdge(shared.thrown, thrownNode());

    Values answers;
    add(answers, shared.result, notObservedForLinkCalls);
    return answers;
}

void CallInLinkCalls::resolveMemberOfSharedCall(uint32_t member)
{
    m_memberThatWatches = member;
    LayoutID layout = m_state.membersOfSharedCalls[member].layout;
    throwError();
    if (!layout) {
        wakeMembersOnWhichFieldIsBornUnderName();
        return;
    }
    switch (m_shared->subjects) {
    case SharedCall::Subjects::Instances:
        callMemberOfInstance(layout);
        return;
    case SharedCall::Subjects::PrototypeObjects:
        callMemberOfPrototype(layout);
        return;
    case SharedCall::Subjects::ClassObjects:
        callMemberOfClosure(m_program.at(layout).constructor);
        return;
    case SharedCall::Subjects::CalledClassObjects:
        enter(m_program.at(layout).constructor, passedByCall());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CallInLinkCalls::layPortersAtMembersOf(FamilyID family, Vector<LayoutID>& membersThatMayAnswer)
{
    auto& sleepers = *m_state.sleepersOfFamilies.ensure(family.index(), [] {
        return makeUniqueWithoutFastMallocCheck<StateOfLinkCalls::SleepersOfFamily>();
    }).iterator->value;
    if (sleepers.portersOfMembers.isEmpty()) {
        bool shouldIgnoreUpdates = AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::IgnoreDormantMemberUpdates);
        sleepers.nameOfFirstCall = m_call.key;
        for (LayoutID member : m_flow.instancesOfClassesIn(family)) {
            uint32_t porter = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
            m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { StateOfLinkCalls::isPorterOfFamily | family.index(), member, 0 });
            sleepers.portersOfMembers.add(member.index(), porter);
            switch (shouldIgnoreUpdates ? LinkFlow::WhatSilentMemberFinds::Nothing : m_flow.whatPorterTakesMemberToFind(FlowObserver { FlowObserver::Kind::MemberOfSharedCall, m_caller, porter }, member, m_call.key)) {
            case LinkFlow::WhatSilentMemberFinds::Nothing:
            case LinkFlow::WhatSilentMemberFinds::NothingThatCanBeCalled:
                break;
            case LinkFlow::WhatSilentMemberFinds::TheUnknownAlone:
                sleepers.someMemberFindsTheUnknownAlone = true;
                break;
            case LinkFlow::WhatSilentMemberFinds::SomethingOfItsOwn:
                if (sleepers.isUp.add(member.index()).isNewEntry)
                    sleepers.membersThatAreUp.append(member);
                break;
            }
        }
    }
    membersThatMayAnswer.appendVector(sleepers.membersThatAreUp);
    std::ranges::sort(membersThatMayAnswer);
    membersThatMayAnswer.shrink(std::ranges::unique(membersThatMayAnswer).begin() - membersThatMayAnswer.begin());
}

void CallInLinkCalls::wakeMembersOnWhichFieldIsBornUnderName()
{
    if (!m_shared->someMembersSleep || AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::IgnoreDormantMemberUpdates))
        return;
    if (m_state.sleepersOfFamilies.get(m_shared->family.index())->someMemberFindsTheUnknownAlone)
        callUnseen();
    FlowObserver keeper { FlowObserver::Kind::MemberOfSharedCall, m_caller, m_memberThatWatches };
    for (LinkFlow::FieldBornOnMember born : m_flow.letKeeperHearOfFieldsBornUnder(m_shared->family, m_call.key, keeper)) {
        watch(born.node, 1);
        if (!heldAt(born.node).isNever())
            wake(born.member);
    }
}

void CallInLinkCalls::wake(LayoutID member)
{
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    if (!m_shared->someMembersSleep || std::ranges::binary_search(m_shared->membersAwakeFromTheStart, member) || !m_shared->membersWokenLater.add(member.index()).isNewEntry)
        return;
    if (!std::exchange(m_shared->someSleeperWasWoken, true))
        m_flow.build.statistics().count(StatisticID::SharedCallsWithActivatedMembers);
    m_flow.build.statistics().count(StatisticID::DormantMembersActivated);
    uint32_t entry = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
    m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { m_shared->index, member, 0 });
    CallInLinkCalls(m_flow, m_state, *m_shared).resolveMemberOfSharedCall(entry);
}

void CallInLinkCalls::wakeEverySleeper()
{
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    uint32_t firstMember = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
    forEachLayoutOf(m_shared->family, [&](LayoutID member) {
        if (!std::ranges::binary_search(m_shared->membersAwakeFromTheStart, member) && !m_shared->membersWokenLater.contains(member.index()))
            m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { m_shared->index, member, 0 });
    });
    m_shared->someMembersSleep = false;
    m_shared->membersAwakeFromTheStart.clear();
    m_shared->membersWokenLater.clear();
    uint32_t endOfMembers = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
    for (uint32_t member = firstMember; member < endOfMembers; ++member)
        CallInLinkCalls(m_flow, m_state, *m_shared).resolveMemberOfSharedCall(member);
}

void CallInLinkCalls::callMemberFoundAbove(FlowNodeID above, const Passed& passed, unsigned stepsLeft)
{
    watch(above, 1);
    const LinkType type = heldAt(above);
    auto lookAt = [&](LayoutID holder) {
        if (std::optional<FieldReference> field = findField(m_program, holder, m_call.key))
            callValueAt(m_flow.field(*field), passed);
        else if (stepsLeft && m_program.at(holder).has(LayoutFlags::IsOfCreatedObject))
            callMemberFoundAbove(m_flow.whatStandsAbove(holder), passed, stepsLeft - 1);
    };
    if (type.objects == LinkType::Objects::Layout)
        lookAt(type.layout);
    else if (type.objects == LinkType::Objects::Set) {
        for (LayoutID member : m_flow.layoutsOfSet(type))
            lookAt(member);
    }
    if (type.classes == LinkType::Classes::Site && stepsLeft)
        callMemberFoundAbove(m_flow.whatStandsAbove(type.site), passed, stepsLeft - 1);
}

void CallInLinkCalls::callMethodsDeclaredOnInstance(LayoutID id)
{
    if (m_shared)
        return;
    Passed passed = passedWithReceiver(objectsForLinkCalls(id));
    for (const LinkFlow::FoundMember& member : m_flow.findMembers(id, ClassSide::Prototype, m_call.key)) {
        if (member.kind == MethodKind::Method)
            callValueAt(m_flow.member(member.classLayout, ClassSide::Prototype, member.index), passed);
    }
}

void CallInLinkCalls::callMemberOfInstance(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        break;
    case LayoutKind::ModuleNamespace:
        callExportOf(layout);
        return;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        callUnseen();
        return;
    }

    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    if (nameClass == NameClass::Proto || m_flow.escapedLayouts.get(id.index()) || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        bool givesUpInSight = nameClass == NameClass::Proto || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        callUnseen(givesUpInSight ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
        if (givesUpInSight) {
            if (nameClass != NameClass::Proto)
                callMethodsDeclaredOnInstance(id);
            return;
        }
    }

    Passed passed = passedWithReceiver(objectsForLinkCalls(id));
    std::optional<FieldReference> field = findField(m_program, id, m_call.key);
    if (field) {
        const Field& listed = fieldAt(m_program, *field);
        if (listed.form.contains(FieldForm::IsAccessor)) {
            callUnseen();
            return;
        }
        callValueAt(m_flow.field(*field), passed);
        if (std::optional<ProvenInEveryVersion> established = listed.declared.established()) {
            if (TypeID type = *established)
                callValue(m_flow.fromTypeID(type), nullptr, false, passed);
        }
    }

    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_call.key);
    for (const LinkFlow::FoundMember& member : members) {
        switch (member.kind) {
        case MethodKind::Method:
            callValueAt(m_flow.member(member.classLayout, ClassSide::Prototype, member.index), passed);
            break;
        case MethodKind::Getter:
            if (callWhatGetterAtReturns(m_flow.member(member.classLayout, ClassSide::Prototype, member.index), passed))
                break;
            callUnseen();
            return;
        case MethodKind::Setter:
            if (std::ranges::contains(members, MethodKind::Getter, &LinkFlow::FoundMember::kind))
                break;
            callUnseen();
            return;
        }
    }

    callValueAt(m_flow.foundBesideFieldsFor(id, m_call.key), passed);
    if (!field)
        callValueAt(m_flow.appendedField(id, m_call.key), passed);
    if (!field && layout.has(LayoutFlags::IsOfCreatedObject))
        callMemberFoundAbove(m_flow.whatStandsAbove(id), passed, LinkFlow::mostStepsAboveCreatedObject);

    bool isPrivateName = m_program.at(m_call.key).kind == PropertyKeyKind::PrivateName;
    if (FlowNodeID foundOnPrototypes = isPrivateName ? FlowNodeID() : m_flow.foundOnPrototypesAbove(id, m_call.key))
        callValueAt(foundOnPrototypes, passed);
    bool isAnsweredByClass = !members.lookupGoesOnAboveClasses();
    if (isAnsweredByClass || isPrivateName || m_flow.isAbsentFromObjectPrototype(m_call.key))
        return;
    if (nameClass == NameClass::Constructor) {
        callUnseen();
        return;
    }
    BuiltinClassID objectClass = BuiltinClasses::Object;
    callMemberOfTable(findAbove(m_builtins.at(objectClass).prototype), valuesAtNode(m_call.receiver, TypeTable::object), WhenAbsent::MayHaveBeenAdded);
}

bool CallInLinkCalls::inheritsFromObjectAlone(LayoutID id) const
{
    for (LayoutID current = id; current; current = m_program.at(current).base) {
        const Layout& layout = m_program.at(current);
        if (layout.kind != LayoutKind::Instance || layout.writtenBuiltinBase || m_flow.isOfConstructorFunction(current) || (!layout.base && layout.has(LayoutFlags::HasHeritage)))
            return false;
    }
    return true;
}

void CallInLinkCalls::callMemberOfPrototype(LayoutID id)
{
    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_call.key);
    FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_call.key);
    bool isResolvedByProgram = !members.lookupGoesOnAboveClasses() || ((members.mayAnswer() || foundOnPrototypes) && m_flow.isAbsentFromObjectPrototype(m_call.key));
    BuiltinID above = findOwnMember(m_program, m_builtins.canonicalID(Builtins::ObjectPrototype), m_call.key);
    bool isResolvedByTable = !isResolvedByProgram && inheritsFromObjectAlone(id) && (isNameOfConversion() || (above && m_builtins.at(above).kind == BuiltinKind::Method));
    if ((!isResolvedByProgram && !isResolvedByTable) || m_flow.escapedLayouts.get(id.index()) || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        bool givesUpInSight = (!isResolvedByProgram && !isResolvedByTable) || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        callUnseen(givesUpInSight ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
        if (givesUpInSight)
            return;
    }
    if (foundOnPrototypes)
        callValueAt(foundOnPrototypes, passedByCall());
    if (foundOnPrototypes || isResolvedByTable)
        callValueAt(m_flow.foundBesideFieldsFor(id, m_call.key), passedByCall());
    if (isResolvedByTable && !m_flow.isAbsentFromObjectPrototype(m_call.key)) {
        BuiltinClassID objectClass = BuiltinClasses::Object;
        LinkType prototype;
        prototype.objects = LinkType::Objects::Prototype;
        prototype.layout = id;
        callMemberOfTable(findAbove(m_builtins.at(objectClass).prototype), valuesOf(prototype), WhenAbsent::MayHaveBeenAdded);
    }
    for (const LinkFlow::FoundMember& member : members) {
        switch (member.kind) {
        case MethodKind::Method:
            callValueAt(m_flow.member(member.classLayout, ClassSide::Prototype, member.index), passedByCall());
            break;
        case MethodKind::Getter:
        case MethodKind::Setter:
            callUnseen();
            return;
        }
    }
}

void CallInLinkCalls::callExportOf(const Layout& layout)
{
    std::optional<uint32_t> field = layout.namespaceOf ? layout.findField(m_call.key) : std::nullopt;
    if (!field) {
        callUnseen();
        return;
    }

    ModuleID module = layout.namespaceOf;
    uint32_t index = *field;
    m_flow.noteExportRead(module, index);
    for (size_t steps = 0; steps <= m_program.modules.size(); ++steps) {
        const auto& exports = m_program.at(module).exports;
        if (index >= exports.size())
            break;
        const ModuleExport& entry = exports[index];
        if (m_flow.testRuns().mayComeThroughMock(module, index) || entry.isHiddenUntilInitialized) {
            callUnseen();
            return;
        }
        switch (entry.kind) {
        case ModuleExportKind::Variable:
            if (entry.variable)
                callValueAt(m_flow.variable(entry.variable), passedByCall());
            else
                callUnseen();
            return;
        case ModuleExportKind::Namespace:
        case ModuleExportKind::Field:
            callUnseen();
            return;
        case ModuleExportKind::Indirect: {
            if (!entry.module || !entry.importName) {
                callUnseen();
                return;
            }
            const auto& exportsOfTarget = m_program.at(entry.module).exports;
            uint32_t found = 0;
            while (found < exportsOfTarget.size() && exportsOfTarget[found].name != entry.importName)
                ++found;
            module = entry.module;
            index = found;
            break;
        }
        }
    }
    callUnseen();
}

BuiltinID CallInLinkCalls::builtinConstructorAbove(LayoutID layout)
{
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft--; current = m_program.at(current).base) {
        const Layout& record = m_program.at(current);
        if (m_flow.hasComputedKeysOfItsOwn(current, ClassSide::Static) || record.has(LayoutFlags::IsConstructorFunction))
            return { };
        if (!record.base)
            return record.writtenBuiltinBase ? m_builtins.at(record.writtenBuiltinBase).constructor : BuiltinID();
    }
    return { };
}

void CallInLinkCalls::callMemberOfClosure(FunctionID function)
{
    const Function& closed = m_program.at(function);
    LayoutID layout = closed.constructedClass;
    if (layout && m_flow.isOfConstructorFunction(layout))
        layout = { };
    BuiltinID constructorAbove = layout ? builtinConstructorAbove(layout) : BuiltinID();
    Passed passed = passedWithReceiver(closuresForLinkCalls(function));
    auto callRowName = [&] {
        if (FunctionID holder = layout ? m_flow.classRowNameHolder(layout, m_call.key) : FunctionID())
            callValueAt(m_flow.laidOnClosuresOf(holder, m_call.key), passed);
    };
    if (layout && !constructorAbove && m_flow.staticNamesMayBeIncompleteBesideConstructorFunctions(layout)) {
        callRowName();
        callUnseen();
        return;
    }

    if (layout && m_flow.namesMayBeIncomplete(layout, ClassSide::Static)) {
        callValueAt(m_flow.foundOnClassObjectsFrom(layout), passed);
        if (!constructorAbove)
            callValueAt(m_flow.constructorChainStoreNode(layout, m_call.key), passed);
    }
    bool findsField = false;
    uint32_t stepsLeft = static_cast<uint32_t>(m_program.layouts.size());
    for (LayoutID current = m_flow.nearestClassWithStaticFieldThatMayMatch(layout, m_call.key); current && stepsLeft-- && m_flow.extendsAloneLinksClassObjects(layout, current); current = m_flow.nearestClassWithStaticFieldThatMayMatch(m_program.at(current).base, m_call.key)) {
        const Layout& record = m_program.at(current);
        std::optional<uint32_t> field = record.findStaticField(m_call.key);
        if (!field)
            continue;
        findsField = true;
        callValueAt(m_flow.variable(record.staticFields[*field].variable), passed);
    }
    bool findsMember = false;
    LinkFlow::FoundMembers members;
    if (layout)
        members = m_flow.findMembers(layout, ClassSide::Static, m_call.key);
    if (members.mayAnswer() && !m_flow.extendsAloneLinksClassObjects(layout, members[0].classLayout))
        members = { };
    for (const LinkFlow::FoundMember& member : members) {
        findsMember = true;
        switch (member.kind) {
        case MethodKind::Method:
            callValueAt(m_flow.member(member.classLayout, ClassSide::Static, member.index), passed);
            break;
        case MethodKind::Getter:
            if (callWhatGetterAtReturns(m_flow.member(member.classLayout, ClassSide::Static, member.index), passed))
                break;
            callUnseen();
            return;
        case MethodKind::Setter:
            if (std::ranges::contains(members, MethodKind::Getter, &LinkFlow::FoundMember::kind))
                break;
            callUnseen();
            return;
        }
    }
    if (findsMember || (findsField && (isPrivateNameInLink(m_program, m_call.key) || !members.lookupGoesOnAboveClasses())))
        return;

    callRowName();
    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    if (nameClass == NameClass::Proto || nameClass == NameClass::Prototype || mayBeElementKey(nameClass)) {
        callUnseen();
        return;
    }
    callValueAt(m_flow.foundOnClosuresOf(function), passed);
    callValueAt(m_flow.followsNamesOnClosuresOf(function, m_call.key) ? m_flow.laidOnClosuresOf(function, m_call.key) : m_flow.foundBesideFieldsUnder(m_call.key), passed);
    LayoutID rowAbove = heldAt(m_flow.foundOnClosuresOf(function)).isNever() ? LayoutID() : m_flow.layoutOfConstructorFunction(function);
    for (LayoutID above = rowAbove ? m_program.at(rowAbove).base : LayoutID(); above && stepsLeft--; above = m_program.at(above).base) {
        if (FunctionID holder = m_program.at(above).constructor; holder && m_flow.followsNamesOnClosuresOf(holder, m_call.key))
            callValueAt(m_flow.laidOnClosuresOf(holder, m_call.key), passed);
    }
    if (constructorAbove) {
        callMemberOfTable(findAbove(constructorAbove), *passed.receiver, WhenAbsent::MayHaveBeenAdded);
        return;
    }
    callMemberOfTable(findOnObjectsOfClassItself(classOfClosuresForLinkCalls(closed)), *passed.receiver, WhenAbsent::CallThrows);
}

void CallInLinkCalls::callMemberOfLostClosures(const LinkType& receiver)
{
    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    if (isPrivateNameInLink(m_program, m_call.key) || nameClass == NameClass::Proto || nameClass == NameClass::Prototype || mayBeElementKey(nameClass)) {
        callUnseen();
        return;
    }
    if (m_flow.lostFunctionsAreAtUnseen(receiver)) {
        callUnseen(UnknownIs::ByEscapeAlone);
        return;
    }
    MemberOfTable found = findOnObjectsOfClassItself(BuiltinClasses::Function);
    for (BuiltinClassID other : { BuiltinClassID(BuiltinClasses::AsyncFunction), BuiltinClassID(BuiltinClasses::GeneratorFunction), BuiltinClassID(BuiltinClasses::AsyncGeneratorFunction) }) {
        MemberOfTable there = findOnObjectsOfClassItself(other);
        if (there.outcome != found.outcome || there.member != found.member) {
            callUnseen();
            return;
        }
    }
    LinkType closures;
    closures.functions = LinkType::Functions::Lost;
    closures.function = receiver.function;
    Passed passed = passedWithReceiver(closures);
    m_flow.noteCallOfLostFunctions(closures);
    FlowNodeID laid = m_flow.laidOnLostClosures(closures);
    if (!watch(laid, 1)) {
        callUnseen();
        return;
    }
    if (LinkType held = heldAt(laid); held.hasUnknownObjects) {
        callUnseen(howUnknownIs(held));
        return;
    }
    callValueAt(laid, passed);
    callValueAt(m_flow.foundBesideFieldsUnder(m_call.key), passed);
    callMemberOfTable(found, *passed.receiver, WhenAbsent::CallThrows);
}

void CallInLinkCalls::callMemberOfClass(const LinkType& instances)
{
    BuiltinClassID builtinClass = instances.builtinClass;
    auto letInstancesMeetUnseen = [&]() -> ReceiverOfUnseenCode {
        if (m_shared)
            return ReceiverOfUnseenCode::AnyThatArrives;
        escape(instances, nullptr);
        return ReceiverOfUnseenCode::UnseenObjectItself;
    };
    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    if (nameClass == NameClass::Proto || mayBeElementKey(nameClass) || m_builtins.at(builtinClass).isPrimitiveWrapper || !m_builtins.hasLockedPrototypeChain(builtinClass)) {
        callUnseen(UnknownIs::InSight, letInstancesMeetUnseen());
        return;
    }

    Passed passed = passedWithReceiver(instances);
    bool isAnsweredByUnseenCode = false;
    auto callContentsAt = [&](FlowNodeID contents) {
        Source source { contents, 1, { } };
        watch(contents, source.distance);
        LinkType held = heldAt(contents);
        if (held.hasUnknownObjects) {
            isAnsweredByUnseenCode = true;
            m_unseenCodeProvidesInstances = true;
            callUnseen(howUnknownIs(held), letInstancesMeetUnseen());
            held.hasUnknownObjects = false;
            held.isUnknownEscapeOnly = false;
        }
        callValue(held, &source, true, passed);
    };
    callContentsAt(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
    callContentsAt(m_flow.contentsOf(instances, SlotOfContents::under(m_call.key)));
    MemberOfTable found = findOnObjectsOfClassItself(builtinClass);
    if (found.outcome == Lookup::Unknown && areListsThatOnlyProgramNames(instances) && isNameUndefinedAbove(builtinClass))
        found.outcome = Lookup::Absent;
    auto passesFunction = [&] {
        return std::ranges::any_of(passed.arguments, [&](const Values& argument) {
            bool holdsFunctions = false;
            forEachType(argument, [&](const LinkType& type, const Source*) {
                holdsFunctions |= type.functions != LinkType::Functions::None;
            });
            return holdsFunctions;
        });
    };
    auto callMember = [&](MemberOfTable member) {
        if (isAnsweredByUnseenCode && member.outcome == Lookup::Found && !m_call.isMadeByProtocol && !laysWhatItIsHandedIntoReceiver(member.member)) {
            if (!m_call.isInLoop || !passesFunction())
                return;
        }
        callMemberOfTable(member, *passed.receiver, WhenAbsent::CallThrows);
    };
    if (found.outcome == Lookup::Unknown)
        callMemberOfTable(found, *passed.receiver, WhenAbsent::CallThrows, letInstancesMeetUnseen());
    else
        callMember(found);
    if (!m_builtins.hasClassBelow(builtinClass))
        return;
    std::optional<bool> isOfClassItself;
    for (BuiltinClassID other : m_builtins.classesWithInstanceMember(m_program.builtinNameOf(m_call.key))) {
        if (other == builtinClass || !m_builtins.isSameOrSubclassOf(other, builtinClass))
            continue;
        if (!isOfClassItself)
            isOfClassItself = this->isOfClassItself(instances, 1);
        if (*isOfClassItself)
            return;
        callMember(findOnObjectsOfClassItself(other));
    }
}

bool CallInLinkCalls::laysWhatItIsHandedIntoReceiver(BuiltinID member) const
{
    return std::ranges::any_of(m_builtins.overloadsOf(m_builtins.canonicalID(member)), [](const BuiltinOverload& row) {
        return std::ranges::any_of(row.flow.transfers, [](const BuiltinTransfer& transfer) {
            return transfer.to.kind == BuiltinPlaceKind::Slot && transfer.to.operand && transfer.to.operand->isReceiver() && !(transfer.from.operand && transfer.from.operand->isReceiver());
        });
    });
}

void CallInLinkCalls::callMemberOfTable(MemberOfTable found, Values receiver, WhenAbsent whenAbsent, ReceiverOfUnseenCode receiverOfUnseenCode)
{
    if (isSettledButForOwnCallees() && (hasGivenUpInSight() || !(m_call.isMadeByProtocol || (found.outcome == Lookup::Found && laysWhatItIsHandedIntoReceiver(found.member)))))
        return;
    switch (found.outcome) {
    case Lookup::Unknown:
        callUnseen(UnknownIs::InSight, receiverOfUnseenCode);
        return;
    case Lookup::Absent:
        if (whenAbsent == WhenAbsent::MayHaveBeenAdded)
            callUnseen();
        return;
    case Lookup::Found:
        break;
    case Lookup::FoundWhereProgramMayWrite:
        RELEASE_ASSERT_NOT_REACHED();
    }
    Passed passed = passedByCall().withoutReceiver();
    passed.receiver = WTF::move(receiver);
    callees().add(found.member);
    if (!m_shared)
        m_state.callsThatEnterMembersOfTable[m_number] = 1;
    give(callBuiltin(found.member, passed, false));
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findOnce(uint64_t question, const Invocable<MemberOfTable()> auto& find)
{
    auto& known = m_state.membersOfTable[m_flow.indexOfLaneAtWork()];
    MemberOfTable found;
    if (auto answer = known.find(question); answer != known.end())
        found = { static_cast<Lookup>(answer->value >> 16), BuiltinID(answer->value & 0xffff) };
    else {
        found = find();
        known.add(question, static_cast<uint32_t>(found.outcome) << 16 | found.member.index());
    }
    if (found.outcome != Lookup::Unknown) {
        PropertyKeyID key(static_cast<uint32_t>(question));
        if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(key); added && (!watch(added, 0) || m_flow.testRuns().isNameAdded(added)))
            return { };
    }
    if (found.outcome != Lookup::FoundWhereProgramMayWrite)
        return found;
    BuiltinID canonical = m_builtins.canonicalID(found.member);
    if (!m_flow.takeForConstant(found.member) || !watch(m_flow.writtenAtMember(found.member), 0)) {
        m_flow.noteLossOf(canonical);
        if (static_cast<uint32_t>(question) == m_call.key.index() && m_state.builtinsWithCodeWaitingForOperand.quickGet(canonical.index())) {
            Passed passed = passedByCall();
            RowCall call { .builtin = canonical, .mode = m_call.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, .passed = passed };
            if (askOperandsOfCallOutOfSightForConditions(call))
                m_memberWhoseOperandsAreRead = canonical;
        }
        if (static_cast<uint32_t>(question) == m_call.key.index())
            m_flow.note(LinkFlow::Note::BuiltinCalled, canonical.index(), (m_shared || !m_flow.rowsSpeakFor(m_caller) ? LinkFlow::byCode : 0) | (m_call.isConstruction ? LinkFlow::asConstructor : 0));
        if (static_cast<uint32_t>(question) == m_call.key.index() && m_state.rowsWhoseCallsAreListed.quickGet(canonical.index()))
            m_flow.note(LinkFlow::Note::BuiltinCalledAtSite, m_number, canonical.index() | static_cast<uint32_t>(m_call.isConstruction) << 23);
        return { };
    }
    return { Lookup::Found, canonical };
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findAbove(BuiltinID firstHolder)
{
    MemberOfTable found = findOnce(static_cast<uint64_t>(firstHolder.index()) << 32 | m_call.key.index(), [&] {
        return findAbove(firstHolder, m_call.key, membersUnderKey());
    });
    if (found.outcome != Lookup::Unknown)
        return found;
    found = findOnce(1ull << 59 | static_cast<uint64_t>(firstHolder.index()) << 32 | m_call.key.index(), [&] {
        return tableListsMemberAbove(firstHolder) ? MemberOfTable { Lookup::Found, { } } : MemberOfTable();
    });
    if (found.outcome != Lookup::Found)
        return { };
    std::optional<HoldersPassed> passed = passHoldersUpToMemberFrom(firstHolder);
    if (!passed)
        return { };
    found = findOnce(1ull << 60 | static_cast<uint64_t>(firstHolder.index()) << 32 | m_call.key.index(), [&] {
        return findAbove(firstHolder, m_call.key, membersUnderKey(), passed->span());
    });
    return found.outcome == Lookup::Found ? found : MemberOfTable();
}

bool CallInLinkCalls::tableListsMemberAbove(BuiltinID firstHolder)
{
    for (BuiltinID holder = m_builtins.canonicalID(firstHolder); ; holder = m_builtins.canonicalID(m_builtins.at(holder).inheritsFrom)) {
        if (compiledModuleWithObject(m_program, holder) || m_builtins.at(m_builtins.rootOf(holder)).scope != BuiltinScope::Realm)
            return false;
        if (membersUnderKey().findOwnMember(holder))
            return true;
        if (!listsEveryMemberUnder(m_program, holder, m_call.key) || m_builtins.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return false;
    }
}

bool CallInLinkCalls::isNameUndefinedFrom(BuiltinID firstHolder)
{
    if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(m_call.key); added && (!watch(added, 0) || m_flow.testRuns().isNameAdded(added)))
        return false;
    for (BuiltinID holder = m_builtins.canonicalID(firstHolder); ; holder = m_builtins.canonicalID(m_builtins.at(holder).inheritsFrom)) {
        if (compiledModuleWithObject(m_program, holder) || m_builtins.at(m_builtins.rootOf(holder)).scope != BuiltinScope::Realm)
            return false;
        if (membersUnderKey().findOwnMember(holder) || !listsEveryMemberUnder(m_program, holder, m_call.key) || !m_builtins.hasLockedPrototypeLink(holder))
            return false;
        bool isClosed = membersUnderKey().isClosedKey(holder) || m_flow.stillNothing(holder, m_call.key, [&](FlowNodeID laid) {
            return watch(laid, 0);
        });
        if (!isClosed)
            return false;
        if (m_builtins.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return m_builtins.at(holder).prototypeLink == BuiltinPrototypeLink::Null;
    }
}

bool CallInLinkCalls::isNameUndefinedOn(BuiltinID holder)
{
    const auto& holding = m_builtins.at(m_builtins.canonicalID(holder));
    if (holding.prototypeLink == BuiltinPrototypeLink::Builtin && m_builtins.at(m_builtins.canonicalID(holding.inheritsFrom)).prototypeLink == BuiltinPrototypeLink::Builtin)
        return false;
    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    if (m_flow.isGlobalObject(m_builtins.canonicalID(holder)) || m_program.at(m_call.key).kind != PropertyKeyKind::Text || mayBeElementKey(nameClass) || nameClass == NameClass::Proto)
        return false;
    return isNameUndefinedFrom(holder);
}

bool CallInLinkCalls::isNameUndefinedAbove(BuiltinClassID builtinClass)
{
    BuiltinName name = m_program.builtinNameOf(m_call.key);
    if (!builtinClass || m_program.at(m_call.key).kind != PropertyKeyKind::Text || mayBeElementKey(m_flow.nameClassOf(m_call.key)) || m_flow.nameClassOf(m_call.key) == NameClass::Proto || m_builtins.mayDefineOnNewInstances(builtinClass, name))
        return false;
    bool isRedefinedBelow = name && std::ranges::any_of(m_builtins.classesWithInstanceMember(name), [&](BuiltinClassID other) {
        return m_builtins.isSameOrSubclassOf(other, builtinClass);
    });
    if (isRedefinedBelow)
        return false;
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (BuiltinID prototype = m_builtins.at(current).prototype)
            return isNameUndefinedFrom(prototype);
    }
    return false;
}

std::optional<CallInLinkCalls::HoldersPassed> CallInLinkCalls::passHoldersUpToMemberFrom(BuiltinID firstHolder)
{
    BuiltinKind kindOfFirstHolder = m_builtins.at(m_builtins.canonicalID(firstHolder)).kind;
    if (kindOfFirstHolder == BuiltinKind::Function || kindOfFirstHolder == BuiltinKind::Method || kindOfFirstHolder == BuiltinKind::Constructor || kindOfFirstHolder == BuiltinKind::Prototype)
        return std::nullopt;
    HoldersPassed passed;
    for (BuiltinID holder = m_builtins.canonicalID(firstHolder); ; holder = m_builtins.canonicalID(m_builtins.at(holder).inheritsFrom)) {
        if (membersUnderKey().findOwnMember(holder) && membersUnderKey().isLockedLocation(holder))
            return passed;
        std::optional<LinkFlow::StillNothing> nothingIsLaid = m_flow.stillNothing(holder, m_call.key, [&](FlowNodeID laid) {
            return watch(laid, 0);
        });
        if (!nothingIsLaid)
            return std::nullopt;
        passed.append(*nothingIsLaid);
        if (membersUnderKey().findOwnMember(holder))
            return passed;
        if (m_builtins.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return std::nullopt;
    }
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findOnInstancesOf(BuiltinClassID builtinClass, PropertyKeyID key)
{
    if (!builtinClass)
        return { };
    return findOnce(1ull << 62 | static_cast<uint64_t>(builtinClass.index()) << 32 | key.index(), [&] {
        return key == m_call.key ? findOnInstancesOf(builtinClass, key, membersUnderKey()) : findOnInstancesOf(builtinClass, key, MembersUnderKey(m_program, key));
    });
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findOnObjectsOfClassItself(BuiltinClassID builtinClass)
{
    return findOnObjectsOfClassItself(builtinClass, m_call.key);
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findOnObjectsOfClassItself(BuiltinClassID builtinClass, PropertyKeyID key)
{
    return findOnce(1ull << 61 | static_cast<uint64_t>(builtinClass.index()) << 32 | key.index(), [&]() -> MemberOfTable {
        if (m_builtins.mayDefineOnNewInstances(builtinClass, m_program.builtinNameOf(key)))
            return { };
        for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
            if (BuiltinID prototype = m_builtins.at(current).prototype)
                return key == m_call.key ? findAbove(prototype, key, membersUnderKey()) : findAbove(prototype, key, MembersUnderKey(m_program, key));
        }
        return { };
    });
}

bool CallInLinkCalls::isOfClassItself(const LinkType& instances, uint8_t distance)
{
    FlowNodeID mark = instances.classes == LinkType::Classes::Site && m_flow.isOfClassItself(instances.site) ? m_flow.contentsOf(instances, SlotOfContents::mayBeOfClassBelow()) : FlowNodeID();
    return mark && watch(mark, distance) && heldAt(mark).isNever();
}

bool CallInLinkCalls::areListsThatOnlyProgramNames(const LinkType& instances)
{
    if (instances.classes != LinkType::Classes::Site || instances.builtinClass != BuiltinClasses::Array || !isOfClassItself(instances, 1))
        return false;
    FlowNodeID mark = m_flow.contentsOf(instances, SlotOfContents::mayHaveNamesOfEngine());
    return mark && watch(mark, 1) && heldAt(mark).isNever() && m_flow.isCreatedWithoutEngineNames(instances.site);
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findAbove(BuiltinID firstHolder, PropertyKeyID key, const MembersUnderKey& members, std::span<const LinkFlow::StillNothing> forEachHolderPassed)
{
    BuiltinID holder = m_builtins.canonicalID(firstHolder);
    while (true) {
        if (compiledModuleWithObject(m_program, holder) || m_builtins.at(m_builtins.rootOf(holder)).scope != BuiltinScope::Realm)
            return { };
        bool nothingIsLaidHere = std::ranges::any_of(forEachHolderPassed, [&](const LinkFlow::StillNothing& passed) {
            return passed.isOf(holder, key);
        });
        if (BuiltinID member = members.findOwnMember(holder)) {
            if (!members.isLockedLocation(holder))
                return { Lookup::FoundWhereProgramMayWrite, member };
            return { Lookup::Found, m_builtins.canonicalID(member) };
        }
        if (!listsEveryMemberUnder(m_program, holder, key))
            return { };
        if (!nothingIsLaidHere && (!members.isClosedKey(holder) || !m_builtins.hasLockedPrototypeLink(holder)))
            return { };
        const Builtin& object = m_builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return { };
        case BuiltinPrototypeLink::Null:
            if (nothingIsLaidHere)
                return { };
            return { Lookup::Absent, { } };
        case BuiltinPrototypeLink::Builtin:
            holder = m_builtins.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findOnInstancesOf(BuiltinClassID builtinClass, PropertyKeyID key, const MembersUnderKey& members)
{
    if (!builtinClass)
        return { };
    BuiltinName name = m_program.builtinNameOf(key);
    bool isRedefinedBelow = name && std::ranges::any_of(m_builtins.classesWithInstanceMember(name), [&](BuiltinClassID other) {
        return other != builtinClass && m_builtins.isSameOrSubclassOf(other, builtinClass);
    });
    if (isRedefinedBelow || m_builtins.mayDefineOnNewInstances(builtinClass, name))
        return { };
    for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent) {
        if (BuiltinID prototype = m_builtins.at(current).prototype)
            return findAbove(prototype, key, members);
    }
    return { };
}

void CallInLinkCalls::enter(FunctionID callee)
{
    if (m_call.callee == SummaryCall::Callee::Binding)
        enterByBinding(callee);
    else
        enter(callee, passedByCall());
}

void CallInLinkCalls::noteCustomInstanceOf()
{
    if (m_shared || !m_call.isMadeByProtocol || m_call.key != m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::HasInstance)))
        return;
    const LinkFlow::InstanceOfGuardedReads* reads = m_flow.instanceOfGuardedReads(m_caller);
    if (!reads)
        return;
    for (auto [test, access] : std::ranges::equal_range(reads->accessesByTest, m_call.site, { }, &std::pair<ValueID, uint32_t>::first))
        m_flow.arrive(nodeAt(m_summary.accesses[access].value), primitivesForLinkCalls(Primitive::Undefined));
}

void CallInLinkCalls::enter(FunctionID callee, const Passed& passed)
{
    noteCustomInstanceOf();
    if (lists(callee) && callees().builtins().isEmpty() && !callees().mayEnterUnseen && !passed.newTarget) {
        std::optional<unsigned> thisParameter = m_program.at(callee).thisParameterIndex();
        if (!passed.isConstruction && passed.receiver && thisParameter)
            flowInto(*passed.receiver, m_flow.parameter(callee, *thisParameter));
        return;
    }

    if (!listsReturnsOf(callee, passed))
        followWholeResults(callee);
    bool isNewCallee = !lists(callee);
    Values constructed;
    switch (passTo(callee, passed, constructed)) {
    case Entry::Refused:
        return;
    case Entry::Unseen:
        give(valuesOf(LinkType::anything()));
        return;
    case Entry::Body:
        break;
    }
    if (const LinkFlow::PrototypeHelper* helper = findFollowedHelper(callee, passed); helper && helper->result == LinkFlow::PrototypeHelper::Result::IsParameter) {
        for (const ResultInLinkCalls& result : results())
            flowInto(passed.arguments[helper->returnedParameter - m_program.at(callee).firstParameterIndex()], result.node);
        return;
    }
    if (listsReturnsOf(callee, passed)) {
        if (isNewCallee)
            m_flow.build.statistics().count(StatisticID::CallsWithSeparateReturns);
        flowReturnsOf(callee, passed);
        return;
    }
    for (const ResultInLinkCalls& result : results())
        flowInto(resultOf(callee, result.index, passed, constructed, notObservedForLinkCalls), result.node);
}

bool CallInLinkCalls::listsReturnsOf(FunctionID callee, const Passed& passed)
{
    return m_flow.listsReturns(callee) && !m_shared && !m_stepThatWatches && !callees().followsWholeResults && !passed.isConstruction && !passed.list && !passed.newTarget && passed.arguments.data() == passedByCall().arguments.data()
        && std::ranges::all_of(results(), [](const ResultInLinkCalls& result) { return !result.index; });
}

void CallInLinkCalls::followWholeResults(FunctionID entered)
{
    if (m_shared || !m_flow.listsReturns(entered) || callees().followsWholeResults)
        return;
    callees().followsWholeResults = true;
    const Passed& passed = passedByCall();
    if (passed.isConstruction || passed.list || passed.newTarget)
        return;
    for (FunctionID callee : Vector<FunctionID, 4>(callees().listed().span())) {
        if (!m_flow.listsReturns(callee))
            continue;
        for (const ResultInLinkCalls& result : results())
            flowInto(resultOf(callee, result.index, passed, { }, notObservedForLinkCalls), result.node);
    }
}

const ValuesInLinkCalls* CallInLinkCalls::operandPassedTo(FunctionID callee, uint32_t parameter, const Passed& passed) const
{
    uint32_t position = m_call.callee == SummaryCall::Callee::Binding ? parameter : parameter - m_program.at(callee).firstParameterIndex();
    return position < passed.arguments.size() ? &passed.arguments[position] : nullptr;
}

LinkType CallInLinkCalls::operandLeftOut() const
{
    return m_call.callee == SummaryCall::Callee::Binding ? LinkType::anything() : primitivesForLinkCalls(Primitive::Undefined);
}

bool CallInLinkCalls::mayPassTests(FunctionID callee, uint32_t parameter, std::span<const SummaryParameterTest> tests, const Passed& passed)
{
    auto mayPass = [&](LinkType type) {
        for (const SummaryParameterTest& test : tests) {
            if (test.parameter == parameter && !type.isNever())
                type = m_flow.passedByTest(type, test);
        }
        return !type.isNever();
    };
    const Values* operand = operandPassedTo(callee, parameter, passed);
    if (!operand)
        return mayPass(operandLeftOut());
    if (mayPass(operand->made))
        return true;
    for (const Source& source : operand->sources) {
        if (!watchTestedOperand(source) || (!m_isEnteredOnReach && mayPass(m_flow.passedBy(heldAt(source.node), source.passes))))
            return true;
    }
    return false;
}

bool CallInLinkCalls::watchTestedOperand(const Source& source)
{
    if (m_call.callee != SummaryCall::Callee::Binding)
        return watch(source.node, source.distance);
    if (source.node == m_flow.unseen())
        return false;
    StateOfLinkCalls::FirstObservedNodes& observed = m_state.firstObserved[m_number];
    if (observed.first == source.node || observed.second == source.node)
        return true;
    if (observed.second)
        return false;
    (observed.first ? observed.second : observed.first) = source.node;
    m_flow.observe(source.node, FlowObserver { FlowObserver::Kind::SubjectOfCall, m_caller, m_index });
    return true;
}

void CallInLinkCalls::flowReturnsOf(FunctionID callee, const Passed& passed)
{
    const ThinSummary& summary = m_flow.summaries.at(callee);
    const Function& function = m_program.at(callee);
    uint32_t firstPositional = function.firstParameterIndex();
    for (const SummaryReturn& returned : summary.returnStatements) {
        std::span<const SummaryParameterTest> tests = summary.testsOf(returned);
        bool mayBeReached = std::ranges::all_of(tests, [&](const SummaryParameterTest& test) {
            return mayPassTests(callee, test.parameter, tests, passed);
        });
        if (!mayBeReached) {
            m_state.callsWithDroppedReturns[m_number] = 1;
            continue;
        }
        for (const SummaryEdge& edge : std::ranges::equal_range(summary.edges.span(), returned.node, { }, &SummaryEdge::to)) {
            const SummaryNode& from = summary.nodes[edge.from];
            bool isOperand = from.kind == SummaryNode::Kind::Parameter && from.first >= firstPositional && from.first < firstPositional + function.numberOfPositionalParameters();
            Values values;
            if (!isOperand)
                add(values, m_flow.nodeOf(callee, edge.from), notObservedForLinkCalls, edge.passes);
            else if (const Values* operand = operandPassedTo(callee, from.first, passed)) {
                for (const Source& source : operand->sources)
                    add(values, source.node, notObservedForLinkCalls, edge.passes ? edge.passes : source.passes);
                add(values, edge.passes ? m_flow.passedBy(operand->made, edge.passes) : operand->made);
            } else
                add(values, m_flow.passedBy(operandLeftOut(), edge.passes));
            for (const ResultInLinkCalls& result : results())
                flowInto(values, result.node);
        }
    }
}

static bool bodyOnlyNamesParameterInLinkCalls(const LinkFlow& flow, FunctionID callee, uint32_t parameter)
{
    const BodyIndex* index = bodiesLookedAt(flow.looks).find(callee);
    ValueID passed(parameter);
    if (!index || parameter >= index->numberOfValues() || !index->isParameter(passed))
        return false;
    ValueID site = index->onlyUsingValue(passed);
    if (!site || !index->isUnused(site))
        return false;
    const ThinSummary& summary = flow.summaries.at(callee);
    if (summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval) || flow.program.at(callee).isCoroutine())
        return false;
    for (const SummaryChange& change : summary.changes) {
        if (change.site != site)
            continue;
        return change.kind == SummaryChange::Kind::DefinesByDescriptor && change.descriptorLaysNoAccessor && change.key
            && summary.nodes[change.subject] == SummaryNode { SummaryNode::Kind::Parameter, parameter, 0 }
            && (change.key == flow.program.keyOf(BuiltinNames::name) || change.key == flow.program.keyOf(BuiltinNames::length));
    }
    return false;
}

void CallInLinkCalls::passOn(const Values& values, FunctionID callee, uint32_t parameter)
{
    FlowNodeID to = m_flow.parameter(callee, parameter);
    if (!bodyOnlyNamesParameterInLinkCalls(m_flow, callee, parameter)) {
        flowInto(values, to);
        return;
    }
    for (const Source& source : values.sources) {
        if (source.passes)
            m_flow.addEdge(source.node, to, source.passes);
        else
            m_flow.observe(source.node, FlowObserver { FlowObserver::Kind::PassedToBeNamed, callee, parameter });
    }
    flowInto(valuesOf(values.made), to);
}

void CallInLinkCalls::enterByBinding(FunctionID callee)
{
    RELEASE_ASSERT(!m_call.isConstruction);
    if (lists(callee))
        return;
    list(callee);

    const Function& function = m_program.at(callee);
    if (!function.hasBody()) {
        callUnseen();
        m_flow.reach(callee);
        return;
    }

    uint32_t numberOfParameters = function.numberOfEntryParameters();
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        if (parameter < m_call.numberOfArguments)
            passOn(valuesAtNode(m_summary.arguments[m_call.firstArgument + parameter]), callee, parameter);
        else
            m_flow.arrive(m_flow.parameter(callee, parameter), LinkType::anything());
    }
    for (uint32_t argument = numberOfParameters; argument < m_call.numberOfArguments; ++argument)
        escape(valuesAtNode(m_summary.arguments[m_call.firstArgument + argument]));
    if (m_flow.summaries.at(callee).has(SummaryTrait::OnlyPassesOnGatheredArguments))
        m_flow.gatherWhatListsThatArriveHold(callee);

    if (listsReturnsOf(callee, passedByCall())) {
        m_flow.build.statistics().count(StatisticID::CallsWithSeparateReturns);
        SetForScope isEnteredOnReach(m_isEnteredOnReach, true);
        flowReturnsOf(callee, passedByCall());
    } else {
        for (const ResultInLinkCalls& result : results())
            flowInto(resultOf(callee, result.index, passedByCall(), { }, notObservedForLinkCalls), result.node);
    }

    m_flow.addEdge(m_flow.thrown(callee), thrownNode());
    m_flow.reach(callee);
}

bool CallInLinkCalls::handsOnOwnNewTarget(const Passed& passed)
{
    const Function& caller = m_program.at(m_caller);
    std::optional<unsigned> ownNewTarget = caller.newTargetParameterIndex();
    if (m_shared || &passed != &passedByCall() || caller.kind != FunctionKind::DerivedConstructor || !caller.constructedClass || !ownNewTarget || m_call.newTarget == noSummaryNode)
        return false;
    return m_summary.nodes[m_call.newTarget] == SummaryNode { SummaryNode::Kind::Parameter, *ownNewTarget, 0 };
}

bool CallInLinkCalls::handsNewTargetUpRowOfClasses(FunctionID callee, const Passed& passed)
{
    if (!handsOnOwnNewTarget(passed))
        return false;
    LayoutID base = m_program.at(m_program.at(m_caller).constructedClass).base;
    return base && m_program.at(base).constructor == callee;
}

const LinkFlow::PrototypeHelper* CallInLinkCalls::findFollowedHelper(FunctionID callee, const Passed& passed)
{
    if (!m_flow.mayBePrototypeHelper(callee) || m_shared || passed.isConstruction || passed.list || passed.newTarget || passed.arguments.data() != passedByCall().arguments.data())
        return nullptr;
    const LinkFlow::PrototypeHelper* helper = m_flow.findPrototypeHelper(callee);
    if (!helper || !helper->linksOperands || (helper->result == LinkFlow::PrototypeHelper::Result::IsUnknown && !results().isEmpty()))
        return nullptr;
    uint32_t firstPositional = m_program.at(callee).firstParameterIndex();
    for (uint32_t parameter : helper->parameters) {
        uint32_t position = parameter - firstPositional;
        if (position >= passed.arguments.size())
            return nullptr;
        const Values& argument = passed.arguments[position];
        if (argument.sources.size() != 1 || argument.sources[0].passes || !argument.made.isNever())
            return nullptr;
    }
    return helper;
}

void CallInLinkCalls::letHeldPrototypeEscape(FunctionID function)
{
    if (m_flow.mayHoldOtherClassPrototype(function))
        m_flow.addEdge(m_flow.laidOnClosuresOf(function, m_program.keyOf(BuiltinNames::prototype)), m_flow.unseen());
}

CallInLinkCalls::Entry CallInLinkCalls::passTo(FunctionID callee, const Passed& passed, Values& constructed)
{
    noteArgumentsEscape(passed);
    const Function& function = m_program.at(callee);
    bool isAccessor = function.kind == FunctionKind::Getter || function.kind == FunctionKind::Setter;
    if (!isAccessor && !isEnteredByCallsAloneForLinkCalls(function.kind)) {
        m_flow.escape(callee);
        passToUnseen(passed);
        return Entry::Unseen;
    }
    if (passed.isConstruction ? function.constructorForm() == ConstructorForm::NotConstructor : isClassConstructorOf(function.kind)) {
        throwError();
        return Entry::Refused;
    }

    list(callee);
    if (!m_shared && m_call.continuesConstruction && callee != parentConstructorOf(m_program, m_caller))
        m_flow.noteConstructionLeavesParent(m_caller);
    if (!function.hasBody()) {
        passToUnseen(passed);
        m_flow.reach(callee);
        return Entry::Unseen;
    }
    m_flow.reach(callee);
    if (passed.isConstruction) {
        letHeldPrototypeEscape(callee);
        if (passed.newTarget) {
            forEachType(*passed.newTarget, [&](const LinkType& newTarget, const Source*) {
                if (newTarget.functions == LinkType::Functions::Closure)
                    letHeldPrototypeEscape(newTarget.function);
                else if (newTarget.functions == LinkType::Functions::Set) {
                    for (FunctionID member : m_flow.functionsOfSet(newTarget))
                        letHeldPrototypeEscape(member);
                }
            });
        }
        noteWhatIsMadeThrough(callee, passed);
        constructed = objectsConstructed(passed, callee);
    }

    LinkType undefined = primitivesForLinkCalls(Primitive::Undefined);
    uint32_t numberOfParameters = function.numberOfEntryParameters();
    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t numberOfPositional = function.numberOfPositionalParameters();
    uint32_t numberWritten = static_cast<uint32_t>(passed.arguments.size());
    bool passesOnWhatCallerGathered = !m_shared && !numberWritten && (passed.listIsGathered || (m_call.passesOnGatheredArguments && &passed == &passedByCall()));
    const LinkFlow::PrototypeHelper* helper = findFollowedHelper(callee, passed);
    for (uint32_t parameter = static_cast<uint32_t>(function.captures.size()); parameter < numberOfParameters; ++parameter) {
        FlowNodeID to = m_flow.parameter(callee, parameter);
        switch (function.entryParameterKind(parameter)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            if (passed.isConstruction && constructed.isEmpty())
                m_flow.arrive(to, objectsThatNoNodeHolds(function.constructedClass));
            else if (passed.isConstruction)
                flowInto(constructed, to);
            else if (passed.receiver)
                flowInto(*passed.receiver, to);
            else
                m_flow.arrive(to, undefined);
            break;
        case EntryParameter::Callee:
            m_flow.arrive(to, closuresForLinkCalls(callee));
            break;
        case EntryParameter::NewTarget:
            if (!passed.isConstruction)
                m_flow.arrive(to, undefined);
            else if (passed.newTarget)
                flowInto(*passed.newTarget, to);
            else
                m_flow.arrive(to, closuresForLinkCalls(callee));
            break;
        case EntryParameter::ArgumentCount:
            m_flow.arrive(to, primitivesForLinkCalls(Primitive::Integer));
            break;
        case EntryParameter::Parameter: {
            uint32_t position = parameter - firstPositional;
            if (position >= numberOfPositional)
                break;
            if (helper && helper->parameters.contains(parameter)) {
                m_flow.arrive(to, LinkType::anything());
                break;
            }
            if (position < passed.arguments.size()) {
                passOn(passed.arguments[position], callee, parameter);
                break;
            }
            if (passesOnWhatCallerGathered) {
                m_flow.passOnGatheredArgument(m_caller, position, to);
                break;
            }
            if (passed.list)
                flowInto(*passed.list, to);
            m_flow.arrive(to, undefined);
            break;
        }
        case EntryParameter::HiddenRest:
            break;
        }
    }

    if (helper) {
        Vector<FlowNodeID, 4> arguments;
        arguments.fill(m_flow.unseen(), numberOfParameters);
        for (uint32_t parameter : helper->parameters)
            arguments[parameter] = passed.arguments[parameter - firstPositional].sources[0].node;
        m_flow.followPrototypeHelperCall(m_number, callee, WTF::move(arguments));
    }

    bool gathersInList = function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    bool usesArguments = m_flow.summaries.at(callee).has(SummaryTrait::UsesArguments);
    auto gather = [&](const Values& argument, bool isTakenByParameter) {
        if (isTakenByParameter)
            return;
        if (gathersInList)
            storeInContents(m_flow.listOfArgumentsOf(callee), ContentsSlot::Element, argument);
        else if (usesArguments)
            escape(argument);
    };
    for (uint32_t position = 0; position < passed.arguments.size(); ++position)
        gather(passed.arguments[position], position < numberOfPositional);
    if (passed.list)
        gather(*passed.list, false);
    if (gathersInList && m_flow.summaries.at(callee).has(SummaryTrait::OnlyPassesOnGatheredArguments)) {
        for (uint32_t position = numberOfPositional; position < numberWritten; ++position)
            flowInto(passed.arguments[position], m_flow.argumentGatheredAt(callee, position - numberOfPositional));
        if (passesOnWhatCallerGathered)
            m_flow.passOnGatheredArguments(m_caller, numberOfPositional, callee);
        else {
            if (passed.list)
                flowInto(*passed.list, m_flow.argumentsGatheredAtAnyPlace(callee));
            m_flow.noteNumberOfArgumentsGathered(callee, numberWritten > numberOfPositional ? numberWritten - numberOfPositional : 0);
        }
    }

    m_flow.addEdge(m_flow.thrown(callee), thrownNode());
    return Entry::Body;
}

ValuesInLinkCalls CallInLinkCalls::objectsConstructed(const Passed& passed, FunctionID callee)
{
    Values constructed;
    const Function& function = m_program.at(callee);
    if (!function.takesThisFromConstruction())
        return constructed;

    auto addObjectMadeFor = [&](FunctionID newTarget) {
        const Function& constructor = m_program.at(newTarget);
        if (constructor.constructedClass)
            m_flow.noteConstructed(constructor.constructedClass);
        std::optional<unsigned> thisParameter = constructor.thisParameterIndex();
        if (constructor.takesThisFromConstruction() && constructor.constructedClass && thisParameter) {
            m_flow.reach(newTarget);
            bool makesMembersOfTaggedFamily = !std::ranges::equal_range(m_program.taggedFamilies, constructor.constructedClass, { }, &TaggedFamily::maker).empty();
            if (!m_program.layoutsExpectedBelow(constructor.constructedClass).empty() && !makesMembersOfTaggedFamily) {
                add(constructed, m_flow.objectsMadeForClassBelowConstructorFunction(constructor.constructedClass), notObservedForLinkCalls, TypeTable::object);
                return;
            }
            add(constructed, m_flow.parameter(newTarget, *thisParameter), notObservedForLinkCalls, TypeTable::object);
            return;
        }
        if (constructor.takesThisFromConstruction() && constructor.constructedClass && newTarget == callee) {
            m_flow.reach(newTarget);
            add(constructed, m_flow.objectsMadeForClassBelowConstructorFunction(constructor.constructedClass), notObservedForLinkCalls, TypeTable::object);
            return;
        }
        if (LayoutID entered = m_flow.layoutOfConstructorFunction(callee); entered && m_flow.classOfConstructor(newTarget) && std::ranges::find(m_program.layoutsExpectedBelow(entered), constructor.constructedClass) != m_program.layoutsExpectedBelow(entered).end()) {
            add(constructed, m_flow.objectsMadeForClassBelowConstructorFunction(constructor.constructedClass), notObservedForLinkCalls, TypeTable::object);
            return;
        }
        const LinkFlow::PrototypeHelper* helper = m_flow.mayBePrototypeHelper(m_caller) ? m_flow.findPrototypeHelper(m_caller) : nullptr;
        bool isSurrogate = helper && helper->surrogates.contains(newTarget);
        if (isSurrogate && thisParameter)
            m_flow.arrive(m_flow.parameter(newTarget, *thisParameter), unseenObjectsForLinkCalls());
        if (!constructor.constructedClass && !isSurrogate)
            m_flow.escape(newTarget);
        add(constructed, objectsThatNoNodeHolds(constructor.constructedClass));
    };

    if (!passed.newTarget) {
        addObjectMadeFor(callee);
        return constructed;
    }
    forEachType(*passed.newTarget, [&](const LinkType& newTarget, const Source* source) {
        switch (newTarget.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            addObjectMadeFor(newTarget.function);
            break;
        case LinkType::Functions::Constructors:
            forEachConstructorOf(newTarget.constructorsOf, addObjectMadeFor);
            break;
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            escape(newTarget, source);
            add(constructed, unseenObjectsForLinkCalls());
            break;
        }
        if (newTarget.hasUnknownObjects || newTarget.builtin || newTarget.objects != LinkType::Objects::None || newTarget.classes != LinkType::Classes::None)
            add(constructed, unseenObjectsForLinkCalls());
    });
    return constructed;
}

LinkType CallInLinkCalls::objectsThatNoNodeHolds(LayoutID layout)
{
    LinkType objects = unseenObjectsForLinkCalls();
    if (!layout)
        return objects;
    m_flow.noteAllocated(layout);
    m_flow.escape(layout);
    objects.objects = LinkType::Objects::Layout;
    objects.layout = layout;
    return objects;
}

void CallInLinkCalls::noteWhatIsMadeThrough(FunctionID callee, const Passed& passed)
{
    const Function& function = m_program.at(callee);
    bool isConstructorOfClass = isClassConstructorOf(function.kind) && function.constructedClass;
    if (!passed.newTarget) {
        if (isConstructorOfClass)
            m_flow.addEdge(m_flow.objectsMadeForClassBelowConstructorFunction(function.constructedClass), m_flow.objectsMadeThroughConstructorOf(function.constructedClass));
        else if (function.constructedClass)
            m_flow.noteAllocated(function.constructedClass);
    } else if (!handsNewTargetUpRowOfClasses(callee, passed)) {
        escape(*passed.newTarget);
        m_flow.noteAllocatedWithRowAbove(function.constructedClass);
    } else if (isConstructorOfClass)
        m_flow.addEdge(m_flow.objectsMadeThroughConstructorOf(m_program.at(m_caller).constructedClass), m_flow.objectsMadeThroughConstructorOf(function.constructedClass));
}

auto CallInLinkCalls::classifyNewTargets(LayoutID above, const Values& newTargets) -> NewTargets
{
    NewTargets named = NewTargets::OwnClasses;
    auto add = [&](NewTargets added) {
        named = std::max(named, added);
    };
    forEachType(newTargets, [&](const LinkType& newTarget, const Source*) {
        switch (newTarget.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure: {
            const Function& constructor = m_program.at(newTarget.function);
            if (!isClassConstructorOf(constructor.kind) || !constructor.constructedClass || !m_flow.standsAtOrBelow(constructor.constructedClass, above))
                add(NewTargets::OtherClasses);
            break;
        }
        case LinkType::Functions::Constructors:
            if (!m_flow.everyMemberStandsAtOrBelow(newTarget.constructorsOf, above))
                add(NewTargets::OtherClasses);
            break;
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            add(NewTargets::OtherClasses);
            break;
        }
        if (newTarget.builtin || newTarget.objects != LinkType::Objects::None || newTarget.classes != LinkType::Classes::None)
            add(NewTargets::OtherClasses);
        else if (newTarget.hasUnknownObjects)
            add(newTarget.hasDirectUnknownObjects() ? NewTargets::OwnOrUnseenClasses : NewTargets::OwnOrEscapedClasses);
    });
    return named;
}

bool CallInLinkCalls::addObjectsMadeForNewTarget(Values& values, FunctionID callee, const Passed& passed, uint8_t distance)
{
    const Function& function = m_program.at(callee);
    if (!isClassConstructorOf(function.kind) || !function.constructedClass || m_flow.constructionMayAnswerWithOlderObject(function.constructedClass))
        return false;
    if (!passed.newTarget) {
        m_flow.noteConstructed(function.constructedClass);
        add(values, m_flow.objectsMadeForClassBelowConstructorFunction(function.constructedClass), notObservedForLinkCalls, TypeTable::object);
        return true;
    }
    if (!handsNewTargetUpRowOfClasses(callee, passed))
        return false;
    LayoutID constructed = m_program.at(m_caller).constructedClass;
    FunctionID constructorAtTop = m_program.at(m_flow.topOfRow(constructed)).constructor;
    NewTargets named = classifyNewTargets(constructed, *passed.newTarget);
    if (named != NewTargets::OwnClasses && !isClassConstructorOf(m_program.at(constructorAtTop).kind))
        return false;
    add(values, m_flow.objectsMadeThroughConstructorOf(constructed), notObservedForLinkCalls, TypeTable::object);
    switch (named) {
    case NewTargets::OwnClasses:
        break;
    case NewTargets::OwnOrEscapedClasses:
        add(values, unseenObjectsForLinkCalls().asEscapeOnly());
        break;
    case NewTargets::OwnOrUnseenClasses:
        add(values, unseenObjectsForLinkCalls());
        break;
    case NewTargets::OtherClasses:
        add(values, m_flow.resultToFollow(constructorAtTop, 0), distance);
        break;
    }
    return true;
}

ValuesInLinkCalls CallInLinkCalls::resultOf(FunctionID callee, uint32_t index, const Passed& passed, const Values& constructed, uint8_t distance)
{
    const Function& function = m_program.at(callee);
    Values values;
    if (index >= function.results.size()) {
        add(values, LinkType::anything());
        return values;
    }

    FlowNodeID result = m_flow.resultToFollow(callee, index);
    if (passed.isConstruction && !index && addObjectsMadeForNewTarget(values, callee, passed, distance))
        return values;
    if (!passed.isConstruction || !function.takesThisFromConstruction()) {
        add(values, result, distance);
        return values;
    }
    add(values, constructed);
    add(values, result, distance, TypeTable::object);
    return values;
}

ValuesInLinkCalls CallInLinkCalls::passToLostFunctions(const Passed& passed, const LinkType& callee, const Source* source)
{
    noteArgumentsEscape(passed);
    if (callee.functions == LinkType::Functions::Set)
        m_flow.loseFunctionsOfSet(callee);
    if (passed.list || passed.newTarget || passed.isConstruction) {
        LinkType functions;
        functions.functions = callee.functions;
        functions.function = callee.function;
        escape(functions, source);
    }
    if (passed.list || passed.newTarget)
        return passToUnseen(passed);

    callees().mayEnterLost = true;
    m_flow.noteCallOfLostFunctions(callee);
    if (passed.isConstruction)
        flowInto(valuesOf(unseenObjectsForLinkCalls()), m_flow.madeByLostFunctions(callee));
    else
        flowInto(passed.receiver ? *passed.receiver : valuesOf(primitivesForLinkCalls(Primitive::Undefined)), m_flow.receiverOfLostFunctions(callee));
    for (uint32_t position = 0; position < passed.arguments.size(); ++position)
        flowInto(passed.arguments[position], m_flow.parameterOfLostFunctions(callee, position));
    throwAnything();

    Values answers;
    add(answers, m_flow.resultOfLostFunctions(callee), beyond(source));
    if (passed.isConstruction)
        add(answers, unseenObjectsForLinkCalls());
    return answers;
}

ValuesInLinkCalls CallInLinkCalls::passToUnseen(const Passed& passed)
{
    noteArgumentsEscape(passed);
    callees().mayEnterUnseen = true;
    escape(passed);
    throwAnything();
    return valuesOf(LinkType::anything());
}

ValuesInLinkCalls CallInLinkCalls::callValues(const Values& calleeValues, const Passed& passed, bool mayGoByRows)
{
    Values answers;
    forEachType(calleeValues, [&](const LinkType& callee, const Source* source) {
        auto callClosure = [&](FunctionID function) {
            Values constructed;
            followWholeResults(function);
            switch (passTo(function, passed, constructed)) {
            case Entry::Refused:
                break;
            case Entry::Unseen:
                add(answers, LinkType::anything());
                break;
            case Entry::Body:
                if (!m_shared && m_call.continuesConstruction && passed.receiver && !m_flow.answersNothing(function) && m_flow.answersOwnThis(function)) {
                    add(answers, *passed.receiver);
                    add(answers, primitivesForLinkCalls(Primitive::Undefined));
                    break;
                }
                add(answers, resultOf(function, 0, passed, constructed, beyond(source)));
                break;
            }
        };
        switch (callee.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            callClosure(callee.function);
            break;
        case LinkType::Functions::Constructors:
            add(answers, callThroughSharedCall(SharedCall::Subjects::CalledClassObjects, callee.constructorsOf, passed));
            break;
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            add(answers, passToLostFunctions(passed, callee, source));
            break;
        }
        if (callee.builtin) {
            m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_builtins.canonicalID(callee.builtin).index());
            add(answers, callBuiltin(callee.builtin, passed, true, mayGoByRows));
        }
        if (callee.hasUnknownObjects || (callee.objects != LinkType::Objects::None && !holdsOnlyObjectsThatCannotBeCalledForLinkCalls(m_flow, callee)) || callee.classes != LinkType::Classes::None)
            add(answers, passToUnseen(passed));
    });
    return answers;
}

bool CallInLinkCalls::listenerMayReceivePromises(const RowCall& call)
{
    uint32_t placeOfPromise = 0;
    if (std::optional<StringID> event = textWrittenAs(call, BuiltinOperand::argument(0))) {
        std::span<const uint8_t> text = m_program.string(*event);
        auto is = [&](ASCIILiteral name) {
            return std::ranges::equal(text, byteCast<uint8_t>(name.span()));
        };
        if (is("unhandledRejection"_s))
            placeOfPromise = 1;
        else if (!is("rejectionHandled"_s))
            return false;
    }
    bool mayBeHanded = false;
    forEachType(valuesAt(call, BuiltinOperand::argument(1)), [&](const LinkType& type, const Source*) {
        LinkType others = type;
        others.primitives = 0;
        if (others.isNever())
            return;
        if (others.functions != LinkType::Functions::Closure || !(others == closuresForLinkCalls(others.function))) {
            mayBeHanded = true;
            return;
        }
        const Function& listener = m_program.at(others.function);
        mayBeHanded |= listener.numberOfPositionalParameters() > placeOfPromise || listener.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || m_flow.summaries.at(others.function).has(SummaryTrait::UsesArguments);
    });
    return mayBeHanded;
}

ValuesInLinkCalls CallInLinkCalls::callBuiltin(BuiltinID builtin, const Passed& passed, bool isCalledByBuiltin, bool mayGoByRows)
{
    RowCall call { .builtin = m_builtins.canonicalID(builtin), .mode = passed.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, .passed = passed };
    call.isCalledByBuiltin = isCalledByBuiltin;
    if (call.builtin != Builtins::FunctionPrototypeApply)
        noteArgumentsEscape(passed);
    if (m_builtins.has(BuiltinMark::MayLayListenerForUncaughtExceptions, call.builtin) && listenerMayReceivePromises(call))
        m_flow.noteListenerMayReceivePromises();
    call.isSeenByScan = !isCalledByBuiltin && m_call.callee == SummaryCall::Callee::Builtin;
    std::optional<SetForScope<Vector<FlowNodeID, 4>>> operandsOnlyLookedAt;
    uint8_t placesOnlyLookedAt = m_flow.everyMarkIsUsed() ? 0 : placesThatEveryRowOnlyLooksAt(call);
    Vector<FlowNodeID, 4> operandsOfThisCall = placesOnlyLookedAt ? operandsPassedAt(call, placesOnlyLookedAt) : Vector<FlowNodeID, 4>();
    if (!operandsOfThisCall.isEmpty() || !m_operandsOnlyLookedAt.isEmpty())
        operandsOnlyLookedAt.emplace(m_operandsOnlyLookedAt, WTF::move(operandsOfThisCall));
    m_flow.note(LinkFlow::Note::BuiltinCalled, call.builtin.index(), (!isCalledByBuiltin && (m_shared || !m_flow.rowsSpeakFor(m_caller)) ? LinkFlow::byCode : 0) | (passed.isConstruction ? LinkFlow::asConstructor : 0));
    if (m_state.rowsWhoseCallsAreListed.quickGet(call.builtin.index()))
        m_flow.note(LinkFlow::Note::BuiltinCalledAtSite, isCalledByBuiltin ? UINT32_MAX : m_number, call.builtin.index() | static_cast<uint32_t>(passed.isConstruction) << 23);
    notePreparationRegisteredBy(call.builtin, passed);
    noteOptionThatRunsCodeAmong(call);
    if (m_flow.testRuns().isTestRun() && m_builtins.has(BuiltinMark::IsSpy, call.builtin) && !passed.arguments.empty()) {
        forEachType(passed.arguments[0], [&](const LinkType& holder, const Source*) {
            m_flow.testRuns().noteHolderOfSpy(holder, { }, m_caller, m_call.site);
        });
    }
    auto holdsNothing = [&](const Values& values, bool isReceiver = false, bool isAssigned = false) {
        bool holdsSomething = false;
        forEachType(values, [&](const LinkType& type, const Source* source) {
            holdsSomething = true;
            auto letNamespaceGo = [&](LayoutID layout) {
                const Layout& record = m_program.at(layout);
                if (record.kind != LayoutKind::ModuleNamespace)
                    return;
                bool isIgnored = isReceiver && record.namespaceOf && m_program.at(record.namespaceOf).kind == ModuleKind::RuntimeSource && m_builtins.ignoresReceiver(call.builtin);
                bool isCopied = isAssigned && !m_flow.ownValuesOf(layout).empty();
                if (!isIgnored && !isCopied)
                    escape(objectsForLinkCalls(layout), source);
            };
            if (type.objects == LinkType::Objects::Layout)
                letNamespaceGo(type.layout);
            else if (type.objects == LinkType::Objects::Set)
                std::ranges::for_each(m_flow.layoutsOfSet(type), letNamespaceGo);
        });
        return !holdsSomething;
    };
    if (passed.receiver && holdsNothing(*passed.receiver, true))
        return { };
    for (uint32_t argument = 0; argument < passed.arguments.size(); ++argument) {
        bool isAssigned = argument && mayGoByRows && call.builtin == m_builtins.canonicalID(Builtins::ObjectAssign) && rowsSpeakOf(call);
        if (holdsNothing(passed.arguments[argument], false, isAssigned))
            return { };
    }
    if (!mayGoByRows || !rowsSpeakOf(call))
        return callOutOfSight(call);

    if (passed.isConstruction && passed.newTarget) {
        bool isOwnNewTarget = true;
        Values made;
        forEachType(*passed.newTarget, [&](const LinkType& newTarget, const Source*) {
            if (newTarget.builtin && newTarget == builtinForLinkCalls(newTarget.builtin) && m_builtins.canonicalID(newTarget.builtin) == call.builtin)
                return;
            isOwnNewTarget = false;
            auto addObjectMadeFor = [&](FunctionID constructor) {
                add(made, objectsThatNoNodeHolds(m_program.at(constructor).constructedClass));
            };
            switch (newTarget.functions) {
            case LinkType::Functions::None:
            case LinkType::Functions::Set:
            case LinkType::Functions::Lost:
                break;
            case LinkType::Functions::Closure:
                addObjectMadeFor(newTarget.function);
                break;
            case LinkType::Functions::Constructors:
                forEachConstructorOf(newTarget.constructorsOf, addObjectMadeFor);
                break;
            }
        });
        if (!isOwnNewTarget) {
            add(made, callOutOfSight(call));
            return made;
        }
    }

    bool hasRow = false;
    bool someRowMayTakeCall = false;
    Vector<const BuiltinOverload*, 4> refusing;
    Vector<Values, 4> remainingArguments;
    std::optional<Passed> remainingPassed;
    std::optional<RowCall> remainingCall;
    for (const BuiltinOverload& row : m_builtins.overloadsOf(call.builtin)) {
        if (row.mode != call.mode)
            continue;
        hasRow = true;
        RowCall& taken = remainingCall ? *remainingCall : call;
        Fit fit = fitOf(taken, row);
        if (!fit.mayBeInside || fit.mayBeOutside)
            describeWhatRowRefuses(taken, row);
        if (!fit.mayBeInside) {
            refusing.append(&row);
            taken.row = &row;
            enterCodeBehindRow(taken);
            continue;
        }
        if (rowSaysTooLittleForLinkCalls(row))
            return callOutOfSight(call);
        someRowMayTakeCall = true;
        taken.row = &row;
        taken.resultIsUnknown = row.hasTypedResult();
        if (m_builtins.at(call.builtin).overloadCount > 1)
            m_flow.note(LinkFlow::Note::RowApplied, m_builtins.at(call.builtin).firstOverload.index() + static_cast<uint32_t>(&row - m_builtins.overloadsOf(call.builtin).data()), call.builtin.index());
        if (fit.mayBeInsideInSight) [[likely]]
            applyRow(taken);
        else {
            SetForScope rowFitsByEscapeAlone(m_rowFitsByEscapeAlone, true);
            applyRow(taken);
        }
        enterCodeBehindRow(taken);
        if (!fit.mayBeOutside)
            return WTF::move(taken.result);
        if (std::optional<uint32_t> place = remainingCall ? std::nullopt : argumentWithAllObjectsTaken(call, row)) {
            for (const Values& argument : passed.arguments)
                remainingArguments.append(argument);
            remainingArguments[*place] = primitivesAmong(remainingArguments[*place]);
            remainingPassed = passed;
            remainingPassed->arguments = remainingArguments.span();
            remainingCall.emplace(RowCall { .builtin = call.builtin, .mode = call.mode, .passed = *remainingPassed, .isSeenByScan = call.isSeenByScan, .isCalledByBuiltin = call.isCalledByBuiltin });
            remainingCall->result = WTF::move(call.result);
        }
    }
    if (!hasRow)
        return callOutOfSight(call);
    if (remainingCall)
        call.result = WTF::move(remainingCall->result);
    if (!someRowMayTakeCall && m_builtins.at(call.builtin).origin == BuiltinOrigin::Host) {
        if (!passed.receiver || !m_builtins.ignoresReceiver(call.builtin)) {
            add(call.result, callOutOfSight(call));
            return WTF::move(call.result);
        }
        Passed handed = passed.withoutReceiver();
        RowCall withoutReceiver { call.builtin, call.mode, handed, call.isSeenByScan, call.isCalledByBuiltin };
        add(call.result, callOutOfSight(withoutReceiver));
        return WTF::move(call.result);
    }
    if (!refusing.isEmpty()) {
        RowCall beside = remainingCall ? *remainingCall : call;
        for (const BuiltinOverload* row : refusing)
            applyOperandsBesideRefused(beside, *row);
    }
    throwError();
    return WTF::move(call.result);
}

BuiltinClassID CallInLinkCalls::classBelowPrototype(BuiltinID canonicalPrototype)
{
    BuiltinID constructor = m_builtins.at(canonicalPrototype).kind == BuiltinKind::Prototype ? m_builtins.at(canonicalPrototype).holder : BuiltinID();
    BuiltinClassID made = constructor ? m_builtins.at(m_builtins.canonicalID(constructor)).instanceClass : BuiltinClassID();
    return made && m_flow.prototypeOfInstancesOf(made) == canonicalPrototype ? made : BuiltinClassID();
}

bool CallInLinkCalls::rowsMayBeTakenAtTheirWord(BuiltinID builtin)
{
    static constexpr uint8_t no = 1;
    static constexpr uint8_t yes = 2;
    if (uint8_t known = m_state.builtinsWhoseRowsAreTakenAtTheirWord.at(builtin.index()))
        return known == yes;
    uint8_t answer = no;
    auto write = makeScopeExit([&] {
        m_state.builtinsWhoseRowsAreTakenAtTheirWord.set(builtin.index(), answer);
    });

    switch (m_builtins.at(builtin).kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        break;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    if (m_program.isUnlocked(builtin) || m_builtins.letsInUnseenCode(builtin))
        return false;
    for (BuiltinMark mark : marksOfRowsThatSayTooLittleForLinkCalls) {
        if (m_builtins.has(mark, builtin))
            return false;
    }
    answer = yes;
    return true;
}

ALWAYS_INLINE void CallInLinkCalls::noteOptionThatRunsCodeAmong(const RowCall& call)
{
    BuiltinID builtin = call.builtin;
    const Passed& passed = call.passed;
    const BuiltinUnseenCode* row = m_builtins.unseenCodeOf(builtin);
    if (!row || !row->condition || !row->condition->hasProperty || isLibraryCode(m_program, m_program.at(m_caller)))
        return;
    PropertyKeyID option = m_program.keyOf(row->condition->hasProperty);
    if (!option)
        return;
    if (std::optional<bool> isWritten = literalMadeForCallHas(call, row->condition->operand, row->condition->hasProperty, option); isWritten && !*isWritten)
        return;
    auto lists = [&](LayoutID layout) {
        size_t stepsLeft = m_program.layouts.size();
        for (LayoutID current = layout; current && stepsLeft--; current = m_program.at(current).base) {
            if (m_program.at(current).findField(option))
                return true;
        }
        return false;
    };
    bool isPassed = false;
    auto look = [&](const Values& values) {
        forEachType(values, [&](const LinkType& type, const Source*) {
            switch (type.objects) {
            case LinkType::Objects::None:
            case LinkType::Objects::Prototype:
            case LinkType::Objects::Prototypes:
                return;
            case LinkType::Objects::Layout:
                isPassed |= lists(type.layout);
                return;
            case LinkType::Objects::Set:
                for (LayoutID member : m_flow.layoutsOfSet(type))
                    isPassed |= lists(member);
                return;
            case LinkType::Objects::Family:
                forEachLayoutOf(type.family, [&](LayoutID member) {
                    isPassed |= lists(member);
                });
                return;
            case LinkType::Objects::Lost:
                for (uint32_t index = 0; index < m_program.layouts.size() && !isPassed; ++index)
                    isPassed = !!m_program.at(LayoutID(index)).findField(option);
                return;
            }
        });
    };
    const BuiltinOperand& operand = row->condition->operand;
    if (operand.isReceiver()) {
        if (passed.receiver)
            look(*passed.receiver);
    } else {
        for (unsigned index = 0; index < passed.arguments.size(); ++index) {
            if (operand.coversArgument(index))
                look(passed.arguments[index]);
        }
    }
    if (isPassed)
        m_flow.note(LinkFlow::Note::OptionThatRunsCodePassed, builtin.index());
}

bool CallInLinkCalls::mayHaveProperty(const LinkType& type, const Source* source, PropertyKeyID key)
{
    if (type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.classes == LinkType::Classes::Mixed)
        return true;
    uint8_t distance = source ? source->distance : 0;
    if (key)
        return mayFind(type, key, distance, false, false);
    if (type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None)
        return true;
    auto mayHoldNamesNobodyKnows = [&](LayoutID layout) {
        FlowNodeID besideFields = m_flow.extraPropertyReadNode(layout);
        return m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype) || !watch(besideFields, static_cast<uint8_t>(distance + 1)) || !heldAt(besideFields).isNever();
    };
    switch (type.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
        return mayHoldNamesNobodyKnows(type.layout);
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(type), mayHoldNamesNobodyKnows);
    case LinkType::Objects::Family:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool asksForNameInLinkCalls(const BuiltinParameter& parameter, BuiltinName name)
{
    auto lists = [&](Protocol protocol, const BuiltinNameStretch& names) {
        return parameter.protocols.contains(protocol) && (names.empty() || std::ranges::contains(names, name));
    };
    return parameter.uses == OperandUses::all() || lists(Protocol::Named, parameter.namedMembers) || lists(Protocol::CallsNamed, parameter.calledMembers);
}

bool CallInLinkCalls::mayMeet(const BuiltinCondition& condition, const RowCall& call)
{
    const Passed& passed = call.passed;
    auto anyTypeAt = [&](BuiltinOperand operand, const Invocable<bool(const LinkType&, const Source*)> auto& holds) {
        bool found = false;
        auto look = [&](const Values& values) {
            forEachType(values, [&](const LinkType& type, const Source* source) {
                found = found || holds(type, source);
            });
        };
        if (operand.isReceiver()) {
            if (!passed.receiver)
                return true;
            look(*passed.receiver);
            return found;
        }
        if (passed.list)
            return true;
        for (unsigned index = 0; index < passed.arguments.size(); ++index) {
            if (operand.coversArgument(index))
                look(passed.arguments[index]);
        }
        return found;
    };

    if (condition.calledOn) {
        BuiltinID wanted = m_builtins.canonicalID(condition.calledOn);
        bool mayBeCalledOnIt = anyTypeAt(BuiltinOperand::receiver(), [&](const LinkType& type, const Source*) {
            return type.builtin == wanted || type.hasUnknownObjects || type.holdsLostBuiltins || type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None;
        });
        if (!mayBeCalledOnIt)
            return false;
    }
    if (condition.hasProperty) {
        const BuiltinParameter* asked = m_builtins.parameterFor(*call.row, condition.operand);
        if (!asked || !asksForNameInLinkCalls(*asked, condition.hasProperty))
            return false;
        PropertyKeyID key = m_program.keyOf(condition.hasProperty);
        if (std::optional<bool> has = literalMadeForCallHas(call, condition.operand, condition.hasProperty, key)) {
            if (!*has || condition.startsWith.empty())
                return *has;
            std::optional<StringID> written = textThatLiteralWritesUnder(call, condition.operand, key);
            if (!written)
                return true;
            std::span<const uint8_t> text = m_program.string(*written);
            return std::ranges::any_of(condition.startsWith, [&](BuiltinName listed) {
                std::span<const uint8_t> start = m_builtins.textOf(listed);
                return text.size() >= start.size() && std::ranges::equal(text.first(start.size()), start);
            });
        }
        return anyTypeAt(condition.operand, [&](const LinkType& type, const Source* source) {
            return mayHaveProperty(type, source, key);
        });
    }

    bool mayBecomeText = anyTypeAt(condition.operand, [&](const LinkType& type, const Source*) {
        return type != primitivesForLinkCalls(type.primitives & ~Primitive::AnyString);
    });
    if (!mayBecomeText)
        return false;
    std::optional<StringID> written = textWrittenAs(call, condition.operand);
    if (!written)
        return true;
    std::span<const uint8_t> text = m_program.string(*written);
    return std::ranges::any_of(condition.isOneOf, [&](BuiltinName listed) {
        return std::ranges::equal(text, m_builtins.textOf(listed));
    }) || std::ranges::any_of(condition.startsWith, [&](BuiltinName listed) {
        std::span<const uint8_t> start = m_builtins.textOf(listed);
        size_t matched = 0;
        for (uint8_t character : text) {
            if (matched == start.size())
                break;
            if (character == '\t' || character == '\n' || character == '\r' || (!matched && character <= ' '))
                continue;
            if (character != start[matched])
                return false;
            ++matched;
        }
        return matched == start.size();
    });
}

void CallInLinkCalls::noteKeysOfPrinterThatRegistryMayGive(const RowCall& call)
{
    std::optional<StringID> written = textWrittenAs(call, BuiltinOperand::argument(0));
    for (BuiltinName key : m_builtins.namesOfHostMethodsCallingStartupLoads()) {
        if (!written || std::ranges::equal(m_program.string(*written), m_builtins.textOf(key)))
            m_flow.note(LinkFlow::Note::OperandMayMeetCondition, call.builtin.index(), key.index());
    }
}

void CallInLinkCalls::noteTrapsOfHandler(const RowCall& call)
{
    m_flow.makersOfProxies.set(m_caller.index());
    Values handlers = valuesAt(call, BuiltinOperand::argument(1));
    for (BuiltinName trap : { BuiltinNames::get, BuiltinNames::set, BuiltinNames::has }) {
        forEachType(whatLiesUnder(trap, handlers), [&](const LinkType& type, const Source*) {
            if (type.functions == LinkType::Functions::Closure)
                m_flow.trapsOfProxies.set(type.function.index());
        });
    }
}

void CallInLinkCalls::noteConditionsThatOperandsMayMeet(const RowCall& call)
{
    Vector<const BuiltinCondition*, 4> asked;
    auto look = [&](const BuiltinCondition* condition) {
        if (!condition || !condition->nameToldUnder() || asked.contains(condition))
            return;
        asked.append(condition);
        if (mayMeet(*condition, call))
            m_flow.note(LinkFlow::Note::OperandMayMeetCondition, call.builtin.index(), condition->nameToldUnder().index());
    };
    for (const BuiltinCalledFunction& called : m_builtins.calledFunctionsOf(call.builtin))
        look(called.condition);
    for (const BuiltinModuleLoad& load : m_builtins.moduleLoadsOf(call.builtin))
        look(load.condition);
    for (const BuiltinFeatureUse& line : m_builtins.featureUsesOf(call.builtin))
        look(line.condition);
}

bool CallInLinkCalls::askOperandsOfCallOutOfSightForConditions(const RowCall& call)
{
    bool hasRow = false;
    for (const BuiltinOverload& row : m_builtins.overloadsOf(call.builtin)) {
        if (row.mode != call.mode)
            continue;
        RowCall byRow = call;
        byRow.row = &row;
        noteConditionsThatOperandsMayMeet(byRow);
        hasRow = true;
    }
    return hasRow;
}

ValuesInLinkCalls CallInLinkCalls::callOutOfSight(const RowCall& call)
{
    m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, call.builtin.index(), LinkFlow::withoutRow);
    if (m_state.builtinsWithCodeWaitingForOperand.quickGet(call.builtin.index()) && !askOperandsOfCallOutOfSightForConditions(call))
        m_flow.note(LinkFlow::Note::BuiltinCalledWithOperandsNobodyRead, call.builtin.index());
    return passToUnseen(call.passed);
}

void CallInLinkCalls::enterCodeBehindRow(RowCall& call)
{
    const RuntimeFunctionEntry* code = findRuntimeFunction(m_program, call.row->code.implementation.function);
    if (code && code->function)
        m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, call.builtin.index());
}

bool CallInLinkCalls::mayBePassedOptionThatRunsCode(const RowCall& call)
{
    const BuiltinUnseenCode* unseenCode = m_builtins.unseenCodeOf(call.builtin);
    if (!unseenCode || !unseenCode->condition)
        return true;
    RowCall byRow = call;
    return std::ranges::any_of(m_builtins.overloadsOf(call.builtin), [&](const BuiltinOverload& row) {
        byRow.row = &row;
        return row.mode == call.mode && mayMeet(*unseenCode->condition, byRow);
    });
}

ALWAYS_INLINE bool CallInLinkCalls::rowsSpeakOf(const RowCall& call)
{
    if (!rowsMayBeTakenAtTheirWord(call.builtin))
        return false;
    if (m_builtins.has(BuiltinMark::RunsCodeThatLocksDoNotBind, call.builtin) && mayBePassedOptionThatRunsCode(call))
        return false;
    const Builtin& entry = m_builtins.at(call.builtin);

    if (call.mode == BuiltinCallMode::Construct) {
        if (!entry.instanceClass)
            return true;
        const BuiltinClass& made = m_builtins.at(entry.instanceClass);
        BuiltinID adder = made.constructorAdder && made.prototype ? m_builtins.findOwnMember(made.prototype, made.constructorAdder) : BuiltinID();
        if (!adder || m_program.isUnlocked(m_builtins.canonicalID(adder)))
            return !adder;
        return !m_program.isLockLeftToProgram(adder) || (m_flow.takeForConstant(adder) && watch(m_flow.writtenAtMember(adder), 0));
    }

    if (!entry.flags.contains(BuiltinFlag::RequiresHolderAsThis))
        return true;
    LinkType expected = entry.holder ? builtinForLinkCalls(m_builtins.canonicalID(entry.holder)) : primitivesForLinkCalls(Primitive::Undefined);
    bool isCalledOnHolder = true;
    forEachType(valuesAt(call, BuiltinOperand::receiver()), [&](const LinkType& receiver, const Source*) {
        LinkType seen = receiver;
        if (seen.builtin)
            seen.builtin = m_builtins.canonicalID(seen.builtin);
        isCalledOnHolder &= seen == expected;
    });
    return isCalledOnHolder && (!entry.holder || membersOfHolderAreWhatTableLists(m_builtins.canonicalID(entry.holder)));
}

void CallInLinkCalls::describeWhatRowRefuses(const RowCall& call, const BuiltinOverload& row)
{
    Questions questions;
    questions.describeObjects = true;
    auto describe = [&](const BuiltinParameter& parameter) {
        if (!parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused) || !fitOf(call, parameter).mayBeOutside)
            return;
        throwAnything();
        if (m_builtins.isUnder(call.builtin, Builtins::NativeFunctions)) {
            callees().mayEnterUnseen = true;
            escape(valuesAt(call, parameter.operand));
            return;
        }
        questions.describesLiteralMadeForCall = isLiteralMadeForCall(call, parameter.operand);
        ask(valuesAt(call, parameter.operand), questions);
    };
    if (row.receiver)
        describe(*row.receiver);
    for (const BuiltinParameter& parameter : row.parameters)
        describe(parameter);
}

bool CallInLinkCalls::someRowInFrontTakesAllThatRowTakes(const RowCall& call, const BuiltinOverload& row)
{
    for (const BuiltinOverload& other : m_builtins.overloadsOf(call.builtin)) {
        if (&other == &row)
            return false;
        if (other.mode != row.mode || !other.receiver != !row.receiver || other.parameters.size() != row.parameters.size())
            continue;
        auto takesAllAt = [&](const BuiltinParameter& parameter) {
            const BuiltinParameter* its = m_builtins.parameterFor(other, parameter.operand);
            if (!its || !(its->operand == parameter.operand))
                return false;
            if (!its->accepted || its->conversion != BuiltinConversion::None || m_builtins.at(its->accepted).isAnything())
                return true;
            return (its->accepted == parameter.accepted && parameter.conversion == BuiltinConversion::None) || !fitOf(call, parameter).mayBeInside || !fitOf(call, *its).mayBeOutside;
        };
        if ((!row.receiver || takesAllAt(*row.receiver)) && std::ranges::all_of(row.parameters, takesAllAt))
            return true;
    }
    return false;
}

void CallInLinkCalls::applyOperandsBesideRefused(RowCall& call, const BuiltinOverload& row)
{
    auto mayRunCode = [&](const BuiltinParameter& parameter) {
        return (!parameter.protocols.isEmpty() || parameter.uses == OperandUses::all()) && fitOf(call, parameter).mayBeInside;
    };
    if (row.receiver && !fitOf(call, *row.receiver).mayBeInside)
        return;
    if (!(row.receiver && mayRunCode(*row.receiver)) && std::ranges::none_of(row.parameters, mayRunCode))
        return;
    if (someRowInFrontTakesAllThatRowTakes(call, row))
        return;
    call.row = &row;
    call.result = { };
    call.literalsTakenApart.clear();
    call.objectsTakenApart.clear();
    auto take = [&](const BuiltinParameter& parameter) {
        if (!mayRunCode(parameter))
            return;
        if (mayTakeApartLiteralAt(call, parameter))
            call.literalsTakenApart.append(parameter.operand);
        else if (mayTakeApartLiteralAt(call, parameter, true))
            call.objectsTakenApart.append(parameter.operand);
        else
            applyParameter(call, parameter);
    };
    if (row.receiver)
        take(*row.receiver);
    for (const BuiltinParameter& parameter : row.parameters)
        take(parameter);
    handOverLiteralsTakenApart(call);
}

std::optional<uint32_t> CallInLinkCalls::argumentWithAllObjectsTaken(const RowCall& call, const BuiltinOverload& row)
{
    if (call.passed.list || (row.receiver && fitOf(call, *row.receiver).mayBeOutside))
        return std::nullopt;
    std::optional<uint32_t> place;
    for (const BuiltinParameter& parameter : row.parameters) {
        if (!fitOf(call, parameter).mayBeOutside)
            continue;
        BuiltinOperand operand = parameter.operand;
        if (place || operand.kind() != BuiltinOperand::Kind::Argument || operand.argumentIndex() >= call.passed.arguments.size() || !parameter.accepted || parameter.conversion != BuiltinConversion::None)
            return std::nullopt;
        Fit fit;
        forEachType(valuesAt(call, operand), [&](const LinkType& type, const Source* source) {
            LinkType objects = type;
            objects.primitives = 0;
            objects.escapeOnlyPrimitives = 0;
            if (!objects.isNever())
                addFit(objects, source, m_builtins.at(parameter.accepted), fit);
        });
        if (!fit.mayBeInside || fit.mayBeOutside)
            return std::nullopt;
        place = operand.argumentIndex();
    }
    return place;
}

ValuesInLinkCalls CallInLinkCalls::primitivesAmong(const Values& values)
{
    Values primitives;
    for (Source source : values.sources) {
        if (source.passes == TypeTable::object)
            continue;
        add(primitives, source.node, source.distance, source.passes ? source.passes : typeOfPrimitives(Primitive::All));
    }
    add(primitives, primitivesForLinkCalls(values.made.primitives));
    return primitives;
}

FitInLinkCalls CallInLinkCalls::fitOf(const RowCall& call, const BuiltinOverload& row)
{
    Fit fit { true, false, true };
    auto take = [&](const BuiltinParameter& parameter) {
        Fit ofOperand = fitOf(call, parameter);
        fit.mayBeInside &= ofOperand.mayBeInside;
        fit.mayBeOutside |= ofOperand.mayBeOutside;
        fit.mayBeInsideInSight &= ofOperand.mayBeInsideInSight;
    };
    if (row.receiver)
        take(*row.receiver);
    for (const BuiltinParameter& parameter : row.parameters)
        take(parameter);
    return fit;
}

FitInLinkCalls CallInLinkCalls::fitOf(const RowCall& call, const BuiltinParameter& parameter)
{
    const BuiltinType* accepted = parameter.accepted && parameter.conversion == BuiltinConversion::None ? &m_builtins.at(parameter.accepted) : nullptr;
    BuiltinOperand operand = parameter.operand;
    Fit fit;

    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        break;
    case BuiltinOperand::Kind::Argument:
        if (operand.argumentIndex() < call.passed.arguments.size())
            break;
        fit.mayBeInside = true;
        fit.mayBeInsideInSight = true;
        fit.mayBeOutside = call.passed.list || (accepted && !accepted->isAnything() && fitOfOmittedOperandInBuiltinType(*accepted) != TypeFit::Inside);
        return fit;
    case BuiltinOperand::Kind::ArgumentsFrom:
        fit.mayBeInside = true;
        fit.mayBeInsideInSight = true;
        break;
    }

    if (accepted && !accepted->stringLiterals.empty() && !(primitivesOfBuiltinType(*accepted) & Primitive::String)) {
        if (std::optional<StringID> written = textWrittenAs(call, operand)) {
            std::span<const uint8_t> text = m_program.string(*written);
            bool isListed = std::ranges::any_of(accepted->stringLiterals, [&](BuiltinName listed) {
                return std::ranges::equal(text, m_builtins.textOf(listed));
            });
            return { isListed, !isListed, isListed };
        }
    }

    bool takesWhateverIsPassed = !accepted || parameter.conversion != BuiltinConversion::None;
    forEachType(valuesAt(call, operand), [&](const LinkType& type, const Source* source) {
        if (takesWhateverIsPassed) {
            fit.mayBeInside = true;
            fit.mayBeInsideInSight = true;
        } else if (TypeID written = accepted->flags.contains(BuiltinTypeFlag::HasOnlyIntegers) ? typeOfIntegerConstantAt(source) : TypeID()) {
            TypeFit fitOfInteger = fitOfPrimitiveInBuiltinType(*accepted, PrimitiveSet::Integer, integersInID(written));
            fit.mayBeInside |= fitOfInteger != TypeFit::Outside;
            fit.mayBeInsideInSight |= fitOfInteger != TypeFit::Outside;
            fit.mayBeOutside |= fitOfInteger != TypeFit::Inside;
        } else
            addFit(type, source, *accepted, fit);
    });
    return fit;
}

TypeID CallInLinkCalls::typeOfIntegerConstantAt(const Source* source) const
{
    if (!source || source->distance || source->passes || m_shared)
        return { };
    for (uint32_t argument = 0; argument < m_call.numberOfArguments; ++argument) {
        uint32_t inSummary = m_summary.arguments[m_call.firstArgument + argument];
        if (inSummary == noSummaryNode || m_summary.nodes[inSummary].kind != SummaryNode::Kind::Constant || nodeAt(inSummary) != source->node)
            continue;
        TypeID written = TypeID::fromBits(m_summary.nodes[inSummary].first);
        return holdsOnlyPrimitives(written) && primitivesInID(written) == Primitive::Integer ? written : TypeID();
    }
    return { };
}

bool CallInLinkCalls::membersAboveInstancesAreWhatTableLists(BuiltinClassID builtinClass)
{
    if (!m_program.admitsReplacedBuiltins)
        return true;
    BuiltinID holder;
    for (BuiltinClassID current = builtinClass; current && !holder; current = m_builtins.at(current).parent)
        holder = m_builtins.at(current).prototype;
    while (holder) {
        holder = m_builtins.canonicalID(holder);
        for (BuiltinID member : m_builtins.membersOf(holder)) {
            if (m_program.isUnlocked(member) || m_program.isUnlocked(m_builtins.canonicalID(member)))
                return false;
            if (m_program.isLockLeftToProgram(member) && (!m_flow.takeForConstant(member) || !watch(m_flow.writtenAtMember(member), 0)))
                return false;
        }
        const Builtin& object = m_builtins.at(holder);
        holder = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
    }
    return true;
}

bool CallInLinkCalls::membersOfHolderAreWhatTableLists(BuiltinID canonicalHolder)
{
    if (!m_program.admitsReplacedBuiltins)
        return true;
    for (BuiltinID member : m_builtins.membersOf(canonicalHolder)) {
        if (m_program.isUnlocked(member) || m_program.isUnlocked(m_builtins.canonicalID(member)))
            return false;
        if (m_program.isLockLeftToProgram(member) && (!m_flow.takeForConstant(member) || !watch(m_flow.writtenAtMember(member), 0)))
            return false;
    }
    BuiltinClassID made = m_builtins.at(canonicalHolder).instanceClass;
    return !made || membersAboveInstancesAreWhatTableLists(made);
}

bool CallInLinkCalls::isAsClassMakesIt(const LinkType& instances, PristineAspectSet aspects, const Source* source)
{
    if (aspects.contains(PristineAspects::Storage) && !indexesAreAbsentAbove(instances.builtinClass))
        return false;
    if (!membersAboveInstancesAreWhatTableLists(instances.builtinClass))
        return false;
    FlowNodeID underUnknownNames = m_flow.contentsOf(instances, SlotOfContents::underUnknownNames());
    return watch(underUnknownNames, beyond(source)) && heldAt(underUnknownNames).isNever();
}

static BuiltinClassID classAboveInstances(BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::Arguments)
        return BuiltinClasses::Object;
    return builtinClass;
}

bool CallInLinkCalls::indexesAreAbsentAbove(BuiltinClassID builtinClass)
{
    if (TypedArrayType view = m_builtins.typedArrayTypeOf(builtinClass); view != NotTypedArray && view != TypeDataView)
        return true;
    BuiltinID holder;
    for (BuiltinClassID current = classAboveInstances(builtinClass); current && !holder; current = m_builtins.at(current).parent)
        holder = m_builtins.at(current).prototype;
    if (!holder)
        return false;
    holder = m_builtins.canonicalID(holder);
    while (true) {
        if (!m_builtins.closesIndexKeys(holder) || !m_builtins.hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = m_builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_builtins.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

ALWAYS_INLINE void CallInLinkCalls::addFit(const LinkType& type, const Source* source, const BuiltinType& accepted, Fit& fit)
{
    if (accepted.isAnything()) {
        fit.mayBeInside = true;
        fit.mayBeInsideInSight = true;
        return;
    }
    auto note = [&](bool mayBeTaken, bool isSurelyTaken, bool mayBeTakenInSight) {
        fit.mayBeInside |= mayBeTaken;
        fit.mayBeOutside |= !isSurelyTaken;
        fit.mayBeInsideInSight |= mayBeTakenInSight;
    };

    if (type.primitives) {
        uint16_t surelyTaken = primitivesOfBuiltinType(accepted);
        uint16_t perhapsTaken = surelyTaken;
        if (surelyTaken & Primitive::String)
            surelyTaken |= Primitive::LiteralString;
        if (surelyTaken & Primitive::Symbol)
            surelyTaken |= Primitive::LiteralSymbol;
        perhapsTaken |= surelyTaken;
        if (!accepted.stringLiterals.empty())
            perhapsTaken |= Primitive::AnyString;
        if (accepted.flags.contains(BuiltinTypeFlag::HasOnlyIntegers)) {
            surelyTaken &= ~Primitive::Number;
            if (perhapsTaken & Primitive::Integer)
                perhapsTaken |= Primitive::WholeNumber;
        }
        if (!accepted.primitives.contains(BuiltinPrimitive::Undefined)) {
            surelyTaken &= ~Primitive::Undefined;
            perhapsTaken &= ~Primitive::Undefined;
        }
        note(type.primitives & LinkType::closed(perhapsTaken), !(type.primitives & ~LinkType::closed(surelyTaken)), type.directPrimitives() & LinkType::closed(perhapsTaken));
    }

    bool takesAnyObject = accepted.flags.contains(BuiltinTypeFlag::HasAnyObject);
    bool takesAnyFunction = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasAnyFunction);
    bool takesSomeObject = accepted.mayBeObject();
    auto somePartMayTake = [&](const Invocable<bool(BuiltinClassID)> auto& classMayTake) {
        return std::ranges::any_of(accepted.parts, [&](const BuiltinTypePart& part) {
            return part.kind != BuiltinTypePartKind::Instance || classMayTake(part.builtinClass);
        });
    };

    auto noteObjectsOf = [&](LayoutID layout) {
        BuiltinClassID base = m_program.at(layout).writtenBuiltinBase;
        bool mayBeTaken = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasPlainData) || somePartMayTake([&](BuiltinClassID taken) {
            return m_builtins.isSameOrSubclassOf(base ? base : BuiltinClassID(BuiltinClasses::Object), taken);
        });
        note(mayBeTaken, takesAnyObject, mayBeTaken);
    };
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        noteObjectsOf(type.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(type))
            noteObjectsOf(member);
        break;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        note(takesSomeObject, takesAnyObject, takesSomeObject);
        break;
    }

    if (type.functions != LinkType::Functions::None) {
        bool mayBeTaken = takesAnyFunction || somePartMayTake([&](BuiltinClassID taken) {
            if (type.functions == LinkType::Functions::Closure)
                return m_builtins.isSameOrSubclassOf(classOfClosuresForLinkCalls(m_program.at(type.function)), taken);
            return !m_builtins.at(taken).hasClosureInstances && (isFunctionClass(m_builtins, taken) || m_builtins.isSameOrSubclassOf(BuiltinClasses::Function, taken));
        });
        note(mayBeTaken, takesAnyFunction, mayBeTaken);
    }
    if (type.hasUnknownObjects)
        note(takesSomeObject, takesAnyObject, takesSomeObject && type.hasDirectUnknownObjects());
    if (type.builtin) {
        bool isNamed = std::ranges::any_of(accepted.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Builtin && m_builtins.canonicalID(part.builtin) == m_builtins.canonicalID(type.builtin);
        });
        bool isConstructor = m_builtins.at(m_builtins.canonicalID(type.builtin)).kind == BuiltinKind::Constructor;
        bool takesOnlyWhatProgramMakes = !takesAnyFunction && !accepted.flags.contains(BuiltinTypeFlag::HasPlainData) && !accepted.parts.empty() && std::ranges::all_of(accepted.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Instance && (m_builtins.at(part.builtinClass).hasClosureInstances || (isConstructor && part.builtinClass == BuiltinClasses::BoundFunction));
        });
        bool isAcceptedBuiltin = takesAnyFunction && isCallableBuiltin(m_builtins, m_builtins.canonicalID(type.builtin));
        note(takesSomeObject && !takesOnlyWhatProgramMakes, takesAnyObject || isNamed || isAcceptedBuiltin, takesSomeObject && !takesOnlyWhatProgramMakes);
    }

    switch (type.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site: {
        BuiltinClassID builtinClass = type.builtinClass;
        bool isFunction = isFunctionClass(m_builtins, builtinClass);
        bool mayBeTaken = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasPlainData) || (isFunction && takesAnyFunction);
        bool isSurelyTaken = takesAnyObject || (isFunction && takesAnyFunction);
        for (const BuiltinTypePart& part : accepted.parts) {
            if (part.kind != BuiltinTypePartKind::Instance) {
                mayBeTaken = true;
                continue;
            }
            if (m_builtins.isSameOrSubclassOf(part.builtinClass, builtinClass))
                mayBeTaken = true;
            if (!m_builtins.isSameOrSubclassOf(builtinClass, part.builtinClass))
                continue;
            mayBeTaken = true;
            bool isOfClassTaken = !part.isOfClassItself || part.builtinClass == builtinClass;
            isSurelyTaken |= isOfClassTaken && part.contents.empty() && (part.pristine.isEmpty() || isAsClassMakesIt(instancesAmongForLinkCalls(type), part.pristine, source));
        }
        note(mayBeTaken, isSurelyTaken, mayBeTaken);
        break;
    }
    case LinkType::Classes::Mixed:
        note(takesSomeObject, takesAnyObject, takesSomeObject);
        break;
    }
}

void CallInLinkCalls::applyRow(RowCall& call)
{
    const BuiltinOverload& row = *call.row;
    const Passed& passed = call.passed;

    if (row.effects.capability == BuiltinCapability::JS && m_state.builtinsWithCodeWaitingForCall.quickGet(call.builtin.index()))
        m_flow.note(LinkFlow::Note::BuiltinCalled, call.builtin.index(), LinkFlow::byRowThatMayRunCode);
    if (row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
        throwAnything();
    else if (row.throws())
        throwError();

    if (row.receiver)
        applyParameter(call, *row.receiver);
    for (const BuiltinParameter& parameter : row.parameters) {
        if (mayTakeApartLiteralAt(call, parameter))
            call.literalsTakenApart.append(parameter.operand);
        else if (mayTakeApartLiteralAt(call, parameter, true))
            call.objectsTakenApart.append(parameter.operand);
        else
            applyParameter(call, parameter);
    }
    for (uint32_t argument = 0; argument < passed.arguments.size(); ++argument) {
        bool isTaken = argument <= BuiltinOperand::maxArgumentIndex && m_builtins.parameterFor(row, BuiltinOperand::argument(argument));
        if (!isTaken && !m_builtins.ignoresArgument(call.builtin, argument))
            escape(passed.arguments[argument]);
    }
    if (passed.list && !m_builtins.parameterFor(row, BuiltinOperand::argument(BuiltinOperand::maxArgumentIndex)))
        escape(*passed.list);

    addResult(call);
    for (const BuiltinTransfer& transfer : row.flow.transfers) {
        if (isTakenApart(call, transfer.from.operand))
            continue;
        const BuiltinParameter* source = transfer.from.kind == BuiltinPlaceKind::OwnValues ? m_builtins.parameterFor(row, *transfer.from.operand) : nullptr;
        bool takesPairs = source && source->roles.contains(BuiltinOperandRole::ContentsOfContentsAreOnlyStored) && transfer.to.kind == BuiltinPlaceKind::ResultSlot;
        if (std::optional<ContentsSlot> slot = takesPairs ? m_builtins.at(transfer.to.slot).contentsSlot : std::nullopt) {
            if (uint32_t parts = nodeOfWrittenParts(call, *slot); parts != noSummaryNode) {
                storeAt(call, transfer.to, valuesAtNode(parts));
                continue;
            }
        }
        Values moved = source && source->roles.contains(BuiltinOperandRole::ContentsOfContentsAreOnlyStored) ? contentsOf(valuesIteratedOf(valuesAt(call, *transfer.from.operand))) : valuesAtPlace(call, transfer.from);
        if (source)
            add(moved, primitivesForLinkCalls(Primitive::Undefined));
        bool onlyItsMemoryIsKeptForResult = transfer.to.kind == BuiltinPlaceKind::NativeMemory && transfer.from.kind == BuiltinPlaceKind::Operand && row.result.rangeOf == transfer.from.operand;
        if (onlyItsMemoryIsKeptForResult)
            add(call.result, moved);
        else
            storeAt(call, transfer.to, moved);
    }
    for (const BuiltinCallback& callback : row.flow.callbacks)
        applyCallback(call, callback);
    for (const BuiltinHandlerCall& handlerCall : row.flow.handlerCalls)
        applyHandlerCall(call, handlerCall);
    handOverLiteralsTakenApart(call);
    for (const BuiltinPropertyAction& action : row.flow.propertyActions)
        applyAction(call, action);
    if (m_state.builtinsWithCodeWaitingForOperand.quickGet(call.builtin.index()))
        noteConditionsThatOperandsMayMeet(call);
    if (call.builtin == m_builtins.canonicalID(Builtins::SymbolFor))
        noteKeysOfPrinterThatRegistryMayGive(call);
    if (call.builtin == m_builtins.canonicalID(Builtins::Proxy))
        noteTrapsOfHandler(call);
}

void CallInLinkCalls::applyParameter(RowCall& call, const BuiltinParameter& parameter)
{
    ValuesAtOperandInLinkCalls atOperand = valuesAt(call, parameter.operand);
    const Values& values = atOperand;
    forEachType(values, [&](const LinkType& type, const Source*) {
        m_flow.noteLossOfLightFormAmong(type);
    });
    OperandUses uses = parameter.uses;
    if (uses.containsAny(usesThatChangeListsForLinkCalls)) {
        forEachType(values, [&](const LinkType& type, const Source*) {
            if (type.classes == LinkType::Classes::Site && type.builtinClass == BuiltinClasses::Array)
                m_flow.arrive(m_flow.contents(type.site, SlotOfContents::isChanged()), m_flow.fromTypeID(TypeTable::undefined));
        });
    }
    bool movesElements = uses.contains(OperandUse::MovesElements);
    bool mayMakeHoles = uses.containsAny(usesThatMayMakeHolesForLinkCalls) || (uses.containsAny({ OperandUse::MovesElements, OperandUse::WritesContents }) && conversionsMayRunCode(call));
    if (movesElements || mayMakeHoles) {
        forEachType(values, [&](const LinkType& type, const Source*) {
            if (type.classes != LinkType::Classes::Site || type.builtinClass != BuiltinClasses::Array || m_flow.hasEscaped(type.site))
                return;
            m_flow.markSite(type.site, SlotOfContents::mayLoseElements());
            if (mayMakeHoles)
                m_flow.markSite(type.site, SlotOfContents::mayHaveHoles());
        });
    }
    if (parameter.roles.contains(BuiltinOperandRole::PrivateFieldsAreCompared))
        inspect(values, questionsOfComparisonOfPrivateFields());
    bool isPrinted = m_builtins.isPrintedByFormatter(call.builtin, parameter);
    auto tellFormatterIsHandedWhateverObjectsThereAre = [&] {
        forEachType(values, [&](const LinkType& type, const Source*) {
            if (isPrinted && type != primitivesForLinkCalls(type.primitives))
                m_flow.note(LinkFlow::Note::ObjectHandedToFormatter, LinkFlow::walkedByFormatter);
        });
    };
    if (uses == OperandUses::all()) {
        tellFormatterIsHandedWhateverObjectsThereAre();
        escape(values);
        call.resultIsUnknown = true;
        throwAnything();
        return;
    }

    const StateOfLinkCalls::WhatTableSaysOfParameter& saidByTable = whatTableSaysOf(*call.row, parameter);
    bool escapesWhole = !saidByTable.recordsSpellOutEveryUse;
    bool changeIsRecorded = call.isSeenByScan && std::ranges::any_of(call.row->flow.propertyActions, [&](const BuiltinPropertyAction& action) {
        if (action.kind == BuiltinPropertyActionKind::ChangesPrototype && action.value.operand && operandsOverlapForLinkCalls(*action.value.operand, parameter.operand))
            return true;
        return operandsOverlapForLinkCalls(action.object, parameter.operand);
    });
    bool isChangedOutOfSight = uses.containsAny(usesThatChangeObjectsForLinkCalls) && !changeIsRecorded;
    escapesWhole |= isChangedOutOfSight && !((uses & usesThatChangeObjectsForLinkCalls) - usesThatOnlyWriteOnObjectsForLinkCalls).isEmpty();
    bool looksNoFurtherThanContents = parameter.roles.contains(BuiltinOperandRole::ContentsOfContentsAreOnlyStored);
    escapesWhole |= parameter.roles.contains(BuiltinOperandRole::KeepsWhatCanBeReached) && !looksNoFurtherThanContents;
    bool escapes = escapesWhole || isChangedOutOfSight;
    if (escapes && parameter.protocols.contains(Protocol::Iterate)) {
        forEachType(values, [&](const LinkType& type, const Source*) {
            tellWhatIterationCalls(type);
        });
    }
    if (escapes && !isPrinted && saidByTable.questions.callMembers) {
        forEachType(values, [&](const LinkType& type, const Source*) {
            tellWhichMembersNativeCodeCallsOffUnseenObjects(type, saidByTable.questions);
        });
    }
    if (escapes)
        tellFormatterIsHandedWhateverObjectsThereAre();
    if (escapesWhole)
        escape(values);
    else if (escapes)
        escapeAllButBuiltins(values, uses.containsAny({ OperandUse::StoresByKey, OperandUse::AssignsFrom }));

    if (uses.contains(OperandUse::Returns))
        add(call.result, valuesHandedOn(call, parameter.operand));
    if (escapes)
        return;

    if (saidByTable.questions.isEmpty() && !saidByTable.questions.listKeys)
        return;
    std::optional<Questions> changed;
    if (saidByTable.questions.listKeys || saidByTable.questions.describeObjects || isPrinted) {
        changed = saidByTable.questions;
        if (std::exchange(changed->listKeys, false))
            tellKeysBecomeTextsAmong(values);
        if (changed->describeObjects && (m_builtins.isUnder(call.builtin, Builtins::NativeFunctions) || !fitOf(call, parameter).mayBeOutside))
            changed->describeObjects = false;
        changed->describesLiteralMadeForCall = changed->describeObjects && isLiteralMadeForCall(call, parameter.operand);
        changed->areAskedByFormatter = isPrinted;
    }
    const Questions& questions = changed ? *changed : saidByTable.questions;
    if (questions.isEmpty())
        return;
    throwAnything();
    if (uses.contains(OperandUse::AppliesTransitively) && !looksNoFurtherThanContents) {
        if (!isMapOfAccessorsAsWritten(values))
            inspect(values, questions);
        return;
    }
    ask(uses.contains(OperandUse::AppliesToContents) ? contentsOf(values, NamesRead::ThoseThatMayBeIndexes) : values, questions);
    if (uses.contains(OperandUse::AppliesTransitively))
        ask(valuesIteratedOf(values), questions);
}

void CallInLinkCalls::applyCallback(RowCall& call, const BuiltinCallback& callback)
{
    if (callback.times == BuiltinCallbackTimes::Repeatedly || callback.times == BuiltinCallbackTimes::PerElement) {
        forEachType(valuesAt(call, callback.callee), [&](const LinkType& type, const Source*) {
            if (type.functions != LinkType::Functions::Closure)
                return;
            m_flow.iterationCallbacks.set(type.function.index());
            if (callback.times == BuiltinCallbackTimes::Repeatedly)
                m_flow.repeatedCallbacks.set(type.function.index());
        });
    }
    PassedMadeInLinkCalls passed;
    passed.isConstruction = callback.constructs;
    if (callback.constructs) {
        if (callback.newTarget.kind != BuiltinPlaceKind::None)
            passed.setNewTarget(valuesAtPlace(call, callback.newTarget));
    } else if (callback.thisValue.kind != BuiltinPlaceKind::None)
        passed.receiver = valuesAtPlace(call, callback.thisValue);

    auto addToList = [&](const Values& values) {
        add(passed.ensureList(), values);
    };
    bool makesPromise = call.mode == BuiltinCallMode::Construct && call.builtin == m_builtins.canonicalID(Builtins::Promise);
    for (const BuiltinPlace& argument : callback.arguments) {
        if (std::optional<LinkType> function = makesPromise && &argument == callback.arguments.data() ? findFulfillingFunction(call) : std::nullopt) [[unlikely]] {
            passed.appendArgument(valuesOf(*function));
            continue;
        }
        bool isRestOfArguments = argument.kind == BuiltinPlaceKind::Operand && !argument.isSettled && argument.operand->kind() == BuiltinOperand::Kind::ArgumentsFrom;
        if (isRestOfArguments && !passed.list) {
            for (uint32_t index = argument.operand->argumentIndex(); index < call.passed.arguments.size(); ++index)
                passed.appendArgument(call.passed.arguments[index]);
            if (call.passed.list)
                addToList(*call.passed.list);
            continue;
        }
        Values values = valuesAtPlace(call, argument);
        if (passed.list) {
            addToList(values);
            continue;
        }
        if (mayBeMissingAt(call, argument))
            add(values, primitivesForLinkCalls(Primitive::Undefined));
        passed.appendArgument(WTF::move(values));
    }
    if (callback.spreadArguments.kind == BuiltinPlaceKind::OwnValues && !callback.spreadArguments.isSettled) {
        Values spread = contentsOf(valuesAt(call, *callback.spreadArguments.operand), NamesRead::ThoseThatMayBeIndexes);
        add(spread, primitivesForLinkCalls(Primitive::Undefined));
        addToList(spread);
        passed.listIsGathered = !m_shared && m_call.appliesGatheredArguments && !call.isCalledByBuiltin && call.passed.arguments.data() == passedByCall().arguments.data() && passed.arguments.empty();
    } else if (callback.spreadArguments.kind != BuiltinPlaceKind::None)
        addToList(valuesAtPlace(call, callback.spreadArguments));
    auto holdsWhatNobodyFollows = [](const Values& values) { return values.made.hasUnknownObjects; };
    if ((passed.receiver && holdsWhatNobodyFollows(*passed.receiver)) || (passed.list && holdsWhatNobodyFollows(*passed.list)) || std::ranges::any_of(passed.arguments, holdsWhatNobodyFollows))
        m_flow.noteRowPassesWhatNobodyFollows();

    Values answers = callValues(valuesAt(call, callback.callee), passed, !call.isCalledByBuiltin);
    switch (callback.resultUse) {
    case BuiltinCallbackResultUse::Ignored:
    case BuiltinCallbackResultUse::Tested:
    case BuiltinCallbackResultUse::AskedWhetherIterable:
        break;
    case BuiltinCallbackResultUse::Returned:
        add(call.result, answers);
        break;
    case BuiltinCallbackResultUse::Escapes:
        escape(answers);
        break;
    case BuiltinCallbackResultUse::Stored:
        if (callback.resultTo.kind == BuiltinPlaceKind::None)
            escape(answers);
        else
            storeAt(call, callback.resultTo, answers);
        break;
    case BuiltinCallbackResultUse::Flattened: {
        Values stored = answers;
        forEachType(answers, [&](const LinkType& answer, const Source* source) {
            if (answer.classes != LinkType::Classes::None)
                add(stored, m_flow.contentsOf(answer, ContentsSlot::Element), beyond(source));
            bool mayBeListInSight = answer.hasDirectUnknownObjects() || answer.builtin || mayBeListWithLayout(answer);
            if (mayBeListInSight || answer.hasUnknownObjects) {
                throwAnything();
                add(stored, mayBeListInSight ? LinkType::anything() : LinkType::anything().asEscapeOnly());
            }
        });
        storeAt(call, callback.resultTo, stored);
        break;
    }
    }

    if (callback.afterwards == BuiltinCallbackAftermath::WritesIntoWhatItFinds) {
        Passed found = call.passed;
        if (call.row && !call.row->receiver)
            found.receiver = std::nullopt;
        escape(found);
        escape(answers);
    }
}

bool CallInLinkCalls::handlerMayBeThere(RowCall& call, const BuiltinPlace& callee)
{
    if (callee.kind != BuiltinPlaceKind::Member || callee.memberPath.empty() || !callee.memberPath.front())
        return true;
    BuiltinName name = callee.memberPath.front();
    for (const FirstStepToHandlerInLinkCalls& asked : call.firstStepsToHandlers) {
        if (asked.operand == *callee.operand && asked.name == name)
            return asked.mayBeThere;
    }
    PropertyKeyID key = m_program.keyOf(name);
    bool mayBeThere = false;
    if (std::optional<bool> has = literalMadeForCallHas(call, *callee.operand, name, key))
        mayBeThere = *has;
    else {
        forEachType(valuesAt(call, *callee.operand), [&](const LinkType& type, const Source* source) {
            mayBeThere = mayBeThere || mayHaveProperty(type, source, key);
        });
    }
    call.firstStepsToHandlers.append({ *callee.operand, name, mayBeThere });
    return mayBeThere;
}

void CallInLinkCalls::followWhatHandlersAnswerForFormatter(RowCall& call, const BuiltinHandlerCall& handlerCall)
{
    if (!m_builtins.has(BuiltinMark::PrintsWhatHandlersAnswer, call.builtin) || handlerCall.times != BuiltinCallbackTimes::PerEvent || handlerCall.resultUse != BuiltinCallbackResultUse::Escapes || !handlerMayBeThere(call, handlerCall.callee))
        return;
    Questions questions;
    questions.readOwnValues = true;
    questions.readThroughPrototypes = true;
    questions.areAskedByFormatter = true;
    questions.onlyFollow = true;
    Vector<FunctionID, 4> handlers;
    ValueID options = handlerCall.callee.kind == BuiltinPlaceKind::Member ? valueWrittenAs(call, *handlerCall.callee.operand) : ValueID();
    if (!options || !gatherClosuresWrittenAt(call, options, m_call.site, handlerCall.callee.memberPath, handlers)) {
        tellFormatterLosesSight(questions);
        return;
    }
    Values answers;
    for (FunctionID handler : handlers) {
        for (FlowNodeID answer : m_flow.nodesOfWhatFunctionAnswers(handler))
            add(answers, answer, notObservedForLinkCalls);
    }
    inspect(answers, questions);
}

bool CallInLinkCalls::makesInstanceWithNothingCalledUnder(ValueID callOrConstruction, BuiltinName step, PropertyKeyID key)
{
    const auto& calls = m_flow.summaries.at(m_caller).calls;
    auto made = std::ranges::find(calls, callOrConstruction, &SummaryCall::site);
    if (made == calls.end() || made->isMadeByProtocol || made->newTarget != noSummaryNode)
        return false;
    bool isByName = made->callee == SummaryCall::Callee::Name && !made->isConstruction && made->key && made->receiver != noSummaryNode;
    if (!isByName && made->callee != SummaryCall::Callee::Value)
        return false;
    FlowNodeID asked = nodeAt(isByName ? made->receiver : made->subject);
    if (!watch(asked, 1))
        return false;
    LinkType callee = heldAt(asked);
    if (callee.isNever())
        return true;
    if (!callee.builtin || callee != builtinForLinkCalls(callee.builtin))
        return false;
    if (isByName) {
        MemberOfTable found = findOnce(static_cast<uint64_t>(callee.builtin.index()) << 32 | made->key.index(), [&] {
            return findAbove(callee.builtin, made->key, MembersUnderKey(m_program, made->key));
        });
        if (found.outcome != Lookup::Found)
            return false;
        BuiltinID receiver = m_builtins.canonicalID(callee.builtin);
        callee = builtinForLinkCalls(found.member);
        const Builtin& member = m_builtins.at(found.member);
        if (member.flags.contains(BuiltinFlag::RequiresHolderAsThis) && (!member.holder || m_builtins.canonicalID(member.holder) != receiver || !membersOfHolderAreWhatTableLists(receiver)))
            return false;
    } else if (!made->isConstruction && m_builtins.at(callee.builtin).flags.contains(BuiltinFlag::RequiresHolderAsThis))
        return false;
    if (!rowsMayBeTakenAtTheirWord(m_builtins.canonicalID(callee.builtin)))
        return false;
    bool mayHandBackWhatSomebodyHolds = std::ranges::any_of(m_builtins.overloadsOf(callee.builtin), [](const BuiltinOverload& row) {
        return row.isUndeclared() || std::ranges::any_of(row.flow.transfers, [](const BuiltinTransfer& transfer) { return transfer.to.kind == BuiltinPlaceKind::Result; });
    });
    if (mayHandBackWhatSomebodyHolds)
        return false;
    auto hasNothingCalledUnder = [&](BuiltinClassID builtinClass) {
        if (!builtinClass || m_builtins.isSameOrSubclassOf(builtinClass, BuiltinClasses::Function))
            return false;
        const BuiltinClass& instances = m_builtins.at(builtinClass);
        if (!instances.listsNamesDefinedOnNewInstances || !instances.prototype)
            return false;
        return !step || (!std::ranges::contains(instances.namesDefinedOnNewInstances, step) && m_builtins.findMember(instances.prototype, step).outcome == BuiltinMemberLookup::Outcome::Absent && isAbsentAboveLiterals(step, key));
    };
    if (made->isConstruction) {
        BuiltinClassID builtinClass = m_builtins.at(callee.builtin).instanceClass;
        if (!hasNothingCalledUnder(builtinClass))
            return false;
        const BuiltinClass& instances = m_builtins.at(builtinClass);
        BuiltinID adder = instances.constructorAdder ? m_builtins.findOwnMember(instances.prototype, instances.constructorAdder) : BuiltinID();
        if (!adder || m_program.isUnlocked(m_builtins.canonicalID(adder)))
            return !adder;
        return !m_program.isLockLeftToProgram(adder) || (m_flow.takeForConstant(adder) && watch(m_flow.writtenAtMember(adder), 0));
    }
    if (!std::ranges::contains(m_builtins.overloadsOf(callee.builtin), BuiltinCallMode::Call, &BuiltinOverload::mode))
        return false;
    return std::ranges::all_of(m_builtins.overloadsOf(callee.builtin), [&](const BuiltinOverload& row) {
        if (row.mode != BuiltinCallMode::Call)
            return true;
        if (!row.result.type)
            return false;
        const BuiltinType& given = m_builtins.at(row.result.type);
        if (!given.flags.isEmpty() || (!given.parts.empty() && given.freshness != BuiltinFreshness::Yes))
            return false;
        return std::ranges::all_of(given.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Instance && hasNothingCalledUnder(part.builtinClass);
        });
    });
}

bool CallInLinkCalls::gatherClosuresWrittenAt(const RowCall& call, ValueID written, ValueID writer, std::span<const BuiltinName> path, Vector<FunctionID, 4>& closures)
{
    if (!written)
        return true;
    const BodyIndex& index = *bodiesLookedAt(m_flow.looks).find(m_caller);
    while (!index.isParameter(written) && opcodesThatStateTypes.contains(index.opcodeOf(written)) && index.onlyUsingValue(written) == writer) {
        writer = written;
        written = index.value(written).operand(0);
    }
    if (index.isParameter(written))
        return false;
    if (primitiveConstants.contains(index.opcodeOf(written)))
        return true;
    BuiltinName step = path.empty() ? BuiltinName() : path.front();
    PropertyKeyID key = step ? m_program.keyOf(step) : PropertyKeyID();
    if (index.opcodeOf(written) == Opcode::NewClosure) {
        if (path.empty()) {
            closures.append(index.value(written).as<Opcode::NewClosure>().function());
            return true;
        }
        return step && index.onlyUsingValue(written) == writer && step != BuiltinNames::prototype && m_builtins.findMember(Builtins::FunctionPrototype, step).outcome == BuiltinMemberLookup::Outcome::Absent && isAbsentAboveLiterals(step, key);
    }
    if (constructionsByValue.contains(index.opcodeOf(written)) || callsByValue.contains(index.opcodeOf(written)))
        return (path.empty() || step) && index.onlyUsingValue(written) == writer && makesInstanceWithNothingCalledUnder(written, step, key);
    size_t literal = literalHandedOn(call, written, writer);
    if (!call.literalsMadeForCall[literal].isInSight)
        return false;
    if (path.empty())
        return true;
    bool isWritten = false;
    for (size_t member = 0; member < call.literalsMadeForCall[literal].written.size(); ++member) {
        MemberWrittenInLinkCalls below = call.literalsMadeForCall[literal].written[member];
        if (step && below.key != key)
            continue;
        isWritten = true;
        if (!gatherClosuresWrittenAt(call, below.value, below.writer, path.subspan(1), closures))
            return false;
    }
    return !step || isWritten || isAbsentAboveLiterals(step, key);
}

void CallInLinkCalls::applyHandlerCall(RowCall& call, const BuiltinHandlerCall& handlerCall)
{
    if (handlerMayBeThere(call, handlerCall.callee)) {
        auto tell = [&](const BuiltinPlace& place) {
            if (place.kind == BuiltinPlaceKind::NewValue && !place.holdsWhatIsThrown && place.newValueType)
                m_flow.note(LinkFlow::Note::TypeHandedToHandler, place.newValueType.index());
        };
        tell(handlerCall.thisValue);
        std::ranges::for_each(handlerCall.arguments, tell);
    }
    if (isTakenApart(call, handlerCall.callee.operand))
        return;
    followWhatHandlersAnswerForFormatter(call, handlerCall);
    if (FlowNodeID kept = handlerCall.callee.kind == BuiltinPlaceKind::NativeCell ? nodeOfFunctionsKeptIn(handlerCall.callee.nativeCell) : FlowNodeID()) {
        Values handlers;
        add(handlers, kept, 1);
        addExportsThatHostKeepsIn(handlerCall.callee.nativeCell, handlers);
        callHandlers(call, handlerCall, handlers);
        return;
    }
    escape(valuesAtPlace(call, handlerCall.callee));
    escape(valuesAtPlace(call, handlerCall.thisValue));
    for (const BuiltinPlace& argument : handlerCall.arguments)
        escape(valuesAtPlace(call, argument));
}

const NativeCellInLinkCalls* CallInLinkCalls::findNativeCell(BuiltinName name) const
{
    size_t position = m_state.nativeCells.findIf([&](const NativeCellInLinkCalls& cell) {
        return cell.name == name;
    });
    return position == notFound ? nullptr : &m_state.nativeCells[position];
}

FlowNodeID CallInLinkCalls::nodeOfFunctionsKeptIn(BuiltinName name)
{
    const NativeCellInLinkCalls* found = findNativeCell(name);
    if (!found || !found->isCalled || found->isValueRead)
        return { };
    NativeCellInLinkCalls& cell = m_state.nativeCells[found - m_state.nativeCells.span().data()];
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    if (!cell.kept)
        cell.kept = m_flow.makeSharedNode();
    return cell.kept;
}

void CallInLinkCalls::addExportsThatHostKeepsIn(BuiltinName name, Values& kept)
{
    for (const RetainedExportInLinkCalls& exported : findNativeCell(name)->retainedExports) {
        for (const Module& module : m_program.modules) {
            if (module.runtimeModule != exported.module || !module.body)
                continue;
            Values found;
            add(found, m_flow.resultToFollow(module.body, 0), 1);
            found = whatLiesUnder(exported.name, found);
            for (BuiltinName member : exported.memberPath)
                found = whatLiesUnder(member, found);
            add(kept, found);
        }
    }
}

ValuesInLinkCalls CallInLinkCalls::whatLiesUnder(BuiltinName name, const Values& holders)
{
    Values found;
    PropertyKeyID key = m_program.keyOf(name);
    forEachType(holders, [&](const LinkType& type, const Source*) {
        bool everyHolderIsRecordInSight = type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin && !type.hasUnknownObjects && !type.holdsLostBuiltins
            && (type.objects == LinkType::Objects::None || type.objects == LinkType::Objects::Layout || type.objects == LinkType::Objects::Set);
        m_flow.forEachLayoutOf(type, [&](LayoutID holder) {
            std::optional<FieldReference> field = key ? findField(m_program, holder, key) : std::nullopt;
            if (field)
                add(found, m_flow.field(*field), 1);
            else
                everyHolderIsRecordInSight = false;
        });
        if (!everyHolderIsRecordInSight)
            add(found, LinkType::anything());
    });
    return found;
}

uint32_t CallInLinkCalls::nodeOfWrittenParts(const RowCall& call, ContentsSlot slot) const
{
    if (!m_call.passesWrittenParts || m_shared || call.isCalledByBuiltin || m_program.admitsReplacedBuiltins || m_flow.testRuns().isTestRun())
        return noSummaryNode;
    for (BuiltinID row : { Builtins::ArrayPrototypeValues, Builtins::ArrayIteratorPrototypeNext }) {
        if (m_program.isUnlocked(row) || m_program.isUnlocked(m_builtins.canonicalID(row)))
            return noSummaryNode;
    }
    return m_summary.arguments[m_call.firstArgument + m_call.numberOfArguments + (slot == ContentsSlot::Value ? 1 : 0)];
}

bool CallInLinkCalls::isTakenApart(const RowCall& call, std::optional<BuiltinOperand> operand) const
{
    return operand && (call.literalsTakenApart.contains(*operand) || call.objectsTakenApart.contains(*operand));
}

std::optional<LayoutID> CallInLinkCalls::layoutOfLiteralInSight(const RowCall& call, ValueID& written, ValueID& writer) const
{
    if (!written)
        return std::nullopt;
    const BodyIndex& index = *bodiesLookedAt(m_flow.looks).find(m_caller);
    while (!index.isParameter(written) && opcodesThatStateTypes.contains(index.opcodeOf(written)) && index.onlyUsingValue(written) == writer) {
        writer = written;
        written = index.value(written).operand(0);
    }
    if (index.isParameter(written) || index.opcodeOf(written) != Opcode::FinishObject)
        return std::nullopt;
    const LiteralMadeForCallInLinkCalls& literal = call.literalsMadeForCall[literalHandedOn(call, written, writer)];
    if (!literal.isInSight)
        return std::nullopt;
    LayoutID layout = index.value(index.value(written).operand(0)).as<Opcode::NewObject>().layout();
    for (const MemberWrittenInLinkCalls& member : literal.written) {
        if (!member.key || !m_program.at(layout).findField(member.key))
            return std::nullopt;
    }
    return layout;
}

bool CallInLinkCalls::stepsAreWrittenOrAbsent(const RowCall& call, ValueID written, ValueID writer, std::span<const PathIntoLiteral> paths) const
{
    const LiteralMadeForCallInLinkCalls& literal = call.literalsMadeForCall[literalHandedOn(call, written, writer)];
    return std::ranges::all_of(paths, [&](const PathIntoLiteral& path) {
        BuiltinName step = path.stepsLeft.empty() ? BuiltinName() : path.stepsLeft.front();
        if (!step)
            return true;
        PropertyKeyID key = m_program.keyOf(step);
        bool isWritten = key && literal.written.containsIf([&](const MemberWrittenInLinkCalls& member) {
            return member.key == key;
        });
        return isWritten || isAbsentAboveLiterals(step, key);
    });
}

bool CallInLinkCalls::findsMethodsAlone(LayoutID layout, PropertyKeyID key) const
{
    LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, key);
    return members.mayAnswer() && !members.lookupGoesOnAboveClasses() && std::ranges::all_of(members, [](const LinkFlow::FoundMember& found) {
        return found.kind == MethodKind::Method;
    });
}

bool CallInLinkCalls::stepsAreFieldsOrAbsent(RowCall& call, const BuiltinParameter& parameter, std::span<const PathIntoLiteral> paths)
{
    bool holdsObject = false;
    bool holds = true;
    forEachType(valuesAt(call, parameter.operand), [&](const LinkType& type, const Source* source) {
        if (!holds || type == primitivesForLinkCalls(type.primitives))
            return;
        holdsObject = true;
        if (type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.classes != LinkType::Classes::None || type.functions != LinkType::Functions::None
            || type.objects != LinkType::Objects::Layout || m_program.at(type.layout).kind == LayoutKind::ModuleNamespace || m_flow.ownValuesOf(type.layout).empty()) {
            holds = false;
            return;
        }
        auto isFieldOrAbsent = [&](BuiltinName step) {
            if (!step)
                return false;
            PropertyKeyID key = m_program.keyOf(step);
            return (key && findField(m_program, type.layout, key)) || !mayHaveProperty(type, source, key);
        };
        auto isFieldMethodOrAbsent = [&](BuiltinName step) {
            SetForScope methodsOfClassesAnswerNothing(m_methodsOfClassesAnswerNothing, true);
            return isFieldOrAbsent(step);
        };
        auto beginsPath = [&](BuiltinName step) {
            return std::ranges::any_of(paths, [&](const PathIntoLiteral& path) {
                return !path.stepsLeft.empty() && path.stepsLeft.front() == step;
            });
        };
        holds = std::ranges::all_of(parameter.namedMembers, [&](BuiltinName step) {
            return beginsPath(step) || isFieldOrAbsent(step);
        }) && std::ranges::all_of(paths, [&](const PathIntoLiteral& path) {
            return !path.stepsLeft.empty() && isFieldMethodOrAbsent(path.stepsLeft.front());
        });
    });
    return holdsObject && holds;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CallInLinkCalls::mayTakeApartLiteralAt(RowCall& call, const BuiltinParameter& parameter, bool byFields)
{
    const BuiltinOverload& row = *call.row;
    BuiltinOperand operand = parameter.operand;
    auto names = [&](const BuiltinPlace& place) {
        return place.operand && operandsOverlapForLinkCalls(*place.operand, operand);
    };
    auto isMember = [&](const BuiltinPlace& place) {
        return place.kind == BuiltinPlaceKind::Member && *place.operand == operand && !place.memberPath.empty();
    };
    if (std::ranges::none_of(row.flow.handlerCalls, isMember, &BuiltinHandlerCall::callee))
        return false;

    static constexpr OperandUses usesOfWhatIsOnlyReadAndKept { OperandUse::Inspects, OperandUse::Retains, OperandUse::ReadsContents, OperandUse::RetainsContents, OperandUse::AppliesTransitively, OperandUse::AppliesToContents };
    if (!(parameter.uses - usesOfWhatIsOnlyReadAndKept).isEmpty() || parameter.conversion != BuiltinConversion::None || m_builtins.isPrintedByFormatter(call.builtin, parameter))
        return false;
    Vector<BuiltinTypeID, 8> visitedTypes;
    if (typeNamesOperandForLinkCalls(m_builtins, row.result.type, operand, SaidOfOperandInLinkCalls::Either, visitedTypes))
        return false;
    bool isNamedByOtherRecords = std::ranges::any_of(row.flow.callbacks, [&](const BuiltinCallback& callback) {
        return operandsOverlapForLinkCalls(callback.callee, operand) || names(callback.newTarget) || names(callback.thisValue) || names(callback.spreadArguments) || names(callback.resultTo) || std::ranges::any_of(callback.arguments, names);
    }) || std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
        return operandsOverlapForLinkCalls(action.object, operand) || names(action.value) || (action.key.operand && operandsOverlapForLinkCalls(*action.key.operand, operand));
    });
    if (isNamedByOtherRecords)
        return false;

    Vector<PathIntoLiteral, 64> paths;
    for (const BuiltinTransfer& transfer : row.flow.transfers) {
        if (names(transfer.to))
            return false;
        if (!names(transfer.from))
            continue;
        if (isMember(transfer.from)) {
            paths.append({ nullptr, &transfer, transfer.from.memberPath });
            continue;
        }
        const NativeCellInLinkCalls* cell = transfer.to.kind == BuiltinPlaceKind::NativeCell ? findNativeCell(transfer.to.nativeCell) : nullptr;
        bool isStoredUnobserved = cell && !cell->isCalled && !cell->isValueRead;
        if (!isStoredUnobserved || *transfer.from.operand != operand || (transfer.from.kind != BuiltinPlaceKind::Operand && transfer.from.kind != BuiltinPlaceKind::OwnValues))
            return false;
    }
    for (const BuiltinHandlerCall& handlerCall : row.flow.handlerCalls) {
        if (!names(handlerCall.callee))
            continue;
        if (!isMember(handlerCall.callee))
            return false;
        paths.append({ &handlerCall, nullptr, handlerCall.callee.memberPath });
    }

    if (byFields)
        return stepsAreFieldsOrAbsent(call, parameter, paths.span());
    ValueID written = valueWrittenAs(call, operand);
    ValueID writer = m_call.site;
    return layoutOfLiteralInSight(call, written, writer) && stepsAreWrittenOrAbsent(call, written, writer, paths.span());
}

void CallInLinkCalls::handOverLiteralsTakenApart(RowCall& call)
{
    for (BuiltinOperand operand : call.literalsTakenApart) {
        Vector<PathIntoLiteral, 64> paths;
        for (const BuiltinTransfer& transfer : call.row->flow.transfers) {
            if (transfer.from.kind == BuiltinPlaceKind::Member && *transfer.from.operand == operand)
                paths.append({ nullptr, &transfer, transfer.from.memberPath });
        }
        for (const BuiltinHandlerCall& handlerCall : call.row->flow.handlerCalls) {
            if (handlerCall.callee.kind == BuiltinPlaceKind::Member && *handlerCall.callee.operand == operand)
                paths.append({ &handlerCall, nullptr, handlerCall.callee.memberPath });
        }
        handOverMembersOfLiteral(call, valueWrittenAs(call, operand), m_call.site, paths.span(), 1);
    }
    for (BuiltinOperand operand : call.objectsTakenApart) {
        Vector<PathIntoLiteral, 64> paths;
        for (const BuiltinTransfer& transfer : call.row->flow.transfers) {
            if (transfer.from.kind == BuiltinPlaceKind::Member && *transfer.from.operand == operand)
                paths.append({ nullptr, &transfer, transfer.from.memberPath });
        }
        for (const BuiltinHandlerCall& handlerCall : call.row->flow.handlerCalls) {
            if (handlerCall.callee.kind == BuiltinPlaceKind::Member && *handlerCall.callee.operand == operand)
                paths.append({ &handlerCall, nullptr, handlerCall.callee.memberPath });
        }
        const BuiltinParameter* parameter = m_builtins.has(BuiltinMark::AsksOptionsForListedNamesAlone, call.builtin) ? m_builtins.parameterFor(*call.row, operand) : nullptr;
        auto isNeverAskedFor = [&](PropertyKeyID key) {
            return parameter && std::ranges::none_of(parameter->namedMembers, [&](BuiltinName name) {
                return m_program.keyOf(name) == key;
            });
        };
        forEachType(valuesAt(call, operand), [&](const LinkType& type, const Source*) {
            if (type.objects != LinkType::Objects::Layout)
                return;
            Vector<LinkFlow::OwnValue, 16> ownValues(m_flow.ownValuesOf(type.layout));
            for (const LinkFlow::OwnValue& own : ownValues) {
                std::optional<FieldReference> field = own.key ? findField(m_program, type.layout, own.key) : std::nullopt;
                if (!field && parameter && watch(own.node, 1) && heldAt(own.node).isNever())
                    continue;
                if (!field) {
                    Values besideFields;
                    add(besideFields, own.node, 1);
                    escape(besideFields);
                    continue;
                }
                Vector<PathIntoLiteral, 16> below;
                for (const PathIntoLiteral& path : paths) {
                    BuiltinName step = path.stepsLeft.front();
                    if (!step || m_program.keyOf(step) == own.key)
                        below.append({ path.handlerCall, path.transfer, path.stepsLeft.subspan(1) });
                }
                if (below.isEmpty() && isNeverAskedFor(own.key))
                    continue;
                handOverMember(call, *field, MemberWrittenInLinkCalls { own.key, ValueID(), ValueID(), true }, below.span(), 1);
            }
            Vector<PropertyKeyID, 8> keysOfMethods;
            for (const PathIntoLiteral& path : paths) {
                PropertyKeyID key = path.stepsLeft.front() ? m_program.keyOf(path.stepsLeft.front()) : PropertyKeyID();
                if (key && !findField(m_program, type.layout, key) && findsMethodsAlone(type.layout, key))
                    keysOfMethods.appendIfNotContains(key);
            }
            for (PropertyKeyID key : keysOfMethods) {
                Values heldAtMethods;
                for (const LinkFlow::FoundMember& found : m_flow.findMembers(type.layout, ClassSide::Prototype, key))
                    add(heldAtMethods, m_flow.member(found.classLayout, ClassSide::Prototype, found.index), 1);
                Vector<PathIntoLiteral, 16> below;
                for (const PathIntoLiteral& path : paths) {
                    if (path.stepsLeft.front() && m_program.keyOf(path.stepsLeft.front()) == key)
                        below.append({ path.handlerCall, path.transfer, path.stepsLeft.subspan(1) });
                }
                handOverMember(call, heldAtMethods, MemberWrittenInLinkCalls { key, ValueID(), ValueID(), true }, below.span(), 1);
            }
        });
    }
}

void CallInLinkCalls::handOverMembersOfLiteral(RowCall& call, ValueID written, ValueID writer, std::span<const PathIntoLiteral> paths, uint8_t distance)
{
    LayoutID layout = *layoutOfLiteralInSight(call, written, writer);
    Vector<MemberWrittenInLinkCalls, 8> members = call.literalsMadeForCall[literalHandedOn(call, written, writer)].written;
    for (const MemberWrittenInLinkCalls& member : members) {
        Vector<PathIntoLiteral, 16> below;
        for (const PathIntoLiteral& path : paths) {
            if (path.stepsLeft.empty())
                continue;
            BuiltinName step = path.stepsLeft.front();
            if (!step || m_program.keyOf(step) == member.key)
                below.append({ path.handlerCall, path.transfer, path.stepsLeft.subspan(1) });
        }
        handOverMember(call, FieldReference { layout, *m_program.at(layout).findField(member.key) }, member, below.span(), distance);
    }
}

void CallInLinkCalls::handOverMember(RowCall& call, FieldReference field, const MemberWrittenInLinkCalls& member, std::span<const PathIntoLiteral> paths, uint8_t distance)
{
    Values heldAtField;
    add(heldAtField, m_flow.field(field), distance);
    handOverMember(call, heldAtField, member, paths, distance);
}

void CallInLinkCalls::handOverMember(RowCall& call, const Values& heldAtMember, const MemberWrittenInLinkCalls& member, std::span<const PathIntoLiteral> paths, uint8_t distance)
{
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_caller);
    ValueID written = member.value;
    ValueID writer = member.writer;
    std::optional<LayoutID> literal = layoutOfLiteralInSight(call, written, writer);
    bool isClosure = index && written && !index->isParameter(written) && index->opcodeOf(written) == Opcode::NewClosure;

    Values held;
    if (isClosure)
        held = valuesOf(closuresForLinkCalls(index->value(written).as<Opcode::NewClosure>().function()));
    else
        held = heldAtMember;

    auto namesOperand = [](const BuiltinPlace& place) {
        return !!place.operand;
    };
    auto handsOnPartsOfLiteral = [&](const PathIntoLiteral& path) {
        return path.handlerCall && (namesOperand(path.handlerCall->thisValue) || std::ranges::any_of(path.handlerCall->arguments, namesOperand));
    };
    auto endsHere = [](const PathIntoLiteral& path) {
        return path.stepsLeft.empty();
    };
    bool leavesWhole = paths.empty() || std::ranges::any_of(paths, [&](const PathIntoLiteral& path) {
        if (endsHere(path))
            return handsOnPartsOfLiteral(path) || (literal && path.transfer);
        return literal && handsOnPartsOfLiteral(path) && path.stepsLeft.size() == 1;
    });
    if (leavesWhole) {
        letGoOfMember(call, member, held, paths);
        return;
    }
    if (literal) {
        if (stepsAreWrittenOrAbsent(call, written, writer, paths))
            handOverMembersOfLiteral(call, written, writer, paths, distance + 1);
        else
            letGoOfMember(call, member, held, paths);
        return;
    }

    bool nothingLiesBelow = std::ranges::all_of(paths, [&](const PathIntoLiteral& path) {
        if (endsHere(path))
            return true;
        BuiltinName step = path.stepsLeft.front();
        return isClosure && step && index->onlyUsingValue(written) == writer && step != BuiltinNames::prototype
            && m_builtins.findMember(Builtins::FunctionPrototype, step).outcome == BuiltinMemberLookup::Outcome::Absent && isAbsentAboveLiterals(step, m_program.keyOf(step));
    });
    bool takesRecordOfHandlersHere = m_builtins.has(BuiltinMark::PassesOverListWhereItTakesRecordOfHandlers, call.builtin) && std::ranges::all_of(paths, [&](const PathIntoLiteral& path) {
        return !endsHere(path) && !path.stepsLeft.front();
    });
    if (takesRecordOfHandlersHere) {
        bool holdsListsAlone = true;
        forEachType(held, [&](const LinkType& type, const Source*) {
            LinkType list = type;
            list.primitives = 0;
            holdsListsAlone &= list.isNever() || (type.classes == LinkType::Classes::Site && type.builtinClass == BuiltinClasses::Array && list == instancesAmongForLinkCalls(list));
        });
        if (holdsListsAlone)
            return;
    }
    if (!nothingLiesBelow) {
        letGoOfMember(call, member, held, paths);
        return;
    }
    for (const PathIntoLiteral& path : paths) {
        if (!endsHere(path))
            continue;
        if (path.transfer)
            storeAt(call, path.transfer->to, held);
        else
            callHandlers(call, *path.handlerCall, held);
    }
}

static QuestionsInLinkCalls questionsOnWhatHandlersAnswerForLinkCalls()
{
    QuestionsInLinkCalls questions;
    questions.readOwnValues = true;
    questions.readThroughPrototypes = true;
    questions.areAskedByFormatter = true;
    questions.onlyFollow = true;
    return questions;
}

bool CallInLinkCalls::answersArePrinted(const RowCall& call, const BuiltinHandlerCall& handlerCall) const
{
    return isTakenApart(call, handlerCall.callee.operand) && m_builtins.has(BuiltinMark::PrintsWhatHandlersAnswer, call.builtin)
        && handlerCall.times == BuiltinCallbackTimes::PerEvent && handlerCall.resultUse == BuiltinCallbackResultUse::Escapes;
}

void CallInLinkCalls::letGoOfMember(RowCall& call, const MemberWrittenInLinkCalls& member, const Values& held, std::span<const PathIntoLiteral> paths)
{
    escape(held);
    auto letGo = [&](const BuiltinPlace& place) {
        bool isPartOfLiteral = place.operand && call.literalsTakenApart.contains(*place.operand);
        bool liesBelowThisMember = place.operand && call.objectsTakenApart.contains(*place.operand) && place.kind == BuiltinPlaceKind::Member && !place.memberPath.empty() && m_program.keyOf(place.memberPath.front()) == member.key;
        if (!isPartOfLiteral && !liesBelowThisMember)
            escape(valuesAtPlace(call, place));
    };
    for (const PathIntoLiteral& path : paths) {
        Vector<FunctionID, 4> handlers;
        if (!path.handlerCall || (!member.nothingIsKnownOfWriter && gatherClosuresWrittenAt(call, member.value, member.writer, path.stepsLeft, handlers) && handlers.isEmpty()))
            continue;
        letGo(path.handlerCall->thisValue);
        std::ranges::for_each(path.handlerCall->arguments, letGo);
    }
    Questions questions = questionsOnWhatHandlersAnswerForLinkCalls();
    Values answers;
    for (const PathIntoLiteral& path : paths) {
        if (!path.handlerCall || !answersArePrinted(call, *path.handlerCall))
            continue;
        Vector<FunctionID, 4> handlers;
        if (member.nothingIsKnownOfWriter || !gatherClosuresWrittenAt(call, member.value, member.writer, path.stepsLeft, handlers)) {
            tellFormatterLosesSight(questions);
            return;
        }
        for (FunctionID handler : handlers) {
            for (FlowNodeID answer : m_flow.nodesOfWhatFunctionAnswers(handler))
                add(answers, answer, notObservedForLinkCalls);
        }
    }
    inspect(answers, questions);
}

void CallInLinkCalls::callHandlers(RowCall& call, const BuiltinHandlerCall& handlerCall, const Values& handlers)
{
    PassedMadeInLinkCalls passed;
    if (handlerCall.thisValue.kind != BuiltinPlaceKind::None)
        passed.receiver = valuesAtPlace(call, handlerCall.thisValue);
    for (const BuiltinPlace& argument : handlerCall.arguments) {
        Values values = valuesAtPlace(call, argument);
        if (mayBeMissingAt(call, argument))
            add(values, primitivesForLinkCalls(Primitive::Undefined));
        passed.appendArgument(WTF::move(values));
    }
    auto holdsWhatNobodyFollows = [](const Values& values) { return values.made.hasUnknownObjects; };
    if ((passed.receiver && holdsWhatNobodyFollows(*passed.receiver)) || std::ranges::any_of(passed.arguments, holdsWhatNobodyFollows))
        m_flow.noteRowPassesWhatNobodyFollows();

    bool formatterFollows = answersArePrinted(call, handlerCall);
    Questions questionsOfFormatter = questionsOnWhatHandlersAnswerForLinkCalls();

    Values answers;
    Values printed;
    forEachType(handlers, [&](const LinkType& handler, const Source* source) {
        LinkType called = handler;
        called.primitives = 0;
        if (called.isNever())
            return;
        if (called.functions != LinkType::Functions::Closure || called != closuresForLinkCalls(called.function)) {
            escape(handler, source);
            if (called.classes == LinkType::Classes::Site && called == instancesAmongForLinkCalls(called) && !isFunctionClass(m_builtins, called.builtinClass))
                return;
            passToUnseen(passed);
            if (formatterFollows)
                tellFormatterLosesSight(questionsOfFormatter);
            return;
        }
        Values constructed;
        followWholeResults(called.function);
        Entry entry = passTo(called.function, passed, constructed);
        if (entry == Entry::Unseen && formatterFollows)
            tellFormatterLosesSight(questionsOfFormatter);
        if (entry != Entry::Body)
            return;
        add(answers, resultOf(called.function, 0, passed, constructed, beyond(source)));
        m_flow.addEdge(m_flow.thrown(called.function), m_flow.unseen());
        for (FlowNodeID answer : m_flow.nodesOfWhatFunctionAnswers(called.function))
            add(printed, answer, notObservedForLinkCalls);
    });
    if (handlerCall.resultUse == BuiltinCallbackResultUse::Escapes)
        escape(answers);
    if (formatterFollows)
        inspect(printed, questionsOfFormatter);
}

bool CallInLinkCalls::findsNoElements(const RowCall& call, const BuiltinPropertyAction& action)
{
    if (action.kind != BuiltinPropertyActionKind::ReadsByKey || action.key.kind != BuiltinKeyKind::Indexes || m_program.admitsReplacedBuiltins)
        return false;
    bool mayFind = false;
    forEachType(valuesAt(call, action.object), [&](const LinkType& type, const Source*) {
        LinkType beside = type;
        beside.primitives = 0;
        mayFind |= !beside.isNever() || type.holdsLostBuiltins || (type.primitives & LinkType::closed(Primitive::AnyString));
    });
    return !mayFind;
}

void CallInLinkCalls::applyAction(RowCall& call, const BuiltinPropertyAction& action)
{
    if (findsNoElements(call, action))
        return;
    bool reads = false;
    bool knowsThatLinkIsLeftAlone = false;
    switch (action.kind) {
    case BuiltinPropertyActionKind::TestsByKey:
        if (call.isSeenByScan)
            return;
        forEachType(valuesAt(call, action.object), [&](const LinkType& type, const Source*) {
            if (type.builtin)
                m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_builtins.canonicalID(type.builtin).index());
        });
        return;
    case BuiltinPropertyActionKind::DescribesResult:
        if (call.isSeenByScan && m_call.describesPrototypeInSight)
            return;
        escape(valuesAt(call, action.object));
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            m_flow.escape(made);
        });
        return;
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
        reads = true;
        break;
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::ChangesIntegrity:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
    case BuiltinPropertyActionKind::DefinesFrom:
        if (call.isSeenByScan)
            return;
        if (action.kind != BuiltinPropertyActionKind::ChangesIntegrity && action.kind != BuiltinPropertyActionKind::ChangesPrototype && action.kind != BuiltinPropertyActionKind::BecomesPrototype)
            knowsThatLinkIsLeftAlone = noteWritesOf(call, action);
        if (action.kind == BuiltinPropertyActionKind::ChangesPrototype) {
            forEachType(valuesAt(call, action.object), [&](const LinkType& type, const Source*) {
                m_flow.noteWhatMayMakeThenableOf(type, m_caller, std::nullopt);
            });
        }
        break;
    }

    if (!reads && onlyWritesOnObject(call, action))
        escapeAllButBuiltins(valuesAt(call, action.object), action.kind == BuiltinPropertyActionKind::StoresByKey || action.kind == BuiltinPropertyActionKind::AssignsFrom, knowsThatLinkIsLeftAlone);
    else
        escape(valuesAt(call, action.object));
    if (reads) {
        tellWhatReadFinds(call, action);
        storeAt(call, action.value, valuesOf(LinkType::anything()));
    } else
        escape(valuesAtPlace(call, action.value));
}

bool CallInLinkCalls::noteWritesOf(RowCall& call, const BuiltinPropertyAction& action)
{
    Vector<PropertyKeyID> keys;
    bool knowsEveryKey = false;
    if (action.key.kind == BuiltinKeyKind::Operand) {
        std::optional<PropertyKeyID> written = keyWrittenAs(call, *action.key.operand);
        if (ValueID passed = written ? ValueID() : valueWrittenAs(call, *action.key.operand)) {
            if (PropertyKeyID symbol = provenSymbolAt(m_program, m_flow.summaries, m_caller, *bodiesLookedAt(m_flow.looks).find(m_caller), passed))
                written = symbol;
        }
        if (written && *written) {
            knowsEveryKey = true;
            keys.append(*written);
        } else if (!written)
            knowsEveryKey = appendSymbolsHeldAt(valuesAt(call, *action.key.operand), keys);
    } else if (action.key.kind == BuiltinKeyKind::Names) {
        knowsEveryKey = true;
        for (BuiltinName name : action.key.names) {
            if (PropertyKeyID key = m_program.keyOf(name))
                keys.append(key);
        }
    }
    LinkFlow::WhatIsStored stored = action.kind == BuiltinPropertyActionKind::DeletesByKey ? LinkFlow::WhatIsStored::Nothing : action.value.kind == BuiltinPlaceKind::NewValue ? LinkFlow::WhatIsStored::TextsNumbersAndBooleans : LinkFlow::WhatIsStored::Anything;
    bool mayLayAccessor = action.kind != BuiltinPropertyActionKind::StoresByKey && action.kind != BuiltinPropertyActionKind::DeletesByKey && action.kind != BuiltinPropertyActionKind::AssignsFrom;
    forEachType(valuesAt(call, action.object), [&](const LinkType& type, const Source*) {
        if (!knowsEveryKey) {
            m_flow.noteWriteOn(type, { }, m_caller, stored);
            if (mayLayAccessor)
                m_flow.noteAccessorMayBeLaidOn(type, { }, m_caller);
            return;
        }
        for (PropertyKeyID key : keys) {
            m_flow.noteWriteOn(type, key, m_caller, stored);
            if (mayLayAccessor)
                m_flow.noteAccessorMayBeLaidOn(type, key, m_caller);
        }
    });
    return knowsEveryKey && std::ranges::none_of(keys, [&](PropertyKeyID key) { return m_program.nameClassOf(key) == NameClass::Proto; });
}

bool CallInLinkCalls::appendSymbolsHeldAt(const Values& values, Vector<PropertyKeyID>& keys)
{
    if (m_shared || !values.made.isNever())
        return false;
    for (const Source& source : values.sources) {
        if (source.passes)
            return false;
        LinkType held = typeAt(source);
        if (held.isNever())
            continue;
        PropertyKeyID symbol = LinkFlow::holdsSymbolsAlone(held) ? m_flow.keyHeldAt(m_caller, source.node) : PropertyKeyID();
        if (!symbol)
            return false;
        keys.appendIfNotContains(symbol);
    }
    return true;
}
bool CallInLinkCalls::onlyWritesOnObject(RowCall& call, const BuiltinPropertyAction& action)
{
    switch (action.kind) {
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::DefinesName:
        return true;
    case BuiltinPropertyActionKind::TakesDescriptor:
        break;
    default:
        return false;
    }
    if (!action.value.operand)
        return false;
    for (BuiltinName accessor : std::initializer_list<BuiltinName> { BuiltinNames::get, BuiltinNames::set }) {
        PropertyKeyID key = m_program.keyOf(accessor);
        if (std::optional<bool> has = literalMadeForCallHas(call, *action.value.operand, accessor, key)) {
            if (*has)
                return false;
            continue;
        }
        bool mayLayAccessor = false;
        forEachType(valuesAt(call, *action.value.operand), [&](const LinkType& descriptor, const Source* source) {
            mayLayAccessor = mayLayAccessor || mayHaveProperty(descriptor, source, key);
        });
        if (mayLayAccessor)
            return false;
    }
    return true;
}

void CallInLinkCalls::escapeAllButBuiltins(const Values& values, bool assigns, bool knowsThatLinkIsLeftAlone)
{
    forEachType(values, [&](const LinkType& type, const Source*) {
        LinkType rest = type;
        bool mayChangeLink = assigns && !knowsThatLinkIsLeftAlone && rest.builtin && !m_builtins.hasLockedPrototypeLink(rest.builtin);
        bool mayRunSetterAbove = assigns && rest.builtin && lookupPassesHolderLinkedAnew(m_program, rest.builtin, { });
        if (!mayChangeLink && !mayRunSetterAbove)
            rest.builtin = { };
        if (!rest.isNever())
            escape(rest, nullptr);
    });
}

ValueID CallInLinkCalls::valueWrittenAs(const RowCall& call, BuiltinOperand operand) const
{
    if (call.isCalledByBuiltin || m_shared || m_call.isMadeByProtocol || m_call.passesList || operand.kind() != BuiltinOperand::Kind::Argument)
        return { };
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_caller);
    if (m_call.isMadeByHost) {
        if (!index || operand.argumentIndex() || !m_call.passesLiteral || !m_flow.modulesWhoseExportHostTakesByCallInSight.get(m_program.at(m_caller).module.index()))
            return { };
        RELEASE_ASSERT(index->opcodeOf(m_call.site) == Opcode::PutModuleVariable);
        ValueID stored = index->value(m_call.site).as<Opcode::PutModuleVariable>().valueOperand();
        return index->isParameter(stored) ? ValueID() : stored;
    }
    if (!index || index->isParameter(m_call.site) || index->opcodeOf(m_call.site) != Opcode::Call)
        return { };
    std::span<const ValueID> arguments = index->value(m_call.site).as<Opcode::Call>().argumentsOperands();
    ValueID written = operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : ValueID();
    return written && !index->isParameter(written) ? written : ValueID();
}

std::optional<StringID> CallInLinkCalls::textWrittenAs(const RowCall& call, BuiltinOperand operand) const
{
    ValueID passed = valueWrittenAs(call, operand);
    if (!passed)
        return std::nullopt;
    const BodyIndex& index = *bodiesLookedAt(m_flow.looks).find(m_caller);
    ValueID written = index.stripCasts(passed);
    if (!written || index.isParameter(written) || index.opcodeOf(written) != Opcode::String)
        return std::nullopt;
    return index.value(written).as<Opcode::String>().string();
}

bool CallInLinkCalls::gatherWhatLiteralWrites(ValueID finished, ValueID user, Vector<MemberWrittenInLinkCalls, 8>& written) const
{
    const BodyIndex& index = *bodiesLookedAt(m_flow.looks).find(m_caller);
    if (index.onlyUsingValue(finished) != user)
        return false;
    if (index.opcodeOf(finished) == Opcode::Data) {
        DataID data = index.value(finished).as<Opcode::Data>().data();
        const auto* object = data ? m_program.at(data).root.tryAs<DataNode::Object>() : nullptr;
        if (!object)
            return false;
        for (const DataMember& member : object->members)
            written.append({ member.key, ValueID(), ValueID() });
    } else {
        if (index.opcodeOf(finished) != Opcode::FinishObject)
            return false;
        ValueID literal = index.value(finished).operand(0);
        if (!literal || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewObject || index.blockOf(literal) != index.blockOf(finished))
            return false;
        auto made = index.value(literal).as<Opcode::NewObject>();
        if (!made.layout() || made.prototypeOperand())
            return false;
        uint32_t operandOfField = 1;
        for (uint32_t field : made.fields())
            written.append({ m_program.at(made.layout()).fields[field].key, index.value(literal).operand(operandOfField++), literal });
        uint32_t numberOfUses = 1;
        for (uint32_t number = literal.index() + 1; number < finished.index(); ++number) {
            BodyValue part = index.value(ValueID(number));
            bool touchesLiteral = false;
            part.forEachOperand([&](ValueID touched, uint32_t) {
                touchesLiteral |= touched && index.stripCasts(touched) == literal;
            });
            if (!touchesLiteral)
                continue;
            if (part.opcode() != Opcode::PropDefine || part.operand(0) != literal)
                return false;
            written.append({ part.as<Opcode::PropDefine>().key(), part.operand(1), ValueID(number) });
            ++numberOfUses;
        }
        if (numberOfUses >= BodyIndex::maxCountedUses || index.useCount(literal) != numberOfUses)
            return false;
    }
    return true;
}

std::optional<bool> CallInLinkCalls::literalMadeForCallHas(const RowCall& call, BuiltinOperand operand, BuiltinName name, PropertyKeyID key) const
{
    ValueID handedOn = valueWrittenAs(call, operand);
    if (!handedOn)
        return std::nullopt;
    const LiteralMadeForCallInLinkCalls& literal = call.literalsMadeForCall[literalHandedOn(call, handedOn, m_call.site)];
    if (!literal.isInSight)
        return std::nullopt;
    bool isWritten = key && literal.written.containsIf([&](const MemberWrittenInLinkCalls& member) {
        return member.key == key;
    });
    if (isWritten)
        return true;
    return isAbsentAboveLiterals(name, key) ? std::optional(false) : std::nullopt;
}

std::optional<StringID> CallInLinkCalls::textThatLiteralWritesUnder(const RowCall& call, BuiltinOperand operand, PropertyKeyID key) const
{
    ValueID handedOn = valueWrittenAs(call, operand);
    const BodyIndex& index = *bodiesLookedAt(m_flow.looks).find(m_caller);
    std::optional<StringID> text;
    if (index.opcodeOf(handedOn) == Opcode::Data) {
        for (const DataMember& member : m_program.at(index.value(handedOn).as<Opcode::Data>().data()).root.as<DataNode::Object>().members) {
            if (member.key != key)
                continue;
            const auto* written = member.value.tryAs<DataNode::String>();
            text = written ? std::optional(written->value) : std::nullopt;
        }
        return text;
    }
    for (const MemberWrittenInLinkCalls& member : call.literalsMadeForCall[literalHandedOn(call, handedOn, m_call.site)].written) {
        if (member.key != key)
            continue;
        ValueID written = index.stripCasts(member.value);
        bool isText = written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::String;
        text = isText ? std::optional(index.value(written).as<Opcode::String>().string()) : std::nullopt;
    }
    return text;
}

size_t CallInLinkCalls::literalHandedOn(const RowCall& call, ValueID handedOn, ValueID user) const
{
    size_t position = call.literalsMadeForCall.findIf([&](const LiteralMadeForCallInLinkCalls& gathered) {
        return gathered.handedOn == handedOn;
    });
    if (position != notFound)
        return position;
    call.literalsMadeForCall.append({ handedOn, false, { } });
    LiteralMadeForCallInLinkCalls& gathered = call.literalsMadeForCall.last();
    gathered.isInSight = gatherWhatLiteralWrites(handedOn, user, gathered.written);
    return call.literalsMadeForCall.size() - 1;
}

bool CallInLinkCalls::isAbsentAboveLiterals(BuiltinName name, PropertyKeyID key) const
{
    if (key)
        return m_flow.isAbsentFromObjectPrototype(key);
    BuiltinClassID objectClass = BuiltinClasses::Object;
    return m_program.openedNames.isEmpty() && m_builtins.findMember(m_builtins.at(objectClass).prototype, name).outcome == BuiltinMemberLookup::Outcome::Absent;
}

std::optional<PropertyKeyID> CallInLinkCalls::keyWrittenAs(const RowCall& call, BuiltinOperand operand) const
{
    std::optional<StringID> text = textWrittenAs(call, operand);
    if (!text)
        return std::nullopt;
    NameClass nameClass = m_program.nameClass(*text);
    if (nameClass == NameClass::Proto || mayBeElementKey(nameClass))
        return std::nullopt;
    return m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = *text }).value_or(PropertyKeyID());
}

void CallInLinkCalls::tellWhatIsFoundUnder(PropertyKeyID key, const Values& objects, bool readsThroughPrototypes)
{
    forEachType(objects, [&](const LinkType& type, const Source*) {
        if (!readsThroughPrototypes) {
            if (type.hasUnknownObjects || type.holdsLostBuiltins)
                m_flow.note(LinkFlow::Note::NameRead, key.bits(), !m_flow.isInLibraryCode());
            return;
        }
        LinkType others = type;
        others.primitives = 0;
        if (type.classes == LinkType::Classes::Site) {
            others.dropInstances();
            MemberOfTable found = findOnInstancesOf(type.builtinClass, key, MembersUnderKey(m_program, key));
            m_flow.noteLossOf(found.member ? m_builtins.canonicalID(found.member) : BuiltinID());
            if (found.outcome == Lookup::Unknown)
                others.hasUnknownObjects = true;
        }
        if (!others.isNever())
            m_flow.note(LinkFlow::Note::NameRead, key.bits(), !m_flow.isInLibraryCode());
    });
}

void CallInLinkCalls::tellKeysBecomeTextsAmong(const Values& objects)
{
    forEachType(objects, [&](const LinkType& type, const Source*) {
        m_flow.noteKeysBecomeTextsAmong(type);
    });
}

void CallInLinkCalls::tellWhatReadFinds(const RowCall& call, const BuiltinPropertyAction& action)
{
    const BuiltinParameter* read = m_builtins.parameterFor(*call.row, action.object);
    bool readsThroughPrototypes = !read || read->protocols.contains(Protocol::PrototypeChain);
    Values objects = valuesAt(call, action.object);
    bool findsSomething = readsThroughPrototypes;
    forEachType(objects, [&](const LinkType& type, const Source*) {
        findsSomething |= type.hasUnknownObjects || type.holdsLostBuiltins;
    });
    if (!findsSomething) {
        if (action.key.kind == BuiltinKeyKind::Operand && !keyWrittenAs(call, *action.key.operand))
            tellKeysBecomeTextsAmong(objects);
        return;
    }
    switch (action.key.kind) {
    case BuiltinKeyKind::Indexes:
        return;
    case BuiltinKeyKind::Names:
        for (BuiltinName name : action.key.names)
            tellWhatIsFoundUnder(m_program.keyOf(name), objects, readsThroughPrototypes);
        return;
    case BuiltinKeyKind::Operand: {
        if (std::optional<PropertyKeyID> written = keyWrittenAs(call, *action.key.operand)) {
            if (*written)
                tellWhatIsFoundUnder(*written, objects, readsThroughPrototypes);
            return;
        }
        tellKeysBecomeTextsAmong(objects);
        bool spellsNoMemberOfTable = true;
        forEachType(valuesAt(call, *action.key.operand), [&](const LinkType& key, const Source*) {
            spellsNoMemberOfTable &= key == primitivesForLinkCalls(key.primitives & LinkType::closed(Primitive::All) & ~LinkType::closed(Primitive::AnyString | Primitive::AnySymbol));
        });
        if (spellsNoMemberOfTable)
            return;
        break;
    }
    case BuiltinKeyKind::None:
    case BuiltinKeyKind::Any:
    case BuiltinKeyKind::ElementsOf:
        break;
    }
    m_flow.note(LinkFlow::Note::ReadUnderKeyNobodyKnows, 0);
}

void CallInLinkCalls::tellWhatIterationCallsAbove(BuiltinClassID builtinClass, bool followsWhatIsMade)
{
    for (BuiltinName name : m_builtins.keysReadBy(Protocol::Iterate)) {
        MemberOfTable found = findOnInstancesOf(builtinClass, m_program.keyOf(name));
        if (found.outcome != Lookup::Found)
            continue;
        m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, found.member.index());
        if (!followsWhatIsMade)
            continue;
        for (const BuiltinOverload& row : m_builtins.overloadsOf(found.member)) {
            if (!row.result.type)
                continue;
            for (const BuiltinTypePart& part : m_builtins.at(row.result.type).parts) {
                if (part.builtinClass)
                    tellWhatIterationCallsAbove(part.builtinClass, false);
            }
        }
    }
}

void CallInLinkCalls::tellWhatIterationCalls(const LinkType& iterable)
{
    if (iterable.classes == LinkType::Classes::Site)
        tellWhatIterationCallsAbove(iterable.builtinClass == BuiltinClasses::Arguments ? BuiltinClasses::Array : iterable.builtinClass, true);
    if (!iterable.hasUnknownObjects && !iterable.holdsLostBuiltins)
        return;
    for (BuiltinName name : m_builtins.keysReadBy(Protocol::Iterate)) {
        if (PropertyKeyID key = name ? m_program.keyOf(name) : PropertyKeyID())
            m_flow.note(LinkFlow::Note::NameCalled, key.bits());
    }
}

void CallInLinkCalls::tellWhichMembersNativeCodeCallsOffUnseenObjects(const LinkType& subject, const Questions& questions)
{
    if (!questions.callMembers || questions.areAskedByFormatter || !subject.hasUnknownObjects)
        return;
    for (PropertyKeyID key : questions.keys)
        m_flow.note(LinkFlow::Note::NameCalledByNativeCodeOffUnseenObjects, key.bits());
}

bool CallInLinkCalls::returnsOperandWithoutFraction(const RowCall& call)
{
    const BuiltinResult& result = call.row->result;
    bool returnsOperand = result.rangeRule == BuiltinRangeRule::MinimumOfArguments || result.rangeRule == BuiltinRangeRule::MaximumOfArguments || result.rangeRule == BuiltinRangeRule::AbsoluteValue;
    if (!returnsOperand || !result.rangeOperand || call.passed.list)
        return false;
    constexpr uint16_t convertedWithoutFraction = LinkType::closed(Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::NumberWithoutFraction);
    bool mayHoldFraction = false;
    forEachType(valuesAt(call, *result.rangeOperand), [&](const LinkType& type, const Source*) {
        mayHoldFraction |= (type.primitives & ~convertedWithoutFraction) || type.hasUnknownObjects || !holdsNothingInSightForLinkCalls(type);
    });
    return !mayHoldFraction;
}

ALWAYS_INLINE void CallInLinkCalls::addResult(RowCall& call)
{
    const BuiltinOverload& row = *call.row;
    const BuiltinResult& result = row.result;
    if (call.resultIsUnknown || !result.type || row.resultIsDeclaredOnly())
        add(call.result, LinkType::anything());
    if (result.type) {
        if (m_program.builtinInjectedAsNull == row.builtin.index()) [[unlikely]]
            add(call.result, primitivesForLinkCalls(Primitive::Null));
        else if (result.testedOperand)
            add(call.result, primitivesForLinkCalls(Primitive::Boolean));
        else if (result.rangeRule == BuiltinRangeRule::IntegerPart || result.rangeRule == BuiltinRangeRule::CodeUnitOrNaN || returnsOperandWithoutFraction(call))
            add(call.result, wholeNumbersForLinkCalls());
        else
            addValuesOfType(call, result.type, true, call.result);
    }

    bool mayBeMadeWithLengthAlone = (call.passed.arguments.size() == 1 || call.passed.list) && m_builtins.canonicalID(call.builtin) == m_builtins.canonicalID(m_builtins.at(BuiltinClassID(BuiltinClasses::Array)).constructor);
    if (mayBeMadeWithLengthAlone) {
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            m_flow.arrive(m_flow.contents(made, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        });
    }

    bool mayBeMadeOverBuffer = false;
    bool resultCanBeView = result.type && std::ranges::any_of(m_builtins.at(result.type).parts, [&](const BuiltinTypePart& part) {
        if (part.kind == BuiltinTypePartKind::Instance)
            return m_builtins.typedArrayTypeOf(part.builtinClass) != NotTypedArray;
        return part.kind == BuiltinTypePartKind::InstanceOfReceiverClass || part.kind == BuiltinTypePartKind::TypedArrayOfReceiverElementType;
    });
    if (resultCanBeView && m_builtins.parameterFor(row, BuiltinOperand::argument(0))) {
        forEachType(valuesAt(call, BuiltinOperand::argument(0)), [&](const LinkType& first, const Source*) {
            mayBeMadeOverBuffer |= first.hasUnknownObjects || first.holdsLostBuiltins || first.classes == LinkType::Classes::Mixed
                || (first.classes == LinkType::Classes::Site && (first.builtinClass == BuiltinClasses::ArrayBuffer || first.builtinClass == BuiltinClasses::SharedArrayBuffer));
        });
    }
    if (mayBeMadeOverBuffer) {
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            if (m_builtins.typedArrayTypeOf(m_flow.classOf(made)) != NotTypedArray)
                m_flow.arrive(m_flow.contents(made, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        });
    }

    if (result.iteratedOperand) {
        Values stepValues = stepValuesOf(result.iterationKind, valuesAt(call, *result.iteratedOperand));
        if (const BuiltinParameter* iterated = m_builtins.parameterFor(row, *result.iteratedOperand); iterated && iterated->conversion == BuiltinConversion::ToString)
            add(stepValues, primitivesForLinkCalls(Primitive::String));
        bool everyIteratorIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            bool hasSlot = false;
            for (BuiltinSlotID id : m_builtins.at(m_flow.classOf(made)).slots) {
                if (!m_builtins.at(id).holdsStepValues)
                    continue;
                hasSlot = true;
                storeInContents(made, m_flow.slotOf(id), stepValues);
            }
            if (!hasSlot)
                escape(stepValues);
        });
        if (!everyIteratorIsFollowed)
            escape(stepValues);
    }
    if (result.prototypeSource)
        escape(valuesAt(call, *result.prototypeSource));
    bool standsBelowOperand = result.prototypeSource || std::ranges::any_of(call.row->flow.propertyActions, [](const BuiltinPropertyAction& action) {
        return action.kind == BuiltinPropertyActionKind::BecomesPrototype && action.value.kind == BuiltinPlaceKind::Result;
    });
    if (standsBelowOperand) {
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart& part) {
            if (!m_builtins.at(part.builtinClass).isFollowedBesideUnseenObject)
                m_flow.escape(made);
        });
    }
    for (const BuiltinResultProperty& property : result.properties) {
        Values held = m_program.builtinInjectedAsHoldingNull == row.builtin.index() ? valuesOf(primitivesForLinkCalls(Primitive::Null)) : valuesOfType(call, property.type);
        escape(held);
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            storeInContents(made, SlotOfContents::under(m_program.keyOf(property.name)), held);
        });
    }
}

ValuesAtOperandInLinkCalls CallInLinkCalls::valuesAt(const RowCall& call, BuiltinOperand operand)
{
    const Passed& passed = call.passed;
    ValuesAtOperandInLinkCalls values;
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver: {
        const Values* written = call.mode == BuiltinCallMode::Construct ? passed.newTarget : passed.receiver ? &*passed.receiver : nullptr;
        if (written)
            values.standing = written;
        else
            values.made = valuesOf(call.mode == BuiltinCallMode::Construct ? builtinForLinkCalls(call.builtin) : primitivesForLinkCalls(Primitive::Undefined));
        return values;
    }
    case BuiltinOperand::Kind::Argument:
        if (operand.argumentIndex() < passed.arguments.size()) {
            values.standing = &passed.arguments[operand.argumentIndex()];
            return values;
        }
        if (passed.list)
            add(values.made, *passed.list);
        add(values.made, primitivesForLinkCalls(Primitive::Undefined));
        return values;
    case BuiltinOperand::Kind::ArgumentsFrom:
        for (size_t index = operand.argumentIndex(); index < passed.arguments.size(); ++index)
            add(values.made, passed.arguments[index]);
        if (passed.list)
            add(values.made, *passed.list);
        return values;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValuesInLinkCalls CallInLinkCalls::valuesHandedOn(RowCall& call, BuiltinOperand operand)
{
    Values values = valuesAt(call, operand);
    const BuiltinParameter* parameter = m_builtins.parameterFor(*call.row, operand);
    if (!parameter || parameter->conversion != BuiltinConversion::ToObject)
        return values;
    Values wrappers;
    forEachType(values, [&](const LinkType& type, const Source*) {
        for (uint16_t group : primitivesWithClassesForLinkCalls) {
            if (uint16_t present = type.primitives & group)
                add(wrappers, m_flow.typeOf(m_flow.siteOf(LinkFlow::MakerOfSite::WrapperAtCall, m_number, 0, primitiveClassOf(LinkType::inNamesOfPrimitive(present)))));
        }
    });
    add(values, wrappers);
    return values;
}

ValuesInLinkCalls CallInLinkCalls::valuesAtPlace(RowCall& call, const BuiltinPlace& place)
{
    LinkType undefined = primitivesForLinkCalls(Primitive::Undefined);
    Values values;
    switch (place.kind) {
    case BuiltinPlaceKind::None:
        break;
    case BuiltinPlaceKind::Operand:
        values = valuesHandedOn(call, *place.operand);
        break;
    case BuiltinPlaceKind::Slot:
        addContentsOfSlot(call, valuesAt(call, *place.operand), place.slot, values);
        break;
    case BuiltinPlaceKind::IteratedValues:
        addIteratedValues(valuesAt(call, *place.operand), values);
        break;
    case BuiltinPlaceKind::OwnValues:
        values = contentsOf(valuesAt(call, *place.operand));
        add(values, undefined);
        break;
    case BuiltinPlaceKind::Result:
        values = call.result;
        break;
    case BuiltinPlaceKind::ResultSlot: {
        bool everyObjectIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            add(values, m_flow.contents(made, m_flow.slotOf(place.slot)), notObservedForLinkCalls);
        });
        if (!everyObjectIsFollowed)
            add(values, LinkType::anything());
        add(values, undefined);
        break;
    }
    case BuiltinPlaceKind::NewValue:
        addValuesOfType(call, place.newValueType, false, values);
        break;
    case BuiltinPlaceKind::Prototype:
        addPrototypes(valuesAt(call, *place.operand), values);
        break;
    case BuiltinPlaceKind::Member:
        escape(valuesAt(call, *place.operand));
        add(values, LinkType::anything());
        break;
    case BuiltinPlaceKind::ResultField: {
        bool everyObjectIsFollowed = forEachSiteOfResultField(call, place, [&](SiteID made, const BuiltinTypePart& part) {
            add(values, m_flow.contents(made, slotOfField(part, place.fieldIndex)), notObservedForLinkCalls);
        });
        if (!everyObjectIsFollowed)
            add(values, LinkType::anything());
        add(values, undefined);
        break;
    }
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::NativeCell:
        add(values, LinkType::anything());
        break;
    }
    if (place.isSettled)
        settle(values);
    return values;
}

ValuesInLinkCalls CallInLinkCalls::valuesOfType(RowCall& call, BuiltinTypeID id)
{
    Values values;
    addValuesOfType(call, id, false, values);
    return values;
}

void CallInLinkCalls::addValuesOfType(RowCall& call, BuiltinTypeID id, bool isResult, Values& values)
{
    if (id && m_builtins.at(id).isAnything()) {
        for (const BuiltinTypePart& part : m_builtins.at(id).parts) {
            if (part.builtinClass)
                m_flow.note(LinkFlow::Note::InstanceMadeOutOfSight, part.builtinClass.index());
        }
    }
    if (!id || m_builtins.at(id).isAnything() || m_typesBeingRead.contains(id)) {
        add(values, LinkType::anything());
        return;
    }
    m_typesBeingRead.append(id);

    const BuiltinType& type = m_builtins.at(id);
    add(values, primitivesWrittenForLinkCalls(type));
    if (!type.stringLiterals.empty())
        add(values, primitivesForLinkCalls(Primitive::String));

    if (type.freshness == BuiltinFreshness::UnlessSpecies || type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }))
        add(values, unseenObjectsForLinkCalls());
    for (const BuiltinTypePart& part : type.parts)
        addValuesOfPart(call, part, type.freshness, isResult, values);

    m_typesBeingRead.removeLast();
}

void CallInLinkCalls::addValuesOfPart(RowCall& call, const BuiltinTypePart& part, BuiltinFreshness freshness, bool isResult, Values& values)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        if (freshness == BuiltinFreshness::No || !isFollowed(part.builtinClass)) {
            m_flow.note(LinkFlow::Note::InstanceMadeOutOfSight, part.builtinClass.index());
            add(values, m_flow.unseenObjectsBelow(m_flow.prototypeOfInstancesOf(part.builtinClass), m_caller));
            for (const BuiltinSlotContents& contents : part.contents)
                escape(valuesOfType(call, contents.type));
            return;
        }
        addInstanceOf(call, part, part.builtinClass, part.contents, isResult, values, freshness == BuiltinFreshness::Yes);
        if (m_builtins.at(part.builtinClass).isFollowedBesideUnseenObject && !areBuiltinFunctionsAlone(valuesAt(call, BuiltinOperand::receiver())))
            add(values, m_flow.unseenObjectsBelow(m_flow.prototypeOfInstancesOf(part.builtinClass), m_caller));
        if (freshness == BuiltinFreshness::Unaliased)
            storeInContents(siteOf(part, part.builtinClass), SlotOfContents::underUnknownNames(), valuesOf(LinkType::anything()));
        return;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        forEachType(valuesAt(call, BuiltinOperand::receiver()), [&](const LinkType& receiver, const Source*) {
            BuiltinClassID made = holdsOnlyInstancesOfOneClassForLinkCalls(receiver) ? receiver.builtinClass : BuiltinClassID();
            if (made && part.kind == BuiltinTypePartKind::TypedArrayOfReceiverElementType)
                made = m_builtins.typedArrayClassOf(made);
            if (made && freshness != BuiltinFreshness::No && isFollowed(made)) {
                if (made == receiver.builtinClass)
                    makeSiteOfClassOfReceiver(part, receiver);
                addInstanceOf(call, part, made, { }, isResult, values);
            } else {
                if (made)
                    m_flow.note(LinkFlow::Note::InstanceMadeOutOfSight, made.index());
                add(values, unseenObjectsForLinkCalls());
            }
        });
        return;
    case BuiltinTypePartKind::Layout:
        if (freshness != BuiltinFreshness::No) {
            addRecordOf(call, part, isResult, values);
            return;
        }
        add(values, unseenObjectsForLinkCalls());
        for (const BuiltinLayoutField& field : m_builtins.at(part.layout).fields)
            escape(valuesOfType(call, field.type));
        return;
    case BuiltinTypePartKind::Builtin:
        if (BuiltinClassID below = classBelowPrototype(m_builtins.canonicalID(part.builtin))) {
            m_flow.note(LinkFlow::Note::InstanceMadeOutOfSight, below.index());
            add(values, builtinForLinkCalls(m_builtins.canonicalID(part.builtin)), m_rowFitsByEscapeAlone);
            return;
        }
        add(values, builtinForLinkCalls(m_builtins.canonicalID(part.builtin)));
        return;
    case BuiltinTypePartKind::Operand:
        add(values, valuesHandedOn(call, *part.operand));
        return;
    case BuiltinTypePartKind::Slot:
        addContentsOfSlot(call, valuesAt(call, *part.operand), part.slot, values);
        return;
    case BuiltinTypePartKind::IteratedValues: {
        if (uint32_t elements = part.isSettled || m_builtins.canonicalID(call.builtin) != Builtins::Set ? noSummaryNode : nodeOfWrittenParts(call, ContentsSlot::Element); elements != noSummaryNode) {
            add(values, valuesAtNode(elements));
            return;
        }
        Values iterated;
        addIteratedValues(valuesAt(call, *part.operand), iterated);
        if (part.isSettled)
            settle(iterated);
        add(values, iterated);
        return;
    }
    case BuiltinTypePartKind::CallbackResult: {
        Values answers;
        addResultsOfCallbacks(call, *part.operand, isResult && !part.isSettled, !isResult && !part.isSettled, answers);
        if (part.isSettled)
            settle(answers);
        add(values, answers);
        return;
    }
    case BuiltinTypePartKind::Prototype:
        addPrototypes(valuesAt(call, *part.operand), values);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

SiteID CallInLinkCalls::siteOf(const BuiltinTypePart& part, BuiltinClassID made, bool isResultOfCall)
{
    size_t numberOfPart = &part - BuiltinRecordsOfPool<BuiltinRecordPool::TypeParts>::records();
    bool isCreatedWithoutHoles = made != BuiltinClasses::Array || part.kind == BuiltinTypePartKind::Layout || part.pristine.contains(PristineAspects::Dense);
    if (m_makesWhatRowsMakeAtOneSiteOfProgram)
        return m_flow.siteOf(LinkFlow::MakerOfSite::PartOfRow, UINT32_MAX, static_cast<uint32_t>(numberOfPart), made, false, part.isOfClassItself, isCreatedWithoutHoles);
    return m_flow.siteOf(LinkFlow::MakerOfSite::PartOfRow, m_number, static_cast<uint32_t>(numberOfPart), made, isResultOfCall && onlyWaitRightBehindSeesResult(), part.isOfClassItself, isCreatedWithoutHoles);
}

void CallInLinkCalls::makeSiteOfClassOfReceiver(const BuiltinTypePart& part, const LinkType& receiver)
{
    if (!m_flow.isOfClassItself(receiver.site))
        return;
    size_t numberOfPart = &part - BuiltinRecordsOfPool<BuiltinRecordPool::TypeParts>::records();
    SiteID made = m_flow.siteOf(LinkFlow::MakerOfSite::PartOfRow, m_number, static_cast<uint32_t>(numberOfPart), receiver.builtinClass, false, true, receiver.builtinClass != BuiltinClasses::Array);
    m_flow.addEdge(m_flow.contentsOf(receiver, SlotOfContents::mayBeOfClassBelow()), m_flow.contents(made, SlotOfContents::mayBeOfClassBelow()));
}

bool CallInLinkCalls::onlyWaitRightBehindSeesResult() const
{
    const BodyIndex* index = m_shared || m_call.isMadeByProtocol ? nullptr : bodiesLookedAt(m_flow.looks).find(m_caller);
    if (!index || index->isParameter(m_call.site))
        return false;
    for (ValueID seen = m_call.site;;) {
        ValueID user = index->onlyUsingValue(seen);
        if (!user || user.index() != seen.index() + 1)
            return false;
        if (index->opcodeOf(user) == Opcode::Await)
            return true;
        if (!opcodesThatStateTypes.contains(index->opcodeOf(user)))
            return false;
        seen = user;
    }
}

SlotOfContents CallInLinkCalls::slotOfField(const BuiltinTypePart& part, uint16_t fieldIndex)
{
    if (part.kind != BuiltinTypePartKind::Layout || m_builtins.at(part.layout).isTuple || fieldIndex >= m_builtins.at(part.layout).fields.size())
        return ContentsSlot::Element;
    return SlotOfContents::under(m_program.keyOf(m_builtins.at(part.layout).fields[fieldIndex].name));
}

uint32_t CallInLinkCalls::lengthWrittenForView(const RowCall& call) const
{
    if (m_shared || call.isCalledByBuiltin || !m_call.isConstruction || m_call.passesList || m_call.passesOnGatheredArguments || m_call.numberOfArguments != 1 || m_flow.testRuns().isTestRun())
        return 0;
    if (m_call.lengthOfWrittenList)
        return m_call.lengthOfWrittenList;
    const ThinSummary& summary = m_flow.summaries.at(m_caller);
    uint32_t given = summary.arguments.span()[m_call.firstArgument];
    WrittenUnit written = given != noSummaryNode && summary.nodes[given].kind == SummaryNode::Kind::Constant ? WrittenUnit::fromBits(summary.nodes[given].second) : WrittenUnit();
    return written.kind() == WrittenUnit::Kind::Integer && written.integer() > 0 ? static_cast<uint32_t>(written.integer()) : 0;
}

uint32_t CallInLinkCalls::numberOfArgumentsWrittenFor(const RowCall& call, BuiltinTypeID held) const
{
    if (m_shared || call.isCalledByBuiltin || call.passed.list || !held || m_builtins.at(held).parts.size() != 1)
        return 0;
    const BuiltinTypePart& part = m_builtins.at(held).parts[0];
    if (part.kind != BuiltinTypePartKind::Operand || part.operand->kind() != BuiltinOperand::Kind::ArgumentsFrom || call.passed.arguments.size() <= part.operand->argumentIndex())
        return 0;
    return static_cast<uint32_t>(call.passed.arguments.size() - part.operand->argumentIndex());
}

void CallInLinkCalls::passOnPrimitivesThatFillMarkedWord(RowCall& call, std::span<const BuiltinSlotContents> listed)
{
    auto wordOfKind = [&](BuiltinWordOfCell kind) {
        return std::ranges::find_if(listed, [&](const BuiltinSlotContents& contents) { return m_builtins.at(contents.slot).wordOfCell == kind; });
    };
    auto followed = wordOfKind(BuiltinWordOfCell::Followed);
    auto marked = wordOfKind(BuiltinWordOfCell::MarkedWhereFilled);
    uint32_t numberWritten = followed != listed.end() && marked != listed.end() ? numberOfArgumentsWrittenFor(call, marked->type) : 0;
    if (!numberWritten)
        return;
    size_t first = m_builtins.at(marked->type).parts[0].operand->argumentIndex();
    forEachType(valuesOfType(call, followed->type), [&](const LinkType& target, const Source*) {
        if (target.functions != LinkType::Functions::Closure || !m_program.at(target.function).hasBody())
            return;
        const Function& function = m_program.at(target.function);
        uint32_t numberTaken = std::min(numberWritten, function.numberOfPositionalParameters());
        for (uint32_t position = 0; position < numberTaken; ++position) {
            FlowNodeID to = m_flow.parameter(target.function, function.firstParameterIndex() + position);
            const Values& bound = call.passed.arguments[first + position];
            for (const Source& source : bound.sources) {
                if (!source.passes)
                    m_flow.addEdge(source.node, to, typeOfPrimitives(Primitive::All));
            }
            if (bound.made.primitives)
                m_flow.arrive(to, primitivesForLinkCalls(bound.made.primitives));
        }
    });
}

bool CallInLinkCalls::conversionsMayRunCode(RowCall& call)
{
    return std::ranges::any_of(call.row->parameters, [&](const BuiltinParameter& parameter) {
        if (parameter.conversion == BuiltinConversion::None || parameter.operand == BuiltinOperand::receiver())
            return false;
        bool mayRunCode = false;
        forEachType(valuesAt(call, parameter.operand), [&](const LinkType& type, const Source*) {
            mayRunCode |= type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None;
        });
        return mayRunCode;
    });
}

void CallInLinkCalls::handHolesOn(RowCall& call, SiteID made)
{
    BuiltinID copier = m_builtins.canonicalID(call.builtin);
    auto isOf = [&](BuiltinID member) { return copier == m_builtins.canonicalID(member); };
    bool isMapped = isOf(Builtins::ArrayPrototypeMap);
    bool isJoined = isOf(Builtins::ArrayPrototypeConcat);
    if (!isMapped && !isJoined && !isOf(Builtins::ArrayPrototypeSlice) && !isOf(Builtins::ArrayPrototypeSplice))
        return;
    if (m_flow.isMarked(made, SlotOfContents::mayHaveHoles()))
        return;
    m_flow.noteListIsCopied(made);
    if (call.passed.list || call.isCalledByBuiltin || conversionsMayRunCode(call)) {
        m_flow.markSite(made, SlotOfContents::mayHaveHoles());
        return;
    }
    auto handOn = [&](const Values& values) {
        forEachType(values, [&](const LinkType& type, const Source*) {
            bool isList = type.classes == LinkType::Classes::Site && type.builtinClass == BuiltinClasses::Array && !m_flow.hasEscaped(type.site);
            if (isList && m_flow.isCreatedWithoutHoles(type.site)) {
                FlowNodeID mayHaveHoles = m_flow.contents(made, SlotOfContents::mayHaveHoles());
                m_flow.addEdge(m_flow.contents(type.site, SlotOfContents::mayHaveHoles()), mayHaveHoles);
                m_flow.addEdge(m_flow.contents(type.site, SlotOfContents::noLongerAsMade()), mayHaveHoles);
                m_flow.addEdge(m_flow.contents(type.site, SlotOfContents::mayHaveNumberNames()), mayHaveHoles);
                if (isMapped)
                    m_flow.addEdge(m_flow.contents(type.site, SlotOfContents::mayLoseElements()), mayHaveHoles);
            } else if (isList || type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.objects != LinkType::Objects::None || type.classes != LinkType::Classes::None)
                m_flow.markSite(made, SlotOfContents::mayHaveHoles());
        });
    };
    handOn(*call.passed.receiver);
    if (isJoined) {
        for (const Values& argument : call.passed.arguments)
            handOn(argument);
    }
}

void CallInLinkCalls::addInstanceOf(RowCall& call, const BuiltinTypePart& part, BuiltinClassID made, std::span<const BuiltinSlotContents> listed, bool isResult, Values& values, bool nobodyElseHoldsIt)
{
    SiteID site = siteOf(part, made, isResult && nobodyElseHoldsIt && !call.isCalledByBuiltin);
    if (isResult && made == BuiltinClasses::Array && !part.pristine.contains(PristineAspects::Dense) && call.passed.receiver && !m_makesWhatRowsMakeAtOneSiteOfProgram && !m_flow.hasEscaped(site))
        handHolesOn(call, site);
    if (TypedArrayType view = m_builtins.typedArrayTypeOf(made); isResult && view != NotTypedArray && view != TypeDataView) {
        uint32_t written = lengthWrittenForView(call);
        if (written && AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::OverstateTypedArrayLength) && !m_flow.isInLibraryCode())
            ++written;
        m_flow.noteLengthIsWritten(site, written);
    }
    add(values, m_flow.typeOf(site));
    bool isIteratorOverOperand = isResult && call.row->result.iteratedOperand;
    for (BuiltinSlotID id : m_builtins.at(made).slots) {
        const BuiltinSlot& slot = m_builtins.at(id);
        auto contents = std::ranges::find(listed, id, &BuiltinSlotContents::slot);
        if (contents == listed.end() && isResult && someRecordFillsResultSlotForLinkCalls(*call.row, id))
            continue;
        if (contents == listed.end() && isIteratorOverOperand && slot.holdsStepValues)
            continue;
        if (slot.wordOfCell == BuiltinWordOfCell::MarkedWhereFilled && contents != listed.end())
            m_flow.noteLengthIsWritten(site, numberOfArgumentsWrittenFor(call, contents->type));
        bool isInjectedAsHoldingNull = isResult && m_program.builtinInjectedAsHoldingNull == call.row->builtin.index();
        storeInContents(site, m_flow.slotOf(slot), isInjectedAsHoldingNull ? valuesOf(primitivesForLinkCalls(Primitive::Null)) : valuesOfType(call, contents == listed.end() ? slot.holds : contents->type), true);
    }
    passOnPrimitivesThatFillMarkedWord(call, listed);
}

void CallInLinkCalls::addRecordOf(RowCall& call, const BuiltinTypePart& part, bool isResult, Values& values)
{
    const BuiltinLayout& layout = m_builtins.at(part.layout);
    SiteID site = siteOf(part, layout.isTuple ? BuiltinClasses::Array : BuiltinClasses::Object);
    if (!layout.isTuple)
        m_flow.noteRecordsAreMadeAt(site, part.layout);
    add(values, m_flow.typeOf(site));
    for (uint16_t index = 0; index < layout.fields.size(); ++index) {
        if (fieldOfResultHoldsOnlyWhatIsStoredForLinkCalls(*call.row, index, !isResult))
            continue;
        storeInContents(site, slotOfField(part, index), valuesOfType(call, layout.fields[index].type));
    }
}

void CallInLinkCalls::addContentsOf(const LinkType& holder, const Source* source, Values& contents, NamesRead names)
{
    if (holder.classes == LinkType::Classes::None)
        return;
    bool isView = holder.builtinClass && m_builtins.typedArrayTypeOf(holder.builtinClass) != NotTypedArray;
    if (isView)
        add(contents, elementsOfViewForLinkCalls(m_builtins.typedArrayTypeOf(holder.builtinClass)));
    if (holder.builtinClass && holder.builtinClass == primitiveClassOf(Primitive::String))
        add(contents, primitivesForLinkCalls(Primitive::String));
    for (ContentsSlot slot : allMembersOf<ContentsSlot>) {
        if (!isView || slot != ContentsSlot::Element)
            add(contents, m_flow.contentsOf(holder, slot), beyond(source));
    }
    bool holdsIndexesApart = names == NamesRead::ThoseThatMayBeIndexes && (holder.builtinClass == BuiltinClasses::Arguments || holder.builtinClass == BuiltinClasses::Array);
    add(contents, m_flow.contentsOf(holder, holdsIndexesApart ? SlotOfContents::underUnknownNames() : SlotOfContents::underAnyName()), beyond(source));
}

ValuesInLinkCalls CallInLinkCalls::contentsOf(const Values& holders, NamesRead names)
{
    Values contents;
    forEachType(holders, [&](const LinkType& holder, const Source* source) {
        addContentsOf(holder, source, contents, names);
        if (holder.primitives & LinkType::closed(Primitive::AnyString))
            add(contents, primitivesForLinkCalls(Primitive::String));
        if (holder.objects == LinkType::Objects::None && holder.functions == LinkType::Functions::None && !holder.hasUnknownObjects && !holder.builtin)
            return;
        if (ownValuesAreListed(holder)) {
            forEachLayoutWithListedOwnValues(holder, [&](LayoutID layout) {
                for (FlowNodeID node : m_flow.nodesWithOwnValuesOf(layout))
                    add(contents, node, beyond(source));
            });
            return;
        }
        escape(holder, source);
        add(contents, LinkType::anything());
    });
    return contents;
}

bool CallInLinkCalls::isViewOfKnownKind(const BuiltinSlot& slot, const LinkType& holder) const
{
    return slot.contentsSlot && holder.classes != LinkType::Classes::None && holder.builtinClass && m_builtins.typedArrayTypeOf(holder.builtinClass) != NotTypedArray;
}

bool CallInLinkCalls::isSlotOfViewsAlone(const BuiltinSlot& slot, const Values& holders)
{
    bool someHolderIsView = false;
    bool everyObjectIsView = true;
    forEachType(holders, [&](const LinkType& holder, const Source*) {
        bool isObject = holder.classes != LinkType::Classes::None || holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.hasUnknownObjects || holder.builtin;
        someHolderIsView |= isViewOfKnownKind(slot, holder);
        everyObjectIsView &= !isObject || isViewOfKnownKind(slot, holder);
    });
    return someHolderIsView && everyObjectIsView;
}

void CallInLinkCalls::addContentsOfSlot(RowCall& call, const Values& holders, BuiltinSlotID id, Values& values)
{
    const BuiltinSlot& slot = m_builtins.at(id);
    bool isOfViewsAlone = isSlotOfViewsAlone(slot, holders);
    if (!isOfViewsAlone)
        add(values, primitivesForLinkCalls(Primitive::Undefined));
    if (!isOfViewsAlone && slot.holds && !m_builtins.at(slot.holds).isAnything())
        addValuesOfType(call, slot.holds, false, values);

    forEachType(holders, [&](const LinkType& holder, const Source* source) {
        if (isViewOfKnownKind(slot, holder))
            add(values, elementsOfViewForLinkCalls(m_builtins.typedArrayTypeOf(holder.builtinClass)));
        else if (holder.classes != LinkType::Classes::None)
            add(values, m_flow.contentsOf(holder, m_flow.slotOf(slot)), beyond(source));
        if (holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.hasUnknownObjects || holder.builtin)
            add(values, LinkType::anything());
    });
}

ValuesInLinkCalls CallInLinkCalls::stepValuesOf(IterationKind kind, const Values& iterated)
{
    Values keys;
    Values held;
    auto isIteratorAsMade = [&](const LinkType& holder, const Source* source) {
        auto holdsNothing = [&](SlotOfContents slot) {
            FlowNodeID contents = m_flow.contentsOf(holder, slot);
            return watch(contents, beyond(source)) && heldAt(contents).isNever();
        };
        return holdsNothing(SlotOfContents::underUnknownNames()) && holdsNothing(SlotOfContents::under(m_program.keyOf(BuiltinNames::next))) && holdsNothing(SlotOfContents::under(m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::Iterator))));
    };
    forEachType(iterated, [&](const LinkType& holder, const Source* source) {
        if (holder.primitives & Primitive::AnyString) {
            add(keys, primitivesForLinkCalls(Primitive::Integer));
            add(held, primitivesForLinkCalls(Primitive::String));
        }
        if (holder.classes == LinkType::Classes::Site) {
            auto findSlot = [&](ContentsSlot wanted) {
                BuiltinSlotID found;
                forEachClassAtOrAbove(holder.builtinClass, [&](BuiltinClassID current) {
                    if (!found)
                        found = m_builtins.findSlot(current, wanted);
                });
                return found;
            };
            bool isTypedArray = m_builtins.typedArrayTypeOf(holder.builtinClass) != NotTypedArray;
            BuiltinSlotID element = findSlot(ContentsSlot::Element);
            auto stepValues = std::ranges::find_if(m_builtins.at(holder.builtinClass).slots, [&](BuiltinSlotID slot) {
                return m_builtins.at(slot).holdsStepValues;
            });
            if (findSlot(ContentsSlot::Key) && findSlot(ContentsSlot::Value)) {
                add(keys, m_flow.contentsOf(holder, ContentsSlot::Key), beyond(source));
                add(held, m_flow.contentsOf(holder, ContentsSlot::Value), beyond(source));
            } else if (isTypedArray) {
                add(keys, primitivesForLinkCalls(Primitive::Integer));
                add(held, elementsOfViewForLinkCalls(m_builtins.typedArrayTypeOf(holder.builtinClass)));
            } else if (element) {
                bool isUnderIndexes = m_builtins.at(element).heap == AbstractHeap::Elements;
                if (isUnderIndexes)
                    add(keys, primitivesForLinkCalls(Primitive::Integer));
                else
                    add(keys, m_flow.contentsOf(holder, ContentsSlot::Element), beyond(source));
                add(held, m_flow.contentsOf(holder, ContentsSlot::Element), beyond(source));
                if (isUnderIndexes)
                    add(held, primitivesForLinkCalls(Primitive::Undefined));
            } else if (stepValues != m_builtins.at(holder.builtinClass).slots.end() && isIteratorAsMade(holder, source)) {
                FlowNodeID contents = m_flow.contentsOf(holder, m_flow.slotOf(*stepValues));
                add(keys, contents, beyond(source));
                add(held, contents, beyond(source));
            } else {
                escape(holder, source);
                add(keys, LinkType::anything());
                add(held, LinkType::anything());
            }
        }
        if (holder.classes == LinkType::Classes::Mixed || holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.hasUnknownObjects || holder.builtin) {
            escape(holder, source);
            add(keys, LinkType::anything());
            add(held, LinkType::anything());
        }
    });

    switch (kind) {
    case IterationKind::Keys:
        return keys;
    case IterationKind::Values:
        return held;
    case IterationKind::Entries:
        break;
    }
    SiteID pair = m_flow.siteOf(LinkFlow::MakerOfSite::PairAtCall, m_number, 0, BuiltinClasses::Array);
    storeInContents(pair, ContentsSlot::Element, keys);
    storeInContents(pair, ContentsSlot::Element, held);
    return valuesOf(m_flow.typeOf(pair));
}

void CallInLinkCalls::addIteratedValues(const Values& iterables, Values& values)
{
    Values withKeys;
    Values others;
    forEachType(iterables, [&](const LinkType& iterable, const Source* source) {
        bool hasKeys = false;
        if (iterable.builtinClass) {
            forEachClassAtOrAbove(iterable.builtinClass, [&](BuiltinClassID current) {
                hasKeys |= !!m_builtins.findSlot(current, ContentsSlot::Key);
            });
        }
        bool handsOutWhatNoNodeHolds = hasKeys || m_builtins.typedArrayTypeOf(iterable.builtinClass) != NotTypedArray;
        if (iterable.classes == LinkType::Classes::Site && handsOutWhatNoNodeHolds)
            add(values, m_flow.contentsOf(iterable, SlotOfContents::underUnknownNames()), beyond(source));
        if (hasKeys && iterable.primitives & Primitive::AnyString)
            add(values, primitivesForLinkCalls(Primitive::String));
        if (hasKeys && (iterable.objects != LinkType::Objects::None || iterable.functions != LinkType::Functions::None || iterable.hasUnknownObjects || iterable.builtin))
            add(values, LinkType::anything());
        Values& list = hasKeys ? withKeys : others;
        if (source)
            add(list, source->node, source->distance, source->passes);
        else
            add(list, iterable);
    });
    if (!withKeys.isEmpty())
        add(values, stepValuesOf(IterationKind::Entries, withKeys));
    if (!others.isEmpty())
        add(values, stepValuesOf(IterationKind::Values, others));
}

ValuesInLinkCalls CallInLinkCalls::valuesIteratedOf(const Values& iterables)
{
    Values iterated;
    addIteratedValues(iterables, iterated);
    return iterated;
}

void CallInLinkCalls::addResultsOfCallbacks(RowCall& call, BuiltinOperand operand, bool isResultAsItIsAnswered, bool containsCallbackResults, Values& values)
{
    auto usesLayAnswers = [&](const BuiltinCallback& callback) {
        if (callback.callee != operand)
            return true;
        if (isResultAsItIsAnswered)
            return callback.resultUse == BuiltinCallbackResultUse::Returned;
        return callback.resultUse == BuiltinCallbackResultUse::Stored && callback.resultTo.kind == BuiltinPlaceKind::ResultSlot && m_builtins.at(callback.resultTo.slot).owner == BuiltinClasses::Array;
    };
    bool usesHaveLaidAnswersOfBuiltins = (isResultAsItIsAnswered || containsCallbackResults) && std::ranges::all_of(call.row->flow.callbacks, usesLayAnswers);
    bool constructs = std::ranges::any_of(call.row->flow.callbacks, [&](const BuiltinCallback& callback) {
        return callback.callee == operand && callback.constructs;
    });
    forEachType(valuesAt(call, operand), [&](const LinkType& callee, const Source* source) {
        if (usesHaveLaidAnswersOfBuiltins && callee.builtin && callee == builtinForLinkCalls(callee.builtin))
            return;
        switch (callee.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            if (constructs || !m_program.at(callee.function).hasBody() || !isEnteredByCallsAloneForLinkCalls(m_program.at(callee.function).kind)) {
                if (constructs)
                    escape(callee, source);
                add(values, LinkType::anything());
            } else
                add(values, resultOf(callee.function, 0, Passed { }, { }, beyond(source)));
            break;
        case LinkType::Functions::Constructors:
            if (constructs) {
                escape(callee, source);
                add(values, LinkType::anything());
            }
            break;
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            m_flow.noteCallOfLostFunctions(callee);
            add(values, m_flow.resultOfLostFunctions(callee), beyond(source));
            break;
        }
        if (callee.builtin || callee.hasUnknownObjects || callee.objects != LinkType::Objects::None || callee.classes != LinkType::Classes::None)
            add(values, LinkType::anything());
    });
}

bool CallInLinkCalls::hasKnownPrototypeChain(LayoutID layout, ClassSide side) const
{
    const Layout& record = m_program.at(layout);
    bool isBelowValue = record.has(LayoutFlags::HasHeritage) && !record.base && !record.writtenBuiltinBase;
    return !isBelowValue && !record.has(LayoutFlags::IsOfCreatedObject) && !m_flow.namesMayBeIncomplete(layout, side) && !m_flow.isBelowFollowedPrototype(layout);
}

LinkType CallInLinkCalls::prototypeOfObjectsOf(LayoutID layout) const
{
    if (m_program.at(layout).kind == LayoutKind::Record && m_flow.hasPristinePrototypes(layout))
        return m_flow.unseenObjectsBelow(m_builtins.canonicalID(Builtins::ObjectPrototype), m_caller);
    return prototypeForLinkCalls(layout);
}

void CallInLinkCalls::addPrototypes(const Values& objects, Values& values)
{
    add(values, primitivesForLinkCalls(Primitive::Null));
    forEachType(objects, [&](const LinkType& object, const Source* source) {
        bool isInSight = object.classes == LinkType::Classes::None && !object.holdsLostBuiltins;
        switch (object.objects) {
        case LinkType::Objects::None:
            break;
        case LinkType::Objects::Layout:
            add(values, prototypeOfObjectsOf(object.layout));
            break;
        case LinkType::Objects::Set:
            for (LayoutID member : m_flow.layoutsOfSet(object))
                add(values, prototypeOfObjectsOf(member));
            break;
        case LinkType::Objects::Family:
            add(values, prototypesForLinkCalls(object.family));
            break;
        case LinkType::Objects::Prototype:
            if (LayoutID base = m_program.at(object.layout).base)
                add(values, prototypeForLinkCalls(base));
            isInSight &= hasKnownPrototypeChain(object.layout, ClassSide::Prototype);
            break;
        case LinkType::Objects::Prototypes: {
            std::span<const LayoutID> members = m_program.types.listOf(m_flow.families.layoutsOf(object.family));
            add(values, prototypesForLinkCalls(object.family));
            for (LayoutID member : members) {
                isInSight &= hasKnownPrototypeChain(member, ClassSide::Prototype);
                if (LayoutID base = m_program.at(member).base; base && !std::ranges::binary_search(members, base))
                    add(values, prototypeForLinkCalls(base));
            }
            break;
        }
        case LinkType::Objects::Lost:
            isInSight = false;
            break;
        }
        LayoutID constructed;
        switch (object.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            constructed = m_flow.layoutOfConstructorFunction(object.function);
            if (!constructed) {
                constructed = m_flow.classOfConstructor(object.function);
                isInSight &= constructed && hasKnownPrototypeChain(constructed, ClassSide::Static);
            }
            if (LayoutID base = constructed ? m_program.at(constructed).base : LayoutID()) {
                if (FunctionID above = m_program.at(base).constructor)
                    add(values, closuresForLinkCalls(above));
            }
            break;
        case LinkType::Functions::Constructors:
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            isInSight = false;
            break;
        }
        if (object.builtin && m_builtins.at(m_builtins.canonicalID(object.builtin)).prototypeLink == BuiltinPrototypeLink::Unknown)
            isInSight = false;
        if (!isInSight)
            escape(object, source);
        add(values, m_flow.builtinsAbove(object, m_caller));
    });
    add(values, unseenObjectsForLinkCalls());
}

void CallInLinkCalls::settle(Values& values, bool rowHasJustMadeThem)
{
    Questions questions;
    addKeys(questions, m_builtins.keysReadBy(Protocol::Thenable));
    Questions ofPromise = questions;
    ofPromise.membersOfTableAnswerNothing = true;

    Values settled;
    auto askForThen = [&](const Values& subjects, const Questions& asked) {
        if (ask(subjects, asked))
            add(settled, LinkType::anything());
    };
    auto settleWhatNobodyHasSeen = [&](const LinkType& type) {
        if (type.hasUnknownObjects)
            add(settled, type.hasDirectUnknownObjects() ? LinkType::anything() : LinkType::anything().asEscapeOnly());
    };
    forEachType(values, [&](const LinkType& type, const Source* source) {
        bool mayBePromise = type.classes == LinkType::Classes::Mixed || (type.builtinClass && m_builtins.isSameOrSubclassOf(type.builtinClass, BuiltinClasses::Promise));
        if (!mayBePromise) {
            if (source)
                add(settled, source->node, source->distance, source->passes);
            else
                add(settled, type);
            Questions asked = questions;
            asked.ownPropertiesAnswerNothing = rowHasJustMadeThem && !source && type.classes == LinkType::Classes::Site;
            askForThen(source ? valuesAtSource(*source) : valuesOf(type), asked);
            settleWhatNobodyHasSeen(type);
            return;
        }

        add(settled, m_flow.contentsOf(type, ContentsSlot::Value), beyond(source));
        LinkType others = type;
        if (type.classes != LinkType::Classes::Mixed) {
            others.dropInstances();
            askForThen(valuesOf(instancesAmongForLinkCalls(type)), ofPromise);
        }
        add(settled, others);
        askForThen(valuesOf(others), questions);
        settleWhatNobodyHasSeen(others);
    });
    values = WTF::move(settled);
}

bool CallInLinkCalls::isValueOfPromise(BuiltinClassID holder, SlotOfContents slot) const
{
    return slot == SlotOfContents(ContentsSlot::Value) && (!holder || m_builtins.isSameOrSubclassOf(holder, BuiltinClasses::Promise));
}

void CallInLinkCalls::storeAt(RowCall& call, const BuiltinPlace& place, const Values& values)
{
    if (values.isEmpty())
        return;
    switch (place.kind) {
    case BuiltinPlaceKind::None:
        return;
    case BuiltinPlaceKind::Result:
        add(call.result, values);
        return;
    case BuiltinPlaceKind::ResultSlot: {
        bool leavesAsWell = false;
        bool everyObjectIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart& part) {
            storeInContents(made, m_flow.slotOf(place.slot), values);
            leavesAsWell |= m_builtins.at(part.builtinClass).isFollowedBesideUnseenObject;
        });
        if (!everyObjectIsFollowed || (leavesAsWell && !isBoundAsReceiverThatNoRowSpeaksOf(call, place)))
            escape(values);
        return;
    }
    case BuiltinPlaceKind::ResultField: {
        bool everyObjectIsFollowed = forEachSiteOfResultField(call, place, [&](SiteID made, const BuiltinTypePart& part) {
            storeInContents(made, slotOfField(part, place.fieldIndex), values);
        });
        if (!everyObjectIsFollowed)
            escape(values);
        return;
    }
    case BuiltinPlaceKind::Slot:
        storeInSlot(valuesAt(call, *place.operand), place.slot, values);
        return;
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::Prototype:
        escape(valuesAt(call, *place.operand));
        escape(values);
        return;
    case BuiltinPlaceKind::NativeCell:
        if (FlowNodeID kept = nodeOfFunctionsKeptIn(place.nativeCell)) {
            flowInto(values, kept);
            return;
        }
        escape(values);
        return;
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::Member:
        escape(values);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CallInLinkCalls::isBoundAsReceiverThatNoRowSpeaksOf(const RowCall& call, const BuiltinPlace& place)
{
    if (m_flow.slotOf(place.slot) != m_flow.slotOf(m_builtins.findSlot(BuiltinClasses::BoundFunction, ContentsSlot::Key)))
        return false;
    Values targets = valuesAt(call, BuiltinOperand::receiver());
    bool noRowSpeaksOfIt = !targets.isEmpty();
    forEachType(targets, [&](const LinkType& target, const Source*) {
        LinkType builtinAlone;
        builtinAlone.builtin = target.builtin;
        noRowSpeaksOfIt &= target.builtin && target == builtinAlone && m_builtins.ignoresReceiver(target.builtin);
    });
    return noRowSpeaksOfIt;
}

void CallInLinkCalls::storeInContents(SiteID site, SlotOfContents slot, const Values& values, bool rowHasJustMadeThem)
{
    if (values.isEmpty())
        return;
    switch (m_flow.wordOfCell(slot)) {
    case BuiltinWordOfCell::None:
    case BuiltinWordOfCell::Followed:
        break;
    case BuiltinWordOfCell::Unfollowed:
        return;
    case BuiltinWordOfCell::MarkedWhereFilled:
        m_flow.arrive(m_flow.contents(site, slot), primitivesForLinkCalls(Primitive::Undefined));
        return;
    }
    if (!isValueOfPromise(m_flow.classOf(site), slot)) {
        flowInto(values, m_flow.contents(site, slot));
        return;
    }
    Values settled = values;
    settle(settled, rowHasJustMadeThem);
    flowInto(settled, m_flow.contents(site, slot));
}

void CallInLinkCalls::storeInSlot(const Values& holders, BuiltinSlotID id, const Values& values)
{
    SlotOfContents slot = m_flow.slotOf(id);
    forEachType(holders, [&](const LinkType& holder, const Source*) {
        if (holder.classes != LinkType::Classes::None) {
            Values stored = values;
            if (isValueOfPromise(holder.builtinClass, slot))
                settle(stored);
            flowInto(stored, m_flow.contentsOf(holder, slot));
            if (slot == SlotOfContents(ContentsSlot::Element))
                m_flow.tellWrittenListIsChanged(holder);
        }
        if (holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.hasUnknownObjects || holder.builtin)
            escape(values);
    });
}

bool CallInLinkCalls::mayBeMissingAt(const RowCall& call, const BuiltinPlace& place)
{
    switch (place.kind) {
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::NativeMemory:
        return false;
    case BuiltinPlaceKind::NewValue: {
        if (!place.newValueType)
            return true;
        const BuiltinType& type = m_builtins.at(place.newValueType);
        if (type.isAnything())
            return false;
        return type.mayBeObject() || type.isNever() || type.primitives.contains(BuiltinPrimitive::NotPassed);
    }
    case BuiltinPlaceKind::Slot:
        return !(isSlotOfViewsAlone(m_builtins.at(place.slot), valuesAt(call, *place.operand)));
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
    case BuiltinPlaceKind::NativeCell:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CallInLinkCalls::isFollowed(BuiltinClassID builtinClass) const
{
    return m_builtins.at(builtinClass).isFollowedBesideUnseenObject || (!m_builtins.at(builtinClass).hasClosureInstances && !isFunctionClass(m_builtins, builtinClass));
}

bool CallInLinkCalls::mayBeListWithLayout(const LinkType& type)
{
    auto isWrittenBelowArray = [&](LayoutID layout) {
        BuiltinClassID base = m_program.at(layout).writtenBuiltinBase;
        return base && m_builtins.isSameOrSubclassOf(base, BuiltinClasses::Array);
    };
    bool mayBe = false;
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        mayBe = isWrittenBelowArray(type.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(type))
            mayBe |= isWrittenBelowArray(member);
        break;
    case LinkType::Objects::Family:
        forEachLayoutOf(type.family, [&](LayoutID member) {
            mayBe |= isWrittenBelowArray(member);
        });
        break;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        mayBe = true;
        break;
    case LinkType::Objects::Lost:
        for (uint32_t index = 0; index < m_program.layouts.size() && !mayBe; ++index)
            mayBe = isWrittenBelowArray(LayoutID(index));
        break;
    }
    return mayBe;
}

void CallInLinkCalls::addKeys(Questions& questions, std::span<const BuiltinName> names)
{
    for (BuiltinName name : names) {
        if (PropertyKeyID key = name ? m_program.keyOf(name) : PropertyKeyID())
            questions.keys.appendIfNotContains(key);
    }
}

void CallInLinkCalls::addKeysAnsweredByProgramAlone(Questions& questions, std::span<const BuiltinName> names)
{
    for (BuiltinName name : names) {
        if (PropertyKeyID key = name ? m_program.keyOf(name) : PropertyKeyID())
            questions.keysAnsweredByProgramAlone.appendIfNotContains(key);
    }
}

void CallInLinkCalls::addConversion(Questions& questions)
{
    questions.convertToPrimitive = true;
    for (BuiltinName name : m_builtins.keysReadBy(Protocol::ToPrimitive)) {
        if (PropertyKeyID key = name ? m_program.keyOf(name) : PropertyKeyID())
            questions.keysOfConversion.appendIfNotContains(key);
    }
    if (PropertyKeyID key = m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::ToStringTag)))
        questions.keysOfConversion.appendIfNotContains(key);
}

ALWAYS_INLINE const StateOfLinkCalls::WhatTableSaysOfParameter& CallInLinkCalls::whatTableSaysOf(const BuiltinOverload& row, const BuiltinParameter& parameter)
{
    static_assert(std::atomic<const StateOfLinkCalls::WhatTableSaysOfParameter*>::is_always_lock_free);
    bool isReceiver = &parameter == row.receiver.get();
    ptrdiff_t placeAmongParameters = &parameter - row.parameters.data();
    RELEASE_ASSERT(isReceiver || (placeAmongParameters >= 0 && static_cast<size_t>(placeAmongParameters) < row.parameters.size()));
    size_t place = isReceiver ? 0 : 1 + static_cast<size_t>(placeAmongParameters);
    size_t numberOfRow = static_cast<size_t>(&row - &m_builtins.at(BuiltinOverloadID(0)));
    RELEASE_ASSERT(numberOfRow < m_builtins.overloadCount());
    const StateOfLinkCalls::WhatTableSaysOfParameter** said = &m_state.whatTableSaysAtPlacesOfRows[numberOfRow * m_state.placesOfRow + place];
    if (const StateOfLinkCalls::WhatTableSaysOfParameter* known = WTF::atomicLoad<MemoryOrder::Acquire>(said)) [[likely]]
        return *known;
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    if (const StateOfLinkCalls::WhatTableSaysOfParameter* known = WTF::atomicLoad<MemoryOrder::Relaxed>(said))
        return *known;
    auto made = makeUnique<StateOfLinkCalls::WhatTableSaysOfParameter>();
    made->questions = questionsOf(parameter);
    made->recordsSpellOutEveryUse = recordsSpellOutEveryUseForLinkCalls(m_builtins, row, parameter);
    const StateOfLinkCalls::WhatTableSaysOfParameter* known = made.get();
    m_state.whatTableSaysOfParameters.append(WTF::move(made));
    WTF::atomicStore<MemoryOrder::Release>(said, known);
    return *known;
}

QuestionsInLinkCalls CallInLinkCalls::questionsOf(const BuiltinParameter& parameter)
{
    Questions questions;
    for (Protocol protocol : parameter.protocols) {
        switch (protocol) {
        case Protocol::OwnKeys:
            questions.listKeys = true;
            break;
        case Protocol::PrototypeChain:
            questions.readThroughPrototypes = true;
            break;
        case Protocol::OwnValues:
            questions.readOwnValues = true;
            break;
        case Protocol::Species:
            questions.readEverything = true;
            break;
        case Protocol::ToPrimitive:
            addConversion(questions);
            break;
        case Protocol::ToJSON:
            questions.callMembers = true;
            addKeys(questions, m_builtins.keysReadBy(protocol));
            break;
        case Protocol::HasInstance:
            addKeys(questions, m_builtins.keysReadBy(protocol));
            break;
        case Protocol::Iterate:
            questions.iterate = true;
            [[fallthrough]];
        case Protocol::Thenable:
            addKeysAnsweredByProgramAlone(questions, m_builtins.keysReadBy(protocol));
            break;
        case Protocol::Named:
            questions.readEverything |= parameter.namedMembers.empty();
            addKeysAnsweredByProgramAlone(questions, parameter.namedMembers);
            questions.askConstructorsOfInstances |= std::ranges::contains(parameter.namedMembers, BuiltinNames::constructor);
            break;
        case Protocol::CallsNamed:
            questions.callMembers = true;
            questions.readEverything |= parameter.calledMembers.empty();
            addKeys(questions, parameter.calledMembers);
            break;
        }
    }
    addKeysAnsweredByProgramAlone(questions, parameter.convertedMembers);
    PropertyKeyID toString = m_program.keyOf(BuiltinNames::toString);
    PropertyKeyID tag = m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::ToStringTag));
    if (toString && questions.keys.contains(m_program.keyOf(BuiltinNames::toLocaleString)))
        questions.keys.appendIfNotContains(toString);
    if (tag && questions.keys.contains(toString))
        questions.keysAnsweredByProgramAlone.appendIfNotContains(tag);
    questions.describeObjects = parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused);
    questions.onlyBoxedPrimitivesAreConverted = parameter.roles.contains(BuiltinOperandRole::OnlyBoxedPrimitivesAreConverted);
    questions.whatIsFoundIsOnlyInspected = parameter.roles.contains(BuiltinOperandRole::MembersAreOnlyInspected);
    return questions;
}

template<typename Functor>
void CallInLinkCalls::forEachLayoutWithListedOwnValues(const LinkType& type, const Functor& functor)
{
    if (type.objects == LinkType::Objects::Layout)
        functor(type.layout);
    else if (type.objects == LinkType::Objects::Set) {
        for (LayoutID member : m_flow.layoutsOfSet(type))
            functor(member);
    } else if (type.objects == LinkType::Objects::Family)
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach(functor);
}

bool CallInLinkCalls::ownValuesAreListed(const LinkType& type)
{
    if (type.functions != LinkType::Functions::None || type.hasUnknownObjects || type.builtin || (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Set && type.objects != LinkType::Objects::Family))
        return false;
    bool areListed = true;
    forEachLayoutWithListedOwnValues(type, [&](LayoutID layout) {
        areListed &= !m_flow.nodesWithOwnValuesOf(layout).isEmpty();
    });
    return areListed;
}

bool CallInLinkCalls::mayFindUnderNumbersAbove(const LinkType& type, const Source* source)
{
    uint8_t distance = source ? source->distance : 0;
    bool mayFind = false;
    forEachLayoutWithListedOwnValues(type, [&](LayoutID layout) {
        if (mayFind)
            return;
        mayFind = m_flow.classesAnswerReadsUnderNumbers(layout);
        FlowNodeID onPrototypes = mayFind ? FlowNodeID() : m_flow.foundOnPrototypesAboveUnderNumbers(layout);
        if (!onPrototypes)
            return;
        if (!watch(onPrototypes, static_cast<uint8_t>(distance + 1))) {
            mayFind = true;
            return;
        }
        LinkType found = heldAt(onPrototypes);
        mayFind = found != primitivesForLinkCalls(found.primitives & Primitive::Undefined);
    });
    return mayFind;
}

bool CallInLinkCalls::tableAloneAnswersFrom(BuiltinID builtin, const Questions& questions)
{
    Questions underNamesAlone;
    underNamesAlone.keysAnsweredByProgramAlone = questions.keysAnsweredByProgramAlone;
    underNamesAlone.whatIsFoundIsOnlyInspected = true;
    if (questions != underNamesAlone)
        return false;
    return std::ranges::all_of(questions.keysAnsweredByProgramAlone, [&](PropertyKeyID key) {
        Lookup outcome = findOnce(static_cast<uint64_t>(builtin.index()) << 32 | key.index(), [&] {
            return findAbove(builtin, key, MembersUnderKey(m_program, key));
        }).outcome;
        return outcome == Lookup::Found || outcome == Lookup::Absent;
    });
}

bool CallInLinkCalls::onlyNamesThatTableAnswersAreRead(BuiltinID builtin, const Questions& questions)
{
    Questions underNamesAlone;
    underNamesAlone.keys = questions.keys;
    underNamesAlone.keysAnsweredByProgramAlone = questions.keysAnsweredByProgramAlone;
    underNamesAlone.whatIsFoundIsOnlyInspected = questions.whatIsFoundIsOnlyInspected;
    if (questions != underNamesAlone)
        return false;
    auto outcomeUnder = [&](PropertyKeyID key) {
        return findOnce(static_cast<uint64_t>(builtin.index()) << 32 | key.index(), [&] {
            return findAbove(builtin, key, MembersUnderKey(m_program, key));
        }).outcome;
    };
    return std::ranges::all_of(questions.keys, [&](PropertyKeyID key) {
        Lookup outcome = outcomeUnder(key);
        return outcome == Lookup::Found || outcome == Lookup::Absent;
    }) && std::ranges::all_of(questions.keysAnsweredByProgramAlone, [&](PropertyKeyID key) {
        Lookup outcome = outcomeUnder(key);
        return outcome == Lookup::Absent || (outcome == Lookup::Found && questions.whatIsFoundIsOnlyInspected);
    });
}

bool CallInLinkCalls::mayAnswer(const LinkType& type, const Source* source, const Questions& questions, bool ownValuesAreFollowed)
{
    if (type.classes == LinkType::Classes::Mixed || (type.builtin && !tableAloneAnswersFrom(type.builtin, questions)))
        return true;
    bool holdsObjectsOfProgram = type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None;
    if (!holdsObjectsOfProgram && type.classes == LinkType::Classes::None)
        return false;
    if (questions.describeObjects || (holdsObjectsOfProgram && (questions.readEverything || (questions.readOwnValues && !ownValuesAreFollowed && !ownValuesAreListed(type)))))
        return true;
    if (questions.readOwnValues && questions.readThroughPrototypes && mayFindUnderNumbersAbove(type, source))
        return true;
    if ((questions.readEverything || questions.readOwnValues) && type.classes == LinkType::Classes::Site && !isAsClassMakesIt(instancesAmongForLinkCalls(type), { PristineAspects::Storage, PristineAspects::Names }, source))
        return true;

    uint8_t distance = source ? source->distance : 0;
    bool primitivesAnswerNothing = !questions.readOwnValues;
    for (PropertyKeyID key : questions.keys) {
        if (mayFind(type, key, distance, questions.membersOfTableAnswerNothing, primitivesAnswerNothing, questions.ownPropertiesAnswerNothing))
            return true;
    }
    for (PropertyKeyID key : questions.keysAnsweredByProgramAlone) {
        if (mayFind(type, key, distance, true, primitivesAnswerNothing))
            return true;
    }
    LinkType converted = questions.onlyBoxedPrimitivesAreConverted ? instancesAmongForLinkCalls(type) : type;
    if (questions.convertToPrimitive && converted.classes == LinkType::Classes::Site && !membersAboveInstancesAreWhatTableLists(converted.builtinClass))
        return true;
    for (PropertyKeyID key : questions.keysOfConversion) {
        if (mayFind(converted, key, distance, true, primitivesAnswerNothing))
            return true;
    }
    return false;
}

bool CallInLinkCalls::isDataDescriptorInSight(const Source* source) const
{
    if (!source || m_shared)
        return false;
    auto found = std::ranges::equal_range(m_summary.definitionsByDescriptorInOrderOfSites.span(), m_call.site.index(), { }, [&](uint32_t change) {
        return m_summary.changes[change].site.index();
    });
    return std::ranges::any_of(found, [&](uint32_t place) {
        const SummaryChange& change = m_summary.changes[place];
        if (change.other == noSummaryNode || m_flow.nodeOf(m_caller, change.other) != source->node)
            return false;
        if (change.laysValueInSight || change.getterEscapesOnlyThroughLink || change.accessorEscapesOnlyThroughLink)
            return true;
        Vector<FlowNodeID, 4> values;
        return change.key && !change.descriptorIsInSight && m_flow.descriptorsInSightLayValuesAlone(m_flow.typeAt(source->node), nullptr, values);
    });
}

bool CallInLinkCalls::isMapOfAccessorsAsWritten(const Values& values) const
{
    if (m_shared || values.sources.isEmpty() || !values.made.isNever())
        return false;
    return std::ranges::all_of(values.sources, [&](const Source& source) {
        bool hasEntry = false;
        for (const SummaryChange& change : m_summary.changes) {
            if (change.site != m_call.site || change.other == noSummaryNode || m_flow.nodeOf(m_caller, change.other) != source.node)
                continue;
            bool laysValueInSight = change.descriptorIsInSight && change.descriptorLaysNoAccessor;
            if (change.kind != SummaryChange::Kind::DefinesByDescriptor || !change.isEntryWrittenInMap || (!change.laysAccessorAsWritten && !laysValueInSight))
                return false;
            hasEntry = true;
        }
        return hasEntry;
    });
}

bool CallInLinkCalls::addConversionMethods(LayoutID id, PropertyKeyID key, uint8_t distance, Values& methods)
{
    if (findField(m_program, id, key))
        return false;
    if (!m_flow.isAbsentFromObjectPrototype(key)) {
        BuiltinClassID objectClass = BuiltinClasses::Object;
        BuiltinID above = m_builtins.at(objectClass).prototype;
        Lookup outcome = findOnce(static_cast<uint64_t>(above.index()) << 32 | key.index(), [&] {
            return findAbove(above, key, MembersUnderKey(m_program, key));
        }).outcome;
        if (outcome == Lookup::Unknown)
            return false;
    }
    for (const LinkFlow::FoundMember& member : m_flow.findMembers(id, ClassSide::Prototype, key)) {
        if (member.kind != MethodKind::Method)
            return false;
        add(methods, m_flow.member(member.classLayout, ClassSide::Prototype, member.index), distance);
    }
    add(methods, m_flow.foundBesideFieldsFor(id, key), distance);
    add(methods, m_flow.appendedField(id, key), distance);
    add(methods, m_flow.extraPropertyReadNode(id), distance);
    if (FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, key))
        add(methods, foundOnPrototypes, distance);
    return true;
}

bool CallInLinkCalls::callConversions(const LinkType& type, const Source* source, const Questions& questions)
{
    Questions conversion;
    addConversion(conversion);
    if (m_shared || m_stepThatWatches || questions != conversion || type.holdsLostBuiltins)
        return false;
    uint8_t distance = static_cast<uint8_t>((source ? source->distance : 0) + 1);
    PropertyKeyID tag = m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::ToStringTag));
    if (tag && mayFind(type, tag, distance, true, true))
        return false;

    Vector<LayoutID, 4> layouts;
    switch (type.objects) {
    case LinkType::Objects::Layout:
        layouts.append(type.layout);
        break;
    case LinkType::Objects::Set:
        layouts.append(m_flow.layoutsOfSet(type));
        break;
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach([&](LayoutID member) {
            layouts.append(member);
        });
        break;
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return false;
    }
    struct Conversion {
        LayoutID layout;
        bool takesHint { false };
        Values methods;
    };
    Vector<Conversion, 4> conversions;
    BuiltinName nameTakingHint = m_builtins.nameOf(WellKnownSymbol::ToPrimitive);
    for (LayoutID id : layouts) {
        const Layout& layout = m_program.at(id);
        bool isFollowed = (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance) && !layout.has(LayoutFlags::IsOfCreatedObject);
        if (!isFollowed || m_flow.escapedLayouts.get(id.index()) || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype))
            return false;
        for (BuiltinName name : m_builtins.keysReadBy(Protocol::ToPrimitive)) {
            PropertyKeyID key = name ? m_program.keyOf(name) : PropertyKeyID();
            if (!key)
                continue;
            Conversion found { id, name == nameTakingHint, { } };
            if (!addConversionMethods(id, key, distance, found.methods))
                return false;
            conversions.append(WTF::move(found));
        }
    }
    for (Conversion& found : conversions) {
        PassedMadeInLinkCalls passed;
        passed.receiver = valuesOf(objectsForLinkCalls(found.layout));
        if (found.takesHint)
            passed.appendArgument(valuesOf(primitivesForLinkCalls(Primitive::String)));
        callValues(found.methods, passed, true);
    }
    return true;
}

CallInLinkCalls::UsedByParts CallInLinkCalls::askByParts(const LinkType& type, const Source* source, const Questions& questions)
{
    bool readsMoreThanNames = questions.describeObjects || questions.readEverything || questions.readOwnValues || questions.readPrivateFields || questions.callMembers || questions.iterate || questions.listKeys || questions.readThroughPrototypes || questions.askConstructorsOfInstances || questions.areAskedByFormatter || questions.onlyFollow;
    if (m_shared || m_stepThatWatches || readsMoreThanNames || !questions.keys.isEmpty() || type.holdsLostBuiltins)
        return UsedByParts::No;

    LinkType objects;
    objects.objects = type.objects;
    objects.layout = type.layout;
    objects.family = type.family;
    LinkType closures;
    closures.functions = type.functions;
    closures.function = type.function;
    closures.constructorsOf = type.constructorsOf;
    LinkType builtin;
    builtin.builtin = type.builtin;
    UsedByParts asked = UsedByParts::NothingAnswers;
    for (const LinkType& part : { objects, closures, instancesAmongForLinkCalls(type), builtin }) {
        if (part.namesNothing())
            continue;
        if (!mayAnswer(part, source, questions, false) || (part.objects != LinkType::Objects::None && callConversions(part, source, questions)))
            continue;
        escape(part, nullptr);
        asked = UsedByParts::SomethingAnswers;
    }
    return asked;
}

bool CallInLinkCalls::ask(const Values& subjects, const Questions& questions)
{
    if (questions.isEmpty())
        return false;
    bool someMayAnswer = false;
    forEachType(subjects, [&](const LinkType& type, const Source* source) {
        if (isDataDescriptorInSight(source))
            return;
        tellWhatFormatterIsHanded(type, questions);
        tellWhichMembersNativeCodeCallsOffUnseenObjects(type, questions);
        if (questions.describeObjects && mayBePrintedWholeWhereItIsRefused(type, questions.describesLiteralMadeForCall))
            m_flow.note(LinkFlow::Note::ObjectHandedToFormatter, LinkFlow::walkedByFormatter);
        if (questions.iterate)
            tellWhatIterationCalls(type);
        UsedByParts asked = UsedByParts::NothingAnswers;
        if (mayAnswer(type, source, questions, false))
            asked = askByParts(type, source, questions);
        if (asked == UsedByParts::SomethingAnswers) {
            tellFormatterLosesSight(questions);
            someMayAnswer = true;
        }
        if (asked == UsedByParts::No) {
            tellFormatterLosesSight(questions);
            if (questions.describeObjects)
                m_flow.note(LinkFlow::Note::ObjectHandedToFormatter, LinkFlow::printedByCodeOfItsClass);
            escape(type, source);
            someMayAnswer = true;
            return;
        }
        if (questions.convertToPrimitive && !questions.onlyBoxedPrimitivesAreConverted && type.classes == LinkType::Classes::Site) {
            Questions ofContents;
            addConversion(ofContents);
            Values contents;
            LinkType instances = instancesAmongForLinkCalls(type);
            FlowNodeID elements = m_flow.contentsOf(instances, ContentsSlot::Element);
            if (m_listsConverted.contains(elements))
                return;
            m_listsConverted.append(elements);
            std::optional<Source> asIfAtHand;
            if (source) {
                asIfAtHand = *source;
                asIfAtHand->distance = 0;
                source = &*asIfAtHand;
            }
            if (type.builtinClass == BuiltinClasses::Arguments) {
                for (PropertyKeyID key : questions.keysAnsweredByProgramAlone)
                    add(contents, m_flow.contentsOf(instances, SlotOfContents::under(key)), beyond(source));
                add(contents, m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()), beyond(source));
            } else
                addContentsOf(instances, source, contents, NamesRead::ThoseThatMayBeIndexes);
            ++m_depthOfConversions;
            ask(contents, ofContents);
            if (!--m_depthOfConversions)
                m_listsConverted.shrink(0);
        }
    });
    return someMayAnswer;
}

void CallInLinkCalls::inspectWhatExpectedBuiltinCompares()
{
    bool isOfCallee = m_call.expectedBuiltin ? m_call.expectsCalleeItself : m_call.callee == SummaryCall::Callee::Value;
    BuiltinID expected = m_call.expectedBuiltin ? m_call.expectedBuiltin : m_call.builtin;
    if (!expected || (!isOfCallee && !m_call.key))
        return;
    expected = m_builtins.canonicalID(expected);
    Vector<BuiltinID, 2> called;
    if (isOfCallee)
        called.append(expected);
    else {
        called.append(findOwnMember(m_program, expected, m_call.key));
        for (const BuiltinOverload& row : m_builtins.overloadsOf(expected)) {
            if (!row.result.type)
                continue;
            for (const BuiltinTypePart& part : m_builtins.at(row.result.type).parts) {
                BuiltinID prototype = part.kind == BuiltinTypePartKind::Instance && part.builtinClass ? m_builtins.at(part.builtinClass).prototype : BuiltinID();
                if (prototype)
                    called.append(findOwnMember(m_program, prototype, m_call.key));
            }
        }
    }
    for (BuiltinID builtin : called) {
        if (!builtin)
            continue;
        RowCall call { .builtin = m_builtins.canonicalID(builtin), .mode = m_call.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, .passed = passedByCall() };
        for (const BuiltinOverload& row : m_builtins.overloadsOf(call.builtin)) {
            for (const BuiltinParameter& parameter : row.parameters) {
                if (parameter.roles.contains(BuiltinOperandRole::PrivateFieldsAreCompared))
                    inspect(valuesAt(call, parameter.operand), questionsOfComparisonOfPrivateFields());
            }
        }
    }
}

QuestionsInLinkCalls CallInLinkCalls::questionsOfComparisonOfPrivateFields()
{
    Questions questions;
    questions.readEverything = true;
    questions.readOwnValues = true;
    questions.readPrivateFields = true;
    return questions;
}

void CallInLinkCalls::inspect(const Values& values, const Questions& questions)
{
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    uint32_t inspection = 0;
    bool isAskedByLibrary = isLibraryCode(m_program, m_program.at(m_caller));
    while (inspection < m_state.inspections.size() && (m_state.inspections[inspection].questions != questions || isLibraryCode(m_program, m_program.at(m_state.inspections[inspection].function)) != isAskedByLibrary))
        ++inspection;
    if (inspection == m_state.inspections.size()) {
        m_state.inspections.append(InspectionInLinkCalls { questions, m_caller, m_index, static_cast<uint32_t>(m_state.stepsOfInspections.size()), { } });
        m_state.stepsOfInspections.append(StepOfInspectionInLinkCalls { inspection, { } });
        for (uint32_t layout = 0; questions.readPrivateFields && layout < m_program.layouts.size(); ++layout) {
            if (m_flow.superCallMayAnswerWithOlderObject(LayoutID(layout)))
                showPrivateFieldsOf(LayoutID(layout), inspection);
        }
    }
    for (const Source& source : values.sources)
        inspectAt(source.node, inspection);
    m_flow.noteUseOf(values.made);
    inspectType(values.made, inspection);
}

void CallInLinkCalls::showPrivateFieldsOf(LayoutID layout, uint32_t inspection)
{
    for (FlowNodeID cell : m_flow.cellsOfWrittenPrivateNamesOf(layout)) {
        if (!m_state.inspections[inspection].nodes.get(cell.index()))
            m_flow.build.statistics().count(StatisticID::PrivateFieldsShownToComparisons);
        m_flow.addEdge(cell, m_flow.unseen());
        inspectAt(cell, inspection);
    }
}

void CallInLinkCalls::inspectAt(FlowNodeID node, uint32_t inspection)
{
    BitVector& nodes = m_state.inspections[inspection].nodes;
    if (node.index() >= nodes.size())
        nodes.ensureSize(node.index() + node.index() / 4 + 64);
    if (node == m_flow.unseen())
        tellFormatterLosesSight(m_state.inspections[inspection].questions);
    if (node == m_flow.unseen() || !nodes.add(node.index()))
        return;
    m_flow.observe(node, FlowObserver { FlowObserver::Kind::Inspected, m_state.inspections[inspection].function, m_state.inspections[inspection].stepForValues });
}

bool CallInLinkCalls::askInspected(SubjectOfInspection kind, uint32_t number, const LinkType& subject, uint32_t inspection)
{
    if (!m_state.subjectsOfInspections.add({ static_cast<uint64_t>(kind) << 32 | number, inspection }).isNewEntry)
        return false;
    uint32_t step = static_cast<uint32_t>(m_state.stepsOfInspections.size());
    m_state.stepsOfInspections.append(StepOfInspectionInLinkCalls { inspection, subject });
    askAgain(step);
    return true;
}

void CallInLinkCalls::askAgain(uint32_t step)
{
    StepOfInspectionInLinkCalls asked = m_state.stepsOfInspections[step];
    Questions questions = m_state.inspections[asked.inspection].questions;
    if (questions.onlyFollow)
        return;
    SetForScope watcher(m_stepThatWatches, std::optional(step));
    if (mayAnswer(asked.subject, nullptr, questions, true)) {
        tellFormatterLosesSight(questions);
        escape(asked.subject, nullptr);
    }
}

void CallInLinkCalls::takeStepOfInspection(uint32_t step, const LinkType& now)
{
    if (m_state.stepsOfInspections[step].subject.isNever())
        inspectType(now, m_state.stepsOfInspections[step].inspection);
    else
        askAgain(step);
}

void CallInLinkCalls::tellWhatFormatterIsHanded(const LinkType& type, const Questions& questions)
{
    if (!questions.areAskedByFormatter)
        return;
    bool isWalked = false;
    bool isPrintedByCodeOfItsClass = false;
    auto look = [&](BuiltinClassID builtinClass) {
        if (!builtinClass || m_builtins.at(builtinClass).origin != BuiltinOrigin::Host || (questions.onlyFollow && m_builtins.hostTakesInstancesAsAnswers(builtinClass)))
            return;
        (m_builtins.formatterWalksInstancesOf(builtinClass) ? isWalked : isPrintedByCodeOfItsClass) = true;
        m_flow.note(LinkFlow::Note::InstanceHandedToFormatter, builtinClass.index(), 1);
    };
    if (type.classes == LinkType::Classes::Mixed)
        m_flow.note(LinkFlow::Note::InstanceHandedToFormatter, 0, 0);
    if (type.classes == LinkType::Classes::Site)
        look(type.builtinClass);
    forEachLayoutWithListedOwnValues(type, [&](LayoutID layout) {
        look(m_program.at(layout).writtenBuiltinBase);
    });
    bool isFunctionThatNobodyAsks = questions.onlyFollow && type.functions != LinkType::Functions::None;
    if (isWalked || isFunctionThatNobodyAsks || type.classes == LinkType::Classes::Mixed || type.builtin || (questions.onlyFollow ? type.hasDirectUnknownObjects() : type.hasUnknownObjects) || type.holdsLostBuiltins)
        m_flow.note(LinkFlow::Note::ObjectHandedToFormatter, LinkFlow::walkedByFormatter);
    else if (isPrintedByCodeOfItsClass || type.hasUnknownObjects)
        m_flow.note(LinkFlow::Note::ObjectHandedToFormatter, LinkFlow::printedByCodeOfItsClass);
}

bool CallInLinkCalls::mayBePrintedWholeWhereItIsRefused(const LinkType& type, bool isLiteralMadeForCall)
{
    if (type.holdsPrimitivesAlone())
        return false;
    FlowNodeID lost = m_flow.whereInstancesLoseTheirConstructor();
    if (!watch(lost, 1) || !heldAt(lost).isNever())
        return true;
    if (isLiteralMadeForCall)
        return false;
    if (type.classes == LinkType::Classes::Mixed || type.builtin || type.hasUnknownObjects || type.holdsLostBuiltins || type.objects == LinkType::Objects::Lost)
        return true;
    if (type.classes == LinkType::Classes::Site) {
        BuiltinID prototype = m_builtins.at(type.builtinClass).prototype;
        if (!prototype || m_builtins.findMember(prototype, BuiltinNames::constructor).outcome != BuiltinMemberLookup::Outcome::Found)
            return true;
    }
    bool isNoPlainObject = false;
    forEachLayoutWithListedOwnValues(type, [&](LayoutID layout) {
        for (LayoutID current = layout; current; current = m_program.at(current).base)
            isNoPlainObject |= !!m_program.at(current).writtenBuiltinBase;
    });
    return isNoPlainObject;
}

bool CallInLinkCalls::isLiteralMadeForCall(const RowCall& call, BuiltinOperand operand) const
{
    ValueID written = valueWrittenAs(call, operand);
    return written && call.literalsMadeForCall[literalHandedOn(call, written, m_call.site)].isInSight;
}

void CallInLinkCalls::tellFormatterLosesSight(const Questions& questions)
{
    if (questions.areAskedByFormatter)
        m_flow.note(LinkFlow::Note::ObjectHandedToFormatter, LinkFlow::walkedByFormatter);
}

void CallInLinkCalls::inspectType(const LinkType& type, uint32_t inspection)
{
    m_flow.noteLossOfLightFormAmong(type);
    if (type.builtin && !onlyNamesThatTableAnswersAreRead(type.builtin, m_state.inspections[inspection].questions)) {
        m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_builtins.canonicalID(type.builtin).index());
        m_flow.noteLossOf(m_builtins.canonicalID(type.builtin));
    }
    tellWhatFormatterIsHanded(type, m_state.inspections[inspection].questions);
    tellWhichMembersNativeCodeCallsOffUnseenObjects(type, m_state.inspections[inspection].questions);
    if (m_state.inspections[inspection].questions.iterate)
        tellWhatIterationCalls(type);

    auto inspectObjectsOf = [&](LayoutID layout) {
        LinkType objects = objectsForLinkCalls(layout);
        if (!askInspected(SubjectOfInspection::Layout, layout.index(), objects, inspection))
            return;
        if (FunctionID constructor = m_program.at(layout).constructor; constructor && m_state.inspections[inspection].questions.askConstructorsOfInstances)
            askInspected(SubjectOfInspection::Function, constructor.index(), closuresForLinkCalls(constructor), inspection);
        if (m_state.inspections[inspection].questions.readPrivateFields)
            showPrivateFieldsOf(layout, inspection);
        if (!m_state.inspections[inspection].questions.readOwnValues)
            return;
        Vector<FlowNodeID, 8> nodes = m_flow.nodesWithOwnValuesOf(layout);
        if (nodes.isEmpty()) {
            tellFormatterLosesSight(m_state.inspections[inspection].questions);
            escape(objects, nullptr);
        }
        for (FlowNodeID node : nodes)
            inspectAt(node, inspection);
        if (FlowNodeID above = m_state.inspections[inspection].questions.readThroughPrototypes ? m_flow.foundOnPrototypesAbove(layout, { }) : FlowNodeID())
            inspectAt(above, inspection);
    };

    LinkType rest = type;
    rest.primitives = 0;
    rest.hasUnknownObjects = false;
    rest.holdsLostBuiltins = false;
    rest.builtin = { };
    rest.dropInstances();

    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        inspectObjectsOf(type.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(type))
            inspectObjectsOf(member);
        break;
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach(inspectObjectsOf);
        break;
    case LinkType::Objects::Lost:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes: {
        LinkType objects = rest;
        objects.functions = LinkType::Functions::None;
        objects.function = { };
        objects.constructorsOf = { };
        tellFormatterLosesSight(m_state.inspections[inspection].questions);
        escape(objects, nullptr);
        break;
    }
    }

    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        askInspected(SubjectOfInspection::Function, type.function.index(), closuresForLinkCalls(type.function), inspection);
        break;
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Set:
    case LinkType::Functions::Lost: {
        LinkType functions = rest;
        functions.objects = LinkType::Objects::None;
        functions.layout = { };
        functions.family = { };
        tellFormatterLosesSight(m_state.inspections[inspection].questions);
        escape(functions, nullptr);
        break;
    }
    }

    switch (type.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site: {
        LinkType instances = instancesAmongForLinkCalls(type);
        if (m_state.inspections[inspection].questions.onlyFollow && !m_builtins.formatterWalksInstancesOf(type.builtinClass)) {
            inspectAt(m_flow.whereInstancesAreMadeThenables(), inspection);
            break;
        }
        if (!askInspected(SubjectOfInspection::Site, type.site.index(), instances, inspection))
            break;
        if (m_state.inspections[inspection].questions.onlyFollow && m_builtins.isSameOrSubclassOf(type.builtinClass, BuiltinClasses::Promise)) {
            inspectAt(m_flow.contents(type.site, ContentsSlot::Value), inspection);
            break;
        }
        for (ContentsSlot slot : allMembersOf<ContentsSlot>)
            inspectAt(m_flow.contents(type.site, slot), inspection);
        inspectAt(m_flow.contents(type.site, SlotOfContents::underAnyName()), inspection);
        for (BuiltinSlotID slot : m_builtins.at(type.builtinClass).slots) {
            if (m_builtins.at(slot).wordOfCell == BuiltinWordOfCell::None)
                inspectAt(m_flow.contents(type.site, m_flow.slotOf(slot)), inspection);
        }
        break;
    }
    case LinkType::Classes::Mixed:
        escape(instancesAmongForLinkCalls(type), nullptr);
        break;
    }
}

bool CallInLinkCalls::mayFind(const LinkType& objects, PropertyKeyID key, uint8_t distance, bool membersOfTableAnswerNothing, bool primitivesAnswerNothing, bool ownPropertiesAnswerNothing)
{
    auto holdsWhatAnswers = [&](FlowNodeID node) {
        if (!watch(node, static_cast<uint8_t>(distance + 1)))
            return true;
        LinkType found = heldAt(node);
        return found != primitivesForLinkCalls(found.primitives & Primitive::Undefined);
    };

    if (objects.classes == LinkType::Classes::Site) {
        NameClass nameClass = m_flow.nameClassOf(key);
        bool isArguments = objects.builtinClass == BuiltinClasses::Arguments;
        BuiltinClassID above = classAboveInstances(objects.builtinClass);
        if (nameClass == NameClass::Proto || mayBeElementKey(nameClass) || (isArguments ? key == m_program.keyOf(BuiltinNames::callee) : !m_builtins.mayHavePristineInstances(above)))
            return true;
        if (!ownPropertiesAnswerNothing && (holdsWhatAnswers(m_flow.contentsOf(objects, SlotOfContents::underUnknownNames())) || holdsWhatAnswers(m_flow.contentsOf(objects, SlotOfContents::under(key)))))
            return true;
        bool isOwnArgumentsProperty = isArguments && (key == m_program.keyOf(BuiltinNames::length) || key == m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::Iterator)));
        Lookup outcome = isOwnArgumentsProperty ? Lookup::Found : (!isArguments && isOfClassItself(objects, static_cast<uint8_t>(distance + 1)) ? findOnObjectsOfClassItself(above, key) : findOnInstancesOf(above, key)).outcome;
        if (outcome == Lookup::Unknown || (outcome == Lookup::Found && !membersOfTableAnswerNothing))
            return true;
    }

    switch (objects.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure: {
        const Function& closed = m_program.at(objects.function);
        LayoutID layout = closed.constructedClass;
        bool isConstructorFunction = layout && m_flow.isOfConstructorFunction(layout);
        if (isConstructorFunction)
            layout = { };
        if (layout && isClassConstructorOf(closed.kind) && m_flow.nameClassOf(key) == NameClass::Prototype)
            break;
        if (layout && (m_flow.staticNamesMayBeIncompleteBesideConstructorFunctions(layout) || m_flow.findMembers(layout, ClassSide::Static, key).mayAnswer()))
            return true;
        uint32_t stepsLeft = static_cast<uint32_t>(m_program.layouts.size());
        for (LayoutID current = layout; current && stepsLeft--; current = m_program.at(current).base) {
            if (m_program.at(current).findStaticField(key))
                return true;
        }
        if (FunctionID holder = layout ? m_flow.classRowNameHolder(layout, key) : FunctionID(); holder && holdsWhatAnswers(m_flow.laidOnClosuresOf(holder, key)))
            return true;
        NameClass nameClass = m_flow.nameClassOf(key);
        if (nameClass == NameClass::Proto || mayBeElementKey(nameClass))
            return true;
        if (isConstructorFunction && !heldAt(m_flow.foundOnClosuresOf(objects.function)).isNever())
            return true;
        if (holdsWhatAnswers(m_flow.foundOnClosuresOf(objects.function)) || holdsWhatAnswers(m_flow.followsNamesOnClosuresOf(objects.function, key) ? m_flow.laidOnClosuresOf(objects.function, key) : m_flow.foundBesideFieldsUnder(key)))
            return true;
        if (nameClass == NameClass::Prototype && (layout || isConstructorFunction))
            break;
        if (layout && m_flow.namesMayBeIncomplete(layout, ClassSide::Static) && (holdsWhatAnswers(m_flow.foundOnClassObjectsFrom(layout)) || holdsWhatAnswers(m_flow.constructorChainStoreNode(layout, key))))
            return true;
        if (findOnObjectsOfClassItself(classOfClosuresForLinkCalls(closed), key).outcome == Lookup::Unknown)
            return true;
        break;
    }
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Set: {
        bool mayFindOnOne = false;
        auto askOne = [&](FunctionID function) {
            mayFindOnOne = mayFindOnOne || mayFind(closuresForLinkCalls(function), key, distance, membersOfTableAnswerNothing, primitivesAnswerNothing, ownPropertiesAnswerNothing);
        };
        if (objects.functions == LinkType::Functions::Set) {
            for (FunctionID member : m_flow.functionsOfSet(objects))
                askOne(member);
        } else
            forEachConstructorOf(objects.constructorsOf, askOne);
        if (mayFindOnOne)
            return true;
        break;
    }
    case LinkType::Functions::Lost: {
        NameClass nameClass = m_flow.nameClassOf(key);
        if (m_flow.lostFunctionsAreAtUnseen(objects) || isPrivateNameInLink(m_program, key) || nameClass == NameClass::Proto || nameClass == NameClass::Prototype || mayBeElementKey(nameClass))
            return true;
        for (BuiltinClassID closureClass : { BuiltinClassID(BuiltinClasses::Function), BuiltinClassID(BuiltinClasses::AsyncFunction), BuiltinClassID(BuiltinClasses::GeneratorFunction), BuiltinClassID(BuiltinClasses::AsyncGeneratorFunction) }) {
            if (findOnObjectsOfClassItself(closureClass, key).outcome == Lookup::Unknown)
                return true;
        }
        LinkType closures;
        closures.functions = LinkType::Functions::Lost;
        closures.function = objects.function;
        if (holdsWhatAnswers(m_flow.laidOnLostClosures(closures)) || holdsWhatAnswers(m_flow.foundBesideFieldsUnder(key)))
            return true;
        break;
    }
    }

    auto mayFindOnObjectsOf = [&](LayoutID layout) {
        auto holdsPrimitivesAlone = [&](FlowNodeID node) {
            if (!watch(node, static_cast<uint8_t>(distance + 1)))
                return false;
            LinkType found = heldAt(node);
            return found == primitivesForLinkCalls(found.primitives);
        };
        if (ModuleID module = m_program.at(layout).namespaceOf; module && std::ranges::any_of(m_program.at(module).requests, &ModuleRequest::isDynamicStarExport))
            return true;
        std::optional<FieldReference> field = findField(m_program, layout, key);
        bool answersWithPrimitivesAlone = primitivesAnswerNothing && field && fieldAt(m_program, *field).form.isEmpty() && holdsPrimitivesAlone(m_flow.field(*field));
        bool membersAnswer = m_flow.findMembers(layout, ClassSide::Prototype, key).mayAnswer() && !(m_methodsOfClassesAnswerNothing && findsMethodsAlone(layout, key));
        if ((field && !answersWithPrimitivesAlone) || membersAnswer || m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype))
            return true;
        if (!m_flow.isAbsentFromObjectPrototype(key)) {
            BuiltinClassID objectClass = BuiltinClasses::Object;
            BuiltinID above = m_builtins.at(objectClass).prototype;
            Lookup outcome = findOnce(static_cast<uint64_t>(above.index()) << 32 | key.index(), [&] {
                return findAbove(above, key, MembersUnderKey(m_program, key));
            }).outcome;
            if (outcome == Lookup::Unknown)
                return true;
        }
        FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(layout, key);
        return (foundOnPrototypes && holdsWhatAnswers(foundOnPrototypes)) || holdsWhatAnswers(m_flow.extraPropertyReadNode(layout)) || holdsWhatAnswers(m_flow.appendedField(layout, key));
    };
    auto mayFindOnPrototypeOf = [&](LayoutID layout) {
        NameClass nameClass = m_flow.nameClassOf(key);
        if (nameClass == NameClass::Proto || nameClass == NameClass::Constructor || mayBeElementKey(nameClass) || !inheritsFromObjectAlone(layout))
            return true;
        if (m_flow.escapedLayouts.get(layout.index()) || m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype) || m_flow.findMembers(layout, ClassSide::Prototype, key).mayAnswer())
            return true;
        if (!m_flow.isAbsentFromObjectPrototype(key)) {
            BuiltinClassID objectClass = BuiltinClasses::Object;
            BuiltinID above = m_builtins.at(objectClass).prototype;
            Lookup outcome = findOnce(static_cast<uint64_t>(above.index()) << 32 | key.index(), [&] {
                return findAbove(above, key, MembersUnderKey(m_program, key));
            }).outcome;
            if (outcome == Lookup::Unknown)
                return true;
        }
        FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(layout, key);
        return (foundOnPrototypes && holdsWhatAnswers(foundOnPrototypes)) || holdsWhatAnswers(m_flow.extraPropertyReadNode(layout)) || holdsWhatAnswers(m_flow.foundBesideFieldsFor(layout, key));
    };
    switch (objects.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
        return mayFindOnObjectsOf(objects.layout);
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(objects), mayFindOnObjectsOf);
    case LinkType::Objects::Prototype:
        return mayFindOnPrototypeOf(objects.layout);
    case LinkType::Objects::Prototypes: {
        bool mayFindOnOne = false;
        forEachLayoutOf(objects.family, [&](LayoutID layout) {
            mayFindOnOne = mayFindOnOne || mayFindOnPrototypeOf(layout);
        });
        return mayFindOnOne;
    }
    case LinkType::Objects::Family:
        return holdsWhatAnswers(m_flow.fieldReadThrough(objects.family, key));
    case LinkType::Objects::Lost: {
        FlowNodeID read = m_flow.fieldReadThroughLost(objects.layout, key);
        if (!watch(read, static_cast<uint8_t>(distance + 1)))
            return true;
        LinkType found = heldAt(read);
        if (BuiltinID lockedAbove = m_flow.whatLiesAboveClasses(objects.layout, key).builtin; lockedAbove && found.builtin == lockedAbove)
            found.builtin = { };
        return found != primitivesForLinkCalls(found.primitives & Primitive::Undefined);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StateOfLinkCalls& LinkFlow::stateOfLinkCalls()
{
    if (!m_stateOfLinkCalls)
        m_stateOfLinkCalls.reset(new StateOfLinkCalls(*this));
    return *m_stateOfLinkCalls;
}

static bool hasSameObjectsAndFunctions(LinkType first, LinkType second)
{
    for (LinkType* type : { &first, &second }) {
        type->primitives = 0;
        type->hasUnknownObjects = false;
        type->holdsLostBuiltins = false;
        type->builtin = { };
    }
    return first == second;
}

bool LinkFlow::describeSharedCallNode(PrintStream& out, FlowNodeID id) const
{
    if (!m_stateOfLinkCalls)
        return false;
    for (const auto& shared : m_stateOfLinkCalls->sharedCalls) {
        size_t place = shared->nodes.find(id);
        if (place == notFound && shared->result != id && shared->thrown != id)
            continue;
        out.print("(", place != notFound ? "operand" : shared->result == id ? "result" : "thrown");
        if (place != notFound)
            out.print(" ", place);
        out.print(" of the shared call ", shared->index, ")");
        return true;
    }
    return false;
}

void LinkFlow::wakeEverySleeperToSeeThatNothingGrows()
{
    if (!m_stateOfLinkCalls)
        return;
    StateOfLinkCalls& state = *m_stateOfLinkCalls;
    for (size_t index = 0; index < state.sharedCalls.size(); ++index) {
        SharedCallInLinkCalls& shared = *state.sharedCalls[index];
        if (!shared.someMembersSleep)
            continue;
        CodeAtWork codeAtWork(*this, shared.firstCaller);
        CallInLinkCalls(*this, state, shared).wakeEverySleeper();
    }
}

String LinkFlow::describeSleeperThatMayFindSomething()
{
    if (!m_stateOfLinkCalls)
        return { };
    StateOfLinkCalls& state = *m_stateOfLinkCalls;
    auto sleepsIn = [](const SharedCallInLinkCalls& shared, LayoutID member) {
        return shared.someMembersSleep && !std::ranges::binary_search(shared.membersAwakeFromTheStart, member) && !shared.membersWokenLater.contains(member.index());
    };
    auto describe = [](const SharedCallInLinkCalls& shared, LayoutID member) {
        return makeString("in the shared call "_s, shared.index, " the layout "_s, member.index(), " still sleeps and may find something under the called name"_s);
    };
    for (const auto& [family, sleepers] : state.sleepersOfFamilies) {
        bool someMemberFindsTheUnknownAlone = false;
        for (LayoutID member : instancesOfClassesIn(FamilyID(family))) {
            LinkFlow::WhatSilentMemberFinds found = whatSilentMemberFinds(member, sleepers->nameOfFirstCall);
            someMemberFindsTheUnknownAlone |= found == LinkFlow::WhatSilentMemberFinds::TheUnknownAlone;
            if (found != LinkFlow::WhatSilentMemberFinds::SomethingOfItsOwn)
                continue;
            for (uint32_t index : sleepers->sharedCalls) {
                if (sleepsIn(*state.sharedCalls[index], member))
                    return describe(*state.sharedCalls[index], member);
            }
        }
        for (uint32_t index : sleepers->sharedCalls) {
            if (someMemberFindsTheUnknownAlone && state.sharedCalls[index]->someMembersSleep && !calleesOfSharedCalls[index].mayEnterUnseen)
                return makeString("members of the shared call "_s, index, " sleep that find the unknown beside their fields, and the call does not enter unseen code"_s);
        }
    }
    for (const auto& shared : state.sharedCalls) {
        if (!shared->someMembersSleep)
            continue;
        for (FieldBornOnMember born : fieldsBornOnMembersUnder(shared->family, shared->call.key)) {
            if (!typeAt(born.node).isNever() && sleepsIn(*shared, born.member))
                return describe(*shared, born.member);
        }
    }
    return { };
}

LinkFlow::WhatSilentMemberFinds LinkFlow::whatSilentMemberFinds(LayoutID member, PropertyKeyID calledName)
{
    using Finds = WhatSilentMemberFinds;
    if (lostLayouts.get(member.index()) && !program.at(member).has(LayoutFlags::IsNeverAllocated))
        return Finds::SomethingOfItsOwn;
    LinkType found = typeAt(foundBesideFieldsFor(member, calledName));
    if (found.isNever())
        return Finds::Nothing;
    if (found.functions != LinkType::Functions::None || found.builtin || found.classes != LinkType::Classes::None)
        return Finds::SomethingOfItsOwn;
    if (found.hasUnknownObjects || (found.objects != LinkType::Objects::None && !holdsOnlyObjectsThatCannotBeCalledForLinkCalls(*this, found)))
        return Finds::TheUnknownAlone;
    return Finds::NothingThatCanBeCalled;
}

LinkFlow::WhatSilentMemberFinds LinkFlow::whatPorterTakesMemberToFind(const FlowObserver& porter, LayoutID member, PropertyKeyID calledName)
{
    letKeeperWatch(porter, foundBesideFieldsFor(member, calledName));
    WhatSilentMemberFinds found = whatSilentMemberFinds(member, calledName);
    if (found == WhatSilentMemberFinds::SomethingOfItsOwn && AOT::hasFlag(build.options().testing, Wire::TestingSwitch::TreatMemberCalleesAsUnknown))
        return WhatSilentMemberFinds::TheUnknownAlone;
    return found;
}

void LinkFlow::letKeeperWatch(const FlowObserver& keeper, FlowNodeID node)
{
    if (stateOfLinkCalls().observedByMembers.add(static_cast<uint64_t>(keeper.record) << 32 | node.index()).isNewEntry)
        observe(node, keeper);
}

bool LinkFlow::tellPorterThatMemberIsLost(FamilyID family, LayoutID lost)
{
    if (!m_stateOfLinkCalls)
        return false;
    StateOfLinkCalls& state = *m_stateOfLinkCalls;
    auto sleepers = state.sleepersOfFamilies.find(family.index());
    if (sleepers == state.sleepersOfFamilies.end() || sleepers->value->isUp.contains(lost.index()))
        return false;
    auto porter = sleepers->value->portersOfMembers.find(lost.index());
    if (porter == sleepers->value->portersOfMembers.end())
        return false;
    if (!state.membersToResolve.set(porter->value))
        state.membersObservedToResolve.append(FlowObserver { FlowObserver::Kind::MemberOfSharedCall, state.sharedCalls[sleepers->value->sharedCalls[0]]->firstCaller, porter->value });
    return true;
}

bool LinkFlow::hasMembersToResolve() const
{
    return m_stateOfLinkCalls && !m_stateOfLinkCalls->membersObservedToResolve.isEmpty();
}

void LinkFlow::callObserved(const FlowObserver& observer, const LinkType& told)
{
    StateOfLinkCalls& state = stateOfLinkCalls();

    if (observer.kind == FlowObserver::Kind::MemberOfSharedCall) {
        MemberOfSharedCallInLinkCalls& member = state.membersOfSharedCalls[observer.record];
        if (member.tellsThatBringNothing && typeObservedBefore().isNever()) {
            --member.tellsThatBringNothing;
            return;
        }
        if (!state.membersToResolve.set(observer.record))
            state.membersObservedToResolve.append(observer);
        return;
    }

    uint32_t& tellsThatBringNothing = state.tellsThatBringNothing[firstCallOfFunction[observer.function.index()] + observer.record];
    if (tellsThatBringNothing && typeObservedBefore().isNever()) {
        --tellsThatBringNothing;
        return;
    }
    if (observer.kind == FlowObserver::Kind::ReceiverOfCall && state.callsOnUnseenReceivers[firstCallOfFunction[observer.function.index()] + observer.record] == StateOfLinkCalls::isDoneButForWhatIsInSight && holdsNothingInSightForLinkCalls(told))
        return;
    uint32_t numberOfCallObserved = firstCallOfFunction[observer.function.index()] + observer.record;
    bool isEnteredNoMore = state.callsOfUnseen[numberOfCallObserved] == StateOfLinkCalls::hasGivenUpInSight || (state.callsOfUnseen[numberOfCallObserved] && !state.callsThatEnterMembersOfTable[numberOfCallObserved]);
    auto hasNewTarget = [&] {
        return summaries.at(observer.function).calls[observer.record].newTarget != noSummaryNode;
    };
    bool ignoresNewTarget = AOT::hasFlag(build.options().testing, Wire::TestingSwitch::IgnoreEscapeAfterUnknownCall) && hasNewTarget();
    if ((isEnteredNoMore || ignoresNewTarget) && hasSameObjectsAndFunctions(told, typeObservedBefore()) && (ignoresNewTarget || (told.hasDirectUnknownObjects() == typeObservedBefore().hasDirectUnknownObjects() && (told.hasUnknownObjects == typeObservedBefore().hasUnknownObjects || !hasNewTarget())))
        && !(state.callsWithDroppedReturns[numberOfCallObserved] && (told.primitives != typeObservedBefore().primitives || told.hasUnknownObjects != typeObservedBefore().hasUnknownObjects || told.builtin != typeObservedBefore().builtin)))
        return;
    if (!std::exchange(state.callsToResolve[firstCallOfFunction[observer.function.index()] + observer.record], 1))
        state.observersToResolve[laneOfObserversOf(observer.function)].append(observer);
}

bool LinkFlow::resolveNotifiedCalls()
{
    StateOfLinkCalls& state = stateOfLinkCalls();
    bool hasResolvedSome = false;
    for (unsigned lane = 0; lane < numberOfLanes(); ++lane)
        hasResolvedSome |= resolveNotifiedCalls(lane);
    for (const FlowObserver& observer : std::exchange(state.membersObservedToResolve, { })) {
        hasResolvedSome = true;
        CodeAtWork codeAtWork(*this, observer.function);
        state.membersToResolve.clear(observer.record);
        MemberOfSharedCallInLinkCalls told = state.membersOfSharedCalls[observer.record];
        if (told.sharedCall & StateOfLinkCalls::isPorterOfFamily) {
            auto& sleepers = *state.sleepersOfFamilies.get(told.sharedCall & ~StateOfLinkCalls::isPorterOfFamily);
            if (AOT::hasFlag(build.options().testing, Wire::TestingSwitch::IgnoreDormantMemberUpdates))
                continue;
            switch (whatPorterTakesMemberToFind(observer, told.layout, sleepers.nameOfFirstCall)) {
            case LinkFlow::WhatSilentMemberFinds::Nothing:
            case LinkFlow::WhatSilentMemberFinds::NothingThatCanBeCalled:
                break;
            case LinkFlow::WhatSilentMemberFinds::TheUnknownAlone:
                if (std::exchange(sleepers.someMemberFindsTheUnknownAlone, true))
                    break;
                for (size_t place = 0; place < sleepers.sharedCalls.size(); ++place) {
                    SharedCallInLinkCalls& shared = *state.sharedCalls[sleepers.sharedCalls[place]];
                    if (!shared.someMembersSleep)
                        continue;
                    CodeAtWork codeOfCall(*this, shared.firstCaller);
                    CallInLinkCalls(*this, state, shared).callUnseenForMembersThatFindTheUnknownAlone();
                }
                break;
            case LinkFlow::WhatSilentMemberFinds::SomethingOfItsOwn:
                if (!sleepers.isUp.add(told.layout.index()).isNewEntry)
                    break;
                sleepers.membersThatAreUp.append(told.layout);
                for (size_t place = 0; place < sleepers.sharedCalls.size(); ++place) {
                    SharedCallInLinkCalls& shared = *state.sharedCalls[sleepers.sharedCalls[place]];
                    CodeAtWork codeOfCall(*this, shared.firstCaller);
                    CallInLinkCalls(*this, state, shared).wake(told.layout);
                }
                break;
            }
            continue;
        }
        CallInLinkCalls(*this, state, *state.sharedCalls[told.sharedCall]).resolveMemberOfSharedCall(observer.record);
    }
    return hasResolvedSome;
}

bool LinkFlow::resolveNotifiedCalls(unsigned lane)
{
    StateOfLinkCalls& state = *m_stateOfLinkCalls;
    if (state.observersToResolve[lane].isEmpty())
        return false;
    Vector<FlowObserver> observers = std::exchange(state.observersToResolve[lane], { });
    switch (m_order) {
    case OrderOfWorklist::NewestFirst:
        break;
    case OrderOfWorklist::OldestFirst:
        std::ranges::reverse(observers);
        break;
    case OrderOfWorklist::Scattered:
        for (size_t place = observers.size(); place > 1; --place)
            std::swap(observers[place - 1], observers[(place * 2654435761u >> 7) % place]);
        break;
    }
    bool resolvesOnceMore = tellsObserversOnceMore();
    for (const FlowObserver& observer : observers) {
        CodeAtWork codeAtWork(*this, observer.function);
        state.callsToResolve[firstCallOfFunction[observer.function.index()] + observer.record] = 0;
        if (resolvesOnceMore) [[unlikely]]
            noteRenotifiedObserver(observer);
        CallInLinkCalls(*this, state, observer.function, observer.record).resolve();
    }
    if (state.observersToResolve[lane].isEmpty() && !state.observersToResolve[lane].capacity()) {
        observers.shrink(0);
        state.observersToResolve[lane] = WTF::move(observers);
    }
    return true;
}

void LinkFlow::settleSharedCallsAs(WhatUnseenCodeDoes reading)
{
    enum class SharedCalls : bool { AreForgotten, StayNamed };
    auto forgetWhatUnseenCodeCovers = [&](CalleesOfCall& callees, SharedCalls sharedCalls) NEVER_INLINE {
        callees.removeListedMatching([&](FunctionID callee) {
            return escapedFunctions.get(callee.index());
        });
        callees.clearBuiltins();
        if (sharedCalls == SharedCalls::AreForgotten)
            callees.clearShared();
        callees.mayEnterLost = false;
    };
    auto settle = [&](CalleesOfCall& callees, SharedCalls sharedCalls) ALWAYS_INLINE_LAMBDA {
        if (callees.mayEnterUnseen && reading == WhatUnseenCodeDoes::Anything) [[unlikely]]
            forgetWhatUnseenCodeCovers(callees, sharedCalls);
        callees.sortListedAndBuiltins();
    };
    for (CalleesOfCall& callees : calleesOfSharedCalls)
        settle(callees, SharedCalls::AreForgotten);
    forEachRangeOfIndicesInParallel(loops, calleesOfCalls.size(), [&](size_t first, size_t end) {
        for (size_t call = first; call < end; ++call) {
            CalleesOfCall& callees = calleesOfCalls[call];
            if (callees.shared().size() > 1) {
                auto isDoneByAnother = [&](uint32_t index) {
                    const SubjectOfSharedCall& subject = subjectsOfSharedCalls[index];
                    return std::ranges::any_of(callees.shared(), [&](uint32_t other) {
                        const SubjectOfSharedCall& larger = subjectsOfSharedCalls[other];
                        return larger.family != subject.family && larger.key == subject.key && larger.form == subject.form && families.nearestCommonFamily(subject.family, larger.family) == larger.family;
                    });
                };
                Vector<uint32_t, 8> kept;
                for (uint32_t index : callees.shared()) {
                    if (!isDoneByAnother(index))
                        kept.append(index);
                }
                std::ranges::sort(kept, { }, [&](uint32_t index) {
                    return subjectsOfSharedCalls[index];
                });
                callees.replaceShared(kept.span());
            }
            for (size_t place = 0; place < callees.shared().size(); ++place) {
                const CalleesOfCall& shared = calleesOfSharedCalls[callees.shared()[place]];
                for (BuiltinID builtin : shared.builtins())
                    callees.add(builtin);
                callees.mayEnterLost |= shared.mayEnterLost;
                callees.mayEnterUnseen |= shared.mayEnterUnseen;
            }
            if (!callees.shared().isEmpty()) {
                callees.removeListedMatching([&](FunctionID callee) {
                    return std::ranges::any_of(callees.shared(), [&](uint32_t index) {
                        return std::ranges::binary_search(calleesOfSharedCalls[index].listed(), callee);
                    });
                });
            }
            settle(callees, SharedCalls::StayNamed);
        }
    });
}

bool LinkFlow::isInspectedByComparisonOfPrivateFields(FlowNodeID node) const
{
    return m_stateOfLinkCalls && std::ranges::any_of(m_stateOfLinkCalls->inspections, [&](const InspectionInLinkCalls& inspection) {
        return inspection.questions.readPrivateFields && inspection.nodes.get(node.index());
    });
}

void LinkFlow::inspectionObserved(const FlowObserver& observer, const LinkType& now)
{
    StateOfLinkCalls& state = stateOfLinkCalls();
    StepOfInspectionInLinkCalls step = state.stepsOfInspections[observer.record];
    CallInLinkCalls(*this, state, state.inspections[step.inspection].function, state.inspections[step.inspection].call).takeStepOfInspection(observer.record, now);
}

void LinkFlow::enter(FunctionID caller, uint32_t callInSummary, FunctionID callee)
{
    CallInLinkCalls(*this, stateOfLinkCalls(), caller, callInSummary).enter(callee);
}

} } } // namespace JSC::AOT::Typed


#endif // ENABLE(AOT)
