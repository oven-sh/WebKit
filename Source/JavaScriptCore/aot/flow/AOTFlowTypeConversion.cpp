/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTypeConversion.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowResultFamilies.h"
#include "AOTIRProgram.h"
#include "AOTSortedSet.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct JoinedValue {
    uint16_t primitives { 0 };
    Range integers { Range::empty() };
    SortedSet<StringID> strings;
    SortedSet<PropertyKeyID> symbols;
    FlowObjectSet objects;
    FlowNodeID firstNodeWithUnknownObject;

    bool isEmpty() const { return !primitives && objects.isEmpty(); }
};

struct ConversionContext {
    const FlowTypeConverter& converter;
    const Type& asRead;
    const FlowConversionSwitches& switches;
    TypeArena& arena;

    const FlowSolution& solution() const { return converter.solution(); }
    const Program& program() const { return converter.solution().program; }
};

enum class ObjectPartKind : uint8_t {
    Unknown,
    Environment,
    Layout,
    Function,
    Builtin,
    Class,
};

struct ClassifiedObject {
    ObjectPartKind kind { ObjectPartKind::Unknown };
    LayoutID layout;
    FlowSiteLoss loss;
    LayoutID lossLayout;

    bool isProven() const { return loss.cause == UnprovenCause::Unknown; }
};

struct ObjectLists {
    Vector<LayoutID, 8> layouts;
    Vector<FunctionID, 8> functions;
    Vector<BuiltinID, 8> builtins;
    Type classParts;
    bool hasAnyFunction { false };

    bool isEmpty() const { return layouts.isEmpty() && functions.isEmpty() && builtins.isEmpty() && classParts.isNever() && !hasAnyFunction; }
};

struct ObjectPart {
    Type type;
    FlowSiteLoss loss;
    LayoutID lossLayout;

    bool isProven() const { return loss.cause == UnprovenCause::Unknown; }
};

using ClassPartCreator = ClassPart (*)(const ConversionContext&, FlowObjectID);

} // anonymous namespace

static JoinedValue joinValues(const FlowSolution& solution, std::span<const FlowNodeID> nodes)
{
    const FlowValueTable& values = solution.values();
    JoinedValue joined;
    Vector<StringID, 16> strings;
    Vector<PropertyKeyID, 4> symbols;
    for (FlowNodeID node : nodes) {
        if (!node)
            continue;
        const FlowValue& value = solution.graph.valueOf(node);
        joined.primitives |= value.primitives;
        joined.integers = joined.integers.unionWith(values.rangeOf(value.integers));
        strings.append(values.stringsOf(value.strings));
        symbols.append(values.symbolsOf(value.symbols));
        joined.objects.unite(value.objects);
        if (!joined.firstNodeWithUnknownObject && value.hasUnknownObject())
            joined.firstNodeWithUnknownObject = node;
    }

    if (joined.primitives & Primitive::String)
        joined.primitives &= ~Primitive::LiteralString;
    else
        joined.strings = SortedSet<StringID>(strings.span());

    if (joined.primitives & Primitive::Symbol)
        joined.primitives &= ~Primitive::LiteralSymbol;
    else
        joined.symbols = SortedSet<PropertyKeyID>(symbols.span());
    return joined;
}

static void addNumberPart(Type& type, const ConversionContext& context, const JoinedValue& joined)
{
    uint16_t joinedNumbers = joined.primitives & Primitive::Number;
    if (!joinedNumbers)
        return;
    const Type& asRead = context.asRead;
    type.primitives |= context.switches.wholeNumbers ? joinedNumbers : (asRead.primitives & Primitive::Number);
    bool hasProvenRange = context.switches.integerRanges && (joinedNumbers & Primitive::Integer);
    type.integers = hasProvenRange ? joined.integers.intersectWith(asRead.integers) : asRead.integers;
}

static void addStringPart(Type& type, const ConversionContext& context, const JoinedValue& joined)
{
    if (!(joined.primitives & Primitive::AnyString))
        return;
    if (context.switches.literalValues)
        type.stringLiterals = joined.strings;
    if (type.stringLiterals.isEmpty())
        type.primitives |= Primitive::String;
}

static bool canListSymbol(const ConversionContext& context, PropertyKeyID key)
{
    if (context.program().at(key).kind != PropertyKeyKind::CreatedSymbol)
        return true;
    return std::ranges::binary_search(context.converter.tables().symbolsCreatedOnce, key);
}

static void addSymbolPart(Type& type, const ConversionContext& context, const JoinedValue& joined)
{
    if (!(joined.primitives & Primitive::AnySymbol))
        return;
    bool canListSymbols = context.switches.literalValues && std::ranges::all_of(joined.symbols, [&](PropertyKeyID key) {
        return canListSymbol(context, key);
    });
    if (canListSymbols)
        type.symbolLiterals = joined.symbols;
    if (type.symbolLiterals.isEmpty())
        type.primitives |= Primitive::Symbol;
}

// Leaves LiteralString and LiteralSymbol to canonicalize(), which sets each bit from its list.
static Type primitivePart(const ConversionContext& context, const JoinedValue& joined)
{
    const Type& asRead = context.asRead;
    const FlowConversionSwitches& switches = context.switches;

    Type type;
    type.primitives = joined.primitives & (Primitive::Boolean | Primitive::BigInt);
    type.primitives |= (switches.removedNullish ? joined.primitives : asRead.primitives) & Primitive::UndefinedOrNull;
    type.primitives |= (switches.initialization ? joined.primitives : asRead.primitives) & Primitive::Absent;
    addNumberPart(type, context, joined);
    addStringPart(type, context, joined);
    addSymbolPart(type, context, joined);
    type.canonicalize();
    return type;
}

static ClassifiedObject objectWithoutPart(UnprovenCause cause, const Witness& witness = noWitness)
{
    ClassifiedObject object;
    object.loss.cause = cause;
    object.loss.witness = witness;
    return object;
}

static ClassifiedObject objectWithPart(ObjectPartKind kind, LayoutID layout = { })
{
    ClassifiedObject object;
    object.kind = kind;
    object.layout = layout;
    return object;
}

static ClassifiedObject classifyAllocation(const FlowTypeConverter& converter, FlowObjectID id, LayoutID layout)
{
    if (!layout)
        return objectWithoutPart(UnprovenCause::NonLayoutObject);
    if (converter.solution().program.at(layout).isEnvironment())
        return objectWithPart(ObjectPartKind::Environment);

    ClassifiedObject object = objectWithPart(ObjectPartKind::Layout, layout);
    FlowShapeResult shape = converter.shapeOf(id);
    switch (shape.state) {
    case FlowShapeState::KeepsShape:
        return object;
    case FlowShapeState::NeverHasShape:
        return objectWithoutPart(shape.loss.cause, shape.loss.witness);
    case FlowShapeState::Unknown:
        break;
    }
    object.loss = shape.loss;
    if (object.loss.cause == UnprovenCause::Unknown)
        object.loss.cause = UnprovenCause::NonLayoutObject;
    object.lossLayout = layout;
    return object;
}

static ClassifiedObject classify(const FlowTypeConverter& converter, const JoinedValue& joined, FlowObjectID id)
{
    const FlowSolution& solution = converter.solution();
    const FlowObject& record = solution.objects.at(id);
    switch (record.kind) {
    case FlowObjectKind::Unknown:
        return objectWithoutPart(UnprovenCause::UnknownObject, solution.records.unknownObjectWitness(joined.firstNodeWithUnknownObject));
    case FlowObjectKind::Allocation:
        return classifyAllocation(converter, id, solution.objects.allocatedLayoutOf(id));
    case FlowObjectKind::ModuleNamespace:
        if (!record.layout)
            return objectWithoutPart(UnprovenCause::NonLayoutObject);
        return objectWithPart(ObjectPartKind::Layout, record.layout);
    case FlowObjectKind::Closure:
        return objectWithPart(ObjectPartKind::Function);
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        // What stands for all that a native binding answers is no object of the realm, and no type names it.
        if (!isObjectKind(solution.builtins().at(record.builtin).kind))
            return objectWithoutPart(UnprovenCause::NonLayoutObject);
        return objectWithPart(ObjectPartKind::Builtin);
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        if (!record.builtinClass)
            return objectWithoutPart(UnprovenCause::NonLayoutObject);
        return objectWithPart(ObjectPartKind::Class);
    case FlowObjectKind::Prototype:
        return objectWithoutPart(UnprovenCause::NonLayoutObject);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ClassifiedObject classifyInPosition(const ConversionContext& context, const JoinedValue& joined, FlowObjectID id)
{
    ClassifiedObject object = classify(context.converter, joined, id);
    switch (object.kind) {
    case ObjectPartKind::Layout:
        if (!context.switches.provenLayoutSets)
            return objectWithoutPart(UnprovenCause::ResultDisabled);
        break;
    case ObjectPartKind::Function:
        break;
    case ObjectPartKind::Unknown:
    case ObjectPartKind::Environment:
    case ObjectPartKind::Builtin:
    case ObjectPartKind::Class:
        return object;
    }
    // The lattice puts layouts and closures outside every class part, so meeting the type as read would drop an instance of a class that extends Array from an Array position.
    if (object.isProven() && !context.solution().values().isObjectInside(id, context.asRead))
        return objectWithoutPart(UnprovenCause::NonLayoutObject);
    return object;
}

static void keepFirstLoss(FlowSiteLoss& loss, LayoutID& lossLayout, const ClassifiedObject& object)
{
    FlowSiteLoss previous = loss;
    keepFirstLoss(loss, object.loss);
    if (loss != previous)
        lossLayout = object.lossLayout;
}

static ClassPart createClassPart(const ConversionContext& context, FlowObjectID id)
{
    const FlowConversionSwitches& switches = context.switches;
    const FlowObject& record = context.solution().objects.at(id);
    const FlowContentsResult& contents = context.converter.contentsOf(id);

    ClassPart part;
    part.builtinClass = record.builtinClass;
    part.contentsAreProven = contents.isProven && switches.provenContents;
    if (part.contentsAreProven || switches.predictions)
        part.contents = contents.contents;
    if (switches.predictions)
        part.predictedContents.appendVector(contents.predictedContents);
    if (part.contentsAreProven)
        part.arrayStorage = contents.provenStorage;
    else if (switches.arrayStorageKinds)
        part.arrayStorage = contents.predictedStorage;
    if (switches.pristineInstances && record.kind == FlowObjectKind::Instance)
        part.provenPristine = context.converter.shapeOf(id).pristine;
    return part;
}

static ClassPart createPredictedClassPart(const ConversionContext& context, FlowObjectID id)
{
    const FlowContentsResult& contents = context.converter.contentsOf(id);

    ClassPart part;
    part.builtinClass = context.solution().objects.at(id).builtinClass;
    part.contents = contents.contents;
    part.predictedContents.appendVector(contents.predictedContents);
    if (context.switches.arrayStorageKinds)
        part.arrayStorage = contents.predictedStorage;
    return part;
}

static ClassPart createStoragePrediction(const ConversionContext& context, FlowObjectID id)
{
    ClassPart part = createClassPart(context, id);
    part.arrayStorage = context.converter.contentsOf(id).predictedStorage;
    return part;
}

static bool hasPredictedArrayStorage(const ConversionContext& context, const FlowObjectSet& objects)
{
    return context.switches.arrayStorageKinds && objects.anyOf([&](FlowObjectID id) {
        return context.converter.contentsOf(id).predictedStorage != ArrayStorage::Other;
    });
}

static void addClassPart(Type& classParts, ClassPart&& part, TypeArena& arena)
{
    Type added;
    added.classes.append(WTF::move(part));
    classParts = classParts.unionWith(added, arena);
}

static void addPart(const ConversionContext& context, ObjectLists& lists, FlowObjectID id, const ClassifiedObject& object, ClassPartCreator createPart)
{
    const FlowObject& record = context.solution().objects.at(id);
    switch (object.kind) {
    case ObjectPartKind::Unknown:
    case ObjectPartKind::Environment:
        return;
    case ObjectPartKind::Layout:
        lists.layouts.append(object.layout);
        return;
    case ObjectPartKind::Function:
        if (context.switches.exactFunctions)
            lists.functions.append(record.function);
        else
            lists.hasAnyFunction = true;
        return;
    case ObjectPartKind::Builtin:
        lists.builtins.append(context.solution().builtins().canonicalID(record.builtin));
        return;
    case ObjectPartKind::Class:
        if (context.solution().builtins().at(record.builtinClass).hasClosureInstances) {
            FunctionID boundFunction = context.converter.boundFunctionOf(id);
            if (boundFunction && context.switches.exactFunctions)
                lists.functions.append(boundFunction);
            else
                lists.hasAnyFunction = true;
            return;
        }
        addClassPart(lists.classParts, createPart(context, id), context.arena);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type createObjectType(const ConversionContext& context, const ObjectLists& lists)
{
    Type type = lists.classParts;
    type.layouts = Type::forLayouts(lists.layouts.span(), context.arena).layouts;
    type.functions = SortedSet<FunctionID>(lists.functions.span());
    type.builtins = SortedSet<BuiltinID>(lists.builtins.span());
    type.anyFunction = lists.hasAnyFunction;
    type.canonicalize();
    return type;
}

// The literals that created the RegExps among the objects. The empty list stands for any RegExp.
static SortedSet<RegExpLiteralInType> collectRegExpLiterals(const ConversionContext& context, const FlowObjectSet& objects)
{
    const FlowObjectTable& table = context.solution().objects;
    Vector<RegExpLiteralInType, 4> literals;
    bool isComplete = true;
    objects.forEach([&](FlowObjectID id) {
        const FlowObject& record = table.at(id);
        if (record.builtinClass != BuiltinClasses::RegExp)
            return;
        if (!record.allocationClass) {
            isComplete = false;
            return;
        }
        for (AllocationSiteID siteID : table.at(record.allocationClass).sites) {
            const FlowSite& site = table.at(siteID);
            if (site.description.origin != FlowSiteOrigin::Operation) {
                isComplete = false;
                return;
            }
            const Value& creation = context.program().at(site.function).at(site.description.value);
            if (creation.opcode != Opcode::NewRegExp) {
                isComplete = false;
                return;
            }
            literals.append({ creation.pattern(), creation.regExpFlags().toRaw() });
        }
    });
    if (!isComplete)
        return { };
    return SortedSet<RegExpLiteralInType>(literals.span());
}

static ObjectPart objectPart(const ConversionContext& context, const JoinedValue& joined)
{
    ObjectPart part;
    ObjectLists lists;
    joined.objects.forEach([&](FlowObjectID id) {
        ClassifiedObject object = classifyInPosition(context, joined, id);
        if (!object.isProven()) {
            keepFirstLoss(part.loss, part.lossLayout, object);
            return;
        }
        if (part.isProven())
            addPart(context, lists, id, object, createClassPart);
    });
    part.type = part.isProven() ? createObjectType(context, lists) : context.asRead.withOnlyObjects();
    if (part.isProven() && context.switches.literalValues && part.type.findClassPart(BuiltinClasses::RegExp))
        part.type.regExpLiteralSet = collectRegExpLiterals(context, joined.objects);
    return part;
}

static void narrowPosition(const ConversionContext& context, const JoinedValue& joined, const Type& primitives, FlowPositionType& position)
{
    const Type& asRead = context.asRead;
    const Program& program = context.program();

    ObjectPart objects = objectPart(context, joined);
    position.type = primitives.unionWith(objects.type, context.arena);
    position.loss = objects.loss;
    position.lossLayout = objects.lossLayout;
    if (position.type.isSubsetOf(asRead, program))
        return;

    position.type = position.type.intersectWith(asRead, context.arena);
    if (position.type.isSubsetOf(asRead, program))
        return;

    position.type = position.type.withoutObjects().unionWith(asRead.withOnlyObjects(), context.arena);
    if (!position.hasLoss())
        position.loss.cause = UnprovenCause::NonLayoutObject;
}

static bool isWrapperMadeByConversion(const ConversionContext& context, FlowObjectID id)
{
    const FlowObjectTable& table = context.solution().objects;
    const FlowObject& record = table.at(id);
    if (!record.allocationClass)
        return false;
    return std::ranges::all_of(table.at(record.allocationClass).sites, [&](AllocationSiteID siteID) {
        const FlowSite& site = table.at(siteID);
        if (site.description.origin != FlowSiteOrigin::Operation || !site.function || !site.description.value)
            return false;
        Opcode opcode = context.program().at(site.function).at(site.description.value).opcode;
        return opcode == Opcode::ToObject || opcode == Opcode::ToThis;
    });
}

// Unseen code hands out what it has, so such an object comes with every value out of unseen code and says nothing about any of them.
static bool isBuiltinThatUnseenCodeHas(const ConversionContext& context, FlowObjectID id)
{
    switch (context.solution().objects.at(id).kind) {
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return context.solution().objectsInNativeCode().contains(id);
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Type> computePrediction(const ConversionContext& context, const JoinedValue& joined, const Type& primitives, const FlowPositionType& position)
{
    if (!position.hasLoss() && !hasPredictedArrayStorage(context, joined.objects))
        return std::nullopt;

    // A storage kind beside proven contents is a proof, so the predicted kind is published here. With a loss no test could establish the proofs of a part.
    ClassPartCreator createPart = position.hasLoss() ? createPredictedClassPart : createStoragePrediction;
    // The unknown value brings every primitive with it, and a conversion to an object makes a wrapper of each. Beside the unknown object the wrappers say as little as the primitives.
    bool holdsUnknownObject = joined.objects.containsUnknownObject();
    ObjectLists lists;
    joined.objects.forEach([&](FlowObjectID id) {
        if (holdsUnknownObject && (isWrapperMadeByConversion(context, id) || isBuiltinThatUnseenCodeHas(context, id)))
            return;
        addPart(context, lists, id, classify(context.converter, joined, id), createPart);
    });
    if (lists.isEmpty())
        return std::nullopt;

    const Type& type = position.type;
    const Program& program = context.program();
    // The unknown value brings every primitive with it. A prediction that leaves out none of them says nothing about them and hides the one class beside them.
    bool holdsEveryPrimitive = (primitives.primitives & Primitive::All) == Primitive::All;
    Type prediction = createObjectType(context, lists);
    if (!holdsEveryPrimitive)
        prediction = primitives.unionWith(prediction, context.arena);
    if (!prediction.isSubsetOf(type, program))
        prediction = prediction.intersectWith(type, context.arena);
    if (prediction.isNever() || prediction == type || !prediction.isSubsetOf(type, program))
        return std::nullopt;
    return prediction;
}

FlowConversionSwitches FlowConversionSwitches::of(const BuildContext& build, uint32_t subject)
{
    auto isEnabled = [&](FlowResultKind kind) {
        return build.isEnabled(kind, subject);
    };
    return {
        .narrowedTypes = isEnabled(FlowResultKind::NarrowedTypes),
        .removedNullish = isEnabled(FlowResultKind::RemovedNullish),
        .literalValues = isEnabled(FlowResultKind::LiteralValues),
        .integerRanges = isEnabled(FlowResultKind::IntegerRanges),
        .wholeNumbers = isEnabled(FlowResultKind::WholeNumbers),
        .exactFunctions = isEnabled(FlowResultKind::ExactFunctions),
        .provenLayoutSets = isEnabled(FlowResultKind::ProvenLayoutSets),
        .provenContents = isEnabled(FlowResultKind::ProvenContents),
        .arrayStorageKinds = isEnabled(FlowResultKind::ArrayStorageKinds),
        .pristineInstances = isEnabled(FlowResultKind::PristineInstances),
        .initialization = isEnabled(FlowResultKind::Initialization),
        .predictions = isEnabled(FlowResultKind::Predictions),
    };
}

FlowTypeConverter::FlowTypeConverter(const FlowSolution& solution, const FlowConversionTables& tables)
    : m_solution(solution)
    , m_tables(tables)
{
    ASSERT(std::ranges::is_sorted(tables.symbolsCreatedOnce));
}

FlowPositionType FlowTypeConverter::convertWith(std::span<const FlowNodeID> nodes, const Type& asRead, const FlowConversionSwitches& switches, TypeArena& arena) const
{
    JoinedValue joined = joinValues(m_solution, nodes);

    FlowPositionType position;
    position.type = asRead;
    position.isEmpty = joined.isEmpty();
    if (!asRead.isJavaScriptValue())
        return position;
    if (position.isEmpty) {
        if (switches.narrowedTypes)
            position.type = Type::never();
        return position;
    }

    ConversionContext context { *this, asRead, switches, arena };
    Type primitives = primitivePart(context, joined);
    if (switches.narrowedTypes)
        narrowPosition(context, joined, primitives, position);
    else
        position.loss.cause = UnprovenCause::ResultDisabled;
    ASSERT(position.type.isSubsetOf(asRead, m_solution.program));

    if (switches.predictions)
        position.prediction = computePrediction(context, joined, primitives, position);
    return position;
}

FlowPositionType FlowTypeConverter::convert(std::span<const FlowNodeID> nodes, const Type& asRead, uint32_t switchSubject, TypeArena& arena) const
{
    return convertWith(nodes, asRead, FlowConversionSwitches::of(m_solution.build, switchSubject), arena);
}

FlowPositionType FlowTypeConverter::convert(FlowNodeID node, const Type& asRead, uint32_t switchSubject, TypeArena& arena) const
{
    std::span<const FlowNodeID> nodes = singleElementSpan(node);
    return convert(nodes, asRead, switchSubject, arena);
}

TypeInclusion FlowTypeConverter::isInside(std::span<const FlowNodeID> nodes, const Type& target, TypeArena& arena) const
{
    // anything() has no Absent, so meeting it would hide an Absent that the nodes hold.
    Type everything = Type::anything().withPrimitives(Primitive::Absent);
    FlowPositionType converted = convertWith(nodes, everything, FlowConversionSwitches::allProofs(), arena);
    if (converted.isEmpty || converted.type.isSubsetOf(target, m_solution.program))
        return TypeInclusion::Inside;
    return TypeInclusion::Unknown;
}

bool FlowTypeConverter::isEverMade(const Type& type) const
{
    if (!type.hasOnlyLayouts())
        return true;
    return std::ranges::any_of(membersOf(m_solution.program, type), [&](LayoutID layout) {
        if (layout.index() >= m_tables.layouts.size())
            return true;
        const LayoutFlow& flow = m_tables.layouts[layout.index()];
        return flow.allocation != AllocationState::NeverAllocated || !flow.members.isEmpty();
    });
}

// A layout stands for what is made of it and below it.
Type FlowTypeConverter::withMembers(const Type& type, TypeArena& arena) const
{
    BitVector isMember(m_solution.program.layouts.size());
    for (LayoutID layout : membersOf(m_solution.program, type)) {
        if (layout.index() >= m_tables.layouts.size())
            continue;
        for (LayoutID member : m_tables.layouts[layout.index()].members)
            isMember.set(member.index());
    }
    Vector<LayoutID, 8> members;
    for (size_t index : isMember)
        members.append(LayoutID(static_cast<uint32_t>(index)));
    return members.isEmpty() ? type : type.unionWith(Type::forLayouts(members.span(), arena), arena);
}

Type FlowTypeConverter::narrowExpectation(const Type& expected, const Type& seen, TypeArena& arena) const
{
    Type agreed = withMembers(expected, arena).intersectWith(seen, arena);
    // Where the unknown value has brought every primitive, what is seen lists none of them.
    if (!seen.primitives)
        agreed = agreed.unionWith(expected.withoutObjects(), arena);
    bool agrees = expected.hasObjects() ? agreed.hasObjects() : !agreed.isNever();
    // What is written does not always name what is made: a layout that was seen to arrive stays predicted.
    bool hasSeenLayouts = expected.layouts && seen.layouts;
    if (!agrees)
        return !hasSeenLayouts && isEverMade(expected.withOnlyObjects()) ? expected : seen;
    if (hasSeenLayouts)
        agreed = agreed.unionWith(Type::forLayoutSet(seen.layouts), arena);

    for (ClassPart& part : agreed.classes) {
        const ClassPart* expectedPart = expected.findClassPart(part.builtinClass);
        const ClassPart* seenPart = seen.findClassPart(part.builtinClass);
        if (part.contentsAreProven || !expectedPart || !seenPart)
            continue;
        for (unsigned index = 0; index < part.contents.size(); ++index) {
            TypeID expectedContents = expectedPart->contentsAt(index);
            TypeID seenContents = seenPart->contentsAt(index);
            if (expectedContents == TypeTable::anything || seenContents == TypeTable::anything)
                continue;
            part.contents[index] = arena.intern(narrowExpectation(Type(arena.at(expectedContents)), Type(arena.at(seenContents)), arena));
        }
    }
    return agreed;
}

std::optional<Type> FlowTypeConverter::refinePrediction(TypeID expectedByChecker, const FlowPositionType& position, TypeArena& arena) const
{
    if (!position.prediction || !position.hasLoss() || !expectedByChecker)
        return position.prediction;

    const Program& program = m_solution.program;
    Type expected = program.at(expectedByChecker).intersectWith(position.type, arena);
    if (expected.isNever() || expected == position.type || !expected.isSubsetOf(position.type, program))
        return position.prediction;

    return narrowExpectation(expected, *position.prediction, arena);
}

LayoutSet FlowTypeConverter::receiversOf(std::span<const FlowNodeID> nodes) const
{
    JoinedValue joined = joinValues(m_solution, nodes);

    LayoutSet receivers;
    receivers.isComplete = true;
    joined.objects.forEach([&](FlowObjectID id) {
        ClassifiedObject object = classify(*this, joined, id);
        switch (object.kind) {
        case ObjectPartKind::Layout:
            receivers.members.append(object.layout);
            return;
        case ObjectPartKind::Environment:
            return;
        case ObjectPartKind::Unknown:
        case ObjectPartKind::Function:
        case ObjectPartKind::Builtin:
        case ObjectPartKind::Class:
            receivers.isComplete = false;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    canonicalize(receivers);
    return receivers;
}

FlowSiteLoss FlowTypeConverter::lossOfObjects(std::span<const FlowNodeID> nodes, LayoutID& lossLayout) const
{
    JoinedValue joined = joinValues(m_solution, nodes);

    FlowSiteLoss loss;
    lossLayout = { };
    joined.objects.forEach([&](FlowObjectID id) {
        keepFirstLoss(loss, lossLayout, classify(*this, joined, id));
    });
    return loss;
}

FlowShapeResult FlowTypeConverter::shapeOf(FlowObjectID object) const
{
    AllocationClassID allocationClass = m_solution.objects.at(object).allocationClass;
    if (!allocationClass || allocationClass.index() >= m_tables.shapesByAllocationClass.size())
        return { };
    return m_tables.shapesByAllocationClass[allocationClass.index()];
}

FunctionID FlowTypeConverter::boundFunctionOf(FlowObjectID object) const
{
    if (object.index() >= m_tables.boundFunctionByObject.size())
        return { };
    return m_tables.boundFunctionByObject[object.index()];
}

const FlowContentsResult& FlowTypeConverter::contentsOf(FlowObjectID object) const
{
    if (object.index() >= m_tables.contentsByObject.size())
        return m_unknownContents;
    return m_tables.contentsByObject[object.index()];
}

TypeID FlowTypeConverter::elementsAllocatedAt(AllocationSiteID site) const
{
    if (site.index() >= m_tables.elementsBySite.size())
        return { };
    return m_tables.elementsBySite[site.index()];
}

ArrayStorage predictedArrayStorageOf(const Type& elements)
{
    return arrayStorageHolding(elements);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
