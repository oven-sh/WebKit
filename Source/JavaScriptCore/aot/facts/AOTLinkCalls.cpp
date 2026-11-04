/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"
#include "AOTLinkStringParsers.h"
#include "AOTWalkStringParsers.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLinkExceptions.h"
#include "AOTNameClass.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include "AOTUnionTaggedMakers.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

// reach() puts an observer at the subject and the receiver of a call. Every other node that a call hangs on is found here while types climb, and StateOfLinkCalls remembers which call stands at which node.
// A node has a distance: 0 for an operand of the call, else one more than the node whose type led to it. What lies beyond the farthest distance is not looked at: it flows into unseen().
static constexpr uint8_t farthestObservedForLinkCalls = 2;
static constexpr uint8_t notObservedForLinkCalls = farthestObservedForLinkCalls + 1;

static constexpr uint16_t primitivesWithClassesForLinkCalls[] { Primitive::AnyString, LinkType::closed(Primitive::AnySymbol), Primitive::BigInt, LinkType::closed(Primitive::Number), Primitive::Boolean };

// The records of the row say where the value goes, what is called with what, and which property changes. A row that claims one of these and names the operand in no record has described nothing.
static constexpr OperandUses usesThatRecordsSpellOutForLinkCalls {
    OperandUse::Retains, OperandUse::RetainsContents, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::PassesToUserCode,
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype,
};

static constexpr OperandUses usesThatChangeObjectsForLinkCalls {
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype,
};

// What such a function does lies with the loader, with a test run or with what native code holds. No record of a row says it.
static constexpr BuiltinMark marksOfRowsThatSayTooLittleForLinkCalls[] {
    BuiltinMark::EvaluatesTextInGlobalScope, BuiltinMark::HoldsLoader, BuiltinMark::IsSpy, BuiltinMark::LoadsCodeWithoutJSAccess, BuiltinMark::IsResolverThatLoaderCalls,
    BuiltinMark::IsRequireThatLoaderCalls, BuiltinMark::MocksModule, BuiltinMark::ExtendsMatchers, BuiltinMark::FakesTimers, BuiltinMark::RunsCodeThatLocksDoNotBind,
    BuiltinMark::RunsCodeWithLoader, BuiltinMark::HandsOutWhatNativeCodeHolds,
};

struct SourceInLinkCalls {
    FlowNodeID node;
    uint8_t distance { 0 };
    TypeID passes;
};

// What may stand at one place: what is at the nodes, and what is made on the spot.
struct ValuesInLinkCalls {
    Vector<SourceInLinkCalls, 2> sources;
    LinkType made;

    bool isEmpty() const { return sources.isEmpty() && made.isNothing(); }
};

struct PassedInLinkCalls {
    // None: undefined.
    std::optional<ValuesInLinkCalls> receiver;
    Vector<ValuesInLinkCalls, 4> arguments;
    // Arguments behind those listed, at places that nobody knows.
    std::optional<ValuesInLinkCalls> list;
    bool isConstruction { false };
    // None: the callee itself.
    std::optional<ValuesInLinkCalls> newTarget;
};

struct ResultInLinkCalls {
    FlowNodeID node;
    uint32_t index { 0 };
};

struct FitInLinkCalls {
    bool mayBeInside { false };
    bool mayBeOutside { false };
};

// What native code asks of an operand, which may run code of the program.
struct QuestionsInLinkCalls {
    Vector<PropertyKeyID, 8> keys;
    // Only what the program has laid under these counts. What the table lists under one for the class of an object is harmless: the engine's own iterator or the `then` of its own promises, and the places of the row say where what that hands out goes; a property that is only read; a conversion of the engine's own, which asks what the object holds (convertToPrimitive).
    Vector<PropertyKeyID, 4> keysAnsweredByProgramAlone;
    // The names of the conversion to a primitive, answered by the program alone too.
    Vector<PropertyKeyID, 4> keysOfConversion;
    bool readEverything { false };
    // Every own value is read out, and the places of the row say where they go.
    bool readOwnValues { false };
    // [[Get]], which goes on above an object that has nothing of its own under the key.
    bool readThroughPrototypes { false };
    bool describeObjects { false };
    bool convertToPrimitive { false };
    // An object of the program, a function and a list are left as they are.
    bool onlyBoxedPrimitivesAreConverted { false };
    bool callMembers { false };
    // What the table lists for the class of an object is native code that asks nothing of anybody: only what has been laid in front of it counts.
    bool membersOfTableAnswerNothing { false };

    bool isEmpty() const { return keys.isEmpty() && keysAnsweredByProgramAlone.isEmpty() && !readEverything && !readOwnValues && !describeObjects && !convertToPrimitive && !callMembers; }
    friend bool operator==(const QuestionsInLinkCalls&, const QuestionsInLinkCalls&) = default;
};

// WHAT NATIVE CODE ONLY LOOKS AT, HOWEVER DEEP: JSON.stringify(value), structuredClone(value), console.log(value). The questions of the row are asked of the operand and of whatever can be reached from it,
// and nothing comes back: what is looked at keeps its writers, its shape and its callers. An object that may answer one of the questions with code of the program escapes, as it does where the question is asked of an operand alone.
// NOTHING IS JOINED: two lists that are printed have not met. So an inspection is no node that values flow into. It is an observer at each node that it comes to: an operand, each field of a layout that arrives there by itself, each slot of a site.
struct InspectionInLinkCalls {
    QuestionsInLinkCalls questions;
    // Whoever asked first. It stands in where a step needs a call to look things up by. As SharedCallInLinkCalls::firstCaller: all who ask are code of the library, or none is.
    FunctionID function;
    uint32_t call { 0 };
    uint32_t stepForValues { 0 };
    BitVector nodes;
};

// FlowObserver::record of Kind::Inspected.
struct StepOfInspectionInLinkCalls {
    uint32_t inspection { 0 };
    // Nothing: what is at the node is looked at. Else the node is one that the questions were decided by: they are asked of the subject again.
    LinkType subject;
};

// LinkFlow::calleesOfSharedCalls. One for each way in which calls go by a family: what of the family, the key, and how many arguments in which form. So what it passes to a callee is what each of its calls would pass.
struct SharedCallInLinkCalls {
    enum class Subjects : uint8_t {
        // The key is looked up on them.
        Instances,
        PrototypeObjects,
        ClassObjects,
        // They are called themselves, or constructed.
        CalledClassObjects,
    };

    Subjects subjects { Subjects::Instances };
    FamilyID family;
    // In LinkFlow::calleesOfSharedCalls.
    uint32_t index { 0 };
    // Whoever came first. It stands in where an observer or an inspection names a call of a summary. WHO THAT IS HANGS ON THE ORDER, so nothing may go by it but what is true of all that go by the shared call:
    // they are code of the library, or none of them is. By that a built-in is lost in sight or out of it: LinkFlow::CodeAtWork.
    FunctionID firstCaller;
    uint32_t firstCall { 0 };
    // The call as a summary would have it. Its nodes are numbers into `nodes`: 0 is LinkFlow::unseen(), 1 the receiver, the arguments follow, behind them the list of those at places that nobody knows, and last the new.target.
    SummaryCall call;
    ThinSummary summary;
    Vector<FlowNodeID, 6> nodes;
    bool hasList { false };
    FlowNodeID result;
    FlowNodeID thrown;
};

// FlowObserver::record of Kind::MemberOfSharedCall. What one member brings is looked at for that member alone, so a climb costs no look at the others.
struct MemberOfSharedCallInLinkCalls {
    uint32_t sharedCall { 0 };
    LayoutID layout;
    // As StateOfLinkCalls::tellsThatBringNothing.
    uint32_t tellsThatBringNothing { 0 };
};

struct RowCallInLinkCalls {
    BuiltinID builtin;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    const PassedInLinkCalls& passed;
    // The scan knew the built-in, and has left a record for each property action of its rows.
    bool isSeenByScan { false };
    bool isCalledByBuiltin { false };
    const BuiltinOverload* row { nullptr };
    bool resultIsUnknown { false };
    ValuesInLinkCalls result;
};

static LinkType primitivesForLinkCalls(uint16_t primitives)
{
    LinkType type;
    type.primitives = LinkType::closed(primitives);
    return type;
}

static LinkType primitivesWrittenForLinkCalls(const BuiltinType& written)
{
    return primitivesForLinkCalls(LinkType::primitivesOf(primitivesOfBuiltinType(written), integerRangeWrittenBy(written)));
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
    LinkType instances;
    instances.classes = type.classes;
    instances.builtinClass = type.builtinClass;
    instances.site = type.site;
    return instances;
}

// Objects of one of the engine's classes, which the type names, and nothing else.
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
    type.holdsUnseen = true;
    return type;
}

// An accessor runs at a read or a store, an initializer at a construction, the body of a module when the loader says so. No call stands for that.
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

// Of an operand a record speaks of the value itself, or of what the value holds: its own values, a slot, what an iteration hands out, what stands above it.
enum class SaidOfOperandInLinkCalls : uint8_t { Itself, WhatItHolds, Either };

// A part may name the operand at any depth: `Promise.all(iterable)` answers a promise of an array of what the iterable hands out.
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

// A RECORD FOR THAT USE, not any record that happens to name the operand: a row that keeps the operand and what it holds, and says where only one of them goes, has described the other not at all.
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
    // Everything that a handler call names escapes.
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
        return goesSomewhere(Said::Itself) || isNamed(row.result.prototypeSource) || isNamed(row.result.typedResultSource) || isActedOn({ Action::BecomesPrototype })
            || std::ranges::any_of(row.flow.callbacks, [&](const BuiltinCallback& callback) {
                return isNamed(callback.callee);
            })
            || std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return isNamedAt(action.value, Said::Itself);
            });
    // An action that reads out of the object lets it escape, one that takes a descriptor lets the descriptor escape, and one that stores or deletes by key leaves what the object holds in the object.
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
    // `Object.setPrototypeOf(object, above)`: the record of the change names what is put above as its value.
    case OperandUse::BecomesPrototype:
        return isActedOn({ Action::BecomesPrototype })
            || std::ranges::any_of(row.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return action.kind == Action::ChangesPrototype && isNamedAt(action.value, Said::Itself);
            });
    default:
        return true;
    }
}

static bool recordsSpellOutEveryUseForLinkCalls(const BuiltinTable& builtins, const BuiltinOverload& row, const BuiltinParameter& parameter)
{
    for (OperandUse use : parameter.uses & usesThatRecordsSpellOutForLinkCalls) {
        if (!recordsSpellOutForLinkCalls(builtins, row, parameter.operand, use))
            return false;
    }
    return true;
}

// The field holds what the records of the row store there and nothing else, whatever its type allows.
static bool fieldOfResultHoldsOnlyWhatIsStoredForLinkCalls(const BuiltinOverload& row, uint16_t fieldIndex)
{
    auto isField = [&](const BuiltinPlace& place) {
        return place.kind == BuiltinPlaceKind::ResultField && place.fieldIndex == fieldIndex && place.holdsOnlyWhatIsStored;
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
    if (row.isUndeclared() || row.hasTypedResult())
        return true;
    if (row.receiver && row.receiver->roles.containsAny(rolesOfLoaders))
        return true;
    return std::ranges::any_of(row.parameters, [&](const BuiltinParameter& parameter) {
        return parameter.roles.containsAny(rolesOfLoaders);
    });
}

// What this file keeps in LinkFlow. A call has the number of its place in LinkFlow::calleesOfCalls.
class StateOfLinkCalls {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkCalls);
    WTF_MAKE_NONCOPYABLE(StateOfLinkCalls);
public:
    explicit StateOfLinkCalls(const LinkFlow&);

    std::span<const ResultInLinkCalls> resultsOf(FunctionID caller, uint32_t call) const LIFETIME_BOUND;

    // The number of a call above the number of a node at which this file has put it. By the lane that tells the observers of the caller.
    Vector<HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> observed;
    // The number of a layout above the number of a node at which this file has laid its objects down.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> layoutsLaidDown;
    Vector<InspectionInLinkCalls> inspections;
    Vector<StepOfInspectionInLinkCalls> stepsOfInspections;
    // The kind and the number of what is asked (a layout, a function, a site) above the number of the inspection.
    HashSet<std::pair<uint64_t, uint32_t>, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> subjectsOfInspections;
    // The number of a step above the number of a node at which it has been put.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> observedBySteps;
    // By call: how often it was put at a node that held something by then. run() tells it of each, and it had looked.
    Vector<uint32_t> tellsThatBringNothing;

    // The family above the key, and the rest of what tells shared calls apart.
    HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> indexesOfSharedCalls;
    Vector<std::unique_ptr<SharedCallInLinkCalls>> sharedCalls;
    Vector<MemberOfSharedCallInLinkCalls> membersOfSharedCalls;
    // The number of a member above the number of a node at which this file has put it.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> observedByMembers;
    // The number of a shared call above the number of a function that it lists: it may list as many as its family has members.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> listedBySharedCalls;
    // Every operand flows into unseen() and every result is anything: nothing that arrives later adds to that. A byte a call, so that each thread writes its own. Behind them the shared calls, which are made as Link goes.
    Vector<uint8_t> callsOfUnseen;
    BitVector sharedCallsOfUnseen;
    bool hasCalledUnseen(uint32_t number) const { return number < callsOfUnseen.size() ? callsOfUnseen[number] : sharedCallsOfUnseen.get(number - callsOfUnseen.size()); }
    // Whether it had.
    // 1: all that is unknown of the call was unknown by escape alone when it called. 2: something was unknown in sight. It answers the level from before.
    uint8_t raiseCallOfUnseen(uint32_t number, uint8_t level)
    {
        if (number < callsOfUnseen.size()) {
            uint8_t before = callsOfUnseen[number];
            callsOfUnseen[number] = std::max(before, level);
            return before;
        }
        // A shared call is told for all who go by it.
        return sharedCallsOfUnseen.set(number - callsOfUnseen.size()) ? 2 : 0;
    }
    uint8_t levelOfCallOfUnseen(uint32_t number) const { return number < callsOfUnseen.size() ? callsOfUnseen[number] : sharedCallsOfUnseen.get(number - callsOfUnseen.size()) ? 2 : 0; }
    // Told in this round. By call, and by member of a shared call.
    Vector<uint8_t> callsToResolve;
    BitVector membersToResolve;
    // By the lane that tells the observers of the caller. The members of shared calls are for one thread.
    Vector<Vector<FlowObserver>> observersToResolve;
    Vector<FlowObserver> membersObservedToResolve;
    // The table of built-ins answers the same way as long as Link runs, and every call under a key asks it again.
    // CallInLinkCalls::findAbove() and findOnInstancesOf(). Which of the two, the number of the built-in or of the class, and the number of the key. The outcome above the number of the member.
    // By the lane at work: each thread keeps what it has asked.
    Vector<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> membersOfTable;
    // CallInLinkCalls::rowsMayBeTakenAtTheirWord(). By built-in. 0: not asked.
    Vector<uint8_t> builtinsWhoseRowsAreTakenAtTheirWord;

private:
    void listResults();

    const LinkFlow& m_flow;
    // By call, into m_results.
    Vector<uint32_t> m_firstResultOfCall;
    Vector<uint32_t> m_endOfResultsOfCall;
    Vector<ResultInLinkCalls> m_results;
};

StateOfLinkCalls::StateOfLinkCalls(const LinkFlow& flow)
    : m_flow(flow)
{
    size_t numberOfCalls = flow.calleesOfCalls.size();
    tellsThatBringNothing.fill(0, numberOfCalls);
    callsOfUnseen.fill(0, numberOfCalls);
    callsToResolve.fill(0, numberOfCalls);
    observed.grow(flow.numberOfLanes());
    observersToResolve.grow(flow.numberOfLanes());
    membersOfTable.grow(flow.numberOfLanes());
    builtinsWhoseRowsAreTakenAtTheirWord.fill(0, flow.program.builtins().builtinCount());
    m_firstResultOfCall.fill(0, numberOfCalls);
    m_endOfResultsOfCall.fill(0, numberOfCalls);
    listResults();
}

std::span<const ResultInLinkCalls> StateOfLinkCalls::resultsOf(FunctionID caller, uint32_t call) const
{
    uint32_t number = m_flow.firstCallOfFunction[caller.index()] + call;
    return m_results.span().subspan(m_firstResultOfCall[number], m_endOfResultsOfCall[number] - m_firstResultOfCall[number]);
}

// A summary names the call at each of its result nodes, and a call names none of them. The node of a conversion takes what each call of the conversion answers.
// The summaries say all of it, so it is listed for every caller at once, a caller at a time on all threads.
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

// One call of one summary, as its operands stand now. Whatever it lays down was right for the lower types too.
class CallInLinkCalls {
    WTF_MAKE_NONCOPYABLE(CallInLinkCalls);
public:
    CallInLinkCalls(LinkFlow&, StateOfLinkCalls&, FunctionID caller, uint32_t index);
    CallInLinkCalls(LinkFlow&, StateOfLinkCalls&, SharedCallInLinkCalls&);

    void resolve();
    void resolveMemberOfSharedCall(uint32_t member);
    void takeStepOfInspection(uint32_t step, const LinkType& now);
    void enter(FunctionID callee);

private:
    using Source = SourceInLinkCalls;
    using Values = ValuesInLinkCalls;
    using Passed = PassedInLinkCalls;
    using Fit = FitInLinkCalls;
    using Questions = QuestionsInLinkCalls;
    using RowCall = RowCallInLinkCalls;
    using SharedCall = SharedCallInLinkCalls;

    enum class Entry : uint8_t { Refused, Unseen, Body };
    // FoundWhereProgramMayWrite: in the list of answers alone. `member` is the location, which is asked of BuiltinsInReach each time: findOnce().
    enum class Lookup : uint8_t { Unknown, Absent, Found, FoundWhereProgramMayWrite };
    enum class WhenAbsent : uint8_t { CallThrows, MayHaveBeenAdded };

    struct MemberOfTable {
        Lookup outcome { Lookup::Unknown };
        BuiltinID member;
    };

    FlowNodeID nodeAt(uint32_t inSummary) const { return m_shared ? m_shared->nodes[inSummary] : m_flow.nodeOf(m_caller, inSummary); }
    FlowNodeID thrownNode() const { return m_shared ? m_shared->thrown : nodeTakingWhatIsThrown(m_flow, m_caller, m_call.thrownTo); }
    LinkType heldAt(FlowNodeID node) const { return node == m_flow.unseen() ? LinkType::anything() : m_flow.typeAt(node); }
    LinkFlow::CalleesOfCall& callees() { return m_shared ? m_flow.calleesOfSharedCalls[m_shared->index] : m_flow.calleesOfCalls[m_number]; }
    bool lists(FunctionID callee) { return m_shared ? m_state.listedBySharedCalls.contains(static_cast<uint64_t>(m_shared->index) << 32 | callee.index()) : callees().listed.contains(callee); }
    void list(FunctionID);
    // A CALL THAT HAS CALLED UNSEEN CODE GOES ON: the callees of the program are still to be handed what it passes, for held(). Its results hold anything, it throws anything and every operand has left:
    // no row adds to that but what a guess can do without: the types that a row would hand a callback.
    bool isSettledButForOwnCallees() const { return m_state.hasCalledUnseen(m_number); }
    const Vector<ResultInLinkCalls, 1>& results();
    const Passed& passedByCall();
    Passed passedWithReceiver(const LinkType&);
    const MembersUnderKey& membersUnderKey();
    static uint8_t beyond(const Source* source) { return source ? std::min<uint8_t>(source->distance + 1, notObservedForLinkCalls) : 1; }

    // ---- Values
    void add(Values&, const LinkType&);
    void add(Values&, FlowNodeID, uint8_t distance, TypeID passes = { });
    void add(Values&, const Values&);
    Values valuesOf(const LinkType&);
    Values valuesAtNode(uint32_t inSummary, TypeID passes = { });
    Values valuesAtSource(const Source&);
    void flowInto(const Values&, FlowNodeID);
    void escape(const Values&);
    void escape(const LinkType&, const Source*);
    void escape(const Passed&);
    bool watch(FlowNodeID, uint8_t distance);
    LinkType typeAt(const Source&);
    void give(const Values&);
    void throwError();
    void throwAnything();

    template<typename Functor>
    void forEachType(const Values& values, const Functor& functor)
    {
        for (size_t index = 0; index < values.sources.size(); ++index) {
            Source source = values.sources[index];
            LinkType type = typeAt(source);
            if (!type.isNothing())
                functor(type, &source);
        }
        if (!values.made.isNothing()) {
            LinkType made = values.made;
            functor(made, static_cast<const Source*>(nullptr));
        }
    }

    template<typename Functor>
    void forEachClassAtOrAbove(BuiltinClassID builtinClass, const Functor& functor)
    {
        for (BuiltinClassID current = builtinClass; current; current = m_builtins.at(current).parent)
            functor(current);
    }

    // Over a copy: what the functor lays down may make types.
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

    // Functions::Constructors: the class objects of the layouts of the family.
    template<typename Functor>
    void forEachConstructorOf(FamilyID family, const Functor& functor)
    {
        forEachLayoutOf(family, [&](LayoutID member) {
            FunctionID constructor = m_program.at(member).constructor;
            if (constructor && m_flow.classOfConstructor(constructor) == member)
                functor(constructor);
        });
    }

    // ---- The four kinds of callee
    void callSubject();
    void callName();
    void callBuiltinOfCall();
    enum class UnknownIs : uint8_t { ByEscapeAlone = 1, InSight = 2 };
    static UnknownIs howUnknownIs(const LinkType& type) { return type.holdsUnseenInSight() ? UnknownIs::InSight : UnknownIs::ByEscapeAlone; }
    void callUnseen(UnknownIs = UnknownIs::InSight);
    void callValue(LinkType callee, const Source*, bool mayGoByRows, const Passed&);
    void callValueAt(FlowNodeID, const Passed&);
    // What the call gives.
    Values callThroughSharedCall(SharedCall::Subjects, FamilyID, const Passed&);
    void callMemberOfInstance(LayoutID);
    void callMemberOfPrototype(LayoutID);
    void callExportOf(const Layout&);
    void callMemberOfClosure(FunctionID);
    void callMemberOfClass(const LinkType& instances);
    void callMemberOfTable(MemberOfTable, Values receiver, WhenAbsent);
    MemberOfTable findAbove(BuiltinID firstHolder);
    MemberOfTable findOnInstancesOf(BuiltinClassID builtinClass) { return findOnInstancesOf(builtinClass, m_call.key); }
    MemberOfTable findOnInstancesOf(BuiltinClassID, PropertyKeyID);
    MemberOfTable findAbove(BuiltinID firstHolder, PropertyKeyID, const MembersUnderKey&);
    MemberOfTable findOnInstancesOf(BuiltinClassID, PropertyKeyID, const MembersUnderKey&);
    MemberOfTable findOnce(uint64_t question, const Invocable<MemberOfTable()> auto& find);

    // ---- Functions of the program
    void enter(FunctionID callee, const Passed&);
    void enterByBinding(FunctionID callee);
    bool handsOnOwnNewTarget(const Passed&);
    bool handsNewTargetUpRowOfClasses(FunctionID callee, const Passed&);
    Entry passTo(FunctionID callee, const Passed&, Values& constructed);
    Values objectsConstructed(const Passed&, FunctionID callee);
    LinkType objectsThatNoNodeHolds(LayoutID);
    Values resultOf(FunctionID callee, uint32_t index, bool isConstruction, const Values& constructed, uint8_t distance);
    // AOTLinkKnownCalleesInlines.h
    template<typename Functor> std::optional<Values> resultThatIsParameter(FunctionID callee, uint32_t index, const Functor& passedAt);
    Values resultGivenTo(FunctionID callee, uint32_t index, const Passed&, const Values& constructed);
    Values resultGivenToCallByBinding(FunctionID callee, uint32_t index, const Values& constructed);
    Values passToLostFunctions(const Passed&, const LinkType& callee, const Source*);
    Values passToUnseen(const Passed&);
    // What native code calls.
    Values callValues(const Values& callees, const Passed&, bool mayGoByRows);

    // ---- Rows
    Values callBuiltin(BuiltinID, const Passed&, bool isCalledByBuiltin);
    void notePreparationRegisteredBy(BuiltinID, const Passed&);
    Values callOutOfSight(const RowCall&);
    void noteOptionThatRunsCodeAmong(BuiltinID, const Passed&);
    void enterCodeBehindRow(RowCall&);
    bool rowsSpeakOf(const RowCall&);
    bool rowsMayBeTakenAtTheirWord(BuiltinID);
    void describeWhatRowRefuses(const RowCall&, const BuiltinOverload&);
    Fit fitOf(const RowCall&, const BuiltinOverload&);
    Fit fitOf(const RowCall&, const BuiltinParameter&);
    void addFit(const LinkType&, const Source*, const BuiltinType& accepted, Fit&);
    bool isAsClassMakesIt(const LinkType& instances, PristineAspectSet, const Source*);
    bool membersAboveInstancesAreWhatTableLists(BuiltinClassID);
    bool indexesAreAbsentAbove(BuiltinClassID);
    void applyRow(RowCall&);
    void applyParameter(RowCall&, const BuiltinParameter&);
    void applyCallback(RowCall&, const BuiltinCallback&);
    void applyHandlerCall(RowCall&, const BuiltinHandlerCall&);
    void applyAction(RowCall&, const BuiltinPropertyAction&);
    void addResult(RowCall&);

    Values valuesAt(const RowCall&, BuiltinOperand);
    Values valuesHandedOn(RowCall&, BuiltinOperand);
    Values valuesAtPlace(RowCall&, const BuiltinPlace&);
    Values valuesOfType(RowCall&, BuiltinTypeID);
    void addValuesOfType(RowCall&, BuiltinTypeID, bool isResult, Values&);
    void addValuesOfPart(RowCall&, const BuiltinTypePart&, BuiltinFreshness, bool isResult, Values&);
    void addInstanceOf(RowCall&, const BuiltinTypePart&, BuiltinClassID, std::span<const BuiltinSlotContents> listed, bool isResult, Values&);
    void addRecordOf(RowCall&, const BuiltinTypePart&, bool isResult, Values&);
    SiteID siteOf(const BuiltinTypePart&, BuiltinClassID);
    SlotOfContents slotOfField(const BuiltinTypePart&, uint16_t fieldIndex);
    Values valuesHandedOutBy(IterationKind, const Values& iterated);
    void addContentsOf(const LinkType& holder, const Source*, Values&);
    void addContentsOfSlot(RowCall&, const Values& holders, BuiltinSlotID, Values&);
    void addIteratedValues(const Values& iterables, Values&);
    Values valuesIteratedOf(const Values& iterables);
    void addResultsOfCallbacks(RowCall&, BuiltinOperand, Values&);
    void addPrototypes(const Values& objects, Values&);
    Values contentsOf(const Values& holders);
    void settle(Values&);
    void storeAt(RowCall&, const BuiltinPlace&, const Values&);
    void storeInContents(SiteID, SlotOfContents, const Values&);
    bool isValueOfPromise(BuiltinClassID holder, SlotOfContents) const;
    void storeInSlot(const Values& holders, BuiltinSlotID, const Values&);
    bool mayBeMissingAt(const BuiltinPlace&) const;
    bool isFollowed(BuiltinClassID) const;
    bool mayBeListWithLayout(const LinkType&);

    // False: the result may also be an object that nobody follows. `list.map(f)` reads `constructor` off the list, which takes any name, and what it finds there makes the result.
    // The functor is handed the site of each part of the result that makes an object, and the part.
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
                    functor(siteOf(part, part.builtinClass), part);
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

    // ---- What native code asks of an operand
    Questions questionsOf(const BuiltinParameter&);
    void addKeys(Questions&, std::span<const BuiltinName>);
    void addKeysAnsweredByProgramAlone(Questions&, std::span<const BuiltinName>);
    void addConversion(Questions&);
    bool ask(const Values&, const Questions&);
    bool isDataDescriptorInSight(const Source*) const;
    bool mayAnswer(const LinkType&, const Source*, const Questions&, bool ownValuesAreFollowed);
    bool ownValuesAreListed(const LinkType&);
    bool mayFindUnderNumbersAbove(const LinkType&, const Source*);
    template<typename Functor> void forEachLayoutWithListedOwnValues(const LinkType&, const Functor&);
    void inspect(const Values&, const Questions&);
    void inspectAt(FlowNodeID, uint32_t inspection);
    void inspectType(const LinkType&, uint32_t inspection);
    enum class SubjectOfInspection : uint8_t { Layout, Function, Site };
    bool askInspected(SubjectOfInspection, uint32_t number, const LinkType& subject, uint32_t inspection);
    void askAgain(uint32_t step);
    bool mayFind(const LinkType& objects, PropertyKeyID, uint8_t distance, bool membersOfTableAnswerNothing = false);

    LinkFlow& m_flow;
    StateOfLinkCalls& m_state;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    FunctionID m_caller;
    uint32_t m_index { 0 };
    // In LinkFlow::calleesOfCalls. Of a shared call: behind all of those, by its index.
    uint32_t m_number { 0 };
    SharedCall* m_shared { nullptr };
    // In StateOfLinkCalls::membersOfSharedCalls.
    uint32_t m_memberThatWatches { 0 };
    const ThinSummary& m_summary;
    const SummaryCall& m_call;

    Vector<ResultInLinkCalls, 1> m_results;
    bool m_hasLookedForResults { false };
    // In place of the call: whoever is put at a node that a question is decided by.
    std::optional<uint32_t> m_stepThatWatches;
    std::optional<Passed> m_passed;
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

// A copy: what is laid at a result may bring another caller alive, and the list of the state moves.
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
    if (m_shared ? m_state.listedBySharedCalls.add(static_cast<uint64_t>(m_shared->index) << 32 | callee.index()).isNewEntry : !callees().listed.contains(callee))
        callees().listed.append(callee);
}

const PassedInLinkCalls& CallInLinkCalls::passedByCall()
{
    if (m_passed)
        return *m_passed;

    // Filled where it stays.
    Passed& passed = m_passed.emplace();
    if (m_shared) {
        if (m_call.receiver != noSummaryNode)
            passed.receiver = valuesAtNode(m_call.receiver);
        for (uint32_t argument = 0; argument + m_shared->hasList < m_call.numberOfArguments; ++argument)
            passed.arguments.append(valuesAtNode(m_summary.arguments[argument]));
        if (m_shared->hasList)
            passed.list = valuesAtNode(m_summary.arguments.last());
        passed.isConstruction = m_call.isConstruction;
        if (m_call.newTarget != noSummaryNode)
            passed.newTarget = valuesAtNode(m_call.newTarget);
        return passed;
    }
    // What finds a function of the program under a name is an object.
    if (m_call.receiver != noSummaryNode)
        passed.receiver = valuesAtNode(m_call.receiver, m_call.callee == SummaryCall::Callee::Name ? TypeID(TypeTable::object) : TypeID());
    for (uint32_t argument = 0; argument < m_call.numberOfArguments; ++argument) {
        Values values = valuesAtNode(m_summary.arguments[m_call.firstArgument + argument]);
        if (!m_call.passesList) {
            passed.arguments.append(WTF::move(values));
            continue;
        }
        if (!passed.list)
            passed.list = Values();
        add(*passed.list, values);
    }
    if (m_call.passesList && !passed.list)
        passed.list = Values();
    passed.isConstruction = m_call.isConstruction;
    if (m_call.newTarget != noSummaryNode)
        passed.newTarget = valuesAtNode(m_call.newTarget);
    return passed;
}

// What a name has found on one kind of receiver gets that kind as `this`, and not whatever else stands at the receiver.
// It is laid down and comes by no edge: flowInto() lays the objects of a layout down as a source does.
PassedInLinkCalls CallInLinkCalls::passedWithReceiver(const LinkType& receiver)
{
    Passed passed = passedByCall();
    passed.receiver = valuesOf(receiver);
    return passed;
}

const MembersUnderKey& CallInLinkCalls::membersUnderKey()
{
    if (!m_membersUnderKey)
        m_membersUnderKey.emplace(m_program, m_call.key);
    return *m_membersUnderKey;
}

// ---- Values

void CallInLinkCalls::add(Values& values, const LinkType& made)
{
    if (made.isNothing())
        return;
    values.made = values.made.isNothing() ? made : m_flow.joinOf(values.made, made);
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
    add(values, added.made);
}

ValuesInLinkCalls CallInLinkCalls::valuesOf(const LinkType& made)
{
    Values values;
    values.made = made;
    return values;
}

ValuesInLinkCalls CallInLinkCalls::valuesAtNode(uint32_t inSummary, TypeID passes)
{
    Values values;
    if (inSummary == noSummaryNode)
        add(values, primitivesForLinkCalls(Primitive::Undefined));
    else
        add(values, nodeAt(inSummary), 0, passes);
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

    // A type that stays as it is would tell nobody behind `to` that the layout has escaped.
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

// False: nobody would tell of a climb there.
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
        if (!m_flow.typeAt(node).isNothing())
            ++m_state.membersOfSharedCalls[m_memberThatWatches].tellsThatBringNothing;
        m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfSharedCall, m_caller, m_memberThatWatches });
        return true;
    }
    if (node == m_flow.unseen() || !m_state.observed[m_flow.laneOfObserversOf(m_caller)].add(static_cast<uint64_t>(m_number) << 32 | node.index()).isNewEntry)
        return true;
    if (!m_flow.typeAt(node).isNothing())
        ++m_state.tellsThatBringNothing[m_number];
    m_flow.observe(node, FlowObserver { FlowObserver::Kind::SubjectOfCall, m_caller, m_index });
    return true;
}

LinkType CallInLinkCalls::typeAt(const Source& source)
{
    if (!watch(source.node, source.distance)) {
        m_flow.addEdge(source.node, m_flow.unseen(), source.passes);
        return LinkType::anything();
    }
    return m_flow.passedBy(m_flow.typeAt(source.node), source.passes);
}

void CallInLinkCalls::give(const Values& values)
{
    if (values.isEmpty())
        return;
    for (const ResultInLinkCalls& result : results()) {
        if (result.index)
            m_flow.arrive(result.node, LinkType::anything());
        else
            flowInto(values, result.node);
    }
}

// An error that the engine makes.
void CallInLinkCalls::throwError()
{
    if (!std::exchange(m_throwsError, true)) {
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrownNode(), unseenObjectsForLinkCalls());
    }
}

void CallInLinkCalls::throwAnything()
{
    if (!std::exchange(m_throwsAnything, true)) {
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrownNode(), LinkType::anything());
    }
}

// ---- The four kinds of callee

void CallInLinkCalls::resolve()
{
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

void CallInLinkCalls::callUnseen(UnknownIs unknownIs)
{
    // A shared call is told for all who go by it.
    uint8_t level = m_shared ? 2 : static_cast<uint8_t>(unknownIs);
    uint8_t levelBefore = m_state.raiseCallOfUnseen(m_number, level);
    if (levelBefore >= level)
        return;
    if (levelBefore) {
        // Once more, unmarked.
        for (const ResultInLinkCalls& result : results())
            m_flow.arrive(result.node, LinkType::anything());
        return;
    }
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(level == 1, LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
    callees().mayEnterUnseen = true;
    if (m_flow.testRuns().isTestRun() && m_call.numberOfArguments && m_summary.arguments[m_call.firstArgument] != noSummaryNode)
        m_flow.testRuns().noteCallOutOfSight(m_caller, m_call.site, nodeAt(m_summary.arguments[m_call.firstArgument]));
    if (m_call.callee == SummaryCall::Callee::Name && m_call.key && (m_shared || !m_flow.rowsSpeakFor(m_caller)))
        m_flow.note(LinkFlow::Note::NameCalled, m_call.key.bits(), !m_flow.codeAtWorkIsOfLibrary());
    auto leave = [&](uint32_t node) {
        if (node != noSummaryNode)
            m_flow.addEdge(nodeAt(node), m_flow.unseen());
    };
    leave(m_call.subject);
    leave(m_call.receiver);
    leave(m_call.newTarget);
    for (uint32_t argument = 0; argument < m_call.numberOfArguments; ++argument)
        leave(m_summary.arguments[m_call.firstArgument + argument]);
    // testing=UnseenCodeAnswersUndefined, wrong on purpose: wherever what such a call gives flows to, Link proves too much.
    bool answersUndefined = AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::UnseenCodeAnswersUndefined);
    for (const ResultInLinkCalls& result : results())
        m_flow.arrive(result.node, answersUndefined ? primitivesForLinkCalls(Primitive::Undefined) : LinkType::anything());
    throwAnything();
}

void CallInLinkCalls::notePreparationRegisteredBy(BuiltinID builtin, const Passed& passed)
{
    if (!m_builtins.has(BuiltinMark::RegistersPreparationOfHolders, m_builtins.canonicalID(builtin)))
        return;
    bool isKnown = !passed.arguments.isEmpty() && passed.arguments[0].made.isNothing() && !passed.arguments[0].sources.isEmpty();
    for (size_t source = 0; isKnown && source < passed.arguments[0].sources.size(); ++source)
        isKnown = !passed.arguments[0].sources[source].distance;
    if (!isKnown) {
        m_flow.noteUnknownSymbolHandedToPreparation();
        return;
    }
    for (const Source& source : passed.arguments[0].sources)
        m_flow.noteSymbolHandedToPreparation(m_caller, source.node);
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
    // Nothing but the closures of a function that the call has entered: enter() would go no further, and nobody need list what is passed.
    LinkType callee = heldAt(subject.node);
    callee.primitives = 0;
    if (callee.functions == LinkType::Functions::Closure && callee == closuresForLinkCalls(callee.function) && m_call.newTarget == noSummaryNode
        && lists(callee.function) && callees().builtins.isEmpty() && !callees().mayEnterUnseen) {
        throwError();
        return;
    }
    callValue(heldAt(subject.node), &subject, true, passedByCall());
}

void CallInLinkCalls::callBuiltinOfCall()
{
    if (!m_call.builtin) {
        callUnseen();
        return;
    }
    // The walk puts the symbol in the place of the call, and asks the same: keyOfSymbolMadeOnce().
    if (keyOfSymbolMadeOnceBy(m_flow.program, m_call)) {
        LinkType symbol;
        symbol.primitives = LinkType::oneSymbol;
        for (const ResultInLinkCalls& result : results())
            m_flow.arrive(result.node, result.index ? LinkType::anything() : symbol, m_call.key);
        return;
    }
    if (!isSettledButForOwnCallees())
        give(callBuiltin(m_call.builtin, passedByCall(), false));
}

void CallInLinkCalls::callValue(LinkType callee, const Source* source, bool mayGoByRows, const Passed& passed)
{
    if (callee.isNothing())
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
    case LinkType::Functions::Lost:
        give(passToLostFunctions(passed, callee, source));
        break;
    }

    if (callee.builtin && !isSettledButForOwnCallees()) {
        if (mayGoByRows) {
            callees().add(m_builtins.canonicalID(callee.builtin));
            give(callBuiltin(callee.builtin, passed, false));
        } else
            callUnseen();
    }
    if (callee.objects != LinkType::Objects::None || callee.classes != LinkType::Classes::None)
        callUnseen();
    else if (callee.holdsUnseen)
        callUnseen(howUnknownIs(callee));
}

// What the key has found lies there. A store over it is seen as a climb.
void CallInLinkCalls::callValueAt(FlowNodeID node, const Passed& passed)
{
    Source source { node, 1, { } };
    watch(node, source.distance);
    callValue(heldAt(node), &source, true, passed);
}

void CallInLinkCalls::callName()
{
    if (!m_call.key || m_call.receiver == noSummaryNode) {
        callUnseen();
        return;
    }
    LinkType receiver = heldAt(nodeAt(m_call.receiver));
    if (receiver.isNothing())
        return;
    throwError();

    switch (receiver.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        callMemberOfInstance(receiver.layout);
        break;
    case LinkType::Objects::Prototype:
        callMemberOfPrototype(receiver.layout);
        break;
    case LinkType::Objects::Prototypes:
        give(callThroughSharedCall(SharedCall::Subjects::PrototypeObjects, receiver.family, passedByCall()));
        break;
    // The callees are the methods of the layouts that can arrive. Read through the aggregate, two methods under one key would both be lost.
    case LinkType::Objects::Family:
        give(callThroughSharedCall(SharedCall::Subjects::Instances, receiver.family, passedByCall()));
        break;
    // Members join as Link goes, and the aggregate is told.
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
    case LinkType::Functions::Lost:
        callUnseen();
        break;
    }

    switch (receiver.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site:
        callMemberOfClass(instancesAmongForLinkCalls(receiver));
        break;
    case LinkType::Classes::Mixed:
        callUnseen();
        break;
    }

    for (uint16_t group : primitivesWithClassesForLinkCalls) {
        if (isSettledButForOwnCallees())
            break;
        if (uint16_t present = receiver.primitives & group)
            callMemberOfTable(findOnInstancesOf(primitiveClassOf(LinkType::inNamesOfPrimitive(present))), valuesOf(primitivesForLinkCalls(present)), WhenAbsent::CallThrows);
    }
    if (receiver.builtin && !isSettledButForOwnCallees())
        callMemberOfTable(findAbove(receiver.builtin), valuesOf(builtinForLinkCalls(receiver.builtin)), WhenAbsent::MayHaveBeenAdded);
    if (receiver.holdsUnseen)
        callUnseen(howUnknownIs(receiver));
}

// The operands flow into the nodes of the shared call, and what it gives and throws flows back: an edge each, however many members the family has. The first to come makes the shared call, which looks at each member once.
ValuesInLinkCalls CallInLinkCalls::callThroughSharedCall(SharedCall::Subjects subjects, FamilyID family, const Passed& passed)
{
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    // `super()`: whether the new.target escapes hangs on who hands it on, and to whom: handsNewTargetUpRowOfClasses(). Any other new.target escapes whoever is entered, and goes by a node like every operand.
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
                add(answers, resultOf(constructor, 0, passed.isConstruction, constructed, notObservedForLinkCalls));
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
        for (uint32_t argument = 0; argument < made->call.numberOfArguments; ++argument) {
            made->summary.arguments.append(static_cast<uint32_t>(made->nodes.size()));
            made->nodes.append(m_flow.makeSharedNode());
        }
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
        forEachLayoutOf(family, [&](LayoutID member) {
            FunctionID constructor = m_program.at(member).constructor;
            bool hasClassObject = constructor && m_flow.classOfConstructor(constructor) == member;
            if (hasClassObject || subjects == SharedCall::Subjects::Instances || subjects == SharedCall::Subjects::PrototypeObjects)
                m_state.membersOfSharedCalls.append(MemberOfSharedCallInLinkCalls { index, member, 0 });
        });
        uint32_t endOfMembers = static_cast<uint32_t>(m_state.membersOfSharedCalls.size());
        for (uint32_t member = firstMember; member < endOfMembers; ++member)
            CallInLinkCalls(m_flow, m_state, *m_state.sharedCalls[index]).resolveMemberOfSharedCall(member);
    }

    const SharedCall& shared = *m_state.sharedCalls[index];
    if (!callees().shared.contains(index))
        callees().shared.append(index);
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

// As a read of the key goes (AccessInLinkAccesses::resolveOnLayout()). An object has the data field if it has been stored by then and not deleted since, else the lookup goes on upward: whatever it may come to is called.
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
    // What nobody lists may stand in front of whatever the name finds.
    if (nameClass == NameClass::Proto || m_flow.escapedLayouts.get(id.index()) || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        bool givesUpInSight = nameClass == NameClass::Proto || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        callUnseen(givesUpInSight ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
        if (givesUpInSight)
            return;
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
        // The lookup runs it, and what it answers is called.
        case MethodKind::Getter:
        case MethodKind::Setter:
            callUnseen();
            return;
        }
    }

    // Anything, once the layout escapes or a change puts something beside its fields. And what a store has laid under the key where no field takes it.
    callValueAt(m_flow.foundBesideFieldsOf(id), passed);
    if (!field)
        callValueAt(m_flow.appendedField(id, m_call.key), passed);

    // A private name is found on the object itself.
    bool isPrivateName = m_program.at(m_call.key).kind == PropertyKeyKind::PrivateName;
    if (FlowNodeID foundOnPrototypes = isPrivateName ? FlowNodeID() : m_flow.foundOnPrototypesAbove(id, m_call.key))
        callValueAt(foundOnPrototypes, passed);
    bool isAnsweredByClass = !members.lookupGoesOnAboveClasses();
    if (isAnsweredByClass || isPrivateName || m_flow.isAbsentAboveClasses(m_call.key))
        return;
    // An instance finds its class there.
    if (nameClass == NameClass::Constructor) {
        callUnseen();
        return;
    }
    // By an edge: a row may hand the receiver on to where nobody looks at the node beside its fields.
    BuiltinClassID objectClass = BuiltinClasses::Object;
    callMemberOfTable(findAbove(m_builtins.at(objectClass).prototype), valuesAtNode(m_call.receiver, TypeTable::object), WhenAbsent::MayHaveBeenAdded);
}

// A node that holds a prototype object does not climb when the class escapes. A Member node does.
void CallInLinkCalls::callMemberOfPrototype(LayoutID id)
{
    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_call.key);
    FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_call.key);
    // Where no class answers for sure the lookup goes on above the classes.
    bool isAnsweredByProgram = !members.lookupGoesOnAboveClasses() || ((members.mayAnswer() || foundOnPrototypes) && m_flow.isAbsentAboveClasses(m_call.key));
    if (!isAnsweredByProgram || m_flow.escapedLayouts.get(id.index()) || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        bool givesUpInSight = !isAnsweredByProgram || m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        callUnseen(givesUpInSight ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
        if (givesUpInSight)
            return;
    }
    if (foundOnPrototypes) {
        callValueAt(foundOnPrototypes, passedByCall());
        // It holds anything once the layout has escaped: the node of the prototype object does not climb then.
        callValueAt(m_flow.foundBesideFieldsOf(id), passedByCall());
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

// The fields of such a layout stand for the exports of the module, in their order. The values lie in variables.
void CallInLinkCalls::callExportOf(const Layout& layout)
{
    std::optional<uint32_t> field = layout.namespaceOf ? layout.findField(m_call.key) : std::nullopt;
    if (!field) {
        callUnseen();
        return;
    }

    // An export may hand on that of another module, and that one of a third.
    ModuleID module = layout.namespaceOf;
    uint32_t index = *field;
    for (size_t steps = 0; steps <= m_program.modules.size(); ++steps) {
        const auto& exports = m_program.at(module).exports;
        if (index >= exports.size())
            break;
        const ModuleExport& entry = exports[index];
        if (m_flow.testRuns().mayComeThroughMock(module, index)) {
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

// A class object is the closure of its constructor. A closure takes any name: what a store has laid under one that its class does not list lies beside the fields, and whatever else may stand in front of what Function.prototype has lets the function escape.
void CallInLinkCalls::callMemberOfClosure(FunctionID function)
{
    const Function& closed = m_program.at(function);
    LayoutID layout = closed.constructedClass;
    // What is stored on a constructor function lies where it lies on any closure.
    if (layout && m_flow.isOfConstructorFunction(layout))
        layout = { };
    if (layout && m_flow.namesMayBeIncomplete(layout, ClassSide::Static)) {
        callUnseen();
        return;
    }

    Passed passed = passedWithReceiver(closuresForLinkCalls(function));
    bool hasFound = false;
    uint32_t stepsLeft = static_cast<uint32_t>(m_program.layouts.size());
    for (LayoutID current = layout; current && stepsLeft--; current = m_program.at(current).base) {
        const Layout& record = m_program.at(current);
        // Under a computed key.
        if (std::ranges::any_of(record.staticFields, [](const StaticField& field) { return !field.key; })) {
            callUnseen();
            return;
        }
        std::optional<uint32_t> field = record.findStaticField(m_call.key);
        if (!field)
            continue;
        hasFound = true;
        callValueAt(m_flow.variable(record.staticFields[*field].variable), passed);
        break;
    }
    LinkFlow::FoundMembers members;
    if (layout)
        members = m_flow.findMembers(layout, ClassSide::Static, m_call.key);
    for (const LinkFlow::FoundMember& member : members) {
        hasFound = true;
        switch (member.kind) {
        case MethodKind::Method:
            callValueAt(m_flow.member(member.classLayout, ClassSide::Static, member.index), passed);
            break;
        case MethodKind::Getter:
        case MethodKind::Setter:
            callUnseen();
            return;
        }
    }
    if (hasFound)
        return;

    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    if (nameClass == NameClass::Proto || nameClass == NameClass::Prototype || mayBeElementKey(nameClass)) {
        callUnseen();
        return;
    }
    callValueAt(m_flow.foundOnClosuresOf(function), passed);
    callValueAt(m_flow.followsNamesOnClosuresOf(function, m_call.key) ? m_flow.laidOnClosuresOf(function, m_call.key) : m_flow.foundBesideFieldsUnder(m_call.key), passed);
    callMemberOfTable(findOnInstancesOf(classOfClosuresForLinkCalls(closed)), *passed.receiver, WhenAbsent::CallThrows);
}

// Such an object takes any name. What a store has laid under the key lies under unknown names, what a row has laid there in the slot under the key, and whatever else may stand in front of what the class has lets the object reach unseen code, which opens what lies under unknown names.
void CallInLinkCalls::callMemberOfClass(const LinkType& instances)
{
    BuiltinClassID builtinClass = instances.builtinClass;
    NameClass nameClass = m_flow.nameClassOf(m_call.key);
    // An element is called, or what the object stands below. Of some classes no instance is as the table says.
    if (nameClass == NameClass::Proto || mayBeElementKey(nameClass) || !m_builtins.mayHavePristineInstances(builtinClass)) {
        callUnseen();
        return;
    }

    Passed passed = passedWithReceiver(instances);
    callValueAt(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()), passed);
    callValueAt(m_flow.contentsOf(instances, SlotOfContents::under(m_call.key)), passed);
    callMemberOfTable(findOnInstancesOf(builtinClass), *passed.receiver, WhenAbsent::CallThrows);
}

void CallInLinkCalls::callMemberOfTable(MemberOfTable found, Values receiver, WhenAbsent whenAbsent)
{
    if (isSettledButForOwnCallees())
        return;
    switch (found.outcome) {
    case Lookup::Unknown:
        callUnseen();
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
    Passed passed = passedByCall();
    passed.receiver = WTF::move(receiver);
    callees().add(found.member);
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
        // What the table lists may well lie there, and the call enters it with operands that nobody sees.
        m_flow.noteLossOf(canonical);
        return { };
    }
    return { Lookup::Found, canonical };
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findAbove(BuiltinID firstHolder)
{
    return findOnce(static_cast<uint64_t>(firstHolder.index()) << 32 | m_call.key.index(), [&] {
        return findAbove(firstHolder, m_call.key, membersUnderKey());
    });
}

CallInLinkCalls::MemberOfTable CallInLinkCalls::findOnInstancesOf(BuiltinClassID builtinClass, PropertyKeyID key)
{
    if (!builtinClass)
        return { };
    return findOnce(1ull << 62 | static_cast<uint64_t>(builtinClass.index()) << 32 | key.index(), [&] {
        return key == m_call.key ? findOnInstancesOf(builtinClass, key, membersUnderKey()) : findOnInstancesOf(builtinClass, key, MembersUnderKey(m_program, key));
    });
}

// Each object that the lookup passes by can never get the key, and stays below what it stands below.
CallInLinkCalls::MemberOfTable CallInLinkCalls::findAbove(BuiltinID firstHolder, PropertyKeyID key, const MembersUnderKey& members)
{
    BuiltinID holder = m_builtins.canonicalID(firstHolder);
    while (true) {
        // Compiled code makes the object of such a module, and each module has its own of an object of that scope: the table speaks of neither.
        if (compiledModuleWithObject(m_program, holder) || m_builtins.at(m_builtins.rootOf(holder)).scope != BuiltinScope::Realm)
            return { };
        if (BuiltinID member = members.findOwnMember(holder)) {
            if (!members.isLockedLocation(holder))
                return { Lookup::FoundWhereProgramMayWrite, member };
            return { Lookup::Found, m_builtins.canonicalID(member) };
        }
        if (!listsEveryMemberUnder(m_program, holder, key) || !members.isClosedKey(holder) || !m_builtins.hasLockedPrototypeLink(holder))
            return { };
        const Builtin& object = m_builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return { };
        case BuiltinPrototypeLink::Null:
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
    // The object may be of a class below, whether or not an instance of that one is ever as its class makes it.
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

// ---- Functions of the program

void CallInLinkCalls::enter(FunctionID callee)
{
    if (m_call.callee == SummaryCall::Callee::Binding)
        enterByBinding(callee);
    else
        enter(callee, passedByCall());
}

void CallInLinkCalls::enter(FunctionID callee, const Passed& passed)
{
    // What a construction makes goes by the type at new.target. A callee that native code has entered too was passed other operands.
    if (lists(callee) && callees().builtins.isEmpty() && !callees().mayEnterUnseen && !passed.newTarget) {
        // Two layouts find one method, and each brings its own objects. The callee may be listed for one layout that was laid down at `this` and came by no edge:
        // a receiver that has since lost its layout finds the method by the call itself, and what stands at the call has to come too. An edge that lies is not laid again.
        std::optional<unsigned> thisParameter = m_program.at(callee).thisParameterIndex();
        if (!passed.isConstruction && passed.receiver && thisParameter)
            flowInto(*passed.receiver, m_flow.parameter(callee, *thisParameter));
        return;
    }

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
    for (const ResultInLinkCalls& result : results())
        flowInto(resultOf(callee, result.index, passed.isConstruction, constructed, notObservedForLinkCalls), result.node);
}

// The arguments are the entry parameters of the callee, in their order.
void CallInLinkCalls::enterByBinding(FunctionID callee)
{
    if (lists(callee))
        return;
    list(callee);

    const Function& function = m_program.at(callee);
    if (!function.hasBody()) {
        callUnseen();
        m_flow.reach(callee);
        return;
    }
    // The new.target is one of the arguments, and nobody looks at which.
    if (m_call.isConstruction && function.constructedClass)
        m_flow.noteAllocated(function.constructedClass);
    if (!m_call.isConstruction && !m_flow.functionsEnteredByPlainCalls.get(callee.index()))
        m_flow.note(LinkFlow::Note::FunctionEnteredByPlainCall, callee.index());

    uint32_t numberOfParameters = function.numberOfEntryParameters();
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        FlowNodeID to = m_flow.parameter(callee, parameter);
        if (parameter < m_call.numberOfArguments)
            flowInto(valuesAtNode(m_summary.arguments[m_call.firstArgument + parameter]), to);
        else
            m_flow.arrive(to, LinkType::anything());
    }
    for (uint32_t argument = numberOfParameters; argument < m_call.numberOfArguments; ++argument)
        escape(valuesAtNode(m_summary.arguments[m_call.firstArgument + argument]));

    Values constructed;
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    if (m_call.isConstruction && thisParameter && *thisParameter < m_call.numberOfArguments)
        constructed = valuesAtNode(m_summary.arguments[m_call.firstArgument + *thisParameter]);
    else if (m_call.isConstruction && function.takesThisFromConstruction())
        add(constructed, LinkType::anything());
    for (const ResultInLinkCalls& result : results())
        flowInto(resultOf(callee, result.index, m_call.isConstruction, constructed, notObservedForLinkCalls), result.node);

    m_flow.addEdge(m_flow.thrown(callee), thrownNode());
    m_flow.reach(callee);
}

// `super()`: the constructor of a derived class hands its own new.target to the constructor of the class that it is written below.
bool CallInLinkCalls::handsOnOwnNewTarget(const Passed& passed)
{
    const Function& caller = m_program.at(m_caller);
    std::optional<unsigned> ownNewTarget = caller.newTargetParameterIndex();
    // Who came first to a shared call says nothing of the others.
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

CallInLinkCalls::Entry CallInLinkCalls::passTo(FunctionID callee, const Passed& passed, Values& constructed)
{
    const Function& function = m_program.at(callee);
    // `o.#x` under a private accessor is written as a call of the closure, which the class keeps to itself. Whoever else has the closure of an accessor calls it like any function. The tables list no such call: isEnteredByCallsAloneForLink().
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
    if (!function.hasBody()) {
        passToUnseen(passed);
        m_flow.reach(callee);
        return Entry::Unseen;
    }
    // In front of what is read off its entry parameters: reach() lays the object that a construct call makes at `this`.
    m_flow.reach(callee);
    if (!passed.isConstruction && !m_flow.functionsEnteredByPlainCalls.get(callee.index()))
        m_flow.note(LinkFlow::Note::FunctionEnteredByPlainCall, callee.index());
    // Which fields an object has when it is first seen goes by the row of constructors of its class: LinkFlow::noteConstructionsOf(). A new.target that comes any other way than up that row leaves some of them out.
    if (passed.isConstruction && !passed.newTarget && function.constructedClass)
        m_flow.noteAllocated(function.constructedClass);
    if (passed.isConstruction && passed.newTarget && !handsNewTargetUpRowOfClasses(callee, passed))
        escape(*passed.newTarget);
    if (passed.isConstruction)
        constructed = objectsConstructed(passed, callee);

    LinkType undefined = primitivesForLinkCalls(Primitive::Undefined);
    uint32_t numberOfParameters = function.numberOfEntryParameters();
    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t numberOfPositional = function.numberOfPositionalParameters();
    for (uint32_t parameter = static_cast<uint32_t>(function.captures.size()); parameter < numberOfParameters; ++parameter) {
        FlowNodeID to = m_flow.parameter(callee, parameter);
        switch (function.entryParameterKind(parameter)) {
        // Whoever makes the closure fills them.
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
            // reach() says what the rest parameter is.
            if (position >= numberOfPositional)
                break;
            if (position < passed.arguments.size()) {
                flowInto(passed.arguments[position], to);
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

    // An argument that no parameter takes goes nowhere, or into the list that the entry makes: a site of the callee, which reach() lays at the parameter. `arguments` is made of that list and of the parameters, by the body.
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

    m_flow.addEdge(m_flow.thrown(callee), thrownNode());
    return Entry::Body;
}

// The constructor of a class makes its own object, and what it returns is what was constructed. For any other function the construct call makes the object, of the layout that the class of new.target makes.
// reach() lays that object at `this` of the function that is its own new.target, and tells those who have looked when the layout escapes. So it is taken from there.
ValuesInLinkCalls CallInLinkCalls::objectsConstructed(const Passed& passed, FunctionID callee)
{
    Values constructed;
    const Function& function = m_program.at(callee);
    if (!function.takesThisFromConstruction())
        return constructed;

    auto addObjectMadeFor = [&](FunctionID newTarget) {
        const Function& constructor = m_program.at(newTarget);
        std::optional<unsigned> thisParameter = constructor.thisParameterIndex();
        if (constructor.takesThisFromConstruction() && constructor.constructedClass && thisParameter) {
            m_flow.reach(newTarget);
            // `this` of a function below which layouts are written holds those too, and objects that nobody has seen: whoever stands below brings its own. The call that has the function for its own new.target makes the one layout.
            // Not the maker of a tagged family, whatever else is written below it: what it makes is of the member that its tag picks, and `this` holds those.
            bool makesMembersOfTaggedFamily = !std::ranges::equal_range(m_program.taggedFamilies, constructor.constructedClass, { }, &TaggedFamily::maker).empty();
            if (!m_program.layoutsExpectedBelow(constructor.constructedClass).empty() && !makesMembersOfTaggedFamily) {
                add(constructed, m_flow.objectsMadeForClassBelowConstructorFunction(constructor.constructedClass), notObservedForLinkCalls, TypeTable::object);
                return;
            }
            add(constructed, m_flow.parameter(newTarget, *thisParameter), notObservedForLinkCalls, TypeTable::object);
            return;
        }
        // ClassTable::createThis() makes an object of the class of new.target where that class stands below the function that is entered.
        if (LayoutID entered = m_flow.layoutOfConstructorFunction(callee); entered && m_flow.classOfConstructor(newTarget) && std::ranges::find(m_program.layoutsExpectedBelow(entered), constructor.constructedClass) != m_program.layoutsExpectedBelow(entered).end()) {
            add(constructed, m_flow.objectsMadeForClassBelowConstructorFunction(constructor.constructedClass), notObservedForLinkCalls, TypeTable::object);
            return;
        }
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
        case LinkType::Functions::Lost:
            escape(newTarget, source);
            add(constructed, unseenObjectsForLinkCalls());
            break;
        }
        if (newTarget.holdsUnseen || newTarget.builtin || newTarget.objects != LinkType::Objects::None || newTarget.classes != LinkType::Classes::None)
            add(constructed, unseenObjectsForLinkCalls());
    });
    return constructed;
}

// reach() tells whoever has looked at an object when its layout escapes, from the nodes at which it laid the layout down itself. These lie at none of them, so the layout escapes at once.
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

ValuesInLinkCalls CallInLinkCalls::resultOf(FunctionID callee, uint32_t index, bool isConstruction, const Values& constructed, uint8_t distance)
{
    const Function& function = m_program.at(callee);
    Values values;
    if (index >= function.results.size()) {
        add(values, LinkType::anything());
        return values;
    }

    // Of a coroutine it holds the promise or the generator: the scan lays that there, and sends what is returned where it goes.
    FlowNodeID result = m_flow.result(callee, index);
    if (!isConstruction || !function.takesThisFromConstruction()) {
        add(values, result, distance);
        return values;
    }
    add(values, constructed);
    add(values, result, distance, TypeTable::object);
    return values;
}

// LinkFlow ties each lost function to these nodes, with undefined at each parameter, and lets it construct with itself as new.target.
ValuesInLinkCalls CallInLinkCalls::passToLostFunctions(const Passed& passed, const LinkType& callee, const Source* source)
{
    // How many parameters the lost functions have nobody says, so a list has no nodes to go to. No node takes a new.target either.
    if (passed.list || passed.newTarget) {
        LinkType functions;
        functions.functions = callee.functions;
        escape(functions, source);
        return passToUnseen(passed);
    }

    callees().mayEnterLost = true;
    // For a function with a class tieInLostFunction() lays what a construction makes at `this` and among the results. For any other nobody has seen the object, as on the road of one callee: objectsThatNoNodeHolds().
    if (passed.isConstruction)
        flowInto(valuesOf(unseenObjectsForLinkCalls()), m_flow.madeForLostFunctionsWithoutClass());
    else
        flowInto(passed.receiver ? *passed.receiver : valuesOf(primitivesForLinkCalls(Primitive::Undefined)), m_flow.receiverOfLostFunctions());
    for (uint32_t position = 0; position < passed.arguments.size(); ++position)
        flowInto(passed.arguments[position], m_flow.parameterOfLostFunctions(position));
    throwAnything();

    Values answers;
    add(answers, m_flow.resultOfLostFunctions(), beyond(source));
    if (passed.isConstruction)
        add(answers, unseenObjectsForLinkCalls());
    return answers;
}

ValuesInLinkCalls CallInLinkCalls::passToUnseen(const Passed& passed)
{
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
            switch (passTo(function, passed, constructed)) {
            case Entry::Refused:
                break;
            case Entry::Unseen:
                add(answers, LinkType::anything());
                break;
            case Entry::Body:
                add(answers, resultOf(function, 0, passed.isConstruction, constructed, beyond(source)));
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
        case LinkType::Functions::Lost:
            add(answers, passToLostFunctions(passed, callee, source));
            break;
        }
        if (callee.builtin)
            add(answers, mayGoByRows ? callBuiltin(callee.builtin, passed, true) : passToUnseen(passed));
        if (callee.holdsUnseen || callee.objects != LinkType::Objects::None || callee.classes != LinkType::Classes::None)
            add(answers, passToUnseen(passed));
    });
    return answers;
}

// ---- Rows

ValuesInLinkCalls CallInLinkCalls::callBuiltin(BuiltinID builtin, const Passed& passed, bool isCalledByBuiltin)
{
    RowCall call { .builtin = m_builtins.canonicalID(builtin), .mode = passed.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call, .passed = passed };
    call.isCalledByBuiltin = isCalledByBuiltin;
    call.isSeenByScan = !isCalledByBuiltin && m_call.callee == SummaryCall::Callee::Builtin;
    m_flow.note(LinkFlow::Note::BuiltinCalled, call.builtin.index());
    notePreparationRegisteredBy(call.builtin, passed);
    noteOptionThatRunsCodeAmong(call.builtin, passed);
    if (m_flow.testRuns().isTestRun() && m_builtins.has(BuiltinMark::IsSpy, call.builtin) && !passed.arguments.isEmpty()) {
        forEachType(passed.arguments[0], [&](const LinkType& holder, const Source*) {
            m_flow.testRuns().noteHolderOfSpy(holder, { }, m_caller, m_call.site);
        });
    }
    // NOTHING AT AN OPERAND: THE CALL DOES NOT RUN, OR NOT YET. The rows are tried one behind the other, so an operand that holds nothing would be refused by the row that takes what arrives there later, and be handed on to the rows behind it.
    // Whoever has looked is told when something arrives.
    auto holdsNothing = [&](const Values& values) {
        bool holdsSomething = false;
        forEachType(values, [&](const LinkType&, const Source*) {
            holdsSomething = true;
        });
        return !holdsSomething;
    };
    if ((passed.receiver && holdsNothing(*passed.receiver)) || std::ranges::any_of(passed.arguments, holdsNothing))
        return { };
    if (!rowsSpeakOf(call))
        return callOutOfSight(call);

    // The rows of a construction have the built-in as new.target. With a class of the program there the object has the layout of that class, and finds what the class has put in place of the members that native code calls.
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
    for (const BuiltinOverload& row : m_builtins.overloadsOf(call.builtin)) {
        if (row.mode != call.mode)
            continue;
        hasRow = true;
        Fit fit = fitOf(call, row);
        if (!fit.mayBeInside || fit.mayBeOutside)
            describeWhatRowRefuses(call, row);
        if (!fit.mayBeInside)
            continue;
        if (rowSaysTooLittleForLinkCalls(row))
            return callOutOfSight(call);
        call.row = &row;
        call.resultIsUnknown = false;
        applyRow(call);
        enterCodeBehindRow(call);
        if (!fit.mayBeOutside)
            return WTF::move(call.result);
    }
    if (!hasRow)
        return callOutOfSight(call);
    // What no row takes is refused.
    throwError();
    return WTF::move(call.result);
}

// What hangs on the built-in alone.
bool CallInLinkCalls::rowsMayBeTakenAtTheirWord(BuiltinID builtin)
{
    static constexpr uint8_t no = 1;
    static constexpr uint8_t yes = 2;
    // It is written once, when it is known: another thread may ask meanwhile, and finds the same.
    uint8_t& known = m_state.builtinsWhoseRowsAreTakenAtTheirWord[builtin.index()];
    if (known)
        return known == yes;
    uint8_t answer = no;
    auto write = makeScopeExit([&] {
        known = answer;
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
    // A test run may have put a function of its own in its place.
    if (m_program.isUnlocked(builtin) || m_builtins.letsInUnseenCode(builtin))
        return false;
    for (BuiltinMark mark : marksOfRowsThatSayTooLittleForLinkCalls) {
        if (m_builtins.has(mark, builtin))
            return false;
    }
    answer = yes;
    return true;
}

// `Bun.serve({ app })`. Only an object that the program has written with the option counts. Any other may have it too and opens nothing: the host refuses the option unless code that nobody has read runs by right,
// and what it starts then is turned away by the locks as that code is. Which layouts have got lost nobody says one by one: there any layout of the program that lists the name counts.
void CallInLinkCalls::noteOptionThatRunsCodeAmong(BuiltinID builtin, const Passed& passed)
{
    const BuiltinUnseenCode* row = m_builtins.unseenCodeOf(builtin);
    if (!row || !row->condition || !row->condition->hasProperty || isLibraryCode(m_program, m_program.at(m_caller)))
        return;
    PropertyKeyID option = m_program.keyOf(row->condition->hasProperty);
    if (!option)
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

// Nobody here says what the code of the library behind the built-in is passed.
ValuesInLinkCalls CallInLinkCalls::callOutOfSight(const RowCall& call)
{
    m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, call.builtin.index());
    return passToUnseen(call.passed);
}

// THE ROW'S WORD STANDS FOR WHAT THE CALL DOES TO THE VALUES OF THE PROGRAM. The code of the library behind it is reached, and nothing of the program flows through it: Link knows no contexts,
// so at its parameters the receivers of all calls would meet and be united, its callbacks would get lost, and whatever its body lets go would take the operands of every call along.
void CallInLinkCalls::enterCodeBehindRow(RowCall& call)
{
    const RuntimeFunctionEntry* code = findRuntimeFunction(m_program, call.row->code.implementation.function);
    if (code && code->function)
        m_flow.note(LinkFlow::Note::BuiltinCalledOutOfSight, call.builtin.index());
}

bool CallInLinkCalls::rowsSpeakOf(const RowCall& call)
{
    if (!rowsMayBeTakenAtTheirWord(call.builtin))
        return false;
    const Builtin& entry = m_builtins.at(call.builtin);

    if (call.mode == BuiltinCallMode::Construct) {
        // `new Map(pairs)` calls the `set` that it finds on the new map.
        if (!entry.instanceClass)
            return true;
        const BuiltinClass& made = m_builtins.at(entry.instanceClass);
        BuiltinID adder = made.constructorAdder && made.prototype ? m_builtins.findOwnMember(made.prototype, made.constructorAdder) : BuiltinID();
        if (!adder || m_program.isUnlocked(m_builtins.canonicalID(adder)))
            return !adder;
        // In a test run the lock of the member is the program's to lay.
        return !m_program.isLockLeftToProgram(adder) || (m_flow.takeForConstant(adder) && watch(m_flow.writtenAtMember(adder), 0));
    }

    // `Promise.resolve.call(Other, x)` makes what Other makes.
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
    return isCalledOnHolder;
}

// Native code that refuses an operand says what it was passed: `Received an instance of Renamed`. It reads `constructor` off the operand and `name` off that, which may be getters of the program.
void CallInLinkCalls::describeWhatRowRefuses(const RowCall& call, const BuiltinOverload& row)
{
    Questions questions;
    questions.describeObjects = true;
    auto describe = [&](const BuiltinParameter& parameter) {
        if (!parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused))
            return;
        throwAnything();
        ask(valuesAt(call, parameter.operand), questions);
    };
    if (row.receiver)
        describe(*row.receiver);
    for (const BuiltinParameter& parameter : row.parameters)
        describe(parameter);
}

FitInLinkCalls CallInLinkCalls::fitOf(const RowCall& call, const BuiltinOverload& row)
{
    Fit fit { true, false };
    auto take = [&](const BuiltinParameter& parameter) {
        Fit ofOperand = fitOf(call, parameter);
        fit.mayBeInside &= ofOperand.mayBeInside;
        fit.mayBeOutside |= ofOperand.mayBeOutside;
    };
    if (row.receiver)
        take(*row.receiver);
    for (const BuiltinParameter& parameter : row.parameters)
        take(parameter);
    return fit;
}

FitInLinkCalls CallInLinkCalls::fitOf(const RowCall& call, const BuiltinParameter& parameter)
{
    // `String(object)`: whatever is passed is converted to what the row takes, or the conversion throws. Either way the row has taken it.
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
        fit.mayBeOutside = call.passed.list || (accepted && !accepted->isAnything() && fitOfOmittedOperandInBuiltinType(*accepted) != TypeFit::Inside);
        return fit;
    // Nothing need be passed.
    case BuiltinOperand::Kind::ArgumentsFrom:
        fit.mayBeInside = true;
        break;
    }

    // A conversion takes whatever is passed, and `accepted` is what it leaves as it is: `Math.abs({ valueOf() { return 7; } })`. It asks an object for a primitive, which questionsOf() has, and the row says whether it may throw.
    bool takesWhateverIsPassed = !accepted || parameter.conversion != BuiltinConversion::None;
    forEachType(valuesAt(call, operand), [&](const LinkType& type, const Source* source) {
        if (takesWhateverIsPassed)
            fit.mayBeInside = true;
        else
            addFit(type, source, *accepted, fit);
    });
    return fit;
}

// Whatever lays a name on an object of one of the engine's classes, defines an element by a descriptor or puts another object above it leaves something under unknown names: a store under a name, and the escape that every other way is.
// While nothing lies there, no code of the program runs when native code reads, stores and moves what the object holds, however its storage has come to be: so the row that wants it as its class makes it says all that the call does.
// `date.toJSON()` calls the toISOString that it finds: native code looks members up by name on an instance, and the row that takes the instance as its class makes it says nothing of them.
// An executable has each of them locked. In a test run the lock is the program's to lay: each member of the objects above the class is taken for what the table lists, and watched.
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

bool CallInLinkCalls::isAsClassMakesIt(const LinkType& instances, PristineAspectSet aspects, const Source* source)
{
    if (aspects.contains(PristineAspects::Storage) && !indexesAreAbsentAbove(instances.builtinClass))
        return false;
    if (!membersAboveInstancesAreWhatTableLists(instances.builtinClass))
        return false;
    FlowNodeID underUnknownNames = m_flow.contentsOf(instances, SlotOfContents::underUnknownNames());
    return watch(underUnknownNames, beyond(source)) && heldAt(underUnknownNames).isNothing();
}

// A read at a hole goes on upward.
bool CallInLinkCalls::indexesAreAbsentAbove(BuiltinClassID builtinClass)
{
    BuiltinID holder;
    for (BuiltinClassID current = builtinClass; current && !holder; current = m_builtins.at(current).parent)
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

void CallInLinkCalls::addFit(const LinkType& type, const Source* source, const BuiltinType& accepted, Fit& fit)
{
    if (accepted.isAnything()) {
        fit.mayBeInside = true;
        return;
    }
    auto note = [&](bool mayBeTaken, bool isSurelyTaken) {
        fit.mayBeInside |= mayBeTaken;
        fit.mayBeOutside |= !isSurelyTaken;
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
        // Written for a call that leaves the argument out.
        if (!accepted.primitives.contains(BuiltinPrimitive::Undefined))
            surelyTaken &= ~Primitive::Undefined;
        note(type.primitives & LinkType::closed(perhapsTaken), !(type.primitives & ~LinkType::closed(surelyTaken)));
    }

    bool takesAnyObject = accepted.flags.contains(BuiltinTypeFlag::HasAnyObject);
    bool takesAnyFunction = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasAnyFunction);
    bool takesSomeObject = accepted.mayBeObject();
    auto somePartMayTake = [&](const Invocable<bool(BuiltinClassID)> auto& classMayTake) {
        return std::ranges::any_of(accepted.parts, [&](const BuiltinTypePart& part) {
            return part.kind != BuiltinTypePartKind::Instance || classMayTake(part.builtinClass);
        });
    };

    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout: {
        BuiltinClassID base = m_program.at(type.layout).writtenBuiltinBase;
        bool mayBeTaken = takesAnyObject || accepted.flags.contains(BuiltinTypeFlag::HasPlainData) || somePartMayTake([&](BuiltinClassID taken) {
            return m_builtins.isSameOrSubclassOf(base ? base : BuiltinClassID(BuiltinClasses::Object), taken);
        });
        note(mayBeTaken, takesAnyObject);
        break;
    }
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        note(takesSomeObject, takesAnyObject);
        break;
    }

    // A closure of the program is of the class of its kind of function. Of a class such as that of bound functions native code alone makes instances.
    if (type.functions != LinkType::Functions::None) {
        bool mayBeTaken = takesAnyFunction || somePartMayTake([&](BuiltinClassID taken) {
            if (type.functions == LinkType::Functions::Closure)
                return m_builtins.isSameOrSubclassOf(classOfClosuresForLinkCalls(m_program.at(type.function)), taken);
            return !m_builtins.at(taken).hasClosureInstances && (isFunctionClass(m_builtins, taken) || m_builtins.isSameOrSubclassOf(BuiltinClasses::Function, taken));
        });
        note(mayBeTaken, takesAnyFunction);
    }
    if (type.holdsUnseen)
        note(takesSomeObject, takesAnyObject);
    if (type.builtin) {
        bool isNamed = std::ranges::any_of(accepted.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Builtin && m_builtins.canonicalID(part.builtin) == m_builtins.canonicalID(type.builtin);
        });
        note(takesSomeObject, takesAnyObject || isNamed);
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
        note(mayBeTaken, isSurelyTaken);
        break;
    }
    case LinkType::Classes::Mixed:
        note(takesSomeObject, takesAnyObject);
        break;
    }
}

void CallInLinkCalls::applyRow(RowCall& call)
{
    const BuiltinOverload& row = *call.row;
    const Passed& passed = call.passed;

    if (row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
        throwAnything();
    else if (row.throws())
        throwError();

    if (row.receiver)
        applyParameter(call, *row.receiver);
    for (const BuiltinParameter& parameter : row.parameters)
        applyParameter(call, parameter);
    for (uint32_t argument = 0; argument < passed.arguments.size(); ++argument) {
        if (argument > BuiltinOperand::maxArgumentIndex || !m_builtins.parameterFor(row, BuiltinOperand::argument(argument)))
            escape(passed.arguments[argument]);
    }
    if (passed.list && !m_builtins.parameterFor(row, BuiltinOperand::argument(BuiltinOperand::maxArgumentIndex)))
        escape(*passed.list);

    addResult(call);
    for (const BuiltinTransfer& transfer : row.flow.transfers) {
        // What the operand hands out is looked at, and what that holds in turn is what is taken out.
        const BuiltinParameter* source = transfer.from.kind == BuiltinPlaceKind::OwnValues ? m_builtins.parameterFor(row, *transfer.from.operand) : nullptr;
        Values moved = source && source->roles.contains(BuiltinOperandRole::ContentsOfContentsAreOnlyStored) ? contentsOf(valuesIteratedOf(valuesAt(call, *transfer.from.operand))) : valuesAtPlace(call, transfer.from);
        if (source)
            add(moved, primitivesForLinkCalls(Primitive::Undefined));
        storeAt(call, transfer.to, moved);
    }
    for (const BuiltinCallback& callback : row.flow.callbacks)
        applyCallback(call, callback);
    for (const BuiltinHandlerCall& handlerCall : row.flow.handlerCalls)
        applyHandlerCall(call, handlerCall);
    for (const BuiltinPropertyAction& action : row.flow.propertyActions)
        applyAction(call, action);
}

void CallInLinkCalls::applyParameter(RowCall& call, const BuiltinParameter& parameter)
{
    Values values = valuesAt(call, parameter.operand);
    OperandUses uses = parameter.uses;
    if (uses == OperandUses::all()) {
        escape(values);
        call.resultIsUnknown = true;
        throwAnything();
        return;
    }

    bool escapes = !recordsSpellOutEveryUseForLinkCalls(m_builtins, *call.row, parameter);
    // The scan goes by the property actions of the rows. Its record of a change of prototype names what is put above too.
    bool changeIsRecorded = call.isSeenByScan && std::ranges::any_of(call.row->flow.propertyActions, [&](const BuiltinPropertyAction& action) {
        if (action.kind == BuiltinPropertyActionKind::ChangesPrototype && action.value.operand && operandsOverlapForLinkCalls(*action.value.operand, parameter.operand))
            return true;
        return operandsOverlapForLinkCalls(action.object, parameter.operand);
    });
    escapes |= uses.containsAny(usesThatChangeObjectsForLinkCalls) && !changeIsRecorded;
    bool looksNoFurtherThanContents = parameter.roles.contains(BuiltinOperandRole::ContentsOfContentsAreOnlyStored);
    // `[[o]].flat()`: whatever can be reached is not only looked at, it is KEPT, and the record names what the operand itself holds and no more. No node stands for all that can be reached: it escapes, and so the place gets anything.
    escapes |= parameter.roles.contains(BuiltinOperandRole::KeepsWhatCanBeReached) && !looksNoFurtherThanContents;
    if (escapes)
        escape(values);

    if (uses.contains(OperandUse::Returns))
        add(call.result, valuesHandedOn(call, parameter.operand));
    if (escapes)
        return;

    Questions questions = questionsOf(parameter);
    if (questions.isEmpty())
        return;
    throwAnything();
    // Whatever can be reached from it is looked at. Unless the row says how far native code looks: at the operand and at what it holds.
    if (uses.contains(OperandUse::AppliesTransitively) && !looksNoFurtherThanContents) {
        inspect(values, questions);
        return;
    }
    ask(uses.contains(OperandUse::AppliesToContents) ? contentsOf(values) : values, questions);
    if (uses.contains(OperandUse::AppliesTransitively))
        ask(valuesIteratedOf(values), questions);
}

void CallInLinkCalls::applyCallback(RowCall& call, const BuiltinCallback& callback)
{
    Passed passed;
    passed.isConstruction = callback.constructs;
    if (callback.constructs) {
        if (callback.newTarget.kind != BuiltinPlaceKind::None)
            passed.newTarget = valuesAtPlace(call, callback.newTarget);
    } else if (callback.thisValue.kind != BuiltinPlaceKind::None)
        passed.receiver = valuesAtPlace(call, callback.thisValue);

    auto addToList = [&](const Values& values) {
        if (!passed.list)
            passed.list = Values();
        add(*passed.list, values);
    };
    for (const BuiltinPlace& argument : callback.arguments) {
        bool isRestOfArguments = argument.kind == BuiltinPlaceKind::Operand && !argument.isSettled && argument.operand->kind() == BuiltinOperand::Kind::ArgumentsFrom;
        // `f.call(receiver, a, b)`: each keeps its place behind those in front of it.
        if (isRestOfArguments && !passed.list) {
            for (uint32_t index = argument.operand->argumentIndex(); index < call.passed.arguments.size(); ++index)
                passed.arguments.append(call.passed.arguments[index]);
            if (call.passed.list)
                addToList(*call.passed.list);
            continue;
        }
        Values values = valuesAtPlace(call, argument);
        if (passed.list) {
            addToList(values);
            continue;
        }
        if (mayBeMissingAt(argument))
            add(values, primitivesForLinkCalls(Primitive::Undefined));
        passed.arguments.append(WTF::move(values));
    }
    if (callback.spreadArguments.kind != BuiltinPlaceKind::None)
        addToList(valuesAtPlace(call, callback.spreadArguments));
    auto holdsWhatNobodyFollows = [](const Values& values) { return values.made.holdsUnseen; };
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
    // What is no array is stored, and of an array the elements are.
    case BuiltinCallbackResultUse::Flattened: {
        Values stored = answers;
        forEachType(answers, [&](const LinkType& answer, const Source* source) {
            if (answer.classes != LinkType::Classes::None)
                add(stored, m_flow.contentsOf(answer, ContentsSlot::Element), beyond(source));
            // No site stands for a list that nobody follows, nor for a proxy of a list, whose traps run. What is taken out of one is marked as the list is.
            bool mayBeListInSight = answer.holdsUnseenInSight() || answer.builtin || mayBeListWithLayout(answer);
            if (mayBeListInSight || answer.holdsUnseen) {
                throwAnything();
                add(stored, mayBeListInSight ? LinkType::anything() : LinkType::anything().byEscapeAlone());
            }
        });
        storeAt(call, callback.resultTo, stored);
        break;
    }
    }

    // Between the calls and behind the last, native code writes into whatever the function has put where it looks.
    if (callback.afterwards == BuiltinCallbackAftermath::WritesIntoWhatItFinds) {
        escape(call.passed);
        escape(answers);
    }
}

// The function lies in an operand, under a name. Whoever finds it there calls it.
void CallInLinkCalls::applyHandlerCall(RowCall& call, const BuiltinHandlerCall& handlerCall)
{
    escape(valuesAtPlace(call, handlerCall.callee));
    escape(valuesAtPlace(call, handlerCall.thisValue));
    for (const BuiltinPlace& argument : handlerCall.arguments)
        escape(valuesAtPlace(call, argument));
}

void CallInLinkCalls::applyAction(RowCall& call, const BuiltinPropertyAction& action)
{
    bool reads = false;
    switch (action.kind) {
    case BuiltinPropertyActionKind::TestsByKey:
        return;
    // `Object.create(above, { x: { value, get, set } })`: what the descriptors hold lies on the result from then on, and their accessors answer reads and stores there. Nobody follows either.
    case BuiltinPropertyActionKind::DescribesResult:
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
        // ScanOfBody::readChangesMadeByBuiltin() has left an access or a change for it.
        if (call.isSeenByScan)
            return;
        if (action.kind != BuiltinPropertyActionKind::ChangesIntegrity && action.kind != BuiltinPropertyActionKind::ChangesPrototype && action.kind != BuiltinPropertyActionKind::BecomesPrototype) {
            forEachType(valuesAt(call, action.object), [&](const LinkType& type, const Source*) {
                m_flow.noteWriteOn(type, { }, m_caller);
            });
        }
        break;
    }

    escape(valuesAt(call, action.object));
    if (reads)
        storeAt(call, action.value, valuesOf(LinkType::anything()));
    else
        escape(valuesAtPlace(call, action.value));
}

void CallInLinkCalls::addResult(RowCall& call)
{
    const BuiltinOverload& row = *call.row;
    const BuiltinResult& result = row.result;
    if (call.resultIsUnknown || !result.type || row.resultIsDeclaredOnly())
        add(call.result, LinkType::anything());
    // What the row says it makes may well be what comes back, whatever else may: `(await WebAssembly.instantiate(bytes)).module.constructor.exports = f`.
    if (result.type) {
        if (result.testedOperand)
            add(call.result, primitivesForLinkCalls(Primitive::Boolean));
        else
            addValuesOfType(call, result.type, true, call.result);
    }

    // What the new iterator will hand out lies in it from the start: an edge goes on bringing what the operand comes to hold.
    if (result.iteratedOperand) {
        Values handedOut = valuesHandedOutBy(result.iterationKind, valuesAt(call, *result.iteratedOperand));
        bool everyIteratorIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            bool hasSlot = false;
            for (BuiltinSlotID id : m_builtins.at(m_flow.classOf(made)).slots) {
                if (!m_builtins.at(id).holdsWhatStepsHandOut)
                    continue;
                hasSlot = true;
                storeInContents(made, m_flow.slotOf(id), handedOut);
            }
            if (!hasSlot)
                escape(handedOut);
        });
        if (!everyIteratorIsFollowed)
            escape(handedOut);
    }
    // Nobody follows what the result finds above itself.
    if (result.prototypeSource)
        escape(valuesAt(call, *result.prototypeSource));
    // `Object.create(above)`: what is made stands below an operand, and not below what its class has above every instance. A store on it may come to a setter there, which is handed what is stored, and a read to a getter, which is handed the object.
    bool standsBelowOperand = result.prototypeSource || std::ranges::any_of(call.row->flow.propertyActions, [](const BuiltinPropertyAction& action) {
        return action.kind == BuiltinPropertyActionKind::BecomesPrototype && action.value.kind == BuiltinPlaceKind::Result;
    });
    if (standsBelowOperand) {
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            m_flow.escape(made);
        });
    }
    // `/(a)/.exec(text)`: the list is born with names of its own, and what lies under them is among its own values.
    for (const BuiltinResultProperty& property : result.properties) {
        Values held = valuesOfType(call, property.type);
        escape(held);
        forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            storeInContents(made, SlotOfContents::under(m_program.keyOf(property.name)), held);
        });
    }
}

ValuesInLinkCalls CallInLinkCalls::valuesAt(const RowCall& call, BuiltinOperand operand)
{
    const Passed& passed = call.passed;
    LinkType undefined = primitivesForLinkCalls(Primitive::Undefined);
    Values values;
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        if (call.mode == BuiltinCallMode::Construct)
            return passed.newTarget ? *passed.newTarget : valuesOf(builtinForLinkCalls(call.builtin));
        return passed.receiver ? *passed.receiver : valuesOf(undefined);
    case BuiltinOperand::Kind::Argument:
        if (operand.argumentIndex() < passed.arguments.size())
            return passed.arguments[operand.argumentIndex()];
        if (passed.list)
            add(values, *passed.list);
        add(values, undefined);
        return values;
    case BuiltinOperand::Kind::ArgumentsFrom:
        for (size_t index = operand.argumentIndex(); index < passed.arguments.size(); ++index)
            add(values, passed.arguments[index]);
        if (passed.list)
            add(values, *passed.list);
        return values;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ToObject makes a wrapper of a primitive.
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
    // Native code takes the property wherever it lies.
    case BuiltinPlaceKind::Member:
        escape(valuesAt(call, *place.operand));
        add(values, LinkType::anything());
        break;
    case BuiltinPlaceKind::ResultField: {
        bool everyObjectIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart& part) {
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
    if (!id || m_builtins.at(id).isAnything() || m_typesBeingRead.contains(id)) {
        add(values, LinkType::anything());
        return;
    }
    m_typesBeingRead.append(id);

    const BuiltinType& type = m_builtins.at(id);
    add(values, primitivesWrittenForLinkCalls(type));
    if (!type.stringLiterals.empty())
        add(values, primitivesForLinkCalls(Primitive::String));

    // What a species constructor makes in place of the instance is an object that nobody follows.
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
        // Native code may hold it, or it is called in ways that nobody follows.
        if (freshness == BuiltinFreshness::No || !isFollowed(part.builtinClass)) {
            add(values, m_flow.unseenObjectsBelow(m_flow.prototypeOfInstancesOf(part.builtinClass), m_caller));
            for (const BuiltinSlotContents& contents : part.contents)
                escape(valuesOfType(call, contents.type));
            return;
        }
        addInstanceOf(call, part, part.builtinClass, part.contents, isResult, values);
        // `Object.fromEntries(pairs)`, a match, an error: nobody else holds it, and that is all the row says. It is born with properties of its own that nobody lists.
        if (freshness == BuiltinFreshness::Unaliased)
            storeInContents(siteOf(part, part.builtinClass), SlotOfContents::underUnknownNames(), valuesOf(LinkType::anything()));
        return;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        forEachType(valuesAt(call, BuiltinOperand::receiver()), [&](const LinkType& receiver, const Source*) {
            BuiltinClassID made = holdsOnlyInstancesOfOneClassForLinkCalls(receiver) ? receiver.builtinClass : BuiltinClassID();
            if (made && part.kind == BuiltinTypePartKind::TypedArrayOfReceiverElementType)
                made = m_builtins.typedArrayClassOf(made);
            if (made && freshness != BuiltinFreshness::No && isFollowed(made))
                addInstanceOf(call, part, made, { }, isResult, values);
            else
                add(values, unseenObjectsForLinkCalls());
        });
        return;
    case BuiltinTypePartKind::Layout:
        if (freshness != BuiltinFreshness::No) {
            addRecordOf(call, part, isResult, values);
            return;
        }
        // Native code may hold it.
        add(values, unseenObjectsForLinkCalls());
        for (const BuiltinLayoutField& field : m_builtins.at(part.layout).fields)
            escape(valuesOfType(call, field.type));
        return;
    case BuiltinTypePartKind::Builtin:
        add(values, builtinForLinkCalls(m_builtins.canonicalID(part.builtin)));
        return;
    case BuiltinTypePartKind::Operand:
        add(values, valuesHandedOn(call, *part.operand));
        return;
    case BuiltinTypePartKind::Slot:
        addContentsOfSlot(call, valuesAt(call, *part.operand), part.slot, values);
        return;
    case BuiltinTypePartKind::IteratedValues: {
        Values iterated;
        addIteratedValues(valuesAt(call, *part.operand), iterated);
        if (part.isSettled)
            settle(iterated);
        add(values, iterated);
        return;
    }
    case BuiltinTypePartKind::CallbackResult: {
        Values answers;
        addResultsOfCallbacks(call, *part.operand, answers);
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

// One site for each part of a type of the table that makes an object, at each call.
SiteID CallInLinkCalls::siteOf(const BuiltinTypePart& part, BuiltinClassID made)
{
    size_t numberOfPart = &part - BuiltinRecordsOfPool<BuiltinRecordPool::TypeParts>::records();
    return m_flow.siteOf(LinkFlow::MakerOfSite::PartOfRow, m_number, static_cast<uint32_t>(numberOfPart), made);
}

// A tuple is a list. Any other record holds each field under its name.
SlotOfContents CallInLinkCalls::slotOfField(const BuiltinTypePart& part, uint16_t fieldIndex)
{
    if (part.kind != BuiltinTypePartKind::Layout || m_builtins.at(part.layout).isTuple || fieldIndex >= m_builtins.at(part.layout).fields.size())
        return ContentsSlot::Element;
    return SlotOfContents::under(m_program.keyOf(m_builtins.at(part.layout).fields[fieldIndex].name));
}

// A slot that the row leaves out holds what the class says it holds, which is anything where the class says nothing.
void CallInLinkCalls::addInstanceOf(RowCall& call, const BuiltinTypePart& part, BuiltinClassID made, std::span<const BuiltinSlotContents> listed, bool isResult, Values& values)
{
    SiteID site = siteOf(part, made);
    add(values, m_flow.typeOf(site));
    bool isIteratorOverOperand = isResult && call.row->result.iteratedOperand;
    // A class lists the slots of the classes above it too.
    for (BuiltinSlotID id : m_builtins.at(made).slots) {
        const BuiltinSlot& slot = m_builtins.at(id);
        auto contents = std::ranges::find(listed, id, &BuiltinSlotContents::slot);
        if (contents == listed.end() && isResult && someRecordFillsResultSlotForLinkCalls(*call.row, id))
            continue;
        // addResult() says what it hands out.
        if (contents == listed.end() && isIteratorOverOperand && slot.holdsWhatStepsHandOut)
            continue;
        storeInContents(site, m_flow.slotOf(slot), valuesOfType(call, contents == listed.end() ? slot.holds : contents->type));
    }
}

// Native code makes an ordinary object with the names of the layout, and the code of the library a cell of the compiled layout. Either way the object is made here, and this site stands for it.
void CallInLinkCalls::addRecordOf(RowCall& call, const BuiltinTypePart& part, bool isResult, Values& values)
{
    const BuiltinLayout& layout = m_builtins.at(part.layout);
    SiteID site = siteOf(part, layout.isTuple ? BuiltinClasses::Array : BuiltinClasses::Object);
    if (!layout.isTuple)
        m_flow.noteRecordsAreMadeAt(site, part.layout);
    add(values, m_flow.typeOf(site));
    for (uint16_t index = 0; index < layout.fields.size(); ++index) {
        if (isResult && fieldOfResultHoldsOnlyWhatIsStoredForLinkCalls(*call.row, index))
            continue;
        storeInContents(site, slotOfField(part, index), valuesOfType(call, layout.fields[index].type));
    }
}

// The elements, keys and values, and what rows have laid under names.
void CallInLinkCalls::addContentsOf(const LinkType& holder, const Source* source, Values& contents)
{
    if (holder.classes == LinkType::Classes::None)
        return;
    // The class fixes what an element is, and nobody follows the stores.
    if (holder.builtinClass && m_builtins.typedArrayTypeOf(holder.builtinClass) != NotTypedArray)
        add(contents, primitivesForLinkCalls(Primitive::Number | Primitive::BigInt));
    // The object that stands for a text has the characters under their indexes from its birth.
    if (holder.builtinClass && holder.builtinClass == primitiveClassOf(Primitive::String))
        add(contents, primitivesForLinkCalls(Primitive::String));
    for (ContentsSlot slot : allMembersOf<ContentsSlot>)
        add(contents, m_flow.contentsOf(holder, slot), beyond(source));
    add(contents, m_flow.contentsOf(holder, SlotOfContents::underAnyName()), beyond(source));
}

ValuesInLinkCalls CallInLinkCalls::contentsOf(const Values& holders)
{
    Values contents;
    forEachType(holders, [&](const LinkType& holder, const Source* source) {
        addContentsOf(holder, source, contents);
        // `Array.prototype.map.call("xyz", f)`: the object that stands for a text has its characters under their indexes. That of another primitive has nothing.
        if (holder.primitives & LinkType::closed(Primitive::AnyString))
            add(contents, primitivesForLinkCalls(Primitive::String));
        if (holder.objects == LinkType::Objects::None && holder.functions == LinkType::Functions::None && !holder.holdsUnseen && !holder.builtin)
            return;
        // `Object.values(record)`: what the fields hold is read out, each field by itself, and the object stays as it is.
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

// A place that is read may be empty: an index without an element, a key that the map lacks.
void CallInLinkCalls::addContentsOfSlot(RowCall& call, const Values& holders, BuiltinSlotID id, Values& values)
{
    const BuiltinSlot& slot = m_builtins.at(id);
    add(values, primitivesForLinkCalls(Primitive::Undefined));
    if (slot.holds && !m_builtins.at(slot.holds).isAnything())
        addValuesOfType(call, slot.holds, false, values);

    forEachType(holders, [&](const LinkType& holder, const Source* source) {
        if (holder.classes != LinkType::Classes::None) {
            if (slot.contentsSlot && holder.builtinClass && m_builtins.typedArrayTypeOf(holder.builtinClass) != NotTypedArray)
                add(values, primitivesForLinkCalls(Primitive::Number | Primitive::BigInt));
            add(values, m_flow.contentsOf(holder, m_flow.slotOf(slot)), beyond(source));
        }
        // `class A extends Map`: what is stored into such an object escapes, in storeInSlot().
        if (holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.holdsUnseen || holder.builtin)
            add(values, LinkType::anything());
    });
}

// What an iterator over the values hands out, by the slots that the table lists for the class: a map has keys and values, a list has an index for each element, a set has elements alone.
ValuesInLinkCalls CallInLinkCalls::valuesHandedOutBy(IterationKind kind, const Values& iterated)
{
    Values keys;
    Values held;
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
            if (findSlot(ContentsSlot::Key) && findSlot(ContentsSlot::Value)) {
                add(keys, m_flow.contentsOf(holder, ContentsSlot::Key), beyond(source));
                add(held, m_flow.contentsOf(holder, ContentsSlot::Value), beyond(source));
            } else if (isTypedArray) {
                add(keys, primitivesForLinkCalls(Primitive::Integer));
                add(held, primitivesForLinkCalls(Primitive::Number | Primitive::BigInt));
            } else if (element) {
                bool isUnderIndexes = m_builtins.at(element).heap == AbstractHeap::Elements;
                if (isUnderIndexes)
                    add(keys, primitivesForLinkCalls(Primitive::Integer));
                else
                    add(keys, m_flow.contentsOf(holder, ContentsSlot::Element), beyond(source));
                add(held, m_flow.contentsOf(holder, ContentsSlot::Element), beyond(source));
                // A hole.
                if (isUnderIndexes)
                    add(held, primitivesForLinkCalls(Primitive::Undefined));
            } else {
                escape(holder, source);
                add(keys, LinkType::anything());
                add(held, LinkType::anything());
            }
        }
        // The steps read it by its keys, which runs its code.
        if (holder.classes == LinkType::Classes::Mixed || holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.holdsUnseen || holder.builtin) {
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

// What an iteration over the value hands out: what the class's own iterator does, which for a map is its entries. The caller has asked whether the value finds another iterator.
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
        Values& list = hasKeys ? withKeys : others;
        if (source)
            add(list, source->node, source->distance, source->passes);
        else
            add(list, iterable);
    });
    if (!withKeys.isEmpty())
        add(values, valuesHandedOutBy(IterationKind::Entries, withKeys));
    if (!others.isEmpty())
        add(values, valuesHandedOutBy(IterationKind::Values, others));
}

ValuesInLinkCalls CallInLinkCalls::valuesIteratedOf(const Values& iterables)
{
    Values iterated;
    addIteratedValues(iterables, iterated);
    return iterated;
}

void CallInLinkCalls::addResultsOfCallbacks(RowCall& call, BuiltinOperand operand, Values& values)
{
    bool constructs = std::ranges::any_of(call.row->flow.callbacks, [&](const BuiltinCallback& callback) {
        return callback.callee == operand && callback.constructs;
    });
    forEachType(valuesAt(call, operand), [&](const LinkType& callee, const Source* source) {
        switch (callee.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            if (constructs || !m_program.at(callee.function).hasBody() || !isEnteredByCallsAloneForLinkCalls(m_program.at(callee.function).kind)) {
                // `anything` speaks of no object of a layout that has not escaped.
                if (constructs)
                    escape(callee, source);
                add(values, LinkType::anything());
            } else
                add(values, resultOf(callee.function, 0, false, { }, beyond(source)));
            break;
        // A call of a class object is refused.
        case LinkType::Functions::Constructors:
            if (constructs) {
                escape(callee, source);
                add(values, LinkType::anything());
            }
            break;
        case LinkType::Functions::Lost:
            add(values, m_flow.resultOfLostFunctions(), beyond(source));
            break;
        }
        if (callee.builtin || callee.holdsUnseen || callee.objects != LinkType::Objects::None || callee.classes != LinkType::Classes::None)
            add(values, LinkType::anything());
    });
}

void CallInLinkCalls::addPrototypes(const Values& objects, Values& values)
{
    add(values, primitivesForLinkCalls(Primitive::Null));
    forEachType(objects, [&](const LinkType& object, const Source* source) {
        LinkType onlyObjects = primitivesForLinkCalls(object.primitives);
        onlyObjects.objects = object.objects;
        onlyObjects.layout = object.layout;
        onlyObjects.family = object.family;
        if (object == onlyObjects) {
            switch (object.objects) {
            case LinkType::Objects::None:
                return;
            case LinkType::Objects::Layout:
                add(values, prototypeForLinkCalls(object.layout));
                return;
            case LinkType::Objects::Family:
                add(values, prototypesForLinkCalls(object.family));
                return;
            case LinkType::Objects::Prototype:
            case LinkType::Objects::Prototypes:
            case LinkType::Objects::Lost:
                break;
            }
        }
        escape(object, source);
        add(values, unseenObjectsForLinkCalls());
        // Of the built-ins that the table has there the writes are followed.
        add(values, m_flow.builtinsAbove(object, m_caller));
    });
    add(values, unseenObjectsForLinkCalls());
}

// What a promise is resolved with, and what a wait for the values gives: the value of a promise, and whatever is no promise as it is. So the value of a promise is never a promise.
void CallInLinkCalls::settle(Values& values)
{
    Questions questions;
    addKeys(questions, m_builtins.keysReadBy(Protocol::Thenable));
    Questions ofPromise = questions;
    ofPromise.membersOfTableAnswerNothing = true;

    Values settled;
    // What answers to `then` is entered with functions that nobody has seen, and what it hands to them is what the promise holds.
    auto askForThen = [&](const Values& subjects, const Questions& asked) {
        if (ask(subjects, asked))
            add(settled, LinkType::anything());
    };
    // An object that nobody has seen may be a promise, or answer to `then`. What it gives is marked as it is.
    auto settleWhatNobodyHasSeen = [&](const LinkType& type) {
        if (type.holdsUnseen)
            add(settled, type.holdsUnseenInSight() ? LinkType::anything() : LinkType::anything().byEscapeAlone());
    };
    forEachType(values, [&](const LinkType& type, const Source* source) {
        bool mayBePromise = type.classes == LinkType::Classes::Mixed || (type.builtinClass && m_builtins.isSameOrSubclassOf(type.builtinClass, BuiltinClasses::Promise));
        if (!mayBePromise) {
            if (source)
                add(settled, source->node, source->distance, source->passes);
            else
                add(settled, type);
            askForThen(source ? valuesAtSource(*source) : valuesOf(type), questions);
            settleWhatNobodyHasSeen(type);
            return;
        }

        add(settled, m_flow.contentsOf(type, ContentsSlot::Value), beyond(source));
        LinkType others = type;
        // Among the classes that have met another there are promises and others.
        if (type.classes != LinkType::Classes::Mixed) {
            others.classes = LinkType::Classes::None;
            others.builtinClass = { };
            others.site = { };
            // Native code takes the value of a promise of its own class by its own `then`.
            askForThen(valuesOf(instancesAmongForLinkCalls(type)), ofPromise);
        }
        // Laid down, and by no edge, which would bring the promise along. The call is told when the type climbs.
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
        bool everyObjectIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart&) {
            storeInContents(made, m_flow.slotOf(place.slot), values);
        });
        if (!everyObjectIsFollowed)
            escape(values);
        return;
    }
    case BuiltinPlaceKind::ResultField: {
        bool everyObjectIsFollowed = forEachSiteOfResult(call, [&](SiteID made, const BuiltinTypePart& part) {
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
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::Member:
    case BuiltinPlaceKind::NativeCell:
        escape(values);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CallInLinkCalls::storeInContents(SiteID site, SlotOfContents slot, const Values& values)
{
    if (values.isEmpty())
        return;
    if (!isValueOfPromise(m_flow.classOf(site), slot)) {
        flowInto(values, m_flow.contents(site, slot));
        return;
    }
    Values settled = values;
    settle(settled);
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
        }
        if (holder.objects != LinkType::Objects::None || holder.functions != LinkType::Functions::None || holder.holdsUnseen || holder.builtin)
            escape(values);
    });
}

// A value that the row reads out of an operand may be missing. A primitive that the built-in makes is always passed, and an operand that the caller leaves out holds undefined already.
bool CallInLinkCalls::mayBeMissingAt(const BuiltinPlace& place) const
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

// A function that native code makes is called in ways that no row of its own says.
bool CallInLinkCalls::isFollowed(BuiltinClassID builtinClass) const
{
    return !m_builtins.at(builtinClass).hasClosureInstances && !isFunctionClass(m_builtins, builtinClass);
}

// `class Stack extends Array`: an object of the layout is a list. Which layouts have got lost nobody says one by one, and what lies under `prototype` of a constructor function is whatever the program has laid there.
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

// ---- What native code asks of an operand

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
    // `String(object)`: where the program has laid none of them Object.prototype.toString answers, and that reads Symbol.toStringTag off the object: its row in object.builtins.ts.
    if (PropertyKeyID key = m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::ToStringTag)))
        questions.keysOfConversion.appendIfNotContains(key);
}

QuestionsInLinkCalls CallInLinkCalls::questionsOf(const BuiltinParameter& parameter)
{
    Questions questions;
    for (Protocol protocol : parameter.protocols) {
        switch (protocol) {
        case Protocol::OwnKeys:
            break;
        case Protocol::PrototypeChain:
            questions.readThroughPrototypes = true;
            break;
        case Protocol::OwnValues:
            questions.readOwnValues = true;
            break;
        // Every class lays itself under `constructor`, where no summary lists it.
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
        case Protocol::Thenable:
        case Protocol::Iterate:
            addKeysAnsweredByProgramAlone(questions, m_builtins.keysReadBy(protocol));
            break;
        // A protocol without a list of names means every name.
        case Protocol::Named:
            questions.readEverything |= parameter.namedMembers.empty();
            addKeysAnsweredByProgramAlone(questions, parameter.namedMembers);
            break;
        case Protocol::CallsNamed:
            questions.callMembers = true;
            questions.readEverything |= parameter.calledMembers.empty();
            addKeys(questions, parameter.calledMembers);
            break;
        }
    }
    addKeysAnsweredByProgramAlone(questions, parameter.convertedMembers);
    // What Object.prototype answers with asks in turn (object.builtins.ts): toLocaleString() calls `toString`, and toString() reads Symbol.toStringTag.
    PropertyKeyID toString = m_program.keyOf(BuiltinNames::toString);
    PropertyKeyID tag = m_program.keyOf(m_builtins.nameOf(WellKnownSymbol::ToStringTag));
    if (toString && questions.keys.contains(m_program.keyOf(BuiltinNames::toLocaleString)))
        questions.keys.appendIfNotContains(toString);
    if (tag && questions.keys.contains(toString))
        questions.keysAnsweredByProgramAlone.appendIfNotContains(tag);
    questions.describeObjects = parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused);
    questions.onlyBoxedPrimitivesAreConverted = parameter.roles.contains(BuiltinOperandRole::OnlyBoxedPrimitivesAreConverted);
    return questions;
}

// Where an object of the program answers to none of the keys, what it finds is native code of Object.prototype, which hands it to nobody. Where it may answer to one, nobody here says what that code is passed: the object escapes.
template<typename Functor>
void CallInLinkCalls::forEachLayoutWithListedOwnValues(const LinkType& type, const Functor& functor)
{
    if (type.objects == LinkType::Objects::Layout)
        functor(type.layout);
    else if (type.objects == LinkType::Objects::Family)
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach(functor);
}

// Whatever of the program the type holds is an object whose own values lie at nodes that can be listed, and reading them out runs nothing.
bool CallInLinkCalls::ownValuesAreListed(const LinkType& type)
{
    if (type.functions != LinkType::Functions::None || type.holdsUnseen || type.builtin || (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Family))
        return false;
    bool areListed = true;
    forEachLayoutWithListedOwnValues(type, [&](LayoutID layout) {
        areListed &= !m_flow.nodesWithOwnValuesOf(layout).isEmpty();
    });
    return areListed;
}

// `new Map(pairs)` reads 0 and 1 off each pair, `Array.from(listLike)` every index: `get 0() {}` in a class above runs, `1() {}` is read out, and no list of own values holds either.
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

bool CallInLinkCalls::mayAnswer(const LinkType& type, const Source* source, const Questions& questions, bool ownValuesAreFollowed)
{
    if (type.builtin || type.classes == LinkType::Classes::Mixed)
        return true;
    bool holdsObjectsOfProgram = type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None;
    if (!holdsObjectsOfProgram && type.classes == LinkType::Classes::None)
        return false;
    if (questions.describeObjects || (holdsObjectsOfProgram && (questions.readEverything || (questions.readOwnValues && !ownValuesAreFollowed && !ownValuesAreListed(type)))))
        return true;
    if (questions.readOwnValues && questions.readThroughPrototypes && mayFindUnderNumbersAbove(type, source))
        return true;
    // What an object of one of the engine's classes holds is read without running anything while the object is as its class makes it, and the places of the row say where it goes.
    if ((questions.readEverything || questions.readOwnValues) && type.classes == LinkType::Classes::Site && !isAsClassMakesIt(instancesAmongForLinkCalls(type), { PristineAspects::Storage, PristineAspects::Names }, source))
        return true;

    uint8_t distance = source ? source->distance : 0;
    for (PropertyKeyID key : questions.keys) {
        if (mayFind(type, key, distance, questions.membersOfTableAnswerNothing))
            return true;
    }
    for (PropertyKeyID key : questions.keysAnsweredByProgramAlone) {
        if (mayFind(type, key, distance, true))
            return true;
    }
    LinkType converted = questions.onlyBoxedPrimitivesAreConverted ? instancesAmongForLinkCalls(type) : type;
    // The native code that converts an instance of one of the engine's classes looks members up by name: Array.prototype.toString() calls the `join` that it finds. As isAsClassMakesIt().
    if (questions.convertToPrimitive && converted.classes == LinkType::Classes::Site && !membersAboveInstancesAreWhatTableLists(converted.builtinClass))
        return true;
    for (PropertyKeyID key : questions.keysOfConversion) {
        if (mayFind(converted, key, distance, true))
            return true;
    }
    return false;
}

// SummaryChange::laysValueInSight, of the change that this call makes.
bool CallInLinkCalls::isDataDescriptorInSight(const Source* source) const
{
    if (!source || m_shared)
        return false;
    return std::ranges::any_of(m_summary.changes, [&](const SummaryChange& change) {
        return change.site == m_call.site && change.laysValueInSight && change.other != noSummaryNode && m_flow.nodeOf(m_caller, change.other) == source->node;
    });
}

// True: one of them may answer.
bool CallInLinkCalls::ask(const Values& subjects, const Questions& questions)
{
    if (questions.isEmpty())
        return false;
    bool someMayAnswer = false;
    forEachType(subjects, [&](const LinkType& type, const Source* source) {
        // What the call reads off it the scan has read: the observer of the change says where the value goes, and lets the descriptor go wherever it does not take the definition for a store.
        if (isDataDescriptorInSight(source))
            return;
        if (mayAnswer(type, source, questions, false)) {
            escape(type, source);
            someMayAnswer = true;
            return;
        }
        // `String(list)`: the engine's own conversion of a list, an error or a pattern converts what the object holds.
        if (questions.convertToPrimitive && !questions.onlyBoxedPrimitivesAreConverted && type.classes == LinkType::Classes::Site) {
            Questions ofContents;
            addConversion(ofContents);
            Values contents;
            addContentsOf(instancesAmongForLinkCalls(type), source, contents);
            ask(contents, ofContents);
        }
    });
    return someMayAnswer;
}

// ---- What native code only looks at, however deep

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
    }
    for (const Source& source : values.sources)
        inspectAt(source.node, inspection);
    inspectType(values.made, inspection);
}

void CallInLinkCalls::inspectAt(FlowNodeID node, uint32_t inspection)
{
    // Nodes are made all the time: the set takes room ahead, or it is copied for each of them.
    BitVector& nodes = m_state.inspections[inspection].nodes;
    if (node.index() >= nodes.size())
        nodes.ensureSize(node.index() + node.index() / 4 + 64);
    if (node == m_flow.unseen() || !nodes.add(node.index()))
        return;
    m_flow.observe(node, FlowObserver { FlowObserver::Kind::Inspected, m_state.inspections[inspection].function, m_state.inspections[inspection].stepForValues });
}

// False: it has been asked before.
bool CallInLinkCalls::askInspected(SubjectOfInspection kind, uint32_t number, const LinkType& subject, uint32_t inspection)
{
    if (!m_state.subjectsOfInspections.add({ static_cast<uint64_t>(kind) << 32 | number, inspection }).isNewEntry)
        return false;
    uint32_t step = static_cast<uint32_t>(m_state.stepsOfInspections.size());
    m_state.stepsOfInspections.append(StepOfInspectionInLinkCalls { inspection, subject });
    askAgain(step);
    return true;
}

// Whatever the answer was decided by tells this step when it climbs.
void CallInLinkCalls::askAgain(uint32_t step)
{
    StepOfInspectionInLinkCalls asked = m_state.stepsOfInspections[step];
    Questions questions = m_state.inspections[asked.inspection].questions;
    SetForScope watcher(m_stepThatWatches, std::optional(step));
    if (mayAnswer(asked.subject, nullptr, questions, true))
        escape(asked.subject, nullptr);
}

void CallInLinkCalls::takeStepOfInspection(uint32_t step, const LinkType& now)
{
    if (m_state.stepsOfInspections[step].subject.isNothing())
        inspectType(now, m_state.stepsOfInspections[step].inspection);
    else
        askAgain(step);
}

void CallInLinkCalls::inspectType(const LinkType& type, uint32_t inspection)
{
    auto inspectObjectsOf = [&](LayoutID layout) {
        LinkType objects = objectsForLinkCalls(layout);
        if (!askInspected(SubjectOfInspection::Layout, layout.index(), objects, inspection))
            return;
        if (!m_state.inspections[inspection].questions.readOwnValues)
            return;
        Vector<FlowNodeID, 8> nodes = m_flow.nodesWithOwnValuesOf(layout);
        if (nodes.isEmpty())
            escape(objects, nullptr);
        for (FlowNodeID node : nodes)
            inspectAt(node, inspection);
    };

    LinkType rest = type;
    rest.primitives = 0;
    rest.holdsUnseen = false;
    rest.holdsLostBuiltins = false;
    rest.builtin = { };
    rest.classes = LinkType::Classes::None;
    rest.builtinClass = { };
    rest.site = { };

    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        inspectObjectsOf(type.layout);
        break;
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach(inspectObjectsOf);
        break;
    // Nobody tells when one gets a member, or when something is put on a prototype object.
    case LinkType::Objects::Lost:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes: {
        LinkType objects = rest;
        objects.functions = LinkType::Functions::None;
        objects.function = { };
        objects.constructorsOf = { };
        escape(objects, nullptr);
        break;
    }
    }

    // What lies on a function under a name has escaped where it was stored.
    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        askInspected(SubjectOfInspection::Function, type.function.index(), closuresForLinkCalls(type.function), inspection);
        break;
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Lost: {
        LinkType functions = rest;
        functions.objects = LinkType::Objects::None;
        functions.layout = { };
        functions.family = { };
        escape(functions, nullptr);
        break;
    }
    }

    switch (type.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site: {
        LinkType instances = instancesAmongForLinkCalls(type);
        if (!askInspected(SubjectOfInspection::Site, type.site.index(), instances, inspection))
            break;
        for (ContentsSlot slot : allMembersOf<ContentsSlot>)
            inspectAt(m_flow.contents(type.site, slot), inspection);
        inspectAt(m_flow.contents(type.site, SlotOfContents::underAnyName()), inspection);
        for (BuiltinSlotID slot : m_builtins.at(type.builtinClass).slots)
            inspectAt(m_flow.contents(type.site, m_flow.slotOf(slot)), inspection);
        break;
    }
    case LinkType::Classes::Mixed:
        escape(instancesAmongForLinkCalls(type), nullptr);
        break;
    }
}

bool CallInLinkCalls::mayFind(const LinkType& objects, PropertyKeyID key, uint8_t distance, bool membersOfTableAnswerNothing)
{
    // An aggregate holds undefined for the layouts that are without the key.
    auto holdsWhatAnswers = [&](FlowNodeID node) {
        if (!watch(node, static_cast<uint8_t>(distance + 1)))
            return true;
        LinkType found = heldAt(node);
        return found != primitivesForLinkCalls(found.primitives & Primitive::Undefined);
    };

    // As callMemberOfClass() looks.
    if (objects.classes == LinkType::Classes::Site) {
        NameClass nameClass = m_flow.nameClassOf(key);
        if (nameClass == NameClass::Proto || mayBeElementKey(nameClass) || !m_builtins.mayHavePristineInstances(objects.builtinClass))
            return true;
        if (holdsWhatAnswers(m_flow.contentsOf(objects, SlotOfContents::underUnknownNames())) || holdsWhatAnswers(m_flow.contentsOf(objects, SlotOfContents::under(key))))
            return true;
        Lookup outcome = findOnInstancesOf(objects.builtinClass, key).outcome;
        if (outcome == Lookup::Unknown || (outcome == Lookup::Found && !membersOfTableAnswerNothing))
            return true;
    }

    // As callMemberOfClosure() looks.
    switch (objects.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure: {
        const Function& closed = m_program.at(objects.function);
        LayoutID layout = closed.constructedClass;
        // What is stored on a constructor function lies where it lies on any closure.
        bool isConstructorFunction = layout && m_flow.isOfConstructorFunction(layout);
        if (isConstructorFunction)
            layout = { };
        if (layout && (m_flow.namesMayBeIncomplete(layout, ClassSide::Static) || m_flow.findMembers(layout, ClassSide::Static, key).mayAnswer()))
            return true;
        uint32_t stepsLeft = static_cast<uint32_t>(m_program.layouts.size());
        for (LayoutID current = layout; current && stepsLeft--; current = m_program.at(current).base) {
            const Layout& record = m_program.at(current);
            if (record.findStaticField(key) || std::ranges::any_of(record.staticFields, [](const StaticField& field) { return !field.key; }))
                return true;
        }
        NameClass nameClass = m_flow.nameClassOf(key);
        if (nameClass == NameClass::Proto || mayBeElementKey(nameClass))
            return true;
        if (holdsWhatAnswers(m_flow.foundOnClosuresOf(objects.function)) || holdsWhatAnswers(m_flow.followsNamesOnClosuresOf(objects.function, key) ? m_flow.laidOnClosuresOf(objects.function, key) : m_flow.foundBesideFieldsUnder(key)))
            return true;
        // A class has the property itself, and it can neither be written nor become an accessor. A constructor function has it too: it can be written, and it cannot become an accessor.
        if (nameClass == NameClass::Prototype && (layout || isConstructorFunction))
            break;
        // What the table lists for a function is a property that the engine lays on it, or native code that hands the function to nobody. Whoever lays an accessor in front of either lets the function escape, which foundOnClosuresOf() tells.
        if (findOnInstancesOf(classOfClosuresForLinkCalls(closed), key).outcome == Lookup::Unknown)
            return true;
        break;
    }
    // Nobody lists here what their members answer to.
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Lost:
        return true;
    }

    switch (objects.objects) {
    case LinkType::Objects::None:
        return false;
    // The nodes tell of a store, a change or an escape that comes later.
    case LinkType::Objects::Layout: {
        if (findField(m_program, objects.layout, key) || m_flow.findMembers(objects.layout, ClassSide::Prototype, key).mayAnswer() || m_flow.namesMayBeIncomplete(objects.layout, ClassSide::Prototype))
            return true;
        // What Object.prototype holds under the key is native code, which hands the object to nobody: unless a test run has laid the name there, or may write the member. The aggregates of the two arms below say so, through tieRead().
        if (!m_flow.isAbsentAboveClasses(key)) {
            BuiltinClassID objectClass = BuiltinClasses::Object;
            BuiltinID above = m_builtins.at(objectClass).prototype;
            Lookup outcome = findOnce(static_cast<uint64_t>(above.index()) << 32 | key.index(), [&] {
                return findAbove(above, key, MembersUnderKey(m_program, key));
            }).outcome;
            if (outcome == Lookup::Unknown)
                return true;
        }
        FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(objects.layout, key);
        return (foundOnPrototypes && holdsWhatAnswers(foundOnPrototypes)) || holdsWhatAnswers(m_flow.foundBesideFieldsOf(objects.layout)) || holdsWhatAnswers(m_flow.foundBesideFieldsUnder(key));
    }
    // Nothing tells when something is put on it.
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return true;
    case LinkType::Objects::Family:
        return holdsWhatAnswers(m_flow.fieldReadThrough(objects.family, key));
    case LinkType::Objects::Lost:
        return holdsWhatAnswers(m_flow.fieldReadThroughLost(objects.layout, key));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- LinkFlow

StateOfLinkCalls& LinkFlow::stateOfLinkCalls()
{
    if (!m_stateOfLinkCalls)
        m_stateOfLinkCalls.reset(new StateOfLinkCalls(*this));
    return *m_stateOfLinkCalls;
}

static bool namesSameObjectsAndFunctionsInLinkCalls(LinkType first, LinkType second)
{
    for (LinkType* type : { &first, &second }) {
        type->primitives = 0;
        type->holdsUnseen = false;
        type->holdsLostBuiltins = false;
        type->builtin = { };
    }
    return first == second;
}

void LinkFlow::callObserved(const FlowObserver& observer, const LinkType& told)
{
    StateOfLinkCalls& state = stateOfLinkCalls();

    if (observer.kind == FlowObserver::Kind::MemberOfSharedCall) {
        MemberOfSharedCallInLinkCalls& member = state.membersOfSharedCalls[observer.record];
        if (member.tellsThatBringNothing && typeObservedBefore().isNothing()) {
            --member.tellsThatBringNothing;
            return;
        }
        if (!state.membersToResolve.set(observer.record))
            state.membersObservedToResolve.append(observer);
        return;
    }

    // Each tell looks at every operand as it is by then. So as many tells of an observer that was never told before go unheard as bring nothing, whichever they are: the last of them is heard.
    uint32_t& tellsThatBringNothing = state.tellsThatBringNothing[firstCallOfFunction[observer.function.index()] + observer.record];
    if (tellsThatBringNothing && typeObservedBefore().isNothing()) {
        --tellsThatBringNothing;
        return;
    }
    // Such a call still owes the program's own functions what it passes. A climb in primitives, or in what is unseen by escape alone, brings none of them.
    if (state.callsOfUnseen[firstCallOfFunction[observer.function.index()] + observer.record] && namesSameObjectsAndFunctionsInLinkCalls(told, typeObservedBefore()) && told.holdsUnseenInSight() == typeObservedBefore().holdsUnseenInSight())
        return;
    if (!std::exchange(state.callsToResolve[firstCallOfFunction[observer.function.index()] + observer.record], 1))
        state.observersToResolve[laneOfObserversOf(observer.function)].append(observer);
}

bool LinkFlow::resolveCallsThatWereTold()
{
    StateOfLinkCalls& state = stateOfLinkCalls();
    bool hasResolvedSome = false;
    for (unsigned lane = 0; lane < numberOfLanes(); ++lane)
        hasResolvedSome |= resolveCallsThatWereToldIn(lane);
    for (const FlowObserver& observer : std::exchange(state.membersObservedToResolve, { })) {
        hasResolvedSome = true;
        CodeAtWork codeAtWork(*this, observer.function);
        state.membersToResolve.clear(observer.record);
        CallInLinkCalls(*this, state, *state.sharedCalls[state.membersOfSharedCalls[observer.record].sharedCall]).resolveMemberOfSharedCall(observer.record);
    }
    return hasResolvedSome;
}

bool LinkFlow::resolveCallsThatWereToldIn(unsigned lane)
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
    for (const FlowObserver& observer : observers) {
        CodeAtWork codeAtWork(*this, observer.function);
        state.callsToResolve[firstCallOfFunction[observer.function.index()] + observer.record] = 0;
        CallInLinkCalls(*this, state, observer.function, observer.record).resolve();
    }
    // The lane gets the room back.
    if (state.observersToResolve[lane].isEmpty() && !state.observersToResolve[lane].capacity()) {
        observers.shrink(0);
        state.observersToResolve[lane] = WTF::move(observers);
    }
    return true;
}

void LinkFlow::settleSharedCallsAs(WhatUnseenCodeDoes reading)
{
    // WHAT A CALL LISTS BESIDE UNSEEN CODE HANGS ON WHAT IT SAW FIRST. All that it calls and passes has flowed into unseen code, so whatever it may enter has escaped, and is entered by unseen code as it is.
    auto settle = [&](CalleesOfCall& callees) {
        if (callees.mayEnterUnseen && reading == WhatUnseenCodeDoes::Anything) {
            callees.listed.removeAllMatching([&](FunctionID callee) {
                return escapedFunctions.get(callee.index());
            });
            callees.builtins.clear();
            callees.shared.clear();
            callees.mayEnterLost = false;
        }
        if (callees.listed.size() > 1)
            std::ranges::sort(callees.listed);
        if (callees.builtins.size() > 1)
            std::ranges::sort(callees.builtins);
    };
    for (CalleesOfCall& callees : calleesOfSharedCalls)
        settle(callees);
    forEachRangeOfIndicesInParallel(loops, calleesOfCalls.size(), [&](size_t first, size_t end) {
        for (size_t call = first; call < end; ++call) {
            CalleesOfCall& callees = calleesOfCalls[call];
            if (callees.shared.size() > 1) {
                auto isDoneByAnother = [&](uint32_t index) {
                    const SubjectOfSharedCall& subject = subjectsOfSharedCalls[index];
                    return std::ranges::any_of(callees.shared, [&](uint32_t other) {
                        const SubjectOfSharedCall& larger = subjectsOfSharedCalls[other];
                        return larger.family != subject.family && larger.key == subject.key && larger.form == subject.form && families.nearestCommonFamily(subject.family, larger.family) == larger.family;
                    });
                };
                Vector<uint32_t, 1> kept;
                for (uint32_t index : callees.shared) {
                    if (!isDoneByAnother(index))
                        kept.append(index);
                }
                std::ranges::sort(kept, { }, [&](uint32_t index) {
                    return subjectsOfSharedCalls[index];
                });
                callees.shared = WTF::move(kept);
            }
            for (uint32_t index : callees.shared) {
                const CalleesOfCall& shared = calleesOfSharedCalls[index];
                for (BuiltinID builtin : shared.builtins)
                    callees.add(builtin);
                callees.mayEnterLost |= shared.mayEnterLost;
                callees.mayEnterUnseen |= shared.mayEnterUnseen;
            }
            // A subject that was one layout before it was a family: what the call then listed itself the shared call lists too.
            if (!callees.shared.isEmpty()) {
                callees.listed.removeAllMatching([&](FunctionID callee) {
                    return std::ranges::any_of(callees.shared, [&](uint32_t index) {
                        return std::ranges::binary_search(calleesOfSharedCalls[index].listed, callee);
                    });
                });
            }
            settle(callees);
        }
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

#include "AOTLinkKnownCalleesInlines.h"

#endif // ENABLE(AOT)
