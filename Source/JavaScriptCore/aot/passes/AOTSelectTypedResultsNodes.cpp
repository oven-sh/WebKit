/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsNodes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowResults.h"
#include "AOTIRBuilder.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <functional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// The nodes of a type.

namespace {

struct FamilyOfMembers {
    LayoutSetID family;
    LayoutID owner;
};

} // anonymous namespace

// Two layouts may fit each other's types: `{ port, host, tags? }` and a literal `{ port, host }`. A parser builds the declared one, which has a field for every member.
static bool isBetterHead(const Program& program, LayoutID candidate, LayoutID other)
{
    auto rankOf = [&](LayoutID id) {
        const Layout& layout = program.at(id);
        return std::tuple { !!layout.name, layout.fields.size(), std::numeric_limits<uint32_t>::max() - id.index() };
    };
    return rankOf(candidate) > rankOf(other);
}

static bool fitsTypeOf(const Program& program, LayoutID layout, LayoutID declared)
{
    std::span<const LayoutID> family = program.at(program.declaredMembersOf(declared)).completeMembers();
    return std::ranges::binary_search(family, layout);
}

static Vector<FamilyOfMembers> familiesOf(const Program& program, std::span<const LayoutID> members)
{
    Vector<FamilyOfMembers> families(members.size(), [&](size_t index) {
        return FamilyOfMembers { program.declaredMembersOf(members[index]), members[index] };
    });
    AOT::sort(families, { }, [](const FamilyOfMembers& family) {
        return std::pair { family.family, family.owner };
    });
    size_t numberOfFamilies = 0;
    for (const FamilyOfMembers& family : families) {
        if (!numberOfFamilies || families[numberOfFamilies - 1].family != family.family)
            families[numberOfFamilies++] = family;
        else if (isBetterHead(program, family.owner, families[numberOfFamilies - 1].owner))
            families[numberOfFamilies - 1].owner = family.owner;
    }
    families.shrink(numberOfFamilies);
    AOT::sort(families, std::ranges::greater { }, [&](const FamilyOfMembers& family) {
        return std::pair { program.at(family.family).written().size(), family.family };
    });
    return families;
}

static Vector<LayoutID, 4> headLayoutsOf(const Program& program, const Type& type)
{
    std::span<const LayoutID> members = membersOf(program, type);
    Vector<LayoutID, 4> candidates;
    for (LayoutID member : members) {
        if (program.at(member).kind == LayoutKind::Record)
            candidates.append(member);
    }
    if (candidates.isEmpty())
        return { };

    // A candidate that is removed by itself keeps its place in the list, which stays sorted, until the list is compacted.
    BitVector isRemoved;
    size_t numberLeft = candidates.size();
    auto compact = [&] {
        if (numberLeft == candidates.size())
            return;
        size_t index = 0;
        candidates.removeAllMatching([&](LayoutID) {
            return isRemoved.get(index++);
        });
        isRemoved.clearAll();
    };

    for (const FamilyOfMembers& family : familiesOf(program, members)) {
        if (!numberLeft)
            break;
        std::span<const LayoutID> subtypes = program.at(family.family).completeMembers();
        auto staysBesideOwner = [&](LayoutID layout) {
            return layout == family.owner || (isBetterHead(program, layout, family.owner) && fitsTypeOf(program, family.owner, layout));
        };
        if (numberLeft <= subtypes.size()) {
            compact();
            candidates.removeAllMatching([&](LayoutID candidate) {
                return std::ranges::binary_search(subtypes, candidate) && !staysBesideOwner(candidate);
            });
            numberLeft = candidates.size();
            continue;
        }
        for (LayoutID subtype : subtypes) {
            auto position = std::ranges::lower_bound(candidates, subtype);
            if (position == candidates.end() || *position != subtype || staysBesideOwner(subtype))
                continue;
            if (!isRemoved.set(static_cast<size_t>(position - candidates.begin())))
                --numberLeft;
        }
    }
    compact();
    return candidates;
}

Vector<TypeAlternative, 4> alternativesOf(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    Vector<TypeAlternative, 4> alternatives;
    if (type.primitives & Primitive::Null)
        alternatives.append(TypeAlternative { .shape = ResultShape::Null });
    if (uint16_t booleans = type.primitives & Primitive::Boolean; booleans == Primitive::Boolean)
        alternatives.append(TypeAlternative { .shape = ResultShape::Boolean });
    else if (booleans)
        alternatives.append(TypeAlternative { .shape = ResultShape::BooleanLiteral, .literal = { .kind = ResultLiteral::Kind::Boolean, .boolean = booleans == Primitive::True } });
    if (type.primitives & Primitive::Number)
        alternatives.append(TypeAlternative { .shape = ResultShape::Number });
    if (type.primitives & Primitive::String)
        alternatives.append(TypeAlternative { .shape = ResultShape::String });
    else {
        for (StringID string : type.stringLiterals)
            alternatives.append(TypeAlternative { .shape = ResultShape::StringLiteral, .literal = { .kind = ResultLiteral::Kind::String, .string = string } });
    }
    if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array))
        alternatives.append(TypeAlternative { .shape = ResultShape::Array, .contents = array->writtenContentsAt(program.builtins(), ContentsSlot::Element) });
    for (LayoutID layout : headLayoutsOf(program, type))
        alternatives.append(TypeAlternative { .shape = ResultShape::Object, .layout = layout });
    return alternatives;
}

// What flow has proven about the arrays in a field says nothing about what a parser has to make. Fields that differ in that alone share a parser: the one of the type as it was read.
static TypeID withoutProofsAboutClasses(const Program& program, TypeID id)
{
    if (!id || TypeTable::isProvisional(id))
        return id;
    auto hasProof = [](const ClassPart& part) {
        return part.contentsAreProven || !part.provenPristine.isEmpty() || part.arrayStorage != ArrayStorage::Other;
    };
    if (std::ranges::none_of(program.at(id).classes, hasProof))
        return id;

    Type type = program.at(id);
    for (ClassPart& part : type.classes) {
        part.contentsAreProven = false;
        part.provenPristine = { };
        part.arrayStorage = ArrayStorage::Other;
    }
    type.canonicalize();
    return program.types.find(type).value_or(id);
}

static ResultNode describeAlternative(const Program& program, TypeID type, const TypeAlternative& alternative)
{
    static constexpr ResultShapeSet shapesOfAlternatives { ResultShape::Null, ResultShape::String, ResultShape::Number, ResultShape::Boolean, ResultShape::BooleanLiteral, ResultShape::StringLiteral, ResultShape::Array, ResultShape::Object };
    RELEASE_ASSERT(shapesOfAlternatives.contains(alternative.shape));

    ResultNode node { .shape = alternative.shape, .type = type, .literal = alternative.literal, .layout = alternative.layout };
    if (node.shape == ResultShape::Array)
        node.children.append(ResultNodeRef { alternative.contents });
    if (node.shape != ResultShape::Object)
        return node;

    const Vector<Field>& fields = program.at(node.layout).fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = fields[index];
        if (!isEnumerableStringField(program, field))
            continue;
        TypeID expectation = withoutProofsAboutClasses(program, expectationOf(field.declared).predicted());
        node.members.append(ResultMember {
            .key = field.key,
            .node = ResultNodeRef { expectation ? expectation : TypeTable::anything },
            .field = index,
            .isOptional = (!field.declared.written() || program.at(field.declared.expected().predicted()).primitives & Primitive::Undefined) && flowOf(program, { node.layout, index }).occupancy != Occupancy::AlwaysFilled,
        });
    }
    return node;
}

ResultNode describeNode(const Program& program, ResultNodeRef ref)
{
    auto [type, part] = ref;
    Vector<TypeAlternative, 4> alternatives = alternativesOf(program, type);
    if (part) {
        RELEASE_ASSERT(part <= alternatives.size());
        return describeAlternative(program, type, alternatives[part - 1]);
    }

    ResultNode node { .type = type };
    if (program.at(type).anyObject || alternatives.isEmpty())
        return node;
    if (alternatives.size() == 1)
        return describeAlternative(program, type, alternatives[0]);

    node.shape = ResultShape::Union;
    for (uint32_t index = 0; index < alternatives.size(); ++index)
        node.children.append(ResultNodeRef { type, index + 1 });
    return node;
}

Value makeArrayOf(const Program& program, const ResultNode& node)
{
    const ClassPart* part = program.at(node.type).findClassPart(BuiltinClasses::Array);
    Value array = IRBuilder::make(Opcode::NewArray);
    array.setContents(part ? part->writtenContentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything);
    array.setElementsOperands(std::span<const ValueID> { });
    return array;
}

static OptionSet<JSONValueClass> classesAcceptedBy(ResultShape shape)
{
    switch (shape) {
    case ResultShape::Unknown:
        return { JSONValueClass::Null, JSONValueClass::Boolean, JSONValueClass::Number, JSONValueClass::String, JSONValueClass::Array, JSONValueClass::Object };
    case ResultShape::Null:
        return JSONValueClass::Null;
    case ResultShape::Boolean:
    case ResultShape::BooleanLiteral:
        return JSONValueClass::Boolean;
    case ResultShape::Number:
        return JSONValueClass::Number;
    case ResultShape::String:
    case ResultShape::StringLiteral:
        return JSONValueClass::String;
    case ResultShape::Array:
        return JSONValueClass::Array;
    case ResultShape::Object:
        return JSONValueClass::Object;
    case ResultShape::Union:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OptionSet<JSONValueClass> valueClassesOf(const Program& program, ResultNodeRef ref)
{
    OptionSet<JSONValueClass> classes;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        classes.add(classesAcceptedBy(node.shape));
    });
    return classes;
}

LayoutID firstLayoutOf(const Program& program, ResultNodeRef ref)
{
    LayoutID first;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        if (node.layout && (!first || node.layout < first))
            first = node.layout;
    });
    return first;
}

static std::optional<ResultLiteral> findTag(const Program& program, const ResultNode& arm, PropertyKeyID key)
{
    static constexpr ResultShapeSet literalShapes { ResultShape::Null, ResultShape::BooleanLiteral, ResultShape::StringLiteral };
    if (arm.shape != ResultShape::Object)
        return std::nullopt;
    const ResultMember* member = std::ranges::find(arm.members, key, &ResultMember::key);
    // Every field of a layout may be missing, and the result stays plain data where no arm is chosen.
    if (member == arm.members.end())
        return std::nullopt;

    ResultNode tag = describeNode(program, member->node);
    if (!literalShapes.contains(tag.shape))
        return std::nullopt;
    return tag.literal;
}

static std::optional<TaggedArms> partitionArmsByTag(const Program& program, std::span<const ResultNodeRef> arms, std::span<const ResultNode> nodes, PropertyKeyID key)
{
    TaggedArms result { .discriminant = { .key = key } };
    for (size_t index = 0; index < arms.size(); ++index) {
        std::optional<ResultLiteral> tag = findTag(program, nodes[index], key);
        if (!tag) {
            result.untagged.append(arms[index]);
            continue;
        }
        if (result.discriminant.tagOfArm.contains(*tag))
            return std::nullopt;
        result.discriminant.tagOfArm.append(*tag);
        result.tagged.append(arms[index]);
    }
    if (result.tagged.size() < 2)
        return std::nullopt;
    return result;
}

std::optional<TaggedArms> findTaggedArms(const Program& program, std::span<const ResultNodeRef> arms)
{
    Vector<ResultNode, 4> nodes(arms.size(), [&](size_t index) {
        return describeNode(program, arms[index]);
    });
    size_t numberOfObjects = std::ranges::count(nodes, ResultShape::Object, &ResultNode::shape);

    std::optional<TaggedArms> firstPartition;
    Vector<PropertyKeyID, 8> triedKeys;
    for (const ResultNode& node : nodes) {
        for (const ResultMember& member : node.members) {
            if (triedKeys.contains(member.key) || !findTag(program, node, member.key))
                continue;
            triedKeys.append(member.key);
            std::optional<TaggedArms> partition = partitionArmsByTag(program, arms, nodes.span(), member.key);
            if (!partition)
                continue;
            if (partition->tagged.size() == numberOfObjects)
                return partition;
            if (!firstPartition)
                firstPartition = WTF::move(partition);
        }
    }
    return firstPartition;
}

// Which nodes are compiled.

static bool canReach(const Program& program, ResultNodeRef start, ResultNodeRef target)
{
    HashSet<std::pair<TypeID, uint32_t>> visited;
    visited.add({ start.type, start.part });
    Vector<ResultNodeRef, 16> worklist { start };
    bool isReached = false;
    while (!isReached && !worklist.isEmpty()) {
        describeNode(program, worklist.takeLast()).forEachChild([&](ResultNodeRef child) {
            if (child == target)
                isReached = true;
            else if (visited.add({ child.type, child.part }).isNewEntry)
                worklist.append(child);
        });
    }
    return isReached;
}

static bool triesSeveralRecursiveArms(const Program& program, std::span<const ResultNodeRef> candidates, ResultNodeRef unionRef)
{
    auto isRecursive = [&](ResultNodeRef arm) {
        return canReach(program, arm, unionRef);
    };

    std::optional<TaggedArms> arms = findTaggedArms(program, candidates);
    unsigned numberOfRecursiveTrials = arms && std::ranges::any_of(arms->tagged, isRecursive) ? 1 : 0;
    for (ResultNodeRef arm : arms ? arms->untagged.span() : candidates) {
        if (isRecursive(arm) && ++numberOfRecursiveTrials > 1)
            return true;
    }
    return false;
}

AOT_THRESHOLD(unsigned, maxLooksAtOnePart, 64);
AOT_THRESHOLD(unsigned, maxDepthOfCountedLooks, 128);

using LooksByNode = HashMap<uint64_t, unsigned, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

static Vector<ResultNodeRef, 4> armsOfClass(const Program& program, std::span<const ResultNodeRef> arms, JSONValueClass valueClass)
{
    Vector<ResultNodeRef, 4> candidates;
    for (ResultNodeRef arm : arms) {
        if (valueClassesOf(program, arm).contains(valueClass))
            candidates.append(arm);
    }
    return candidates;
}

// How often one part of a value is looked at below a node, at most: each arm of a union that is tried looks anew at all that it holds, so unions below unions multiply.
// Counting ends above the threshold. A node that is met again below itself counts once.
static unsigned countLooksAtOnePart(const Program& program, ResultNodeRef ref, LooksByNode& known, unsigned depth)
{
    if (depth > maxDepthOfCountedLooks)
        return maxLooksAtOnePart + 1;
    uint64_t key = static_cast<uint64_t>(ref.type.index()) << 32 | ref.part;
    if (auto added = known.add(key, 1); !added.isNewEntry)
        return added.iterator->value;

    auto looksBelow = [&](ResultNodeRef child) {
        return countLooksAtOnePart(program, child, known, depth + 1);
    };
    ResultNode node = describeNode(program, ref);
    unsigned looks = 1;
    if (node.shape != ResultShape::Union) {
        node.forEachChild([&](ResultNodeRef child) {
            looks = std::max(looks, looksBelow(child));
        });
    } else {
        for (JSONValueClass valueClass : { JSONValueClass::Array, JSONValueClass::Object }) {
            Vector<ResultNodeRef, 4> candidates = armsOfClass(program, node.children.span(), valueClass);
            std::optional<TaggedArms> tagged = findTaggedArms(program, candidates.span());
            unsigned looksOfClass = 0;
            if (tagged) {
                for (ResultNodeRef arm : tagged->tagged)
                    looksOfClass = std::max(looksOfClass, looksBelow(arm));
            }
            for (ResultNodeRef arm : tagged ? tagged->untagged.span() : candidates.span())
                looksOfClass += looksBelow(arm);
            looks = std::max(looks, looksOfClass);
        }
    }
    looks = std::min(looks, maxLooksAtOnePart + 1);
    known.set(key, looks);
    return looks;
}

static bool hasRepeatedRecursion(const Program& program, ResultNodeRef unionRef, const ResultNode& unionNode)
{
    LooksByNode known;
    if (countLooksAtOnePart(program, unionRef, known, 0) > maxLooksAtOnePart)
        return true;

    std::span<const ResultNodeRef> arms = unionNode.children.span();
    Vector<OptionSet<JSONValueClass>, 4> classesOfArm(arms.size(), [&](size_t index) {
        return valueClassesOf(program, arms[index]);
    });

    for (JSONValueClass valueClass : { JSONValueClass::Array, JSONValueClass::Object }) {
        Vector<ResultNodeRef, 4> candidates;
        for (size_t index = 0; index < arms.size(); ++index) {
            if (classesOfArm[index].contains(valueClass))
                candidates.append(arms[index]);
        }
        if (candidates.size() > 1 && triesSeveralRecursiveArms(program, candidates.span(), unionRef))
            return true;
    }
    return false;
}

bool allocatesLayout(const NodeDecisionInputs& inputs, LayoutID layout)
{
    if (!layout || inputs.program.at(layout).flow.allocation == AllocationState::NeverAllocated)
        return false;
    return !inputs.flow.hasResults() || !!inputs.flow.typedResultSite(layout);
}

NodeDecision decideNodeCode(const NodeDecisionInputs& inputs, ResultNodeRef ref)
{
    ResultNode node = describeNode(inputs.program, ref);
    if (node.shape == ResultShape::Union && hasRepeatedRecursion(inputs.program, ref, node))
        return { NodeCode::Interpreted, InterpretedCause::RepeatedRecursion };
    if (node.shape == ResultShape::Object && !allocatesLayout(inputs, node))
        return { NodeCode::Interpreted, InterpretedCause::LayoutIsNotModeled };
    return { NodeCode::Compiled, InterpretedCause::None };
}

void addInterpretedNodeDecline(const NodeDecisionInputs& inputs, LayoutID declaration, InterpretedCause cause)
{
    RELEASE_ASSERT(cause != InterpretedCause::None);
    if (!inputs.build.options().collectsDeclines())
        return;

    Subject subject { .layout = declaration };
    subject.location = findLocation(&inputs.program, subject).value_or(noSourceLocation);
    inputs.build.addDecline({
        .kind = DeclineKind::ResultNodeInterpreted,
        .limit = LimitID::None,
        .count = 0,
        .reason = encodeDeclineReason(DeclineKind::ResultNodeInterpreted, cause),
        .witness = witnessOf(subject),
    });
}

DerivedFunctionKey parserKeyOf(ResultNodeRef ref)
{
    RELEASE_ASSERT(!ref.part);
    return derivedFunctionKey(FunctionRole::Parser, ref.type);
}

ResultNodeRef nodeOfKey(const DerivedFunctionKey& key)
{
    RELEASE_ASSERT(key.role == FunctionRole::Parser);
    return ResultNodeRef { TypeID(key.parameter) };
}

// The type that the users of a call ask of its result.

TypedResultForm typedResultFormOf(const Program& program, const Value& call)
{
    if (call.opcode == Opcode::CallBuiltin)
        return program.builtins().at(call.overload()).result.typedResultForm;
    // The signature that the call is written against says that it answers a query, whoever the callee is.
    bool answersQuery = call.opcode == Opcode::Call && call.expectedResult() && program.at(call.expectedResult()).flags.contains(ExpectedResultFlags::IsRowsOfQuery);
    return answersQuery ? TypedResultForm::Rows : TypedResultForm::None;
}

ValueID typedResultSourceOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin)
        return { };
    BuiltinCall builtinCall = builtinCallOf(program, call);
    std::optional<BuiltinOperand> source = program.builtins().at(builtinCall.overload).result.typedResultSource;
    return source ? builtinCall.operandAt(*source) : ValueID();
}

std::optional<uint32_t> indexOfAwaitedOperand(const Value& value)
{
    if (value.opcode == Opcode::Await)
        return OperandIndex::Await::value;
    if (value.opcode == Opcode::AwaitBySavingFrame)
        return OperandIndex::AwaitBySavingFrame::awaited;
    return std::nullopt;
}

static Vector<TypeID, 4> collectRequestedTypes(const Program& program, const Function& function, const UseLists& uses, ValueID call, Vector<PropertyKeyID, 4>& namesRead)
{
    struct FollowedValue {
        ValueID value;
        bool mayBePromise;
    };

    Vector<TypeID, 4> requestedTypes;
    auto consider = [&](TypeID id) {
        if (!id || TypeTable::isProvisional(id))
            return;
        const Type& type = program.at(id);
        if (!membersOf(program, type).empty() || type.findClassPart(BuiltinClasses::Array))
            requestedTypes.append(id);
    };

    // Flow asks about calls that are not resolved to a built-in yet, and those record no form.
    TypedResultForm form = typedResultFormOf(program, function.at(call));
    Vector<FollowedValue, 4> worklist { { call, form != TypedResultForm::ParsedText } };
    BitVector visited;
    visited.set(call.index());
    Vector<PropertyKeyID, 4> namesReadBeforeAwait;
    bool isAwaited = false;

    while (!worklist.isEmpty()) {
        FollowedValue followed = worklist.takeLast();
        for (const Use& use : uses.usesOf(followed.value)) {
            if (use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::ReturnCall) {
                for (TailCast cast : function.at(use.block).terminator.casts())
                    consider(cast.declared.expected().predicted());
            }
            if (use.kind != Use::Kind::Operand)
                continue;

            const Value& user = function.at(use.user);
            if (user.opcode == Opcode::PropGet && use.operandIndex == OperandIndex::PropGet::object)
                (followed.mayBePromise ? namesReadBeforeAwait : namesRead).append(user.key());
            // AddGenericVersion makes a test and a branch of a Check.
            if (user.opcode == Opcode::TypeTest && use.operandIndex == OperandIndex::TypeTest::value)
                consider(user.testedType());
            if (user.opcode == Opcode::CallDirect) {
                std::optional<uint32_t> entryParameter = user.variadicOperandIndex(use.operandIndex);
                const Function& callee = program.at(user.function());
                if (entryParameter && *entryParameter < callee.numberOfEntryParameters() && callee.entryParameterKind(*entryParameter) == EntryParameter::Parameter)
                    consider(callee.declaredEntryParameter(*entryParameter)->expected().predicted());
            }

            bool isSameValue = isCast(user.opcode) && !use.operandIndex;
            bool isResolvedValue = followed.mayBePromise && indexOfAwaitedOperand(user) == use.operandIndex;
            if (!isSameValue && !isResolvedValue)
                continue;
            isAwaited |= isResolvedValue;
            if (user.opcode == Opcode::Check)
                consider(user.expectedType().predicted());
            consider(user.expected().predicted());
            if (visited.add(use.user.index()))
                worklist.append({ use.user, followed.mayBePromise && isSameValue });
        }
    }
    if (!isAwaited)
        namesRead.appendVector(namesReadBeforeAwait);
    return requestedTypes;
}

static bool isDeepSubset(const Program& program, TypeID narrow, TypeID wide)
{
    using TypePair = std::pair<TypeID, TypeID>;
    Vector<TypePair, 4> worklist { TypePair { narrow, wide } };
    HashSet<TypePair> visited;
    while (!worklist.isEmpty()) {
        TypePair pair = worklist.takeLast();
        if (pair.first == pair.second || !visited.add(pair).isNewEntry)
            continue;
        const Type& narrowType = program.at(pair.first);
        const Type& wideType = program.at(pair.second);
        if (!narrowType.isSubsetOf(wideType, program))
            return false;
        for (const ClassPart& part : narrowType.classes) {
            const ClassPart* partner = wideType.findClassPart(part.builtinClass);
            if (!partner)
                continue;
            for (unsigned index = 0; index < partner->writtenContents.size(); ++index)
                worklist.append(TypePair { part.writtenContentsAt(index), partner->writtenContentsAt(index) });
        }
    }
    return true;
}

static bool isWrittenInLibrary(const Program& program, ModuleID module)
{
    return module && (program.at(module).kind == ModuleKind::RuntimeSource || program.at(module).flags.contains(ModuleFlags::IsLibrary));
}

static bool declaresNamesRead(const Program& program, const Function& function, const Type& type, std::span<const PropertyKeyID> namesRead)
{
    bool hasObjectType = false;
    bool isAskedByLibrary = isWrittenInLibrary(program, function.module);
    for (LayoutID member : membersOf(program, type)) {
        const Layout& layout = program.at(member);
        if (layout.kind != LayoutKind::Record)
            continue;
        hasObjectType = true;
        if (!isAskedByLibrary && isWrittenInLibrary(program, layout.module))
            continue;
        if (std::ranges::all_of(namesRead, [&](PropertyKeyID name) { return !!layout.findField(name); }))
            return true;
    }
    return !hasObjectType;
}

TypeID findTypeOfTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    Vector<PropertyKeyID, 4> namesRead;
    Vector<TypeID, 4> requestedTypes = collectRequestedTypes(program, function, uses, call, namesRead);
    TypeID mostSpecific;
    for (TypeID requested : requestedTypes) {
        if (!mostSpecific || isDeepSubset(program, requested, mostSpecific))
            mostSpecific = requested;
    }
    for (TypeID requested : requestedTypes) {
        if (!isDeepSubset(program, mostSpecific, requested))
            return { };
    }
    if (mostSpecific && !function.has(FunctionFlags::IsChecked) && !declaresNamesRead(program, function, program.at(mostSpecific), namesRead.span()))
        return { };
    return mostSpecific;
}

TypedResultTarget findTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call, const HeadLayoutsOfFamilies* headLayouts)
{
    TypedResultTarget target { findTypeOfTypedResultTarget(program, function, uses, call), { } };
    if (!target)
        return { };
    auto append = [&](LayoutID layout) {
        target.layouts.append(layout);
        return IterationStatus::Continue;
    };
    forEachLayoutReachedBy(program, target.type, headLayouts, ScopedLambda<IterationStatus(LayoutID)>(append));
    AOT::sort(target.layouts);
    return target;
}

std::span<const LayoutID> HeadLayoutsOfFamilies::of(const Program& program, const Type& type) const
{
    {
        Locker locker { m_lock };
        if (auto found = m_headsOfFamily.find(type.layouts); found != m_headsOfFamily.end())
            return found->value->span();
    }
    auto heads = makeUniqueWithoutFastMallocCheck<Vector<LayoutID, 4>>(headLayoutsOf(program, type));
    Locker locker { m_lock };
    return m_headsOfFamily.ensure(type.layouts, [&] {
        return WTF::move(heads);
    }).iterator->value->span();
}

void forEachLayoutReachedBy(const Program& program, TypeID root, const HeadLayoutsOfFamilies* headLayouts, const ScopedLambda<IterationStatus(LayoutID)>& functor)
{
    HashSet<LayoutID> visitedLayouts;
    HashSet<LayoutSetID> visitedFamilies;
    HashSet<TypeID> visitedTypes;
    Vector<TypeID, 8> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };

    visit(root);
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array))
            visit(array->writtenContentsAt(program.builtins(), ContentsSlot::Element));
        if (!type.layouts || !visitedFamilies.add(type.layouts).isNewEntry)
            continue;
        Vector<LayoutID, 4> headsOfThisWalk;
        if (!headLayouts)
            headsOfThisWalk = headLayoutsOf(program, type);
        for (LayoutID layout : headLayouts ? headLayouts->of(program, type) : headsOfThisWalk.span()) {
            if (!visitedLayouts.add(layout).isNewEntry)
                continue;
            if (functor(layout) == IterationStatus::Done)
                return;
            for (const Field& field : program.at(layout).fields) {
                if (isEnumerableStringField(program, field))
                    visit(expectationOf(field.declared).predicted());
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
