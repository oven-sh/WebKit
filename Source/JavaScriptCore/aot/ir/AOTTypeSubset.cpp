/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeRelations.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

} // anonymous namespace

ClassRelation relate(const BuiltinTable& builtins, BuiltinClassID first, BuiltinClassID second)
{
    const BuiltinClass& a = builtins.at(first);
    const BuiltinClass& b = builtins.at(second);
    if (a.instanceTest == BuiltinInstanceTest::None || b.instanceTest == BuiltinInstanceTest::None)
        return ClassRelation::Unknown;

    bool haveSameCellType = a.type == b.type;
    if (a.instanceTest == BuiltinInstanceTest::JSTypeEquals && b.instanceTest == BuiltinInstanceTest::JSTypeEquals)
        return haveSameCellType ? ClassRelation::Unknown : ClassRelation::Disjoint;
    if (a.instanceTest == BuiltinInstanceTest::SharingModeEquals && b.instanceTest == BuiltinInstanceTest::SharingModeEquals)
        return a.sharingMode == b.sharingMode ? ClassRelation::Unknown : ClassRelation::Disjoint;
    if (builtins.isSameOrSubclassOf(first, second) || builtins.isSameOrSubclassOf(second, first))
        return ClassRelation::Unknown;
    if (a.instanceTest != b.instanceTest && haveSameCellType)
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

bool hasCallableParts(const Type& type)
{
    return type.anyFunction || !type.functions.isEmpty();
}

bool includesFunction(const Type& type, FunctionID function)
{
    return type.anyFunction || type.functions.contains(function);
}

bool includesBuiltin(const Type& type, BuiltinID builtin, const BuiltinTable& builtins)
{
    return type.builtins.contains(builtin) || (type.anyFunction && isCallableBuiltin(builtins, builtin));
}

bool mayContainAnyCallable(const BuiltinTable& builtins, const Type& type)
{
    return type.anyFunction || std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return isFunctionClass(builtins, part.builtinClass);
    });
}

bool mayContainAnyBuiltin(const Type& type)
{
    return type.anyFunction || !type.classes.isEmpty();
}

const RuntimeFunctionData* findTableRecordOf(const Program& program, FunctionID function)
{
    const BuiltinTable& builtins = program.builtins();
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function == function)
            return builtins.contains(entry.id) ? &builtins.at(entry.id) : nullptr;
    }
    return nullptr;
}

static bool putsValueInto(const BuiltinTable& builtins, const BuiltinHostUses& uses, BuiltinID entry)
{
    return std::ranges::any_of(uses.list, [&](const BuiltinHostUse& use) {
        if (use.use == BuiltinHostReadUse::IsValueOf)
            return builtins.canonicalID(use.builtin) == entry;
        return use.use == BuiltinHostReadUse::Called && use.call && putsValueInto(builtins, use.call->result, entry);
    });
}

static bool holdsValueFromLibrary(const BuiltinTable& builtins, BuiltinID entry)
{
    bool isExport = std::ranges::any_of(builtins.moduleLoadsOf(entry), [&](const BuiltinModuleLoad& load) {
        return std::ranges::any_of(load.usesOfExports, [&](const BuiltinHostUses& uses) {
            return putsValueInto(builtins, uses, entry);
        });
    });
    return isExport || std::ranges::any_of(builtins.calledFunctionsOf(entry), [&](const BuiltinCalledFunction& called) {
        return putsValueInto(builtins, called.uses, entry);
    });
}

bool mayBeClosureOf(const TypeTable& types, BuiltinID builtin, FunctionID function, const RuntimeFunctionData* recordOfFunction)
{
    const Program* program = types.program();
    if (!program)
        return true;
    if (!isLibraryCode(*program, program->at(function)))
        return false;

    const BuiltinTable& builtins = types.builtins();
    BuiltinID canonical = builtins.canonicalID(builtin);
    if (recordOfFunction && std::ranges::find(recordOfFunction->implements, canonical) != recordOfFunction->implements.end())
        return true;
    if (holdsValueFromLibrary(builtins, canonical))
        return true;
    return std::ranges::any_of(builtins.overloadsOf(canonical), [&](const BuiltinOverload& overload) {
        const RuntimeFunctionEntry* entry = findRuntimeFunction(*program, overload.code.implementation.function);
        return entry && entry->function == function;
    });
}

bool mayBeClosureOf(const TypeTable& types, BuiltinID builtin, FunctionID function)
{
    const Program* program = types.program();
    return mayBeClosureOf(types, builtin, function, program ? findTableRecordOf(*program, function) : nullptr);
}

LibraryClosuresOfBuiltin libraryClosuresOf(const TypeTable& types, BuiltinID builtin)
{
    LibraryClosuresOfBuiltin closures;
    const Program* program = types.program();
    if (!program) {
        closures.mayBeAnyFunctionOfLibrary = true;
        return closures;
    }

    const BuiltinTable& builtins = types.builtins();
    BuiltinID canonical = builtins.canonicalID(builtin);
    closures.mayBeAnyFunctionOfLibrary = holdsValueFromLibrary(builtins, canonical);
    for (const BuiltinOverload& overload : builtins.overloadsOf(canonical)) {
        if (const RuntimeFunctionEntry* entry = findRuntimeFunction(*program, overload.code.implementation.function))
            closures.functions.appendIfNotContains(entry->function);
    }
    return closures;
}

bool mayContainBuiltin(const TypeTable& types, const Type& type, BuiltinID builtin)
{
    if (mayContainAnyBuiltin(type) || type.builtins.contains(builtin))
        return true;
    return std::ranges::any_of(type.functions, [&](FunctionID function) {
        return mayBeClosureOf(types, builtin, function);
    });
}

bool overlapsAnotherClass(const BuiltinTable& builtins, BuiltinClassID builtinClass, const Type& type)
{
    return std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return part.builtinClass != builtinClass && relate(builtins, builtinClass, part.builtinClass) == ClassRelation::Unknown;
    });
}

bool isPartSubset(const ClassPart& a, const ClassPart& b, const TypeTable& types)
{
    if (!a.provenPristine.containsAll(b.provenPristine))
        return false;
    if (!b.contentsAreProven)
        return true;
    if (!a.contentsAreProven)
        return false;
    if (b.arrayStorage != ArrayStorage::Other && a.arrayStorage != b.arrayStorage)
        return false;
    for (unsigned index = 0; index < b.writtenContents.size(); ++index) {
        if (!isSubset(types.at(a.writtenContentsAt(index)), types.at(b.writtenContentsAt(index)), types))
            return false;
    }
    return true;
}

static bool areCallablesSubset(const Type& a, const Type& b, const BuiltinTable& builtins)
{
    if (a.anyFunction && !b.anyFunction)
        return false;
    bool includesFunctions = std::ranges::all_of(a.functions, [&](FunctionID function) {
        return includesFunction(b, function);
    });
    return includesFunctions && std::ranges::all_of(a.builtins, [&](BuiltinID builtin) {
        return includesBuiltin(b, builtin, builtins);
    });
}

static const ClassPart* findPartHolding(const Type& type, BuiltinClassID builtinClass, const BuiltinTable& builtins)
{
    if (const ClassPart* part = type.findClassPart(builtinClass))
        return part;
    const BuiltinClass& held = builtins.at(builtinClass);
    if (held.instanceTest != BuiltinInstanceTest::JSTypeEquals)
        return nullptr;
    for (const ClassPart& part : type.classes) {
        const BuiltinClass& holder = builtins.at(part.builtinClass);
        bool areToldByOneTest = holder.instanceTest == BuiltinInstanceTest::JSTypeEquals && holder.type == held.type;
        if (areToldByOneTest && !part.contentsAreProven && !part.provenPristine.contains(PristineAspects::Names) && builtins.isSameOrSubclassOf(builtinClass, part.builtinClass))
            return &part;
    }
    return nullptr;
}

static bool areClassesSubset(const Type& a, const Type& b, const TypeTable& types)
{
    return std::ranges::all_of(a.classes, [&](const ClassPart& part) {
        const ClassPart* partner = findPartHolding(b, part.builtinClass, types.builtins());
        return partner && isPartSubset(part, *partner, types);
    });
}

static bool areRegExpLiteralsSubset(const Type& a, const Type& b)
{
    if (b.regExpLiteralSet.isEmpty() || !a.findClassPart(BuiltinClasses::RegExp))
        return true;
    return !a.regExpLiteralSet.isEmpty() && a.regExpLiteralSet.isSubsetOf(b.regExpLiteralSet);
}

static bool areLayoutsSubset(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.layouts || a.layouts == b.layouts)
        return true;
    if (!b.layouts)
        return false;
    return isSubsetOf(types.at(a.layouts), types.at(b.layouts));
}

static bool areObjectsSubset(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.hasObjects() || b.anyObject)
        return true;
    if (a.anyObject)
        return false;
    return areCallablesSubset(a, b, types.builtins())
        && areClassesSubset(a, b, types)
        && areRegExpLiteralsSubset(a, b)
        && areLayoutsSubset(a, b, types);
}

bool isSubset(const Type& a, const Type& b, const TypeTable& types)
{
    if (&a == &b)
        return true;
    uint16_t missingPrimitives = a.primitives & ~b.primitives;
    if (b.primitives & Primitive::String)
        missingPrimitives &= ~Primitive::LiteralString;
    if (b.primitives & Primitive::Symbol)
        missingPrimitives &= ~Primitive::LiteralSymbol;
    if (missingPrimitives)
        return false;
    if (a.primitives & Primitive::Integer && !a.integers.isSubsetOf(b.integers))
        return false;
    if (!(b.primitives & Primitive::String) && !a.stringLiterals.isSubsetOf(b.stringLiterals))
        return false;
    if (!(b.primitives & Primitive::Symbol) && !a.symbolLiterals.isSubsetOf(b.symbolLiterals))
        return false;
    if (a.opaque && a.opaque != b.opaque)
        return false;
    if (a.environment && a.environment != b.environment)
        return false;
    return areObjectsSubset(a, b, types);
}

bool Type::isSubsetOf(const Type& other, const Program& program) const
{
    return isSubset(*this, other, program.types);
}

template<typename T>
static bool literalsOverlap(const Type& a, const Type& b, LiteralPart<T> part)
{
    uint16_t bits = part.widenedBit | part.literalBit;
    if (!(a.primitives & bits) || !(b.primitives & bits))
        return false;
    return (a.primitives | b.primitives) & part.widenedBit || (a.*part.literals).overlaps(b.*part.literals);
}

static bool callablesOverlap(const BuiltinTable& builtins, const Type& a, const Type& b)
{
    return (hasCallableParts(a) && mayContainAnyCallable(builtins, b))
        || (hasCallableParts(b) && mayContainAnyCallable(builtins, a))
        || a.functions.overlaps(b.functions);
}

static bool mayContainBuiltinOf(const TypeTable& types, const Type& container, const Type& type)
{
    return std::ranges::any_of(type.builtins, [&](BuiltinID builtin) {
        return mayContainBuiltin(types, container, builtin);
    });
}

static bool builtinsOverlap(const Type& a, const Type& b, const TypeTable& types)
{
    return mayContainBuiltinOf(types, b, a) || mayContainBuiltinOf(types, a, b);
}

static bool regExpLiteralsOverlap(const Type& a, const Type& b)
{
    return a.regExpLiteralSet.isEmpty() || b.regExpLiteralSet.isEmpty() || a.regExpLiteralSet.overlaps(b.regExpLiteralSet);
}

static bool classesOverlap(const BuiltinTable& builtins, const Type& a, const Type& b)
{
    return std::ranges::any_of(a.classes, [&](const ClassPart& part) {
        if (overlapsAnotherClass(builtins, part.builtinClass, b))
            return true;
        if (!b.findClassPart(part.builtinClass))
            return false;
        return part.builtinClass != BuiltinClasses::RegExp || regExpLiteralsOverlap(a, b);
    });
}

static bool layoutsOverlap(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.layouts || !b.layouts)
        return false;
    return a.layouts == b.layouts || overlaps(types.at(a.layouts), types.at(b.layouts));
}

static bool objectsOverlap(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.hasObjects() || !b.hasObjects())
        return false;
    if (a.anyObject || b.anyObject)
        return true;
    auto listsObjectsWithoutLayout = [](const Type& type) {
        return type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty() || !type.classes.isEmpty();
    };
    if ((hasMembersWithoutLayout(types, a) && listsObjectsWithoutLayout(b)) || (hasMembersWithoutLayout(types, b) && listsObjectsWithoutLayout(a)))
        return true;
    const BuiltinTable& builtins = types.builtins();
    return callablesOverlap(builtins, a, b) || builtinsOverlap(a, b, types) || classesOverlap(builtins, a, b) || layoutsOverlap(a, b, types);
}

bool overlap(const Type& a, const Type& b, const TypeTable& types)
{
    if (a.primitives & b.primitives & plainPrimitives)
        return true;
    if (!a.integers.intersectWith(b.integers).isEmpty())
        return true;
    if (literalsOverlap(a, b, stringPart) || literalsOverlap(a, b, symbolPart))
        return true;
    if (a.opaque && a.opaque == b.opaque)
        return true;
    if (a.environment && a.environment == b.environment)
        return true;
    return objectsOverlap(a, b, types);
}

bool mayBeSameObject(const Type& a, const Type& b, const TypeTable& types)
{
    if (a.opaque && a.opaque == b.opaque)
        return true;
    if (a.environment && a.environment == b.environment)
        return true;
    return objectsOverlap(a, b, types);
}

bool Type::overlaps(const Type& other, const Program& program) const
{
    return overlap(*this, other, program.types);
}

std::span<const LayoutID> membersOf(const TypeTable& types, const Type& type)
{
    if (!type.layouts)
        return { };
    return types.at(type.layouts).completeMembers();
}

std::span<const LayoutID> membersOf(const Program& program, const Type& type)
{
    return membersOf(program.types, type);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
