/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjects.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t numberOfResultObjectsOfBuiltin = 4;
static_assert(ordinalOfSetterFunctionOfAccessor + 1 == numberOfResultObjectsOfBuiltin);

static ObjectRecord objectRecordOfKind(ObjectKind kind, uint32_t payload, OptionSet<ObjectTrait> traits)
{
    ASSERT((traits & traitsOfShape).hasExactlyOneBitSet());
    ASSERT((traits & traitsOfIdentity).hasExactlyOneBitSet());
    ObjectRecord record;
    record.kind = kind;
    record.payload = payload;
    record.traits = traits;
    return record;
}

static void setLayoutOfObjectRecord(ObjectRecord& record, LayoutID layout, bool mayHaveLayoutBelow)
{
    record.traits.remove(traitsOfShape);
    record.traits.add(ObjectTrait::HasLayout);
    if (mayHaveLayoutBelow)
        record.traits.add(ObjectTrait::MayHaveLayoutBelow);
    record.layout = layout;
    record.builtinClass = { };
}

static void setClassOfObjectRecord(ObjectRecord& record, const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    record.traits.remove(traitsOfShape);
    record.traits.add(ObjectTrait::HasClass);
    // A proxy is callable only where its target is.
    if (isFunctionClass(builtins, builtinClass) && builtins.at(builtinClass).type != ProxyObjectType)
        record.traits.add(ObjectTrait::IsCallable);
    record.layout = { };
    record.builtinClass = builtinClass;
}

// The record must have HasNoShape. A type has no part for a class whose instances are closures: it says any function.
static void makeObjectRecordInstanceOf(ObjectRecord& record, const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    if (builtins.at(builtinClass).hasClosureInstances)
        record.traits.add(ObjectTrait::IsCallable);
    else
        setClassOfObjectRecord(record, builtins, builtinClass);
}

static ObjectRecord objectRecordOfBuiltin(const BuiltinTable& builtins, ObjectKind kind, BuiltinID canonical)
{
    OptionSet<ObjectTrait> traits { ObjectTrait::HasNoShape, ObjectTrait::IsBuiltin };
    if (isCallableBuiltin(builtins, canonical))
        traits.add(ObjectTrait::IsCallable);
    if (kind == ObjectKind::Builtin && isObjectKind(builtins.at(canonical).kind) && isTestableBuiltin(builtins, canonical))
        traits.add(ObjectTrait::IsFoundByIdentity);
    return objectRecordOfKind(kind, canonical.index(), traits);
}

// One object stands for all that the rows of a built-in make in one place. It has a class only where every row that makes it agrees.
class ObjectShapeInRows {
public:
    void noteInstanceOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
    {
        if (builtins.at(builtinClass).hasClosureInstances)
            note(State::IsFunction, { });
        else
            note(State::HasClass, builtinClass);
    }

    void notePlainObject() { note(State::IsPlainObject, { }); }
    void noteObjectThatNoRowDescribes() { m_state = State::IsNotDescribed; }

    void applyTo(ObjectRecord& record, const BuiltinTable& builtins) const
    {
        switch (m_state) {
        case State::IsNeverMade:
        case State::IsNotDescribed:
            return;
        case State::IsPlainObject:
            record.traits.add(ObjectTrait::IsPlainObjectOfRows);
            return;
        case State::IsFunction:
            record.traits.add(ObjectTrait::IsCallable);
            return;
        case State::HasClass:
            setClassOfObjectRecord(record, builtins, m_builtinClass);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    enum class State : uint8_t { IsNeverMade, HasClass, IsFunction, IsPlainObject, IsNotDescribed };

    void note(State state, BuiltinClassID builtinClass)
    {
        if (m_state == State::IsNeverMade) {
            m_state = state;
            m_builtinClass = builtinClass;
            return;
        }
        if (m_state != state || m_builtinClass != builtinClass)
            m_state = State::IsNotDescribed;
    }

    State m_state { State::IsNeverMade };
    BuiltinClassID m_builtinClass;
};

struct ObjectShapesOfBuiltin {
    ObjectShapeInRows result;
    ObjectShapeInRows contents;
    Vector<BuiltinTypeID, 8> typesSeenInside;
};

static void noteObjectShapesInside(const BuiltinTable&, BuiltinID, BuiltinTypeID, ObjectShapesOfBuiltin&);

static void noteObjectShapeOfPart(const BuiltinTable& builtins, BuiltinID builtin, const BuiltinTypePart& part, ObjectShapeInRows& shape, ObjectShapesOfBuiltin& shapes)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        shape.noteInstanceOf(builtins, part.builtinClass);
        break;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        if (BuiltinClassID receiverClass = builtins.receiverClassOf(builtin))
            shape.noteInstanceOf(builtins, receiverClass);
        else
            shape.noteObjectThatNoRowDescribes();
        break;
    case BuiltinTypePartKind::Layout: {
        const BuiltinLayout& layout = builtins.at(part.layout);
        if (layout.isTuple)
            shape.noteInstanceOf(builtins, BuiltinClasses::Array);
        else
            shape.notePlainObject();
        for (const BuiltinLayoutField& field : layout.fields)
            noteObjectShapesInside(builtins, builtin, field.type, shapes);
        break;
    }
    case BuiltinTypePartKind::Builtin:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        break;
    }
    for (const BuiltinSlotContents& contents : part.contents)
        noteObjectShapesInside(builtins, builtin, contents.type, shapes);
}

static void noteObjectShapesInside(const BuiltinTable& builtins, BuiltinID builtin, BuiltinTypeID type, ObjectShapesOfBuiltin& shapes)
{
    if (!type || shapes.typesSeenInside.contains(type))
        return;
    shapes.typesSeenInside.append(type);
    for (const BuiltinTypePart& part : builtins.at(type).parts)
        noteObjectShapeOfPart(builtins, builtin, part, shapes.contents, shapes);
}

// The second object of these is the list in which the engine hands the arguments on. No type of their rows names it.
static bool handsArgumentsOnInListOfEngine(const BuiltinTable& builtins, BuiltinID canonical)
{
    constexpr NamedBuiltin builtinsThatHandArgumentsOn[] { Builtins::FunctionPrototypeBind, Builtins::FunctionPrototypeCall, Builtins::FunctionPrototypeApply, Builtins::ReflectApply, Builtins::ReflectConstruct };
    for (BuiltinID builtin : builtinsThatHandArgumentsOn) {
        if (builtins.canonicalID(builtin) == canonical)
            return true;
    }
    return false;
}

static ObjectShapesOfBuiltin objectShapesInRowsOf(const BuiltinTable& builtins, BuiltinID canonical)
{
    ObjectShapesOfBuiltin shapes;
    if (handsArgumentsOnInListOfEngine(builtins, canonical))
        shapes.contents.noteInstanceOf(builtins, BuiltinClasses::Array);

    auto notePlace = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NewValue)
            noteObjectShapesInside(builtins, canonical, place.newValueType, shapes);
    };
    auto notePlaces = [&](std::span<const BuiltinPlace> places) {
        for (const BuiltinPlace& place : places)
            notePlace(place);
    };

    for (const BuiltinOverload& overload : builtins.overloadsOf(canonical)) {
        if (BuiltinTypeID resultType = overload.result.type) {
            const BuiltinType& type = builtins.at(resultType);
            // For plain data, the first result object stands for the objects and the second for the arrays.
            if (type.flags.contains(BuiltinTypeFlag::HasPlainData)) {
                shapes.result.notePlainObject();
                shapes.contents.noteInstanceOf(builtins, BuiltinClasses::Array);
            }
            for (const BuiltinTypePart& part : type.parts)
                noteObjectShapeOfPart(builtins, canonical, part, shapes.result, shapes);
        }
        for (const BuiltinResultProperty& property : overload.result.properties)
            noteObjectShapesInside(builtins, canonical, property.type, shapes);
        // The second object of a row that iterates by entries is the pair that a step hands out.
        if (overload.result.iteratedOperand && overload.result.iterationKind == JSC::IterationKind::Entries)
            shapes.contents.noteInstanceOf(builtins, BuiltinClasses::Array);

        for (const BuiltinTransfer& transfer : overload.flow.transfers) {
            notePlace(transfer.from);
            notePlace(transfer.to);
        }
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            notePlace(callback.newTarget);
            notePlace(callback.thisValue);
            notePlaces(callback.arguments);
            notePlace(callback.spreadArguments);
            notePlace(callback.resultTo);
        }
        for (const BuiltinHandlerCall& call : overload.flow.handlerCalls) {
            notePlace(call.callee);
            notePlace(call.thisValue);
            notePlaces(call.arguments);
        }
        for (const BuiltinPropertyAction& action : overload.flow.propertyActions)
            notePlace(action.value);
    }

    // The records of module loads and called functions do not give a type for everything the host passes to library code.
    if (!builtins.moduleLoadsOf(canonical).empty() || !builtins.calledFunctionsOf(canonical).empty())
        shapes.contents.noteObjectThatNoRowDescribes();
    return shapes;
}

bool rulesMeetBuiltinByName(const BuiltinTable& builtins, BuiltinID canonical)
{
    return std::ranges::any_of(builtins.overloadsOf(canonical), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Call && overload.hasTypedResult();
    });
}

bool callOrReadOfBuiltinIsEvent(const BuiltinTable& builtins, BuiltinID canonical)
{
    constexpr BuiltinMark marksOfEvents[] { BuiltinMark::EvaluatesTextInGlobalScope, BuiltinMark::LoadsCodeWithoutJSAccess, BuiltinMark::ReadLoadsCodeByName };
    for (BuiltinMark mark : marksOfEvents) {
        if (builtins.has(mark, canonical))
            return true;
    }
    auto isThisBuiltin = [&](BuiltinID other) {
        return other && builtins.canonicalID(other) == canonical;
    };
    for (const BuiltinLoaderOfMachineCode& loader : builtins.loadersOfMachineCode()) {
        if (isThisBuiltin(loader.callee) || isThisBuiltin(loader.named))
            return true;
    }
    // letsInUnseenCode() and raisesEventWhenCalled() skip a row that runs unseen code only if an operand has some property. Whoever calls it is asked for that operand, so it stands for itself too.
    return builtins.letsInUnseenCode(canonical) || builtins.unseenCodeOf(canonical) || raisesEventWhenCalled(builtins, canonical);
}

static Vector<uint32_t> placesOfFunctionsByGroup(std::span<const FunctionInOrderOfObjects> functions, NOESCAPE const Invocable<unsigned(const FunctionInOrderOfObjects&)> auto& groupOf)
{
    std::array<uint32_t, FunctionInOrderOfObjects::numberOfGroups> next { };
    for (const FunctionInOrderOfObjects& function : functions)
        ++next[groupOf(function)];
    uint32_t first = 0;
    for (uint32_t& place : next)
        first += std::exchange(place, first);

    Vector<uint32_t> places;
    places.reserveInitialCapacity(functions.size());
    for (const FunctionInOrderOfObjects& function : functions)
        places.append(next[groupOf(function)]++);
    return places;
}

// Each index once, in no particular order. Null: on the calling thread.
static void forEachObjectRecordInParallel(const ParallelLoops* loops, uint32_t count, NOESCAPE const Invocable<void(uint32_t)> auto& write)
{
    constexpr uint32_t recordsWrittenAtOnce = 1 << 12;
    if (!loops) {
        for (uint32_t index = 0; index < count; ++index)
            write(index);
        return;
    }
    forEachIndexInParallel(*loops, (count + recordsWrittenAtOnce - 1) / recordsWrittenAtOnce, [&](unsigned part) {
        uint32_t first = part * recordsWrittenAtOnce;
        for (uint32_t index = first, end = first + std::min(recordsWrittenAtOnce, count - first); index < end; ++index)
            write(index);
    });
}

// The list moves to where it has more room. What takes the time is the first touch of each page there, so many threads carry the records over.
static void carryObjectRecordsOver(std::span<const ObjectRecord> from, std::span<ObjectRecord> to, const ParallelLoops& loops)
{
    constexpr size_t recordsCarriedAtOnce = 1 << 14;
    forEachIndexInParallel(loops, static_cast<unsigned>((from.size() + recordsCarriedAtOnce - 1) / recordsCarriedAtOnce), [&](unsigned part) {
        size_t first = part * recordsCarriedAtOnce;
        size_t count = std::min(recordsCarriedAtOnce, from.size() - first);
        memcpySpan(to.subspan(first, count), from.subspan(first, count));
    });
}

ObjectTable::ObjectTable(const BuiltinTable& builtins, std::span<const FunctionInOrderOfObjects> functions, const ParallelLoops* loops)
    : m_builtins(builtins)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(functions.size());
    m_placesByGroup = placesOfFunctionsByGroup(functions, [](const FunctionInOrderOfObjects& function) -> unsigned {
        return static_cast<unsigned>(function.group);
    });
    m_placesWithCoroutinesFirst = placesOfFunctionsByGroup(functions, [](const FunctionInOrderOfObjects& function) -> unsigned {
        return !function.isCoroutine;
    });
    m_placesWithRestArraysFirst = placesOfFunctionsByGroup(functions, [](const FunctionInOrderOfObjects& function) -> unsigned {
        return !function.hasRestArray;
    });

    uint32_t numberOfBuiltins = builtins.builtinCount();
    uint32_t numberOfClasses = builtins.classCount();
    std::span<const BuiltinHostObject> hostObjects = builtins.hostObjects();
    m_records.reserveInitialCapacity(1 + (2 + numberOfResultObjectsOfBuiltin) * static_cast<size_t>(numberOfBuiltins) + hostObjects.size() + numberOfClasses + 2 + 5 * static_cast<size_t>(numberOfFunctions));

    auto begin = [&](ObjectKind kind) {
        m_first[indexOf(kind)] = size();
    };
    auto appendForEachFunction = [&](ObjectRecord record, const Vector<uint32_t>& places) {
        begin(record.kind);
        uint32_t first = size();
        m_records.grow(first + numberOfFunctions);
        // Whoever writes a new record touches its page for the first time, and that is what takes the time.
        forEachObjectRecordInParallel(loops, numberOfFunctions, [&](uint32_t function) {
            ObjectRecord recordOfFunction = record;
            recordOfFunction.payload = function;
            m_records[first + places[function]] = recordOfFunction;
        });
    };

    begin(ObjectKind::Unknown);
    m_records.append(objectRecordOfKind(ObjectKind::Unknown, 0, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }));

    // The place of an entry that names the object of another entry is never handed out. It holds the record of that other entry.
    begin(ObjectKind::Builtin);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index)
        m_records.append(objectRecordOfBuiltin(builtins, ObjectKind::Builtin, builtins.canonicalID(BuiltinID(index))));

    m_builtinFunctionsThatStayApart.ensureSize(numberOfBuiltins);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index) {
        const ObjectRecord& record = m_records[firstOf(ObjectKind::Builtin) + index];
        if (record.payload == index && record.has(ObjectTrait::IsCallable) && !record.has(ObjectTrait::IsFoundByIdentity) && (callOrReadOfBuiltinIsEvent(builtins, BuiltinID(index)) || rulesMeetBuiltinByName(builtins, BuiltinID(index))))
            m_builtinFunctionsThatStayApart.quickSet(index);
    }

    begin(ObjectKind::ResultOfBuiltin);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index) {
        BuiltinID canonical = builtins.canonicalID(BuiltinID(index));
        ObjectShapesOfBuiltin shapes = objectShapesInRowsOf(builtins, canonical);
        for (uint32_t ordinal = 0; ordinal < numberOfResultObjectsOfBuiltin; ++ordinal) {
            ObjectRecord record = objectRecordOfKind(ObjectKind::ResultOfBuiltin, canonical.index(), { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject });
            record.origin = SiteOrigin::BuiltinResult;
            record.ordinal = ordinal;
            if (!isFunctionOfNativeAccessor(record))
                (ordinal ? shapes.contents : shapes.result).applyTo(record, builtins);
            // The functions that `new Promise` hands its executor.
            if (isFunctionOfNativeAccessor(record) || (ordinal == 1 && canonical == builtins.canonicalID(Builtins::Promise)))
                record.traits.add(ObjectTrait::IsCallable);
            m_records.append(record);
        }
    }

    begin(ObjectKind::HostObject);
    for (uint32_t index = 0; index < hostObjects.size(); ++index) {
        ObjectRecord record = objectRecordOfKind(ObjectKind::HostObject, index, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject });
        if (hostObjects[index].function)
            record.traits.add(ObjectTrait::IsCallable);
        m_records.append(record);
    }

    begin(ObjectKind::EscapedFunctionsOfHolder);
    for (uint32_t index = 0; index < numberOfBuiltins; ++index)
        m_records.append(objectRecordOfKind(ObjectKind::EscapedFunctionsOfHolder, builtins.canonicalID(BuiltinID(index)).index(), { ObjectTrait::HasNoShape, ObjectTrait::IsBuiltin, ObjectTrait::IsCallable, ObjectTrait::IsMerged }));

    begin(ObjectKind::EscapedOfClass);
    for (uint32_t index = 0; index < numberOfClasses; ++index) {
        ObjectRecord record = objectRecordOfKind(ObjectKind::EscapedOfClass, index, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject, ObjectTrait::IsMerged });
        setClassOfObjectRecord(record, builtins, BuiltinClassID(index));
        m_records.append(record);
    }

    begin(ObjectKind::EscapedWithoutShape);
    for (MakerOfShapelessObject maker : { MakerOfShapelessObject::Host, MakerOfShapelessObject::OperationOrRow }) {
        ObjectRecord record = objectRecordOfKind(ObjectKind::EscapedWithoutShape, static_cast<uint32_t>(maker), { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject, ObjectTrait::IsMerged });
        // Either stands for objects that native code has made.
        record.origin = SiteOrigin::BuiltinResult;
        m_records.append(record);
    }

    // What a coroutine answers with and what a construction makes get their shapes once the program is read.
    ObjectRecord restArray = objectRecordOfKind(ObjectKind::RestArray, 0, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject });
    setClassOfObjectRecord(restArray, builtins, BuiltinClasses::Array);
    appendForEachFunction(objectRecordOfKind(ObjectKind::Closure, 0, { ObjectTrait::HasNoShape, ObjectTrait::IsClosure, ObjectTrait::IsCallable }), m_placesByGroup);
    appendForEachFunction(objectRecordOfKind(ObjectKind::CoroutineResult, 0, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }), m_placesWithCoroutinesFirst);
    appendForEachFunction(restArray, m_placesWithRestArraysFirst);
    appendForEachFunction(objectRecordOfKind(ObjectKind::FunctionPrototype, 0, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }), m_placesByGroup);
    appendForEachFunction(objectRecordOfKind(ObjectKind::ConstructedObject, 0, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }), m_placesByGroup);

    for (unsigned kind = indexOf(ObjectKind::Namespace); kind <= numberOfKinds; ++kind)
        m_first[kind] = size();
    m_firstSiteOfFunction.fill(0, static_cast<size_t>(numberOfFunctions) + 1);
}

void ObjectTable::addObjectsOfProgram(const Program& program, const BitVector& modulesNamedByModuleBuiltins, const ParallelLoops& loops)
{
    uint32_t numberOfFunctions = endOf(ObjectKind::Closure) - firstOf(ObjectKind::Closure);
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    uint32_t numberOfModules = static_cast<uint32_t>(program.modules.size());
    RELEASE_ASSERT(size() == firstOf(ObjectKind::Namespace) && size() == firstObjectOfInstances());
    RELEASE_ASSERT(program.functions.size() == numberOfFunctions);

    auto begin = [&](ObjectKind kind) {
        m_first[indexOf(kind)] = size();
    };

    // As for functions: classes, then the other layouts that objects are made with. An environment is no value and nothing has a layout that is never allocated, so their numbers come last.
    constexpr unsigned numberOfGroupsOfLayouts = 3;
    auto groupOfLayout = [](const Layout& layout) -> unsigned {
        if (!isObjectLayoutOf(layout.kind) || layout.has(LayoutFlags::IsNeverAllocated) || layout.has(LayoutFlags::IsTypeParameter))
            return numberOfGroupsOfLayouts - 1;
        return layout.kind != LayoutKind::Instance;
    };

    // The layouts are tens of megabytes. Every thread reads some of them, once, and lays down the few words of each that the loops of one thread below go by.
    enum class DeclaredAs : uint8_t { NoClass, ConstructorFunction, OtherClass };
    struct WordsOfLayout {
        LayoutID writtenBase;
        FunctionID constructor;
        DeclaredAs declaredAs;
        uint8_t group;
    };
    Vector<WordsOfLayout> wordsOfLayouts;
    wordsOfLayouts.grow(numberOfLayouts);
    // This reads only the written layouts, which are complete here. Program::indexClasses() fills the derived tables later.
    forEachRangeOfIndicesInParallel(loops, numberOfLayouts, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const Layout& layout = program.layouts[index];
            WordsOfLayout words { writtenBaseOf(layout), layout.constructor, DeclaredAs::NoClass, static_cast<uint8_t>(groupOfLayout(layout)) };
            if (LayoutID declaredLayout = declaredLayoutOf(program, LayoutID(index))) {
                const Layout& declared = program.at(declaredLayout);
                if (declared.kind == LayoutKind::Instance && declared.constructor)
                    words.declaredAs = declared.has(LayoutFlags::IsConstructorFunction) ? DeclaredAs::ConstructorFunction : DeclaredAs::OtherClass;
            }
            wordsOfLayouts[index] = words;
        }
    });

    BitVector hasLayoutRightBelow(numberOfLayouts);
    for (const WordsOfLayout& words : wordsOfLayouts) {
        if (words.writtenBase)
            hasLayoutRightBelow.quickSet(words.writtenBase.index());
    }
    BitVector mayHaveLayoutBelow(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        DeclaredAs declaredAs = wordsOfLayouts[index].declaredAs;
        if (declaredAs == DeclaredAs::NoClass)
            continue;
        // Nobody has followed heritages yet: a class may stand below any other. By the layout alone, as Program::prototypeMaySelectOtherClass(): the trait reaches Compile.
        if (hasLayoutRightBelow.quickGet(index) || declaredAs == DeclaredAs::OtherClass)
            mayHaveLayoutBelow.quickSet(index);
    }

    // Each function has a record of its own.
    forEachObjectRecordInParallel(&loops, numberOfFunctions, [&](uint32_t index) {
        const Function& function = program.functions[index];
        ObjectRecord& result = m_records[coroutineResultOf(FunctionID(index)).index()];
        bool isAsync = function.has(FunctionFlags::IsAsync);
        if (function.has(FunctionFlags::IsGenerator))
            setClassOfObjectRecord(result, m_builtins, isAsync ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator);
        else if (isAsync)
            setClassOfObjectRecord(result, m_builtins, BuiltinClasses::Promise);
    });
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        // verifyRecords() reports an id outside its table, and it runs behind the read.
        FunctionID constructor = wordsOfLayouts[index].constructor;
        if (constructor && constructor.index() < numberOfFunctions)
            setLayoutOfObjectRecord(m_records[constructedObjectOf(constructor).index()], LayoutID(index), mayHaveLayoutBelow.quickGet(index));
    }

    m_bodiesOfLibraryModules.ensureSize(numberOfFunctions);
    for (const Module& module : program.modules) {
        if (module.kind == ModuleKind::RuntimeSource && module.body && module.body.index() < numberOfFunctions)
            m_bodiesOfLibraryModules.quickSet(module.body.index());
    }

    Vector<uint8_t> isModuleOfLibrary(numberOfModules, [&](size_t index) -> uint8_t {
        const Module& module = program.modules[index];
        return module.kind == ModuleKind::RuntimeSource || module.flags.contains(ModuleFlags::IsLibrary);
    });
    uint32_t numberOfBuiltins = m_builtins.builtinCount();
    for (uint32_t index = 0; index < numberOfBuiltins; ++index) {
        BuiltinID builtin(index);
        if (m_builtins.canonicalID(builtin) != builtin || m_builtins.at(builtin).scope != BuiltinScope::Module)
            continue;
        // Only a native binding answers with such a function, and only code of the library names a native binding.
        bool isMadeForLibraryAlone = m_builtins.isUnder(builtin, Builtins::NativeFunctions);
        // The run time makes the module object of any other file when require() first answers for it, and objectOf() stands for those: moduleObjectOfCommonJSFile() of facts/AOTFlowRulesCommonJS.cpp.
        bool isMadeForCommonJSFilesAlone = m_builtins.isUnder(builtin, Builtins::ModuleObject);
        for (uint32_t module = 0; module < numberOfModules; ++module) {
            if (isMadeForLibraryAlone && !isModuleOfLibrary[module])
                continue;
            if (isMadeForCommonJSFilesAlone && (program.modules[module].kind != ModuleKind::CommonJs || !program.modules[module].exportsVariable))
                continue;
            if (!isMadeForLibraryAlone && !isMadeForCommonJSFilesAlone && !modulesNamedByModuleBuiltins.get(module))
                continue;
            m_moduleBuiltins.append({ builtin, ModuleID(module) });
        }
    }
    m_moduleBuiltins.shrinkToFit();
    ASSERT(std::ranges::is_sorted(m_moduleBuiltins));

    Vector<ObjectRecord> records;
    records.reserveInitialCapacity(size() + numberOfModules + m_moduleBuiltins.size() + 3 * static_cast<size_t>(numberOfLayouts));
    records.grow(size());
    carryObjectRecordsOver(m_records.span(), records.mutableSpan(), loops);
    m_records = WTF::move(records);

    // A test run may hold several namespaces of one file, and so may a program that loads a file once more from disk.
    constexpr OptionSet<ModuleFlags> flagsOfModulesWithSeveralNamespaces { ModuleFlags::RunsAgain, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted };
    begin(ObjectKind::Namespace);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        ObjectRecord record = objectRecordOfKind(ObjectKind::Namespace, index, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject });
        if (!program.modules[index].flags.containsAny(flagsOfModulesWithSeveralNamespaces))
            record.traits.add(ObjectTrait::IsFoundByIdentity);
        if (LayoutID layout = program.layoutOfWholeNamespace(ModuleID(index)))
            setLayoutOfObjectRecord(record, layout, false);
        m_records.append(record);
    }

    begin(ObjectKind::ModuleBuiltin);
    ObjectRecord recordOfModuleBuiltin;
    BuiltinID builtinOfRecord;
    for (const ModuleBuiltin& entry : m_moduleBuiltins) {
        if (entry.builtin != builtinOfRecord) {
            builtinOfRecord = entry.builtin;
            recordOfModuleBuiltin = objectRecordOfBuiltin(m_builtins, ObjectKind::ModuleBuiltin, entry.builtin);
        }
        recordOfModuleBuiltin.module = entry.module;
        m_records.append(recordOfModuleBuiltin);
    }

    std::array<uint32_t, numberOfGroupsOfLayouts> nextPlaceOfLayout { };
    for (const WordsOfLayout& words : wordsOfLayouts)
        ++nextPlaceOfLayout[words.group];
    uint32_t firstPlaceOfGroup = 0;
    for (uint32_t& place : nextPlaceOfLayout)
        firstPlaceOfGroup += std::exchange(place, firstPlaceOfGroup);
    m_placesOfLayouts.reserveInitialCapacity(numberOfLayouts);
    for (const WordsOfLayout& words : wordsOfLayouts)
        m_placesOfLayouts.append(nextPlaceOfLayout[words.group]++);

    auto appendForEachLayout = [&](ObjectKind kind, OptionSet<ObjectTrait> traits, bool hasLayout) {
        begin(kind);
        uint32_t first = size();
        m_records.grow(first + numberOfLayouts);
        // The first touch of each page, as for the records of functions.
        forEachObjectRecordInParallel(&loops, numberOfLayouts, [&](uint32_t index) {
            ObjectRecord record = objectRecordOfKind(kind, index, traits);
            if (hasLayout)
                setLayoutOfObjectRecord(record, LayoutID(index), mayHaveLayoutBelow.quickGet(index));
            m_records[first + m_placesOfLayouts[index]] = record;
        });
    };
    appendForEachLayout(ObjectKind::Instance, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }, true);
    appendForEachLayout(ObjectKind::Prototype, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }, false);
    appendForEachLayout(ObjectKind::EscapedOfLayout, { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject, ObjectTrait::IsMerged }, true);

    begin(ObjectKind::Site);
    m_first[numberOfKinds] = size();
}

void ObjectTable::reserveSites(std::span<const uint32_t> numberOfSitesOfFunctions, const ParallelLoops& loops)
{
    RELEASE_ASSERT(numberOfSitesOfFunctions.size() + 1 == m_firstSiteOfFunction.size());
    RELEASE_ASSERT(size() == firstOf(ObjectKind::Site) && size() == firstObjectOfInstances());

    uint64_t next = size();
    for (size_t function = 0; function < numberOfSitesOfFunctions.size(); ++function) {
        m_firstSiteOfFunction[function] = static_cast<uint32_t>(next);
        next += numberOfSitesOfFunctions[function];
    }
    RELEASE_ASSERT(next <= largestIDPosition);
    m_firstSiteOfFunction.last() = static_cast<uint32_t>(next);

    Vector<ObjectRecord> records;
    records.grow(static_cast<size_t>(next));
    carryObjectRecordsOver(m_records.span(), records.mutableSpan(), loops);
    m_records = WTF::move(records);
    m_first[numberOfKinds] = size();
}

void ObjectTable::defineSites(FunctionID function, std::span<const ObjectSite> sortedByValueAndOrdinal)
{
    uint32_t first = m_firstSiteOfFunction[function.index()];
    RELEASE_ASSERT(sortedByValueAndOrdinal.size() == m_firstSiteOfFunction[function.index() + 1] - first);
    ASSERT(std::ranges::is_sorted(sortedByValueAndOrdinal, { }, [](const ObjectSite& site) {
        return std::pair { site.value, site.ordinal };
    }));

    std::span<ObjectRecord> records = m_records.mutableSpan().subspan(first, sortedByValueAndOrdinal.size());
    for (size_t index = 0; index < records.size(); ++index) {
        const ObjectSite& site = sortedByValueAndOrdinal[index];
        ObjectRecord record = objectRecordOfKind(ObjectKind::Site, site.value.index(), { ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject });
        record.origin = site.origin;
        record.ordinal = site.ordinal;
        record.owner = function;
        record.siteTraits = site.traits;
        if (site.layout)
            setLayoutOfObjectRecord(record, site.layout, site.mayHaveLayoutBelow);
        else if (site.builtinClass)
            makeObjectRecordInstanceOf(record, m_builtins, site.builtinClass);
        records[index] = record;
    }
}

ObjectID ObjectTable::addObjectsOfInstances(std::span<const ObjectOfInstance> withoutRepeats)
{
    uint32_t first = size();
    RELEASE_ASSERT(first + withoutRepeats.size() <= largestIDPosition);
    for (const ObjectOfInstance& wanted : withoutRepeats) {
        ObjectRecord record = at(wanted.sharedForm);
        RELEASE_ASSERT(!record.has(ObjectTrait::IsOfInstance));
        RELEASE_ASSERT(!record.has(SiteTrait::IsTemplateObject));
        record.traits.add(ObjectTrait::IsOfInstance);
        record.sharedForm = wanted.sharedForm;
        record.instance = wanted.instance;
        m_records.append(record);
    }
    return ObjectID(first);
}

ObjectID ObjectTable::objectOf(BuiltinID builtin) const
{
    return ObjectID(firstOf(ObjectKind::Builtin) + m_builtins.canonicalID(builtin).index());
}

ObjectID ObjectTable::objectOf(BuiltinID builtin, ModuleID module) const
{
    BuiltinID canonical = m_builtins.canonicalID(builtin);
    if (m_builtins.at(canonical).scope == BuiltinScope::Realm)
        return objectOf(canonical);

    ModuleBuiltin wanted { canonical, module };
    auto found = std::ranges::lower_bound(m_moduleBuiltins, wanted);
    if (found == m_moduleBuiltins.end() || *found != wanted)
        return { };
    return ObjectID(firstOf(ObjectKind::ModuleBuiltin) + static_cast<uint32_t>(found - m_moduleBuiltins.begin()));
}

ObjectID ObjectTable::resultOf(BuiltinID builtin, uint32_t ordinal) const
{
    RELEASE_ASSERT(ordinal < numberOfResultObjectsOfBuiltin);
    return ObjectID(firstOf(ObjectKind::ResultOfBuiltin) + numberOfResultObjectsOfBuiltin * m_builtins.canonicalID(builtin).index() + ordinal);
}

ObjectID ObjectTable::escapedFunctionsOf(BuiltinID holder) const
{
    return ObjectID(firstOf(ObjectKind::EscapedFunctionsOfHolder) + m_builtins.canonicalID(holder).index());
}

ObjectID ObjectTable::siteObject(FunctionID function, ValueID value, uint32_t ordinal) const
{
    std::span<const ObjectRecord> sites = sitesOf(function);
    auto keyOf = [](const ObjectRecord& site) {
        return std::pair { site.payload, site.ordinal };
    };
    std::pair wanted { value.index(), ordinal };
    auto found = std::ranges::lower_bound(sites, wanted, { }, keyOf);
    if (found == sites.end() || keyOf(*found) != wanted)
        return { };
    return ObjectID(m_firstSiteOfFunction[function.index()] + static_cast<uint32_t>(found - sites.begin()));
}

static bool isProxy(const ObjectRecord& record)
{
    switch (record.kind) {
    case ObjectKind::ResultOfBuiltin:
        if (BuiltinID(record.payload) == Builtins::Proxy)
            return !record.ordinal;
        if (BuiltinID(record.payload) == Builtins::ProxyRevocable)
            return record.ordinal == 1;
        return record.builtinClass == BuiltinClasses::Proxy;
    case ObjectKind::Site:
        return record.has(SiteTrait::IsProxyFromNamedConstructor) || record.builtinClass == BuiltinClasses::Proxy;
    case ObjectKind::EscapedOfClass:
        return BuiltinClassID(record.payload) == BuiltinClasses::Proxy;
    case ObjectKind::HostObject:
        return record.builtinClass == BuiltinClasses::Proxy;
    case ObjectKind::Unknown:
    case ObjectKind::Builtin:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::Closure:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::ModuleBuiltin:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
    case ObjectKind::EscapedOfLayout:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nothing: an escaped object of the record stays apart.
std::optional<MakerOfShapelessObject> ObjectTable::makerOfShapelessObjectThatMerges(ObjectID object) const
{
    const ObjectRecord& record = at(object);
    ASSERT(record.has(ObjectTrait::HasNoShape));
    // `f()` behind `register(f)`: a bound function is entered as a closure is. A proxy is told by what made it.
    if (record.has(ObjectTrait::IsCallable) || isProxy(record))
        return std::nullopt;

    switch (record.kind) {
    case ObjectKind::ResultOfBuiltin:
        // No row makes these. The rules that fill them name them.
        if (BuiltinID(record.payload) == m_builtins.canonicalID(Builtins::UndeclaredNativeCode))
            return std::nullopt;
        return MakerOfShapelessObject::OperationOrRow;
    case ObjectKind::HostObject: {
        // What is stored on these is not what is read, or is refused, and only the record says so.
        const BuiltinHostObject& hostObject = m_builtins.hostObjects()[record.payload];
        if (hostObject.keepsStringsOfStoredValues || !hostObject.nativeAccessors.empty() || !hostObject.lockedNames.empty() || hostObject.holder)
            return std::nullopt;
        return MakerOfShapelessObject::Host;
    }
    case ObjectKind::Site:
        if (!record.owner || record.origin == SiteOrigin::BuiltinResult)
            return MakerOfShapelessObject::Host;
        if (m_bodiesOfLibraryModules.get(record.owner.index()))
            return std::nullopt;
        return MakerOfShapelessObject::OperationOrRow;
    // What a constructor function makes is a family, and what stands above a family is found name by name.
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
        return std::nullopt;
    case ObjectKind::Unknown:
    case ObjectKind::Builtin:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::Closure:
    case ObjectKind::Namespace:
    case ObjectKind::ModuleBuiltin:
    case ObjectKind::EscapedOfLayout:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ObjectSetAliasIndex in AOTTypeRelations.cpp rests on this: what has a layout turns into escapedOf() of that layout or stays itself, and the answer for a copy is itself or the answer for its shared form.
ObjectID ObjectTable::mergedFormOf(ObjectID object) const
{
    const ObjectRecord& record = at(object);
    switch (record.kind) {
    case ObjectKind::Unknown:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::EscapedOfLayout:
    case ObjectKind::Closure:
    case ObjectKind::Namespace:
        return object;
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin: {
        if (record.has(ObjectTrait::IsFoundByIdentity))
            return object;
        BuiltinID builtin(record.payload);
        if (!record.has(ObjectTrait::IsCallable)) {
            // `module.exports`: an object holds what its own module has stored on it. An entry that is no object stands for the native code of an accessor, which is the same in every module.
            if (isObjectKind(m_builtins.at(builtin).kind))
                return object;
            ObjectID ofWhicheverModule = objectOf(builtin);
            return at(ofWhicheverModule).has(ObjectTrait::IsFoundByIdentity) ? object : ofWhicheverModule;
        }
        BuiltinID holder = m_builtins.at(builtin).holder;
        if (!holder || m_builtinFunctionsThatStayApart.quickGet(builtin.index()))
            return object;
        return escapedFunctionsOf(holder);
    }
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::HostObject:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
    case ObjectKind::Site:
        break;
    }

    ObjectID merged;
    if (record.has(ObjectTrait::HasLayout))
        merged = escapedOf(record.layout);
    else if (record.has(ObjectTrait::HasClass))
        merged = escapedOf(record.builtinClass);
    else {
        std::optional<MakerOfShapelessObject> maker = makerOfShapelessObjectThatMerges(object);
        return maker ? escapedWithoutShape(*maker) : object;
    }
    // A merged form claims no more of the layouts of its objects than each of them does.
    if (record.has(ObjectTrait::MayHaveLayoutBelow) && !at(merged).has(ObjectTrait::MayHaveLayoutBelow))
        return object;
    return merged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
