/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTArrayStorageForms.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionEditing.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include "AOTTypeClosure.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FlowApplyContext {
    Program& program;
    const FlowResults& results;
    const FlowApplication& application;
    BuildContext& build;
};

using FamilyFieldTypeCache = UncheckedKeyHashMap<uint64_t, std::optional<Type>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

struct TypeOfPlaceholder {
    FieldReference field;
    Type declared;
};

} // anonymous namespace

static bool isOfRuntimeModule(const Program& program, ModuleID module)
{
    return module && program.at(module).kind == ModuleKind::RuntimeSource;
}

static void applyPosition(DeclaredType& declared, const FlowPositionUpdate& update)
{
    ProvenInEveryVersion type = Proven::byFlowOfPosition(update.type ? update.type : declared.written());
    if (update.replacesExpectation)
        declared.expect(ExpectedType(type));
    else if (update.isProven)
        declared.establish(type);
    else
        declared.changeType(type);
}

static BitVector findLayoutsNeverStoredOtherTypes(const Program& program, const FlowResults& results, const Vector<FlowLayoutUpdate>& updates)
{
    auto baseOf = [&](LayoutID id) {
        const LayoutFlow& flow = updates[id.index()].flow;
        if (flow.baseState == BaseState::Layout)
            return flow.base;
        return flow.baseState == BaseState::Unknown ? predictedHeritageOf(program.at(id)) : LayoutID();
    };
    BitVector found(updates.size());
    for (size_t index = 0; index < updates.size(); ++index) {
        bool hasClassNeverStoredOtherTypes = std::ranges::any_of(updates[index].flow.allocationClasses, [&](AllocationClassID allocationClass) {
            return results.at(allocationClass).impossibleMutations.contains(ObjectMutation::StoreOfOtherType);
        });
        if (!hasClassNeverStoredOtherTypes)
            continue;
        for (LayoutID current(index); current && !found.quickSet(current.index()); current = baseOf(current)) { }
    }
    return found;
}

static void applyLayoutUpdate(Layout& layout, const FlowLayoutUpdate& update)
{
    layout.flow = update.flow;
    layout.classFlow = update.classFlow;
    RELEASE_ASSERT(update.fields.size() == layout.fields.size());
    for (size_t index = 0; index < layout.fields.size(); ++index) {
        Field& field = layout.fields[index];
        const FlowFieldUpdate& fieldUpdate = update.fields[index];
        if (!fieldUpdate.accessor.isAccessor())
            applyPosition(field.declared, fieldUpdate.position);
        field.flow = fieldUpdate.flow;
        field.typeBeforeNarrowing = { };
        field.flags.remove(FieldFlags::IsCountedWith);

        bool wasAnalyzed = field.flow.location.writers == WriterState::AllKnown;
        if (wasAnalyzed && !field.flow.stability.contains(FieldStability::IsNeverDeleted) && field.flags.contains(FieldFlags::IsRequired)) {
            field.flags.remove(FieldFlags::IsRequired);
            field.flags.add(FieldFlags::IsRequiredUntilDeleted);
        }
    }
}

static void becomeAccessor(TypeArena& arena, Field& field, const FlowAccessorField& accessor)
{
    field.form = accessor.form;
    field.flags.remove(FieldFlags::IsImmutable);
    Type getter = accessor.getters.isEmpty() ? Type::function() : Type::never();
    for (FunctionID function : accessor.getters)
        getter = getter.unionWith(Type::forFunction(function), arena);
    field.slotType = arena.intern(getter);
    Type setter = Type::never();
    for (FunctionID function : accessor.setters)
        setter = setter.unionWith(Type::forFunction(function), arena);
    field.setterType = accessor.setters.isEmpty() ? TypeID() : arena.intern(setter);
}

static void applyAccessorFields(TypeArena& arena, Layout& layout, const FlowLayoutUpdate& update)
{
    for (size_t index = 0; index < layout.fields.size(); ++index) {
        if (update.fields[index].accessor.isAccessor())
            becomeAccessor(arena, layout.fields[index], update.fields[index].accessor);
    }
}

static bool isPlaceholder(const Program& program, const Field& field)
{
    return field.key && field.form.isEmpty() && field.declared.isExpected() && program.at(field.declared.expected().predicted()).isUndefinedOrNull();
}

static std::optional<Type> uniteTypesDeclaredIn(const Program& program, std::span<const LayoutID> layouts, PropertyKeyID key, TypeArena& arena)
{
    std::optional<Type> united;
    for (LayoutID member : layouts) {
        std::optional<uint32_t> index = program.at(member).findField(key);
        if (!index)
            continue;
        const Field& declaration = fieldAt(program, declarationOf(program, { member, *index }));
        if (isPlaceholder(program, declaration))
            continue;
        const Type& type = program.at(slotTypeOf(program, declaration));
        united = united ? united->unionWith(type, arena) : type;
        if (united->isAnything())
            break;
    }
    return united;
}

static std::optional<Type> typeDeclaredInFamilies(const Program& program, FamilyFieldTypeCache& cache, std::span<const LayoutSetID> families, std::span<const LayoutID> views, PropertyKeyID key, TypeArena& arena)
{
    if (families.empty())
        return std::nullopt;
    std::optional<Type> declared = uniteTypesDeclaredIn(program, views, key, arena);
    for (LayoutSetID family : families) {
        uint64_t cacheKey = static_cast<uint64_t>(family.index()) << 32 | key.index();
        const std::optional<Type>& declaredInFamily = cache.ensure(cacheKey, [&] {
            return uniteTypesDeclaredIn(program, program.at(family).completeMembers(), key, arena);
        }).iterator->value;
        if (declaredInFamily)
            declared = declared ? declared->unionWith(*declaredInFamily, arena) : *declaredInFamily;
    }
    if (!declared || declared->isAnything())
        return std::nullopt;
    return declared;
}

static Vector<Vector<std::optional<Type>>> findTypesDeclaredInFamilies(const Program& program, const Vector<FlowLayoutUpdate>& updates, TypeArena& arena)
{
    Vector<Vector<std::optional<Type>>> types(updates.size());
    FamilyFieldTypeCache cache;
    BitVector declaredLayoutsWithFields(program.layouts.size());
    for (size_t index = 0; index < updates.size(); ++index) {
        if (!updates[index].appendedFields.isEmpty())
            declaredLayoutsWithFields.quickSet(declaredLayoutOf(program, LayoutID(index)).index());
    }
    Vector<Vector<LayoutID>> declaredViews = declaredViewsOf(program, declaredLayoutsWithFields);
    for (size_t index = 0; index < updates.size(); ++index) {
        if (updates[index].appendedFields.isEmpty())
            continue;
        Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, LayoutID(index));
        std::span<const LayoutID> views = declaredViews[declaredLayoutOf(program, LayoutID(index)).index()].span();
        types[index] = WTF::map(updates[index].appendedFields, [&](const FlowAppendedField& appended) {
            return typeDeclaredInFamilies(program, cache, families.span(), views, appended.key, arena);
        });
    }
    return types;
}

Vector<BitVector> findAppendedFieldsDeclaredInFamilies(const Program& program, const Vector<FlowLayoutUpdate>& updates, const ParallelLoops& loops)
{
    struct Worker {
        explicit Worker(const TypeTable& types)
            : arena(types)
        {
        }

        ScratchTypeArena arena;
        FamilyFieldTypeCache cache;
    };

    Vector<BitVector> isDeclared(updates.size());
    Vector<uint32_t> layouts = findIndicesInParallel(loops, updates.size(), [&](unsigned index) {
        return !updates[index].appendedFields.isEmpty();
    });
    BitVector declaredLayoutsWithFields(program.layouts.size());
    for (uint32_t index : layouts)
        declaredLayoutsWithFields.quickSet(declaredLayoutOf(program, LayoutID(index)).index());
    Vector<Vector<LayoutID>> declaredViews = declaredViewsOf(program, declaredLayoutsWithFields);
    Vector<std::unique_ptr<Worker>> workers(loops.width());
    forEachIndexInParallel(loops, layouts.size(), [&](unsigned position, unsigned number) {
        if (!workers[number])
            workers[number] = makeUniqueWithoutFastMallocCheck<Worker>(program.types);
        Worker& worker = *workers[number];
        uint32_t index = layouts[position];
        Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, LayoutID(index));
        std::span<const LayoutID> views = declaredViews[declaredLayoutOf(program, LayoutID(index)).index()].span();
        const Vector<FlowAppendedField>& fields = updates[index].appendedFields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            if (typeDeclaredInFamilies(program, worker.cache, families.span(), views, fields[field].key, worker.arena))
                isDeclared[index].set(field);
        }
    });
    return isDeclared;
}

static Vector<TypeOfPlaceholder> findTypesOfPlaceholders(const Program& program, TypeArena& arena)
{
    BitVector declaredLayoutsWithPlaceholders(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || layout.isDerived())
            continue;
        if (std::ranges::any_of(layout.fields, [&](const Field& field) { return !field.flags.contains(FieldFlags::IsInherited) && isPlaceholder(program, field); }))
            declaredLayoutsWithPlaceholders.quickSet(index);
    }
    Vector<TypeOfPlaceholder> found;
    if (declaredLayoutsWithPlaceholders.isEmpty())
        return found;
    FamilyFieldTypeCache cache;
    Vector<Vector<LayoutID>> declaredViews = declaredViewsOf(program, declaredLayoutsWithPlaceholders);
    for (size_t index : declaredLayoutsWithPlaceholders) {
        LayoutID id(static_cast<uint32_t>(index));
        const Layout& layout = program.at(id);
        Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, id);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (layout.fields[field].flags.contains(FieldFlags::IsInherited) || !isPlaceholder(program, layout.fields[field]))
                continue;
            std::optional<Type> declared = typeDeclaredInFamilies(program, cache, families.span(), declaredViews[index].span(), layout.fields[field].key, arena);
            if (!declared || (declared->isNumberOrNullish() && declared->primitives & Primitive::Double))
                continue;
            found.append({ { id, field }, declared->unionWith(program.at(layout.fields[field].declared.written()), arena) });
        }
    }
    return found;
}

static LayoutID provenBaseOfLayout(const LayoutFlow& flow)
{
    return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
}

static void dropTypesThatDataLiesOutside(const Program& program, const Vector<FlowLayoutUpdate>& updates, Vector<Vector<std::optional<Type>>>& types)
{
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (LayoutID layout = object->layout; layout; layout = provenBaseOfLayout(updates[layout.index()].flow)) {
                Vector<std::optional<Type>>& typesOfLayout = types[layout.index()];
                const Vector<FlowAppendedField>& appendedFields = updates[layout.index()].appendedFields;
                for (size_t index = 0; index < typesOfLayout.size(); ++index) {
                    if (!typesOfLayout[index])
                        continue;
                    bool liesOutside = std::ranges::any_of(object->members, [&](const DataMember& member) {
                        return member.key == appendedFields[index].key && !includesData(program, *typesOfLayout[index], member.value);
                    });
                    if (liesOutside)
                        typesOfLayout[index] = std::nullopt;
                }
            }
        });
    }
}

static void applyTypeDeclaredInFamily(const Program& program, TypeArena& arena, const std::optional<Type>& declared, Field& field)
{
    if (!declared)
        return;
    const Type& proven = program.at(field.declared.written());
    bool fitsDeclaredType = proven.isSubsetOf(declared->withPrimitives(Primitive::Absent), program);
    if (field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType)) {
        if (fitsDeclaredType)
            return;
        field.flow.stability.remove(FieldStability::IsNeverStoredWithOtherType);
    }
    const Type& slotType = fitsDeclaredType ? proven : *declared;
    if (!fitsDeclaredType)
        field.declared.changeType(Proven::byPosition(arena.intern(*declared)));
    if (!fitsDeclaredType && slotType.isNumberOrNullish() && slotType.primitives & Primitive::Double)
        field.declared.expect(field.declared.expected());
}

static void appendFlowFields(const Program& program, TypeArena& arena, std::span<const std::optional<Type>> typesDeclaredInFamilies, bool mustKeepTypes, Layout& layout, const FlowLayoutUpdate& update)
{
    for (size_t index = 0; index < update.appendedFields.size(); ++index) {
        const FlowAppendedField& appended = update.appendedFields[index];
        RELEASE_ASSERT(!layout.findField(appended.key));
        Field field;
        field.key = appended.key;
        field.declared.establish(Proven::byPosition(TypeTable::anything));
        if (!appended.accessor.isAccessor())
            applyPosition(field.declared, appended.position);
        field.flags = { FieldFlags::IsAddedLate };
        if (appended.isFilledByCreators)
            field.flags.add(FieldFlags::IsFilledByCreator);
        field.flow = appended.flow;
        field.form = appended.formOfValue;
        if (appended.accessor.isAccessor())
            becomeAccessor(arena, field, appended.accessor);
        else if (!mustKeepTypes)
            applyTypeDeclaredInFamily(program, arena, typesDeclaredInFamilies[index], field);
        layout.appendField(WTF::move(field));
    }
}

static void appendLiteralKeyOrders(Layout& layout, const FlowLayoutUpdate& update)
{
    size_t numberOfIndexOrders = layout.flow.constructorOrders.size();
    for (const Vector<PropertyKeyID>& keys : update.literalKeyOrders) {
        FieldOrder order;
        bool listsOnlyFields = std::ranges::all_of(keys, [&](PropertyKeyID key) {
            std::optional<uint32_t> field = layout.findField(key);
            if (field)
                order.fields.append(*field);
            return field.has_value();
        });
        if (listsOnlyFields && !std::ranges::contains(layout.flow.constructorOrders.span().first(numberOfIndexOrders), order))
            layout.flow.constructorOrders.append(WTF::move(order));
    }
}

static void applyLayoutUpdates(const FlowApplyContext& context)
{
    Program& program = context.program;
    const Vector<FlowLayoutUpdate>& updates = context.application.layouts;
    RELEASE_ASSERT(updates.size() == program.layouts.size());

    PhaseTimes times(context.build, PassID::ApplyFlowResults);
    TypeArena arena(program.types);
    Vector<Vector<std::optional<Type>>> typesDeclaredInFamilies = findTypesDeclaredInFamilies(program, updates, arena);
    BitVector layoutsNeverStoredOtherTypes = findLayoutsNeverStoredOtherTypes(program, context.results, updates);
    dropTypesThatDataLiesOutside(program, updates, typesDeclaredInFamilies);
    Vector<TypeOfPlaceholder> typesOfPlaceholders = findTypesOfPlaceholders(program, arena);
    times.didFinish("findTypesDeclaredInFamilies"_s);

    forEachIndexInParallel(context.build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        applyLayoutUpdate(program.layouts[index], updates[index]);
    });
    for (uint32_t index : findIndicesInParallel(context.build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        return std::ranges::any_of(updates[index].fields, [](const FlowFieldUpdate& update) {
            return update.accessor.isAccessor();
        });
    }))
        applyAccessorFields(arena, program.layouts[index], updates[index]);
    for (const TypeOfPlaceholder& placeholder : typesOfPlaceholders) {
        Field& field = program.at(placeholder.field.layout).fields[placeholder.field.field];
        if (!isPlaceholder(program, field))
            continue;
        field.declared.establish(Proven::byFlowOfPosition(arena.intern(placeholder.declared)));
    }
    program.setKeysStoredThroughUnknownReceivers(Vector<PropertyKeyID>(context.results.keysStoredThroughUnknownReceivers()));
    program.setKeysDefinedOnObjectsWithoutKnownLayout(Vector<PropertyKeyID>(context.results.keysDefinedOnObjectsWithoutKnownLayout()));
    program.indexClasses();
    times.didFinish("applyLayoutUpdate"_s);

    BitVector done(program.layouts.size());
    Vector<LayoutID, 8> incompleteChain;
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        for (LayoutID current(index); current && !done.quickSet(current.index()); current = provenBaseOfLayout(program.at(current).flow))
            incompleteChain.append(current);
        while (!incompleteChain.isEmpty()) {
            LayoutID current = incompleteChain.takeLast();
            appendInheritedFields(program, current);
            appendFlowFields(program, arena, typesDeclaredInFamilies[current.index()].span(), layoutsNeverStoredOtherTypes.get(current.index()), program.at(current), updates[current.index()]);
            appendLiteralKeyOrders(program.at(current), updates[current.index()]);
        }
    }
    markEnforcedFields(program, context.build);
    times.didFinish("completeFields"_s);

    program.indexNames(context.build.parallelLoops());
    times.didFinish("indexNames"_s);
}

static void applyVariableUpdates(const FlowApplyContext& context)
{
    Program& program = context.program;
    const Vector<FlowVariableUpdate>& updates = context.application.variables;
    RELEASE_ASSERT(updates.size() == program.variables.size());

    forEachIndexInParallel(context.build.parallelLoops(), program.variables.size(), [&](unsigned index) {
        Variable& variable = program.variables[index];
        applyPosition(variable.declared, updates[index].position);
        variable.flow = updates[index].flow;
    });
}

static void applyEntryParameterUpdates(Function& function, std::span<const FlowPositionUpdate> updates)
{
    if (updates.empty())
        return;
    RELEASE_ASSERT(updates.size() == function.numberOfEntryParameters());
    for (unsigned index = 0; index < updates.size(); ++index) {
        if (DeclaredType* declared = function.declaredEntryParameter(index))
            applyPosition(*declared, updates[index]);
    }
    if (function.hasBody() && std::ranges::any_of(updates, &FlowPositionUpdate::replacesExpectation)) {
        for (Value& value : function.editAllValues())
            value.setExpected({ });
    }
}

static void applyResultUpdates(Function& function, std::span<const FlowPositionUpdate> updates)
{
    if (updates.empty())
        return;
    RELEASE_ASSERT(updates.size() == function.results.size());
    for (size_t index = 0; index < updates.size(); ++index)
        applyPosition(function.results[index], updates[index]);
}

static StringID internNameOfFunctionUnder(Program& program, const PropertyKey& key)
{
    if (key.kind == PropertyKeyKind::Text)
        return key.text;
    Vector<uint8_t, 64> name;
    name.append('[');
    name.append(program.strings[key.text]);
    name.append(']');
    return program.strings.intern(name.span());
}

static void noteLooksForNameWhereClosuresMayBeRenamed(const FlowResults& results, FunctionID id, Function& function)
{
    if (results.closuresMayBeRenamed(id))
        function.capabilities.add(FunctionCapabilities::NotesLooksForName);
}

static void applySignatureUpdates(const FlowApplyContext& context)
{
    const Vector<FlowSignatureUpdate>& updates = context.application.signatures;
    RELEASE_ASSERT(updates.size() == context.application.functionCountAsRead);

    for (unsigned index = 0; index < updates.size(); ++index) {
        if (PropertyKeyID key = updates[index].keyThatNamesFunction) {
            context.program.functions[index].name = internNameOfFunctionUnder(context.program, context.program.at(key));
            context.program.functions[index].flags.add(FunctionFlags::IsNamedByComputedKey);
        }
    }
    forEachIndexInParallel(context.build.parallelLoops(), updates.size(), [&](unsigned index) {
        Function& function = context.program.functions[index];
        const FlowSignatureUpdate& update = updates[index];
        applyEntryParameterUpdates(function, update.entryParameters.span());
        applyResultUpdates(function, update.results.span());
        function.capabilities.add(update.provenCapabilities);
        noteLooksForNameWhereClosuresMayBeRenamed(context.results, FunctionID(index), function);
    });
}

static void removeUnreachedBodies(const FlowApplyContext& context, ProgramEditor& editor)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.results.functionCountAsRead(), [&](unsigned index) {
        FunctionID id(index);
        if (context.results.function(id).reachability == FunctionReachability::Unreached && context.program.at(id).hasBody())
            editor.removeBody(id);
    });
}

static void dropDataOfUnreachedCode(const FlowApplyContext& context)
{
    Program& program = context.program;
    Vector<DataID> named = collectInParallel<DataID>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<DataID>& found) {
        for (const Value& value : program.functions[index].values) {
            if (Value::opcodesWithData.contains(value.opcode))
                found.append(value.data());
        }
    });
    BitVector isNamed(program.data.size());
    for (DataID data : named)
        isNamed.quickSet(data.index());
    for (const Module& module : program.modules) {
        if (module.data)
            isNamed.quickSet(module.data.index());
    }
    for (uint32_t index = 0; index < program.data.size(); ++index) {
        if (!isNamed.quickGet(index))
            program.data[index].root = DataNode();
    }
}

static void narrowContentsOfData(const FlowApplyContext& context)
{
    struct RootSite {
        DataID data;
        AllocationSiteID site;
    };

    Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    Vector<RootSite> sites = collectInParallel<RootSite>(loops, context.application.bodies.size(), [&](unsigned index, Vector<RootSite>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        for (const FlowAllocationStamp& stamp : context.application.bodies[index].allocations) {
            const Value& value = function.at(stamp.value);
            if (value.opcode == Opcode::Data)
                found.append({ value.data(), stamp.site });
        }
    });
    Vector<AllocationSiteID> rootSites(program.data.size());
    BitVector isNamedTwice(program.data.size());
    for (const RootSite& root : sites) {
        AllocationSiteID& rootSite = rootSites[root.data.index()];
        if (rootSite)
            isNamedTwice.quickSet(root.data.index());
        rootSite = root.site;
    }
    forEachIndexInParallel(loops, program.data.size(), [&](unsigned index) {
        if (!rootSites[index] || isNamedTwice.quickGet(index))
            return;
        forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t ordinal, const DataNode& node) {
            const auto* array = node.tryAs<DataNode::Array>();
            if (!array || !isStorageOfArray(*array))
                return;
            if (TypeID elements = context.results.dataNodeStamp(rootSites[index], ordinal).elements)
                const_cast<DataNode::Array*>(array)->contents = elements;
        });
    });
}

static void proveEveryPosition(Program& program, BuildContext& build)
{
    auto prove = [](DeclaredType& declared) {
        declared.establish(Proven::byTestingSwitch(declared.expected()));
    };
    for (Layout& layout : program.layouts) {
        if (isOfRuntimeModule(program, layout.module))
            continue;
        for (Field& field : layout.fields)
            prove(field.declared);
    }
    for (Variable& variable : program.variables) {
        if (!isOfRuntimeModule(program, variable.module))
            prove(variable.declared);
    }
    for (Function& function : program.functions) {
        if (isOfRuntimeModule(program, function.module))
            continue;
        std::ranges::for_each(function.captures, prove);
        prove(function.thisType);
        std::ranges::for_each(function.parameters, prove);
        std::ranges::for_each(function.results, prove);
    }
    markEnforcedFields(program, build);
    program.indexNames();
}

static Vector<FunctionID> collectBodiesLargestFirst(const Program& program, size_t numberOfFunctions, const ParallelLoops& loops)
{
    Vector<FunctionID> functions = collectInParallel<FunctionID>(loops, numberOfFunctions, [&](unsigned index, Vector<FunctionID>& functionsWithBody) {
        if (program.functions[index].hasBody())
            functionsWithBody.append(FunctionID(index));
    });
    sortLargestFirst(program, loops, functions);
    return functions;
}

static VariableID addMark(ProgramEditor& editor, ModuleID module)
{
    Variable variable;
    variable.module = module;
    variable.declared.establish(Proven::byPosition(TypeTable::anythingOrAbsent));
    return editor.addVariable(WTF::move(variable));
}

static Vector<VariableID> createFirstRunMarks(const FlowApplyContext& context, ProgramEditor& editor)
{
    const Program& program = context.program;
    Vector<VariableID> marks(context.application.bodies.size());
    if (!context.build.validates())
        return marks;

    for (uint32_t index = 0; index < marks.size(); ++index) {
        FunctionID id(index);
        if (program.at(id).hasBody() && context.results.runsAtMostOnce(id))
            marks[index] = addMark(editor, program.at(id).module);
    }
    return marks;
}

static FlowFirstTimeMarks createFirstTimeMarks(const FlowApplyContext& context, ProgramEditor& editor)
{
    FlowFirstTimeMarks marks;
    if (!context.build.validates())
        return marks;

    const Program& program = context.program;
    marks.storesByVariable.grow(program.variables.size());
    marks.evaluationsByLayout.grow(program.layouts.size());
    for (uint32_t index = 0; index < context.application.bodies.size(); ++index) {
        FunctionID id(index);
        for (const Value& value : program.at(id).values) {
            VariableID* mark = nullptr;
            if (value.opcode == Opcode::PutModuleVariable && !program.at(value.variable()).owner && program.at(value.variable()).flow.stores == StoreCount::AtMostOnce)
                mark = &marks.storesByVariable[value.variable().index()];
            else if (value.opcode == Opcode::NewClass && program.at(value.layout()).classFlow.evaluations == ExecutionCount::AtMostOnce)
                mark = &marks.evaluationsByLayout[value.layout().index()];
            if (mark && !*mark)
                *mark = addMark(editor, program.at(id).module);
        }
    }
    return marks;
}

static Vector<BitVector> findExpectedFields(const Program& program, const ParallelLoops& loops)
{
    Vector<BitVector> expectedFieldsOfLayouts(program.layouts.size());
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        const Vector<Field>& fields = program.layouts[index].fields;
        BitVector expectedFields(fields.size());
        for (size_t field = 0; field < fields.size(); ++field) {
            if (fields[field].declared.isExpected())
                expectedFields.quickSet(field);
        }
        expectedFieldsOfLayouts[index] = WTF::move(expectedFields);
    });
    return expectedFieldsOfLayouts;
}

static bool makesTailCall(const Function& function)
{
    return std::ranges::any_of(function.blocks, [](const Block& block) { return block.terminator.kind == TerminatorKind::ReturnCall; });
}

// The functions with a tail call whose callers are all known and all construct them.
static BitVector findFunctionsThatAreOnlyConstructed(const FlowApplyContext& context)
{
    const Program& program = context.program;
    std::span<const FlowBodyUpdate> bodies = context.application.bodies.span();
    BitVector constructed(program.functions.size());
    auto forEachStampedCallee = [&](bool ofConstructions, const auto& functor) {
        for (size_t index = 0; index < bodies.size(); ++index) {
            for (const FlowCallStamp& stamp : bodies[index].calls) {
                if (!!OperandPosition::newTarget(program.functions[index].at(stamp.value).opcode) != ofConstructions)
                    continue;
                ProvenValue<std::span<const FunctionID>> followed = program.at(stamp.callees).followed();
                for (FunctionID callee : followed.value())
                    functor(callee);
            }
        }
    };
    auto noteConstruction = [&](FunctionID callee) {
        const Function& function = program.at(callee);
        if (function.kind == FunctionKind::Plain && context.results.function(callee).callers == CallerState::AllKnown && makesTailCall(function))
            constructed.quickSet(callee.index());
    };
    forEachStampedCallee(true, noteConstruction);
    for (const FlowIndirectCall& call : context.results.indirectCalls()) {
        if (call.isAlwaysConstruction)
            noteConstruction(call.callee);
    }
    if (constructed.isEmpty())
        return constructed;

    forEachStampedCallee(false, [&](FunctionID callee) {
        constructed.quickClear(callee.index());
    });
    for (const FlowIndirectCall& call : context.results.indirectCalls()) {
        if (!call.isAlwaysConstruction)
            constructed.quickClear(call.callee.index());
    }
    for (const Function& function : program.functions) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::CallDirect && !function.at(id).isConstruction())
                    constructed.quickClear(function.at(id).function().index());
            }
        }
    }
    return constructed;
}

static void applyBodyUpdates(const FlowApplyContext& context, ProgramEditor& editor, const Vector<BitVector>& fieldsExpectedAsRead, const BitVector& keysOfWidenedFields, const BitVector& widenedVariables)
{
    BitVector onlyConstructed = findFunctionsThatAreOnlyConstructed(context);
    FlowFirstTimeMarks firstTimeMarks = createFirstTimeMarks(context, editor);
    Vector<VariableID> firstRunMarks = createFirstRunMarks(context, editor);
    Vector<FunctionID> functions = collectBodiesLargestFirst(context.program, context.application.bodies.size(), context.build.parallelLoops());
    Vector<FlowBodyCounts> countsByTask(functions.size());
    bool provesEverything = context.build.recordedOptions().has(TestingSwitch::ProveEverything);
    editFunctionsInParallel(editor, context.build, BehindEdit::Nothing, functions.span(), [&](unsigned task, FunctionID id, Function& function, TaskArenas& arenas) {
        FlowBodyApplication body { context.program, context.results, context.application, id, function, arenas.types, context.build, fieldsExpectedAsRead, firstRunMarks[id.index()], &firstTimeMarks };
        body.provesEverything = provesEverything && !isOfRuntimeModule(context.program, function.module);
        body.isOnlyConstructed = onlyConstructed.quickGet(id.index());
        body.keysOfWidenedFields = &keysOfWidenedFields;
        body.widenedVariables = &widenedVariables;
        countsByTask[task] = applyBodyUpdate(body);
    });

    FlowBodyCounts total;
    for (const FlowBodyCounts& counts : countsByTask) {
        total.removedChecks += counts.removedChecks;
        total.remainingChecks += counts.remainingChecks;
    }
    context.build.statistics().count(StatisticID::FlowRemovedChecks, total.removedChecks);
    context.build.statistics().count(StatisticID::FlowRemainingChecks, total.remainingChecks);
}

static void applyUnknownResults(Program& program, const FlowResults& results, BuildContext& build)
{
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        noteLooksForNameWhereClosuresMayBeRenamed(results, FunctionID(index), program.functions[index]);
    });
    ProgramEditor editor(program);
    Vector<FunctionID> functions = collectBodiesLargestFirst(program, program.functions.size(), build.parallelLoops());
    editFunctionsInParallel(editor, build, BehindEdit::Nothing, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        demoteTailCallsWithChecks(program, id, function, arenas.types, build);
        initializeMembersOfListedHolders(program, results, id, function, arenas.types, build);
        nameSymbolsCreatedByLibraryModule(program, id, function, arenas.types, build);
    });
}

static void nameObjectInPlaceOfDictionaryLayouts(Program& program, const FlowApplication& application, const ParallelLoops& loops)
{
    if (application.typesBehindDictionaries.isEmpty())
        return;
    DictionaryTypeRewriter rewriter { application };
    auto rewriteList = [&](auto& list) {
        forEachIndexInParallel(loops, list.size(), [&](unsigned index) {
            forEachReference(list[index], rewriter);
        });
    };
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        forEachReference(static_cast<FunctionRecord&>(program.functions[index]), rewriter);
    });
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        forEachReference(static_cast<LayoutRecord&>(program.layouts[index]), rewriter);
    });
    rewriteList(program.typeSites);
    rewriteList(program.expectedResults);
    rewriteList(program.variables);
    rewriteList(program.modules);
    rewriteList(program.moduleFailures);
    rewriteList(program.evalScopes);
    rewriteList(program.data);
    rewriteList(program.entries);
    rewriteList(program.runtimeFunctions);
    rewriteList(program.webAssemblyModules);
}

// What an allocation lists is tested by nobody. Where it may lie outside the type of its field it is stored behind the allocation, by a store that tests it.
// Where that cannot be done the type of the field is only expected, and this is settled before a body is told what its stores take.
static void takeBackFieldsThatListUntestedValues(Program& program, FlowResults& results, const FlowApplication& application, BuildContext& build)
{
    Vector<FieldReference> fields;
    for (uint32_t layout = 0; layout < application.layouts.size(); ++layout) {
        for (const FlowListedValue& listed : application.layouts[layout].valuesListedWithOtherTypes) {
            FieldReference declaration { LayoutID(layout), listed.field };
            if ((!fields.isEmpty() && fields.last() == declaration) || fieldAt(program, declaration).declared.isExpected())
                continue;
            const Value& allocation = program.at(listed.function).at(listed.allocation);
            std::span<const uint32_t> listedFields = allocation.fields();
            for (uint32_t valueIndex = 0; valueIndex < listedFields.size(); ++valueIndex) {
                if (declarationOf(program, { allocation.layout(), listedFields[valueIndex] }) != declaration || canStoreInitialValueBehindAllocation(program, allocation, valueIndex))
                    continue;
                fields.append(declaration);
                break;
            }
        }
    }
    if (fields.isEmpty())
        return;
    expectFields(program, fields.span(), build);
    results.setFieldsTakenBack(WTF::move(fields));
}

// A value of any type is still expected to be the integer that the program shows. A number is computed with as it is.
static BitVector findKeysOfWidenedFields(const Program& program, const FlowApplication& application)
{
    BitVector keys;
    for (uint32_t layout = 0; layout < application.layouts.size(); ++layout) {
        const Vector<Field>& fields = program.layouts[layout].fields;
        for (uint32_t index = 0; index < fields.size(); ++index) {
            TypeID type = application.layouts[layout].fields[index].position.type;
            if (fields[index].typeBeforeNarrowing && type && program.at(type).isNumberOrNullish() && !program.at(type).isSubsetOf(program.at(fields[index].declared.written()), program))
                keys.set(fields[index].key.index());
        }
    }
    return keys;
}

static BitVector findWidenedVariables(const Program& program, const FlowApplication& application)
{
    BitVector variables;
    for (uint32_t index = 0; index < application.variables.size(); ++index) {
        const FlowPositionUpdate& position = application.variables[index].position;
        const DeclaredType& declared = program.variables[index].declared;
        if (!position.isProven || !position.type || !program.at(position.type).isNumber())
            continue;
        if (!program.at(position.type).isSubsetOf(declared.isExpected() ? program.at(declared.written()) : program.at(TypeTable::int32), program))
            variables.set(index);
    }
    return variables;
}

void applyFlowResults(Program& program, FlowResults& results, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    if (!results.hasResults()) {
        // The host keeps every feature that it was not asked to remove.
        BitVector removedBuiltins(program.builtins().builtinCount());
        for (uint32_t index = 0; index < program.builtins().builtinCount(); ++index)
            removedBuiltins.quickSet(index, isGuardedByRemovedFeature(program.builtins(), BuiltinID(index), build.options().removedFeatures));
        program.setBuiltinsThatMayBeTrimmed(WTF::move(removedBuiltins));
        results.setMayListKeysOfEveryHolder();
        applyUnknownResults(program, results, build);
        return;
    }
    program.setBuiltinsThatMayBeTrimmed(BitVector(results.builtinsThatMayBeTrimmed()));
    const FlowApplication* application = FlowApplyAccess::applicationOf(results);
    RELEASE_ASSERT(application);
    RELEASE_ASSERT(application->functionCountAsRead == program.functions.size());

    FlowApplyContext context { program, results, *application, build };
    ProgramEditor editor(program);
    PhaseTimes times(build, PassID::ApplyFlowResults);
    Vector<BitVector> fieldsExpectedAsRead = findExpectedFields(program, build.parallelLoops());
    nameObjectInPlaceOfDictionaryLayouts(program, *application, build.parallelLoops());
    BitVector keysOfWidenedFields = findKeysOfWidenedFields(program, *application);
    BitVector widenedVariables = findWidenedVariables(program, *application);
    applyLayoutUpdates(context);
    takeBackFieldsThatListUntestedValues(program, results, *application, build);
    times.didFinish("applyLayoutUpdates"_s);
    applyVariableUpdates(context);
    applySignatureUpdates(context);
    if (build.recordedOptions().has(TestingSwitch::ProveEverything))
        proveEveryPosition(program, build);
    times.didFinish("applySignatureUpdates"_s);
    removeUnreachedBodies(context, editor);
    dropDataOfUnreachedCode(context);
    narrowContentsOfData(context);
    times.didFinish("removeUnreachedBodies"_s);
    applyBodyUpdates(context, editor, fieldsExpectedAsRead, keysOfWidenedFields, widenedVariables);
    times.didFinish("applyBodyUpdates"_s);
    ProgramCaches caches(program, build);
    computeOperandSummaries(program, results, build);
    times.didFinish("computeOperandSummaries"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
