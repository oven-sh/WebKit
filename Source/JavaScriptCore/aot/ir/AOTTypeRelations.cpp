/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeRelations.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static BuiltinInstanceTest testThatCellsOfClassWouldPass(const BuiltinClass& builtinClass)
{
    bool sharesCellType = builtinClass.instanceTest == BuiltinInstanceTest::None && builtinClass.type != CellType;
    return sharesCellType ? BuiltinInstanceTest::JSTypeEquals : builtinClass.instanceTest;
}

ClassRelation relate(const BuiltinTable& builtins, BuiltinClassID first, BuiltinClassID second)
{
    const BuiltinClass& a = builtins.at(first);
    const BuiltinClass& b = builtins.at(second);
    BuiltinInstanceTest testOfFirst = testThatCellsOfClassWouldPass(a);
    BuiltinInstanceTest testOfSecond = testThatCellsOfClassWouldPass(b);
    if (testOfFirst == BuiltinInstanceTest::None || testOfSecond == BuiltinInstanceTest::None)
        return ClassRelation::Unknown;

    bool haveSameCellType = a.type == b.type;
    if (testOfFirst == BuiltinInstanceTest::JSTypeEquals && testOfSecond == BuiltinInstanceTest::JSTypeEquals)
        return haveSameCellType ? ClassRelation::Unknown : ClassRelation::Disjoint;
    if (testOfFirst == BuiltinInstanceTest::SharingModeEquals && testOfSecond == BuiltinInstanceTest::SharingModeEquals)
        return a.sharingMode == b.sharingMode ? ClassRelation::Unknown : ClassRelation::Disjoint;
    if (builtins.isSameOrSubclassOf(first, second) || builtins.isSameOrSubclassOf(second, first))
        return ClassRelation::Unknown;
    if (testOfFirst != testOfSecond && haveSameCellType)
        return ClassRelation::Unknown;
    return ClassRelation::Disjoint;
}

bool isFunctionClass(const BuiltinTable& builtins, BuiltinClassID id)
{
    if (builtins.isSameOrSubclassOf(id, BuiltinClasses::Function))
        return true;
    JSType type = builtins.at(id).type;
    return isFunctionType(type) || type == ProxyObjectType;
}

bool holdsOnlyCallables(const BuiltinTable& builtins, BuiltinClassID id)
{
    return isFunctionClass(builtins, id) && builtins.at(id).type != ProxyObjectType;
}

bool isCallableBuiltin(const BuiltinTable& builtins, BuiltinID id)
{
    switch (builtins.at(id).kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return true;
    case BuiltinKind::Prototype:
        return !!builtins.dispatchOf(id, BuiltinCallMode::Call);
    case BuiltinKind::Namespace:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isTestableBuiltin(const BuiltinTable& builtins, BuiltinID id)
{
    return BuiltinCellKey::object(id).hasCell(builtins);
}

bool hasInstanceTest(const BuiltinTable& builtins, BuiltinClassID id)
{
    const BuiltinClass& builtinClass = builtins.at(id);
    return builtinClass.instanceTest != BuiltinInstanceTest::None && !builtinClass.hasClosureInstances;
}

std::optional<uint8_t> positionInTypeOf(const BuiltinTable& builtins, BuiltinClassID builtinClass, ContentsSlot slot)
{
    BuiltinSlotID id = builtins.findSlot(builtinClass, slot);
    if (!id)
        return std::nullopt;
    return builtins.at(id).indexInType;
}

ContentsSlot defaultContentsSlotOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    return builtins.findSlot(builtinClass, ContentsSlot::Element) ? ContentsSlot::Element : ContentsSlot::Value;
}

static bool inheritsFromPrototypeOfClass(const BuiltinTable& builtins, BuiltinID object, BuiltinClassID builtinClass)
{
    BuiltinID prototype = builtins.at(builtinClass).prototype;
    if (!prototype)
        return true;
    prototype = builtins.canonicalID(prototype);
    while (true) {
        const Builtin& entry = builtins.at(object);
        switch (entry.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return true;
        case BuiltinPrototypeLink::Null:
            return false;
        case BuiltinPrototypeLink::Builtin:
            object = builtins.canonicalID(entry.inheritsFrom);
            break;
        }
        if (object == prototype)
            return true;
    }
}

static bool instanceTestRefusesPrototypeOfClass(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    const BuiltinClass& tested = builtins.at(builtinClass);
    return tested.instanceTest == BuiltinInstanceTest::JSTypeEquals && tested.type == ArrayType;
}

static bool isFilledWithValueOfLibrary(const BuiltinTable&, BuiltinID entry);

bool mayBeInstanceOf(const BuiltinTable& builtins, BuiltinID builtin, BuiltinClassID builtinClass)
{
    if (isCallableBuiltin(builtins, builtin))
        return holdsOnlyCallables(builtins, builtinClass);
    BuiltinID object = builtins.canonicalID(builtin);
    const Builtin& entry = builtins.at(object);
    switch (entry.kind) {
    case BuiltinKind::Prototype:
        if (!entry.instanceClass)
            return true;
        if (builtins.isSameOrSubclassOf(entry.instanceClass, builtinClass) && !(entry.instanceClass == builtinClass && instanceTestRefusesPrototypeOfClass(builtins, builtinClass)))
            return true;
        if (builtins.at(builtinClass).type == ProxyObjectType)
            return false;
        return inheritsFromPrototypeOfClass(builtins, object, builtinClass);
    case BuiltinKind::Namespace: {
        bool isMadeByNativeCode = entry.prototypeLink != BuiltinPrototypeLink::Unknown && !isFilledWithValueOfLibrary(builtins, object);
        if (builtins.at(builtinClass).type == ProxyObjectType && isMadeByNativeCode)
            return false;
        return inheritsFromPrototypeOfClass(builtins, object, builtinClass);
    }
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const RuntimeFunctionData* findTableRecordOfLibraryFunction(const Program& program, const BuiltinTable& builtins, FunctionID function)
{
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function == function)
            return builtins.contains(entry.id) ? &builtins.at(entry.id) : nullptr;
    }
    return nullptr;
}

bool hostUsesPutValueInto(const BuiltinTable& builtins, const BuiltinHostUses& uses, BuiltinID entry)
{
    return std::ranges::any_of(uses.list, [&](const BuiltinHostUse& use) {
        if (use.use == BuiltinHostReadUse::IsValueOf)
            return builtins.canonicalID(use.builtin) == entry;
        return use.use == BuiltinHostReadUse::Called && use.call && hostUsesPutValueInto(builtins, use.call->result, entry);
    });
}

static bool isFilledWithValueOfLibrary(const BuiltinTable& builtins, BuiltinID entry)
{
    bool isExport = std::ranges::any_of(builtins.moduleLoadsOf(entry), [&](const BuiltinModuleLoad& load) {
        return load.readsEveryExport || std::ranges::any_of(load.usesOfExports, [&](const BuiltinHostUses& uses) {
            return !uses.areListed || hostUsesPutValueInto(builtins, uses, entry);
        });
    });
    bool isMadeByHost = builtins.at(entry).origin == BuiltinOrigin::Host;
    return isExport || std::ranges::any_of(builtins.calledFunctionsOf(entry), [&](const BuiltinCalledFunction& called) {
        return (isMadeByHost && !called.uses.areListed) || hostUsesPutValueInto(builtins, called.uses, entry);
    });
}

bool mayBeClosureOf(TypeReader reader, BuiltinID builtin, FunctionID function)
{
    const Program* program = reader.program();
    if (!program)
        return true;
    if (!isLibraryCode(*program, program->at(function)))
        return false;

    const BuiltinTable& builtins = reader.builtins();
    BuiltinID canonical = builtins.canonicalID(builtin);
    const RuntimeFunctionData* recordOfFunction = findTableRecordOfLibraryFunction(*program, builtins, function);
    if (recordOfFunction && std::ranges::find(recordOfFunction->implements, canonical) != recordOfFunction->implements.end())
        return true;
    if (isFilledWithValueOfLibrary(builtins, canonical))
        return true;
    return std::ranges::any_of(builtins.overloadsOf(canonical), [&](const BuiltinOverload& overload) {
        const RuntimeFunctionEntry* entry = findRuntimeFunction(*program, overload.code.implementation.function);
        return entry && entry->function == function;
    });
}

const ClassPart* findPartHolding(TypeReader reader, std::span<const ClassPart> parts, BuiltinClassID builtinClass)
{
    auto sameClass = std::ranges::find(parts, builtinClass, &ClassPart::builtinClass);
    if (sameClass != parts.end())
        return &*sameClass;

    const BuiltinTable& builtins = reader.builtins();
    const BuiltinClass& held = builtins.at(builtinClass);
    if (held.instanceTest != BuiltinInstanceTest::JSTypeEquals)
        return nullptr;
    for (const ClassPart& part : parts) {
        const BuiltinClass& holder = builtins.at(part.builtinClass);
        bool areToldByOneTest = holder.instanceTest == BuiltinInstanceTest::JSTypeEquals && holder.type == held.type;
        if (areToldByOneTest && builtins.isSameOrSubclassOf(builtinClass, part.builtinClass))
            return &part;
    }
    return nullptr;
}

static BuiltinClassID builtinClassOfCells(const Layout& layout)
{
    if (layout.holdsOrdinaryObjects)
        return BuiltinClasses::Object;
    return builtinClassOfInstances(layout);
}

BuiltinClassID builtinClassWrittenAbove(TypeReader reader, LayoutID layout)
{
    const Program* program = reader.program();
    return program ? builtinClassOfCells(program->at(layout)) : BuiltinClassID();
}

bool mayHoldConstructedInstances(const BuiltinTable& builtins, BuiltinClassID holder, BuiltinClassID constructed)
{
    if (builtins.isSameOrSubclassOf(constructed, holder) || builtins.isSameOrSubclassOf(holder, constructed))
        return true;
    if (!hasInstanceTest(builtins, holder) || !hasInstanceTest(builtins, constructed))
        return false;
    return relate(builtins, constructed, holder) == ClassRelation::Unknown;
}

bool isInsideSomeClassPart(TypeReader reader, LayoutID layout, std::span<const ClassPart> parts)
{
    BuiltinClassID builtinClass = builtinClassWrittenAbove(reader, layout);
    if (!builtinClass || reader.program()->at(layout).mayBeOrdinaryObject)
        return false;
    return std::ranges::any_of(parts, [&](const ClassPart& part) {
        return reader.builtins().isSameOrSubclassOf(builtinClass, part.builtinClass);
    });
}

bool mayBeInsideSomeClassPart(TypeReader reader, LayoutID layout, std::span<const ClassPart> parts)
{
    if (!reader.program())
        return !parts.empty();
    const BuiltinTable& builtins = reader.builtins();
    BuiltinClassID builtinClass = builtinClassWrittenAbove(reader, layout);
    return std::ranges::any_of(parts, [&](const ClassPart& part) {
        if (testThatCellsOfClassWouldPass(builtins.at(part.builtinClass)) == BuiltinInstanceTest::None)
            return true;
        return builtinClass && mayHoldConstructedInstances(builtins, part.builtinClass, builtinClass);
    });
}

static std::span<const ClassPart> unnamedClassPartsOf(TypeReader reader, const Type& type)
{
    return type.unnamedClasses ? reader.at(type.unnamedClasses) : std::span<const ClassPart>();
}

static bool mayHaveLayoutBelowTheOneNamed(const ObjectTable& objects, const ObjectRecord& record)
{
    if (record.has(ObjectTrait::MayHaveLayoutBelow))
        return true;
    return record.kind == ObjectKind::EscapedOfLayout && objects.at(objects.instanceOf(record.layout)).has(ObjectTrait::MayHaveLayoutBelow);
}

template<typename Predicate>
static bool anyPossibleLayoutOf(const ObjectTable& objects, const Program& program, const ObjectRecord& record, const Predicate& matches)
{
    auto matchesLayoutOrTaggedMember = [&](LayoutID layout) {
        if (matches(layout))
            return true;
        return std::ranges::any_of(std::ranges::equal_range(program.taggedFamilies, layout, { }, &TaggedFamily::maker), [&](const TaggedFamily& family) {
            return std::ranges::any_of(family.members, [&](const TaggedLayout& member) {
                return matches(member.layout);
            });
        });
    };
    if (matchesLayoutOrTaggedMember(record.layout))
        return true;
    return mayHaveLayoutBelowTheOneNamed(objects, record) && std::ranges::any_of(program.layoutsExpectedBelow(record.layout), matchesLayoutOrTaggedMember);
}

template<typename Predicate>
static bool everyPossibleLayoutOf(const ObjectTable& objects, const Program& program, const ObjectRecord& record, const Predicate& holds)
{
    return !anyPossibleLayoutOf(objects, program, record, [&](LayoutID layout) {
        return !holds(layout);
    });
}

static bool mayBeCellNobodyDescribes(const ObjectRecord& record)
{
    switch (record.kind) {
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::HostObject:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
        return true;
    case ObjectKind::Unknown:
    case ObjectKind::Builtin:
    case ObjectKind::EscapedFunctionsOfHolder:
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

bool surelyFitsUnknownOf(TypeReader reader, ObjectID object, const Type& type)
{
    if (type.anyObject)
        return true;
    const ObjectRecord& record = reader.at(object);
    if (type.anyFunction && record.has(ObjectTrait::IsCallable))
        return true;

    std::span<const ClassPart> parts = unnamedClassPartsOf(reader, type);
    if (record.has(ObjectTrait::HasClass))
        return !!findPartHolding(reader, parts, record.builtinClass);
    if (!record.has(ObjectTrait::HasLayout) || (!type.unnamedLayouts && parts.empty()))
        return false;

    const Program* program = reader.program();
    if (!program)
        return false;
    SetView<LayoutID, LayoutSetID> listed;
    if (type.unnamedLayouts)
        listed = reader.at(type.unnamedLayouts);
    return everyPossibleLayoutOf(reader.objects(), *program, record, [&](LayoutID layout) {
        bool isListed = type.unnamedLayouts && listed.contains(layout);
        return (isListed && !program->at(layout).mayBeOrdinaryObject) || isInsideSomeClassPart(reader, layout, parts);
    });
}

static bool mayBeCallableObject(const BuiltinTable& builtins, const ObjectRecord& record)
{
    if (record.has(ObjectTrait::IsCallable) || record.has(ObjectTrait::IsBuiltin))
        return true;
    if (record.has(ObjectTrait::HasClass))
        return isFunctionClass(builtins, record.builtinClass);
    return record.has(ObjectTrait::HasNoShape) && mayBeCellNobodyDescribes(record);
}

static bool mayStandBelowAnyConstructor(const Program& program, LayoutID layout)
{
    for (size_t steps = 0; layout && steps <= program.layouts.size(); ++steps) {
        const Layout& current = program.at(layout);
        if (!current.isDerived() && !current.has(LayoutFlags::HasHeritage))
            return false;
        layout = writtenBaseOf(current);
    }
    return true;
}

static bool mayBeMadeWithLayout(const BuiltinTable& builtins, const Program& program, const ObjectRecord& made, LayoutID layout)
{
    const Layout& candidate = program.at(layout);
    BuiltinClassID classOfCells = builtinClassOfCells(candidate);
    if (made.has(ObjectTrait::HasClass))
        return classOfCells && mayHoldConstructedInstances(builtins, made.builtinClass, classOfCells);
    return candidate.kind == LayoutKind::BuiltinResult || classOfCells || mayStandBelowAnyConstructor(program, layout);
}

static bool mayHaveLayoutAmong(TypeReader reader, const ObjectRecord& record, const SetView<LayoutID, LayoutSetID>& listed, LayoutsFoundForCellsNobodyDescribes& found)
{
    const Program* program = reader.program();
    if (record.has(ObjectTrait::HasLayout)) {
        if (listed.contains(record.layout))
            return true;
        if (!program)
            return record.has(ObjectTrait::MayHaveLayoutBelow);
        return anyPossibleLayoutOf(reader.objects(), *program, record, [&](LayoutID layout) {
            return listed.contains(layout);
        });
    }
    if (!mayBeCellNobodyDescribes(record))
        return false;
    if (!program)
        return true;
    BuiltinClassID builtinClass = record.has(ObjectTrait::HasClass) ? record.builtinClass : BuiltinClassID();
    for (const auto& known : found.byClass) {
        if (known.builtinClass == builtinClass)
            return known.mayHaveListedLayout;
    }
    bool mayHaveListedLayout = listed.anyOf([&](LayoutID layout) {
        return mayBeMadeWithLayout(reader.builtins(), *program, record, layout);
    });
    found.byClass.append({ builtinClass, mayHaveListedLayout });
    return mayHaveListedLayout;
}

static bool mayBeOfClass(const BuiltinTable& builtins, const ObjectRecord& record, BuiltinClassID builtinClass)
{
    if (record.has(ObjectTrait::IsCallable) && !isFunctionClass(builtins, builtinClass) && builtins.at(builtinClass).instanceTest == BuiltinInstanceTest::JSTypeEquals)
        return false;
    if (record.has(ObjectTrait::HasClass))
        return relate(builtins, record.builtinClass, builtinClass) == ClassRelation::Unknown;

    switch (record.kind) {
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin:
        return mayBeInstanceOf(builtins, BuiltinID(record.payload), builtinClass);
    case ObjectKind::Closure:
        return holdsOnlyCallables(builtins, builtinClass) && !builtins.at(builtinClass).hasClosureInstances;
    case ObjectKind::EscapedFunctionsOfHolder:
        return holdsOnlyCallables(builtins, builtinClass);
    case ObjectKind::Unknown:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::HostObject:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
        return true;
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
    case ObjectKind::EscapedOfLayout:
        return !hasInstanceTest(builtins, builtinClass) || (record.has(ObjectTrait::IsCallable) && isFunctionClass(builtins, builtinClass));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayFitSomeClassPart(TypeReader reader, const ObjectRecord& record, std::span<const ClassPart> parts)
{
    if (!record.has(ObjectTrait::HasLayout)) {
        return std::ranges::any_of(parts, [&](const ClassPart& part) {
            return mayBeOfClass(reader.builtins(), record, part.builtinClass);
        });
    }
    const Program* program = reader.program();
    if (!program)
        return !parts.empty();
    return anyPossibleLayoutOf(reader.objects(), *program, record, [&](LayoutID layout) {
        return mayBeInsideSomeClassPart(reader, layout, parts);
    });
}

bool mayFitUnknownOf(TypeReader reader, ObjectID object, const Type& type)
{
    LayoutsFoundForCellsNobodyDescribes found;
    return mayFitUnknownOf(reader, object, type, found);
}

bool mayFitUnknownOf(TypeReader reader, ObjectID object, const Type& type, LayoutsFoundForCellsNobodyDescribes& found)
{
    if (type.anyObject)
        return true;
    const ObjectRecord& record = reader.at(object);
    if (record.kind == ObjectKind::Unknown)
        return type.hasUnknown();
    if (type.anyFunction && mayBeCallableObject(reader.builtins(), record))
        return true;
    if (type.unnamedLayouts) {
        RELEASE_ASSERT(!found.listed || found.listed == type.unnamedLayouts);
        found.listed = type.unnamedLayouts;
        if (mayHaveLayoutAmong(reader, record, reader.at(type.unnamedLayouts), found))
            return true;
    }
    return type.unnamedClasses && mayFitSomeClassPart(reader, record, reader.at(type.unnamedClasses));
}

static bool distinctIDsOfKindAreDistinctObjects(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Builtin:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::HostObject:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::Closure:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::ModuleBuiltin:
        return true;
    case ObjectKind::Unknown:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
    case ObjectKind::EscapedOfLayout:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayBeBuiltinUnderAnotherName(TypeReader reader, const ObjectRecord& entry, const ObjectRecord& other)
{
    switch (other.kind) {
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin:
        return entry.payload == other.payload;
    case ObjectKind::Closure:
        return mayBeClosureOf(reader, BuiltinID(entry.payload), FunctionID(other.payload));
    case ObjectKind::HostObject: {
        const BuiltinTable& builtins = reader.builtins();
        BuiltinID holder = builtins.hostObjects()[other.payload].holder;
        return holder && builtins.canonicalID(holder) == BuiltinID(entry.payload);
    }
    case ObjectKind::Unknown:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::FunctionPrototype:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::Instance:
    case ObjectKind::Prototype:
    case ObjectKind::EscapedOfLayout:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayBeEscapedFunctionOfLibrary(TypeReader reader, FunctionID function)
{
    const Program* program = reader.program();
    return !program || isLibraryCode(*program, program->at(function));
}

static bool mayBePrototypeUnderAnotherName(TypeReader reader, const ObjectRecord& prototype, const ObjectRecord& other)
{
    if (other.kind != ObjectKind::Prototype && other.kind != ObjectKind::FunctionPrototype)
        return false;
    const Program* program = reader.program();
    if (!program)
        return true;
    LayoutID declared = declaredLayoutOf(*program, LayoutID(prototype.payload));
    if (other.kind == ObjectKind::Prototype)
        return declared == declaredLayoutOf(*program, LayoutID(other.payload));
    FunctionID function(other.payload);
    LayoutID constructed = program->at(function).constructedClass;
    return program->at(declared).constructor == function || (constructed && declaredLayoutOf(*program, constructed) == declared);
}

static bool mayHaveOneLayout(TypeReader reader, const ObjectRecord& a, const ObjectRecord& b)
{
    if (a.layout == b.layout)
        return true;
    const Program* program = reader.program();
    if (!program)
        return a.has(ObjectTrait::MayHaveLayoutBelow) || b.has(ObjectTrait::MayHaveLayoutBelow);
    const ObjectTable& objects = reader.objects();
    return anyPossibleLayoutOf(objects, *program, a, [&](LayoutID ofFirst) {
        return anyPossibleLayoutOf(objects, *program, b, [&](LayoutID ofSecond) {
            return ofFirst == ofSecond;
        });
    });
}

static bool mayBeInstanceWithLayout(TypeReader reader, const ObjectRecord& made, const ObjectRecord& instance)
{
    const Program* program = reader.program();
    if (!program)
        return true;
    return anyPossibleLayoutOf(reader.objects(), *program, instance, [&](LayoutID layout) {
        return mayBeMadeWithLayout(reader.builtins(), *program, made, layout);
    });
}

static bool mayBeOneObjectByShape(TypeReader reader, const ObjectRecord& a, const ObjectRecord& b)
{
    bool firstHasLayout = a.has(ObjectTrait::HasLayout);
    bool secondHasLayout = b.has(ObjectTrait::HasLayout);
    if (firstHasLayout && secondHasLayout)
        return mayHaveOneLayout(reader, a, b);
    if (firstHasLayout)
        return mayBeCellNobodyDescribes(b) && mayBeInstanceWithLayout(reader, b, a);
    if (secondHasLayout)
        return mayBeCellNobodyDescribes(a) && mayBeInstanceWithLayout(reader, a, b);
    if (!mayBeCellNobodyDescribes(a) || !mayBeCellNobodyDescribes(b))
        return false;
    if (!a.has(ObjectTrait::HasClass) || !b.has(ObjectTrait::HasClass))
        return true;
    return relate(reader.builtins(), a.builtinClass, b.builtinClass) == ClassRelation::Unknown;
}

static bool namesEntryOfBuiltinTable(ObjectKind kind)
{
    return kind == ObjectKind::Builtin || kind == ObjectKind::ModuleBuiltin;
}

bool mayBeSameObject(TypeReader reader, ObjectID first, ObjectID second)
{
    if (first == second || first == ObjectTable::unknown() || second == ObjectTable::unknown())
        return true;

    const ObjectTable& objects = reader.objects();
    const ObjectRecord& a = objects.at(first);
    const ObjectRecord& b = objects.at(second);
    if (a.kind == b.kind && distinctIDsOfKindAreDistinctObjects(a.kind))
        return false;
    if ((b.has(ObjectTrait::IsMerged) && objects.mergedFormOf(first) == second) || (a.has(ObjectTrait::IsMerged) && objects.mergedFormOf(second) == first))
        return true;

    if (namesEntryOfBuiltinTable(a.kind))
        return mayBeBuiltinUnderAnotherName(reader, a, b);
    if (namesEntryOfBuiltinTable(b.kind))
        return mayBeBuiltinUnderAnotherName(reader, b, a);
    if (a.kind == ObjectKind::Closure)
        return b.kind == ObjectKind::EscapedFunctionsOfHolder && mayBeEscapedFunctionOfLibrary(reader, FunctionID(a.payload));
    if (b.kind == ObjectKind::Closure)
        return a.kind == ObjectKind::EscapedFunctionsOfHolder && mayBeEscapedFunctionOfLibrary(reader, FunctionID(b.payload));
    if (a.kind == ObjectKind::Prototype)
        return mayBePrototypeUnderAnotherName(reader, a, b);
    if (b.kind == ObjectKind::Prototype)
        return mayBePrototypeUnderAnotherName(reader, b, a);
    return mayBeOneObjectByShape(reader, a, b);
}

static constexpr ObjectKinds kindsThatMayBeClosure = kindsWith(ObjectKind::Builtin) | kindsWith(ObjectKind::ModuleBuiltin) | kindsWith(ObjectKind::EscapedFunctionsOfHolder);
static constexpr ObjectKinds kindsNeverComparedByShape = kindsWith(ObjectKind::Builtin) | kindsWith(ObjectKind::ModuleBuiltin) | kindsWith(ObjectKind::Closure) | kindsWith(ObjectKind::Prototype);

static bool isFoundBesideMergedFormByShape(const ObjectRecord& record)
{
    return record.has(ObjectTrait::HasLayout) && !(kindsWith(record.kind) & kindsNeverComparedByShape);
}

ObjectSetAliasIndex::ObjectSetAliasIndex(TypeReader reader, const SetView<ObjectID, ObjectSetID>& members)
    : m_reader(reader)
    , m_members(members)
{
}

void ObjectSetAliasIndex::lookAtMembersButClosures()
{
    if (std::exchange(m_hasLookedAtMembers, true))
        return;
    const ObjectTable& objects = m_reader.objects();
    auto lookAt = [&](ObjectID member) {
        lookAtMember(member);
        return false;
    };
    m_members.anyOfWithin(0, objects.firstOf(ObjectKind::Closure), lookAt);
    m_members.anyOfWithin(objects.endOf(ObjectKind::Closure), objects.size(), lookAt);
}

void ObjectSetAliasIndex::lookAtMember(ObjectID member)
{
    const ObjectTable& objects = m_reader.objects();
    const ObjectRecord& record = objects.at(member);
    m_kinds |= kindsWith(record.kind);
    if (member == ObjectTable::unknown()) {
        m_namesUnknown = true;
        return;
    }
    if (!isFoundBesideMergedFormByShape(record)) {
        ObjectID merged = objects.mergedFormOf(member);
        if (merged != member)
            m_mergedForms.at(merged.index());
    } else
        ASSERT(objects.mergedFormOf(member) == member || objects.mergedFormOf(member) == objects.escapedOf(record.layout));

    switch (record.kind) {
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin:
        ASSERT(m_reader.builtins().canonicalID(BuiltinID(record.payload)).index() == record.payload);
        m_builtins.append(member);
        m_kindsOfBuiltins.at(record.payload) |= kindsWith(record.kind);
        return;
    case ObjectKind::Closure:
        RELEASE_ASSERT_NOT_REACHED();
    case ObjectKind::Prototype:
        notePrototype(member, record);
        return;
    case ObjectKind::HostObject:
        if (BuiltinID holder = m_reader.builtins().hostObjects()[record.payload].holder)
            m_holdersOfHostObjects.at(m_reader.builtins().canonicalID(holder).index());
        break;
    case ObjectKind::FunctionPrototype:
        noteFunctionPrototype(record);
        break;
    case ObjectKind::Unknown:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::EscapedFunctionsOfHolder:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::Instance:
    case ObjectKind::EscapedOfLayout:
        break;
    }
    noteShapeOf(record);
}

void ObjectSetAliasIndex::noteShapeOf(const ObjectRecord& record)
{
    ObjectKinds kind = kindsWith(record.kind);
    if (record.has(ObjectTrait::HasLayout)) {
        const ObjectTable& objects = m_reader.objects();
        ObjectKinds& spread = m_layouts.at(record.layout.index()).kindsSpread[mayHaveLayoutBelowTheOneNamed(objects, record)];
        if (spread & kind)
            return;
        spread |= kind;
        anyPossibleLayoutOf(objects, *m_reader.program(), record, [&](LayoutID layout) {
            noteLayout(layout, record);
            return false;
        });
        return;
    }
    if (!mayBeCellNobodyDescribes(record))
        return;
    bool hasClass = record.has(ObjectTrait::HasClass);
    for (CellsNobodyDescribes& cells : m_cellsNobodyDescribes) {
        if (cells.example->has(ObjectTrait::HasClass) == hasClass && (!hasClass || cells.example->builtinClass == record.builtinClass)) {
            cells.kinds |= kind;
            return;
        }
    }
    m_cellsNobodyDescribes.append(CellsNobodyDescribes { &record, kind });
}

void ObjectSetAliasIndex::noteLayout(LayoutID layout, const ObjectRecord& ofMember)
{
    ObjectKinds kind = kindsWith(ofMember.kind);
    LayoutOfMembers& known = m_layouts.at(layout.index());
    if (known.kinds & kind)
        return;
    known.kinds |= kind;

    const Program& program = *m_reader.program();
    if (!(m_kindsWithLayoutOfCellWithoutClass & kind) && mayBeMadeWithLayout(m_reader.builtins(), program, ObjectRecord { }, layout))
        m_kindsWithLayoutOfCellWithoutClass |= kind;
    BuiltinClassID classOfCells = builtinClassOfCells(program.at(layout));
    if (!classOfCells)
        return;
    for (ClassOfCells& known : m_classesOfCellsOfLayouts) {
        if (known.builtinClass == classOfCells) {
            known.kinds |= kind;
            return;
        }
    }
    m_classesOfCellsOfLayouts.append(ClassOfCells { classOfCells, kind });
}

void ObjectSetAliasIndex::notePrototype(ObjectID member, const ObjectRecord& record)
{
    const Program& program = *m_reader.program();
    LayoutID declared = declaredLayoutOf(program, LayoutID(record.payload));
    if (!declared) {
        m_askedOneByOne.append(member);
        return;
    }
    m_layouts.at(declared.index()).isOfPrototype = true;
    if (FunctionID constructor = program.at(declared).constructor)
        m_constructorsOfPrototypes.at(constructor.index());
}

void ObjectSetAliasIndex::noteFunctionPrototype(const ObjectRecord& record)
{
    const Program& program = *m_reader.program();
    m_functionsOfFunctionPrototypes.at(record.payload);
    LayoutID constructed = program.at(FunctionID(record.payload)).constructedClass;
    if (!constructed)
        return;
    if (LayoutID declared = declaredLayoutOf(program, constructed))
        m_layouts.at(declared.index()).isOfPrototypeOfConstructor = true;
}

bool ObjectSetAliasIndex::namesPrototypeUnderAnotherName(const ObjectRecord& prototype)
{
    const Program& program = *m_reader.program();
    LayoutID declared = declaredLayoutOf(program, LayoutID(prototype.payload));
    if (!declared)
        return false;
    const LayoutOfMembers* ofMembers = m_layouts.find(declared.index());
    if (ofMembers && (ofMembers->isOfPrototype || ofMembers->isOfPrototypeOfConstructor))
        return true;
    FunctionID constructor = program.at(declared).constructor;
    return constructor && m_functionsOfFunctionPrototypes.find(constructor.index());
}

bool ObjectSetAliasIndex::namesPrototypeOfClassOf(FunctionID constructor)
{
    if (m_constructorsOfPrototypes.find(constructor.index()))
        return true;
    const Program& program = *m_reader.program();
    LayoutID constructed = program.at(constructor).constructedClass;
    if (!constructed)
        return false;
    LayoutID declared = declaredLayoutOf(program, constructed);
    if (!declared)
        return false;
    const LayoutOfMembers* ofMembers = m_layouts.find(declared.index());
    return ofMembers && ofMembers->isOfPrototype;
}

bool ObjectSetAliasIndex::namesBuiltinThatMayBeClosureOf(FunctionID ofLibrary)
{
    if (m_builtins.isEmpty())
        return false;
    const Program& program = *m_reader.program();
    const BuiltinTable& builtins = m_reader.builtins();
    if (!std::exchange(m_hasReadBuiltinsForClosures, true)) {
        for (ObjectID member : m_builtins) {
            BuiltinID canonical = builtins.canonicalID(BuiltinID(m_reader.at(member).payload));
            m_namesBuiltinFilledWithValueOfLibrary |= isFilledWithValueOfLibrary(builtins, canonical);
            for (const BuiltinOverload& overload : builtins.overloadsOf(canonical)) {
                const RuntimeFunctionEntry* entry = findRuntimeFunction(program, overload.code.implementation.function);
                if (entry && entry->function)
                    m_functionsThatMayBeBuiltins.at(entry->function.index());
            }
        }
        SlotsByNumber<NoValue> functionsWithRecord;
        for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
            if (!entry.function || functionsWithRecord.find(entry.function.index()))
                continue;
            functionsWithRecord.at(entry.function.index());
            if (!builtins.contains(entry.id))
                continue;
            bool implementsMember = std::ranges::any_of(builtins.at(entry.id).implements, [&](BuiltinID implemented) {
                return m_kindsOfBuiltins.find(implemented.index());
            });
            if (implementsMember)
                m_functionsThatMayBeBuiltins.at(entry.function.index());
        }
    }
    return m_namesBuiltinFilledWithValueOfLibrary || m_functionsThatMayBeBuiltins.find(ofLibrary.index());
}

bool ObjectSetAliasIndex::namesClosureThatMayBe(BuiltinID builtin)
{
    std::span<const ObjectID> closures = closuresOfLibrary();
    if (closures.empty())
        return false;
    const Program& program = *m_reader.program();
    const BuiltinTable& builtins = m_reader.builtins();
    if (!std::exchange(m_hasReadClosuresForBuiltins, true)) {
        for (ObjectID closure : closures)
            m_functionsOfClosuresOfLibrary.at(m_reader.at(closure).payload);
        for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
            if (!entry.function)
                continue;
            const bool* hasFoundRecord = m_functionsOfClosuresOfLibrary.find(entry.function.index());
            if (!hasFoundRecord || *hasFoundRecord)
                continue;
            m_functionsOfClosuresOfLibrary.at(entry.function.index()) = true;
            if (!builtins.contains(entry.id))
                continue;
            for (BuiltinID implemented : builtins.at(entry.id).implements)
                m_builtinsThatClosuresImplement.at(implemented.index());
        }
    }
    BuiltinID canonical = builtins.canonicalID(builtin);
    if (m_builtinsThatClosuresImplement.find(canonical.index()) || isFilledWithValueOfLibrary(builtins, canonical))
        return true;
    return std::ranges::any_of(builtins.overloadsOf(canonical), [&](const BuiltinOverload& overload) {
        const RuntimeFunctionEntry* entry = findRuntimeFunction(program, overload.code.implementation.function);
        return entry && entry->function && m_functionsOfClosuresOfLibrary.find(entry->function.index());
    });
}

std::span<const ObjectID> ObjectSetAliasIndex::closuresOfLibrary()
{
    if (!std::exchange(m_hasListedClosuresOfLibrary, true)) {
        const ObjectTable& objects = m_reader.objects();
        auto listIfOfLibrary = [&](ObjectID closure) {
            if (mayBeEscapedFunctionOfLibrary(m_reader, FunctionID(objects.at(closure).payload)))
                m_closuresOfLibrary.append(closure);
            return false;
        };
        m_members.anyOfWithin(objects.firstOf(ObjectKind::Closure), objects.endOf(ObjectKind::Closure), listIfOfLibrary);
    }
    return m_closuresOfLibrary.span();
}

bool ObjectSetAliasIndex::namesObjectOfShape(ObjectID object, const ObjectRecord& record)
{
    const BuiltinTable& builtins = m_reader.builtins();
    ObjectKinds kind = kindsWith(record.kind);
    ObjectKinds compared = distinctIDsOfKindAreDistinctObjects(record.kind) ? ~kind : ~ObjectKinds(0);
    auto areCompared = [&](const CellsNobodyDescribes& cells) {
        return cells.kinds & compared;
    };

    if (record.has(ObjectTrait::HasLayout)) {
        const ObjectTable& objects = m_reader.objects();
        const Program& program = *m_reader.program();
        bool mayLieBelow = mayHaveLayoutBelowTheOneNamed(objects, record);
        ASSERT_UNUSED(object, distinctIDsOfKindAreDistinctObjects(record.kind) || object == objects.instanceOf(record.layout) || object == objects.escapedOf(record.layout));
        const LayoutOfMembers& known = m_layouts.at(record.layout.index());
        if (known.kindsAsked[mayLieBelow] & kind)
            return known.kindsWithAlias[mayLieBelow] & kind;

        bool hasAlias = anyPossibleLayoutOf(objects, program, record, [&](LayoutID layout) {
            const LayoutOfMembers* ofMembers = m_layouts.find(layout.index());
            if (ofMembers && ofMembers->kinds & compared)
                return true;
            return std::ranges::any_of(m_cellsNobodyDescribes, [&](const CellsNobodyDescribes& cells) {
                return areCompared(cells) && mayBeMadeWithLayout(builtins, program, *cells.example, layout);
            });
        });
        LayoutOfMembers& asked = m_layouts.at(record.layout.index());
        asked.kindsAsked[mayLieBelow] |= kind;
        if (hasAlias)
            asked.kindsWithAlias[mayLieBelow] |= kind;
        return hasAlias;
    }

    if (!mayBeCellNobodyDescribes(record))
        return false;
    if (!record.has(ObjectTrait::HasClass))
        return m_kindsWithLayoutOfCellWithoutClass & compared || std::ranges::any_of(m_cellsNobodyDescribes, areCompared);
    bool mayHaveLayoutOfMember = std::ranges::any_of(m_classesOfCellsOfLayouts, [&](const ClassOfCells& ofLayouts) {
        return ofLayouts.kinds & compared && mayHoldConstructedInstances(builtins, record.builtinClass, ofLayouts.builtinClass);
    });
    return mayHaveLayoutOfMember || std::ranges::any_of(m_cellsNobodyDescribes, [&](const CellsNobodyDescribes& cells) {
        return areCompared(cells) && (!cells.example->has(ObjectTrait::HasClass) || relate(builtins, record.builtinClass, cells.example->builtinClass) == ClassRelation::Unknown);
    });
}

bool ObjectSetAliasIndex::namesAliasOf(ObjectID object)
{
    bool answer = answerFromWhatWasRead(object);
#if ASSERT_ENABLED || ASAN_ENABLED
    static thread_local size_t numberOfAnswersSinceCheck = 0;
    if (++numberOfAnswersSinceCheck >= 16 + m_members.size()) {
        numberOfAnswersSinceCheck = 0;
        RELEASE_ASSERT(answer == m_members.anyOf([&](ObjectID member) {
            return mayBeSameObject(m_reader, object, member);
        }));
    }
#endif
    return answer;
}

bool ObjectSetAliasIndex::answerFromWhatWasRead(ObjectID object)
{
    const ObjectTable& objects = m_reader.objects();
    auto mayBeThisObject = [&](ObjectID member) {
        ++m_numberOfPairsAsked;
        return mayBeSameObject(m_reader, object, member);
    };
    if (!m_reader.program() || object == ObjectTable::unknown())
        return m_members.anyOf(mayBeThisObject);

    lookAtMembersButClosures();
    if (m_namesUnknown)
        return true;
    if (object.index() >= objects.firstOf(ObjectKind::Closure) && object.index() < objects.endOf(ObjectKind::Closure) && !(m_kinds & kindsThatMayBeClosure))
        return false;

    const ObjectRecord& record = objects.at(object);
    if (!isFoundBesideMergedFormByShape(record)) {
        ObjectID merged = objects.mergedFormOf(object);
        if (merged != object && m_members.contains(merged) && objects.at(merged).has(ObjectTrait::IsMerged))
            return true;
    } else
        ASSERT(objects.mergedFormOf(object) == object || objects.mergedFormOf(object) == objects.escapedOf(record.layout));
    if (std::ranges::any_of(m_askedOneByOne, mayBeThisObject))
        return true;
    if (record.has(ObjectTrait::IsMerged) && m_mergedForms.find(object.index()))
        return true;

    switch (record.kind) {
    case ObjectKind::Builtin:
    case ObjectKind::ModuleBuiltin: {
        const ObjectKinds* kindsOfSameEntry = m_kindsOfBuiltins.find(record.payload);
        if (kindsOfSameEntry && *kindsOfSameEntry & ~kindsWith(record.kind))
            return true;
        return m_holdersOfHostObjects.find(record.payload) || namesClosureThatMayBe(BuiltinID(record.payload));
    }
    case ObjectKind::Closure:
        if (!mayBeEscapedFunctionOfLibrary(m_reader, FunctionID(record.payload)))
            return false;
        return m_kinds & kindsWith(ObjectKind::EscapedFunctionsOfHolder) || namesBuiltinThatMayBeClosureOf(FunctionID(record.payload));
    case ObjectKind::Prototype:
        return namesPrototypeUnderAnotherName(record);
    case ObjectKind::HostObject: {
        BuiltinID holder = m_reader.builtins().hostObjects()[record.payload].holder;
        if (holder && m_kindsOfBuiltins.find(m_reader.builtins().canonicalID(holder).index()))
            return true;
        break;
    }
    case ObjectKind::EscapedFunctionsOfHolder:
        if (!closuresOfLibrary().empty())
            return true;
        break;
    case ObjectKind::FunctionPrototype:
        if (namesPrototypeOfClassOf(FunctionID(record.payload)))
            return true;
        break;
    case ObjectKind::Unknown:
    case ObjectKind::ResultOfBuiltin:
    case ObjectKind::EscapedOfClass:
    case ObjectKind::EscapedWithoutShape:
    case ObjectKind::CoroutineResult:
    case ObjectKind::RestArray:
    case ObjectKind::ConstructedObject:
    case ObjectKind::Namespace:
    case ObjectKind::Instance:
    case ObjectKind::EscapedOfLayout:
        break;
    }
    return namesObjectOfShape(object, record);
}

bool mayHaveLayoutOfRecord(const Program& program, const ObjectRecord& record)
{
    if (record.layout)
        return program.at(record.layout).kind == LayoutKind::Record;
    bool isMadeByNativeCode = record.kind == ObjectKind::ResultOfBuiltin || record.kind == ObjectKind::EscapedWithoutShape;
    return isMadeByNativeCode && record.has(ObjectTrait::HasNoShape);
}

ObjectID ordinaryObjectMadeByCopy(TypeReader reader, const Program& program, const Type& typeOfCopy)
{
    if (!typeOfCopy.objects)
        return { };
    ObjectID made;
    bool namesSeveral = false;
    reader.at(typeOfCopy.objects).forEach([&](ObjectID object) {
        if (mayHaveLayoutOfRecord(program, reader.at(object)))
            return;
        namesSeveral |= !!made;
        made = object;
    });
    return namesSeveral ? ObjectID() : made;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
