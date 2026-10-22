/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowPositions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRPrinter.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isDefinedByClass(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
}

namespace {

// The constructor of a base class allocates the instances of the classes below it too: the run time answers with an instance of the class whose prototype object the allocation is given (ClassTable::instanceWithPrototype).
class LayoutPicks {
public:
    LayoutPicks(const ValueFlowResultInputs& inputs, ValueSetMaker& maker)
        : m_inputs(inputs)
        , m_maker(maker)
    {
    }

    ValueFlowPositionCounts::LayoutsOfSites find();

private:
    struct Allocation {
        ValueFlowObjectID object;
        LayoutID layout;
        ValueSetID prototypes;
    };

    void listSites();
    void listInstances();
    void collectOperands();
    LayoutID baseOf(LayoutID) const;
    bool isBelow(LayoutID, LayoutID base) const;
    void notePicksOf(const Allocation&);
    ValueSetID closuresIn(ValueSetID);
    bool getsOwnLayoutFromEach(LayoutID newTarget, std::span<const LayoutID> sortedConstructorFunctions) const;
    void findInstancesMadeForOtherConstructorFunctions();

    const ValueFlowResultInputs& m_inputs;
    ValueSetMaker& m_maker;
    ValueFlowPositionCounts::LayoutsOfSites m_layoutsOfSites;
    // By class: all that its heritage may be. Nothing for a class that no code evaluates whose operations the solve has followed.
    Vector<ValueSetID> m_heritageOfClass;
    Vector<Allocation> m_allocations;
};

} // anonymous namespace

// Of an allocation whose operands nobody has followed nothing is known.
void LayoutPicks::listSites()
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    Vector<uint32_t> sites = findIndicesInParallel(m_inputs.loops(), universe.numberOfObjects(), [&](unsigned index) {
        ValueFlowObjectID object(index);
        const ValueFlowSite* site = universe.siteOf(object);
        if (!site || !site->layout || site->origin != ValueFlowSiteOrigin::Operation || !program.prototypeMaySelectOtherClass(site->layout))
            return false;
        return mayAllocateOtherLayout(program, program.at(universe.functionOf(object)).at(site->value));
    });
    for (uint32_t index : sites)
        m_layoutsOfSites.add(index, ValueFlowLayoutsOfSite { { universe.siteOf(ValueFlowObjectID(index))->layout }, true });
}

// The rules hand a constructor function the instance of new.target, whatever the function is.
void LayoutPicks::listInstances()
{
    const Program& program = m_inputs.program;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Instance && program.layouts[index].constructor)
            m_layoutsOfSites.add(m_inputs.universe().instanceOf(LayoutID(index)).index(), ValueFlowLayoutsOfSite { { LayoutID(index) }, false });
    }
}

void LayoutPicks::collectOperands()
{
    const Program& program = m_inputs.program;
    const ValueFlowBodies& bodies = m_inputs.bodies();
    m_heritageOfClass.fill(ValueSets::nothing, program.layouts.size());
    // A heritage that is a primitive other than null makes the definition of the class throw, so no class has it.
    ValueSetID heritagesOfClasses = m_maker.unite(m_inputs.universe().objects(), m_maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
    Vector<const ValueFlowStatement*> statements = collectInParallel<const ValueFlowStatement*>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<const ValueFlowStatement*>& found) {
        if (!m_inputs.isReached(FunctionID(index)))
            return;
        const Function& function = program.functions[index];
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            if (statement.opcode == Opcode::NewClass) {
                if (function.at(statement.site).heritageOperand())
                    found.append(&statement);
                continue;
            }
            if (statement.opcode != Opcode::NewObject)
                continue;
            ValueFlowObjectID object = m_inputs.universe().siteObject(statement);
            if (object && m_layoutsOfSites.contains(object.index()))
                found.append(&statement);
        }
    });
    for (const ValueFlowStatement* found : statements) {
        const ValueFlowStatement& statement = *found;
        const Value& operation = program.at(statement.function).at(statement.site);
        if (statement.opcode == Opcode::NewClass) {
            ValueSetID& heritage = m_heritageOfClass[operation.layout().index()];
            heritage = m_maker.unite(heritage, m_maker.intersect(m_inputs.valueOf(m_maker, bodies.operandsOf(statement)[OperandIndex::NewClass::heritage]), heritagesOfClasses));
            continue;
        }
        m_allocations.append({ m_inputs.universe().siteObject(statement), operation.layout(), m_inputs.valueOf(m_maker, bodies.operandsOf(statement)[OperandIndex::NewObject::prototype]) });
    }
}

// None unless the heritage is the constructor of one class whatever happens. Then whatever the results say about the base of the class, they name that class or nothing.
LayoutID LayoutPicks::baseOf(LayoutID layout) const
{
    const Program& program = m_inputs.program;
    ValueFlowObjectID heritage = m_inputs.sets().onlyMember(m_heritageOfClass[layout.index()]);
    FunctionID constructor = heritage ? m_inputs.universe().closureFunction(heritage) : FunctionID();
    LayoutID base = constructor ? program.at(constructor).constructedClass : LayoutID();
    return base && isDefinedByClass(program.at(base)) ? base : LayoutID();
}

bool LayoutPicks::isBelow(LayoutID layout, LayoutID base) const
{
    const Program& program = m_inputs.program;
    LayoutID current = layout;
    for (size_t steps = 0; current && steps <= program.layouts.size(); ++steps) {
        if (current == base)
            return true;
        if (!program.at(current).has(LayoutFlags::HasHeritage))
            return false;
        current = baseOf(current);
    }
    return false;
}

// The prototype object of the class itself and an object that is the prototype object of no class leave the allocation as it is.
void LayoutPicks::notePicksOf(const Allocation& allocation)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();

    // A placeholder may be the prototype object of any class.
    ValueFlowLayoutsOfSite picked { { allocation.layout }, ValueSets::isComplement(allocation.prototypes) || sets.meet(allocation.prototypes, universe.placeholdersAlone()) };
    LayoutID ownClass = declaredLayoutOf(program, allocation.layout);
    sets.forEachMember(ValueSets::isComplement(allocation.prototypes) ? ValueSets::nothing : allocation.prototypes, [&](ValueFlowObjectID prototype) {
        LayoutID requested = universe.classLayoutOf(prototype);
        if (requested == ownClass)
            return;
        if (!requested || !isDefinedByClass(program.at(requested)))
            return;
        ValueFlowObjectID made = universe.objectAllocatedUnder(program, allocation.object, prototype);
        ValueFlowLayoutsOfSite& picksOfMade = made == allocation.object ? picked : m_layoutsOfSites.find(made.index())->value;
        if (!isBelow(requested, ownClass))
            picksOfMade.mayBeOtherObject = true;
        else if (made == allocation.object)
            picked.layouts.append(requested);
    });
    m_layoutsOfSites.set(allocation.object.index(), WTF::move(picked));
}

// The closures that the rule of a group finds in the set: those it lists, and behind the untracked object those that the analysis has stopped following.
ValueSetID LayoutPicks::closuresIn(ValueSetID set)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowSolver& solver = m_inputs.solver();
    ValueSetID found = set;
    if (ValueSets::isComplement(set) || m_inputs.sets().contains(set, ValueFlowUniverse::untrackedObject()))
        found = m_maker.unite(found, solver.valueOf(ValueFlowProgramNodes::lost()));
    return m_maker.intersect(found, universe.closures());
}

// ClassTable::createThis() walks up from the layout of new.target.
bool LayoutPicks::getsOwnLayoutFromEach(LayoutID newTarget, std::span<const LayoutID> sortedConstructorFunctions) const
{
    const Program& program = m_inputs.program;
    size_t numberOnChain = 0;
    LayoutID current = newTarget;
    for (size_t steps = 0; steps <= program.layouts.size(); ++steps) {
        numberOnChain += std::ranges::binary_search(sortedConstructorFunctions, current);
        if (!program.at(current).has(LayoutFlags::HasHeritage))
            return numberOnChain == sortedConstructorFunctions.size();
        ValueFlowObjectID heritage = m_inputs.sets().onlyMember(m_heritageOfClass[current.index()]);
        FunctionID constructor = heritage ? m_inputs.universe().closureFunction(heritage) : FunctionID();
        current = constructor ? program.at(constructor).constructedClass : LayoutID();
        if (!current)
            return true;
    }
    return true;
}

// For a new.target that is not below the constructor function the run time makes an ordinary object.
void LayoutPicks::findInstancesMadeForOtherConstructorFunctions()
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowSolver& solver = m_inputs.solver();
    ValueSetID newTargetsFromOperands = closuresIn(solver.valueOf(m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands)));
    if (m_inputs.sets().isEmpty(newTargetsFromOperands))
        return;

    uint32_t firstMadeNode = solver.input().numberOfNodes;
    Vector<uint32_t> groupsOfConstructions = findIndicesInParallel(m_inputs.loops(), solver.graph().numberOfNodes() - firstMadeNode, [&](unsigned position) {
        ValueFlowNodeID node(firstMadeNode + position);
        const ValueFlowNodeKey& key = *solver.keyOfMadeNode(node);
        return key.kind == ValueFlowMadeNodeKind::CallGroup && isConstruction(key.callKind()) && solver.firstOfMadeNodesAt(solver.indexOfMadeNodes(key)) == node;
    });
    for (uint32_t position : groupsOfConstructions) {
        ValueFlowNodeID node(firstMadeNode + position);
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        ValueFlowCallGroup group = callGroupAt(node);

        Vector<LayoutID, 8> constructorFunctions;
        // A function without a layout has no class that new.target could be below.
        bool constructsFunctionWithoutLayout = false;
        m_inputs.sets().forEachMember(closuresIn(key->set), [&](ValueFlowObjectID callee) {
            const Function& function = program.at(universe.closureFunction(callee));
            if (function.kind != FunctionKind::Plain)
                return;
            if (function.constructedClass)
                constructorFunctions.append(function.constructedClass);
            else
                constructsFunctionWithoutLayout = true;
        });
        if (constructorFunctions.isEmpty() && !constructsFunctionWithoutLayout)
            continue;
        std::ranges::sort(constructorFunctions);
        constructorFunctions.shrink(constructorFunctions.size() - std::ranges::unique(constructorFunctions).size());

        m_inputs.sets().forEachMember(m_maker.intersect(closuresIn(solver.valueOf(group.newTarget)), newTargetsFromOperands), [&](ValueFlowObjectID newTarget) {
            LayoutID layout = program.at(universe.closureFunction(newTarget)).constructedClass;
            if (!layout || (!constructsFunctionWithoutLayout && getsOwnLayoutFromEach(layout, constructorFunctions.span())))
                return;
            auto instance = m_layoutsOfSites.find(universe.instanceOf(layout).index());
            if (instance != m_layoutsOfSites.end())
                instance->value.mayBeOtherObject = true;
        });
    }
}

ValueFlowPositionCounts::LayoutsOfSites LayoutPicks::find()
{
    listSites();
    listInstances();
    if (m_layoutsOfSites.isEmpty())
        return { };
    collectOperands();
    for (const Allocation& allocation : m_allocations)
        notePicksOf(allocation);
    findInstancesMadeForOtherConstructorFunctions();
    return WTF::move(m_layoutsOfSites);
}

ValueFlowPositionCounts::ValueFlowPositionCounts(Vector<String>* lines, ValueFlowTypesOfSets typesOfSets)
    : listsThem(lines)
    , sharesTypesOfSets(typesOfSets == ValueFlowTypesOfSets::StayTheSame)
    , lines(lines)
{
}

ValueFlowPositionCounts::~ValueFlowPositionCounts() = default;

void ValueFlowPositionCounts::note(const Program& program, const String& place, const Type& held, const Type& family)
{
    StringPrintStream line;
    line.print(place, " holds "_s);
    printType(line, program, held);
    line.print(", declared "_s);
    printType(line, program, family);
    Locker locker { lock };
    lines->append(line.toString());
}

const ValueFlowSharedTypeOfSet* ValueFlowPositionCounts::findTypeOfSet(ValueSetID set)
{
    TypesOfSets& types = typesOfSetsFor(set);
    Locker locker { types.lock };
    auto found = types.bySet.find(set.index());
    return found == types.bySet.end() ? nullptr : found->value.get();
}

void ValueFlowPositionCounts::addTypeOfSet(ValueSetID set, std::unique_ptr<ValueFlowSharedTypeOfSet>&& type)
{
    const Type& listed = type->withoutLayoutsAndInstances;
    size_t bytes = sizeof(ValueFlowSharedTypeOfSet) + listed.stringLiterals.span().size_bytes() + listed.symbolLiterals.span().size_bytes() + listed.functions.span().size_bytes() + listed.builtins.span().size_bytes()
        + type->layouts.sizeInBytes() + type->instances.sizeInBytes();
    if (m_bytesOfTypesOfSets.load(std::memory_order_relaxed) + bytes > mostBytesOfTypesOfSets)
        return;
    TypesOfSets& types = typesOfSetsFor(set);
    Locker locker { types.lock };
    if (types.bySet.add(set.index(), WTF::move(type)).isNewEntry)
        m_bytesOfTypesOfSets.fetch_add(bytes, std::memory_order_relaxed);
}

ValueFlowLayoutsOfSites findLayoutsOfSites(const ValueFlowResultInputs& inputs)
{
    BorrowedValueSetMaker maker(inputs.makerPool());
    return { LayoutPicks(inputs, maker.get()).find() };
}

const ValueFlowPositionCounts::LayoutsOfSites& ValueFlowPositionCounts::layoutsOfSites(const ValueFlowResultInputs& inputs)
{
    if (inputs.layoutsOfSites)
        return inputs.layoutsOfSites->bySite;
    std::call_once(m_findLayoutsOfSites, [&] {
        m_layoutsOfSites = findLayoutsOfSites(inputs).bySite;
    });
    return m_layoutsOfSites;
}

const ValueFlowBirthsOfUnknownObject* ValueFlowPositionCounts::births(const ValueFlowResultInputs& inputs)
{
    if (!inputs.solver().input().remembersWhoLoggedEachEdge)
        return nullptr;
    std::call_once(m_findBirths, [&] {
        m_births.emplace(inputs);
    });
    return &*m_births;
}

ValueFlowPositions::ValueFlowPositions(const ValueFlowResultInputs& inputs, TypeArena& types, ValueFlowPositionCounts& counts, uint32_t switchSubject, ValueSetMaker* makerOfThread)
    : m_inputs(inputs)
    , m_types(types)
    , m_counts(counts)
    , m_switchSubject(switchSubject)
    , m_maker(makerOfThread)
{
}

ValueFlowPositions::~ValueFlowPositions()
{
    if (m_hasBorrowedMaker)
        m_inputs.makerPool().giveBack(*m_maker);
}

ValueSetMaker& ValueFlowPositions::maker()
{
    if (!m_maker) {
        m_maker = &m_inputs.makerPool().take();
        m_hasBorrowedMaker = true;
    }
    return *m_maker;
}

const ValueFlowLayoutsOfSite* ValueFlowPositions::findLayoutsPickedBy(ValueFlowObjectID object)
{
    const ValueFlowPositionCounts::LayoutsOfSites& layoutsOfSites = m_counts.layoutsOfSites(m_inputs);
    auto found = layoutsOfSites.find(object.index());
    return found == layoutsOfSites.end() ? nullptr : &found->value;
}

bool ValueFlowPositions::isSurelyInside(ValueSetID set, const Type& type)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID picking = universe.objectsThatPickTheirLayout();
    // The universe remembers the members by the number of the type. To unite them again takes longer than all else that is asked of a check.
    std::optional<TypeID> number = program.types.find(type);
    ValueSetID surelyInside = number ? universe.membersSurelyInside(maker(), program, *number) : universe.membersSurelyInside(maker(), program, type);
    if (!m_inputs.sets().isSubset(maker().intersect(set, ValueSets::complement(picking)), surelyInside))
        return false;
    if (type.anyObject)
        return true;

    std::span<const LayoutID> members = membersOf(program, type);
    bool isInside = true;
    m_inputs.sets().forEachMember(maker().intersect(set, picking), [&](ValueFlowObjectID object) {
        const ValueFlowLayoutsOfSite* picked = findLayoutsPickedBy(object);
        isInside &= picked && !picked->mayBeOtherObject && std::ranges::all_of(picked->layouts, [&](LayoutID layout) {
            return std::ranges::binary_search(members, layout);
        });
    });
    return isInside;
}

// A check tells what the type expects from all else, which goes on by name behind it.
std::optional<Type> ValueFlowPositions::typeWhereNothingPasses(ValueSetID set, const Type& type)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (ValueSets::isComplement(set) || holds(set, ValueFlowUniverse::unknownObject()) || holds(set, ValueFlowUniverse::untrackedObject()) || !type.isJavaScriptValue())
        return std::nullopt;
    if (!m_inputs.sets().meet(set, universe.objects()))
        return std::nullopt;
    bool mayPass = false;
    m_inputs.sets().forEachMember(set, [&](ValueFlowObjectID member) {
        if (mayPass || member == ValueFlowUniverse::primitive(PrimitiveSet::Undefined) || member == ValueFlowUniverse::primitive(PrimitiveSet::Null) || member == ValueFlowUniverse::primitive(PrimitiveSet::Absent))
            return;
        Type typeOfMemberAlone = typeOfMember(member);
        // An object of which nothing says what it is may be made as the type expects it: what a parser answers.
        mayPass = typeOfMemberAlone.anyObject || typeOfMemberAlone.isSubsetOf(type, program) || ((!universe.isObject(member) || findLayoutsPickedBy(member)) && typeOfMemberAlone.overlaps(type, program));
    });
    if (mayPass)
        return std::nullopt;
    return withinLimits(typeOf(set));
}

bool ValueFlowPositions::holdsObjectThatMayLieOutside(ValueSetID set, const Type& type)
{
    if (type.anyObject)
        return false;
    if (holds(set, ValueFlowUniverse::untrackedObject()))
        return true;

    std::span<const LayoutID> members = membersOf(m_inputs.program, type);
    bool mayLieOutside = false;
    m_inputs.sets().forEachMember(maker().intersect(set, m_inputs.universe().objectsThatPickTheirLayout()), [&](ValueFlowObjectID object) {
        const ValueFlowLayoutsOfSite* picked = findLayoutsPickedBy(object);
        mayLieOutside |= picked && (picked->mayBeOtherObject || !std::ranges::all_of(picked->layouts, [&](LayoutID layout) {
            return std::ranges::binary_search(members, layout);
        }));
    });
    return mayLieOutside;
}

// A compiled object keeps its shape for life, so an object that is allocated with a layout has that layout wherever it arrives.
void ValueFlowPositions::addObjectWithLayout(Type& type, Vector<LayoutID, 8>& layouts, ValueFlowObjectID member, LayoutID layout)
{
    if (const ValueFlowSite* site = m_inputs.universe().siteOf(member)) {
        switch (site->origin) {
        case ValueFlowSiteOrigin::Operation:
        case ValueFlowSiteOrigin::DataNode:
        case ValueFlowSiteOrigin::TypedResult:
            break;
        case ValueFlowSiteOrigin::BuiltinResult:
            // What the constructor of a built-in class makes for a class of the program is an object of the built-in class, without slots.
            type.anyObject = type.anyFunction = true;
            return;
        }
    }
    const ValueFlowLayoutsOfSite* picked = findLayoutsPickedBy(member);
    if (!picked) {
        layouts.append(layout);
        return;
    }
    layouts.appendVector(picked->layouts);
    if (picked->mayBeOtherObject)
        type.anyObject = type.anyFunction = true;
}

bool ValueFlowPositions::addInstanceOfClass(Type& type, ValueFlowObjectID member, BuiltinClassID builtinClass)
{
    Type instance = Type::builtinClass(builtinClass);
    ClassPart& part = instance.classes[0];
    if (const ValueFlowContainerFacts* facts = m_inputs.containers ? m_inputs.containers->find(member) : nullptr) {
        // The parsers of a typed result are made from the types of the fields, which say what an array is expected to hold. Beside the arrays of the generic parser the proof says "anything", and it would take the place of that.
        const ValueFlowSite* allocation = m_inputs.universe().siteOf(member);
        bool isArrayOfTypedResult = allocation && allocation->origin == ValueFlowSiteOrigin::TypedResult;
        if (isEnabled(FlowResultKind::ProvenContents) && !isArrayOfTypedResult) {
            part.contents = facts->contents;
            part.contentsAreProven = facts->contentsAreProven;
        }
        // The kind rests on what is stored.
        if (isEnabled(FlowResultKind::ArrayStorageKinds) && part.contentsAreProven)
            part.arrayStorage = facts->arrayStorage;
        if (isEnabled(FlowResultKind::PristineInstances))
            part.provenPristine = facts->pristine;
    }
    // The type of a literal names the pattern and the flags that the literal is created with. Beside a RegExp that something else creates the union names none.
    bool isRegExp = builtinClass == BuiltinClasses::RegExp;
    const ValueFlowSite* site = isRegExp ? m_inputs.universe().siteOf(member) : nullptr;
    if (site && site->origin == ValueFlowSiteOrigin::Operation) {
        const Value& creation = m_inputs.program.at(m_inputs.universe().functionOf(member)).at(site->value);
        if (creation.opcode == Opcode::NewRegExp)
            instance.regExpLiteralSet.add({ creation.pattern(), creation.regExpFlags().toRaw() });
    }
    const ClassPart* listed = type.findClassPart(builtinClass);
    if (listed && *listed == part && (!isRegExp || type.regExpLiteralSet == instance.regExpLiteralSet))
        return false;
    Type united = type.unionWith(instance, m_types);
    bool saysSomethingElse = united.classes != type.classes || united.regExpLiteralSet != type.regExpLiteralSet;
    type = WTF::move(united);
    return saysSomethingElse;
}

void ValueFlowPositions::add(Type& type, Vector<LayoutID, 8>& layouts, ValueFlowObjectID member)
{
    if (BuiltinClassID builtinClass = addUnlessInstanceOfClass(type, layouts, member))
        addInstanceOfClass(type, member, builtinClass);
}

BuiltinClassID ValueFlowPositions::addUnlessInstanceOfClass(Type& type, Vector<LayoutID, 8>& layouts, ValueFlowObjectID member)
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = m_inputs.universe();

    switch (universe.kindOf(member)) {
    case ValueFlowObjectKind::Primitive: {
        uint16_t kind = static_cast<uint16_t>(1u << member.index());
        if (kind == Primitive::LiteralString)
            kind = Primitive::String;
        else if (kind == Primitive::LiteralSymbol)
            kind = Primitive::Symbol;
        type.primitives |= kind & Primitive::Defined;
        // A union on the way takes integers without a range for none.
        if (kind == Primitive::Integer)
            type.integers = Range::full();
        return { };
    }
    case ValueFlowObjectKind::Key: {
        PropertyKeyID id = universe.keyOf(member);
        const PropertyKey& key = program.at(id);
        if (key.kind == PropertyKeyKind::Text)
            type.stringLiterals.add(key.text);
        else if (key.kind == PropertyKeyKind::RegisteredSymbol || key.kind == PropertyKeyKind::WellKnownSymbol
            || (key.kind == PropertyKeyKind::CreatedSymbol && m_inputs.symbolsCreatedOnce && m_inputs.symbolsCreatedOnce->get(id.index()))) {
            // Any other symbol that an operation creates is another one each time the operation runs.
            type.symbolLiterals.add(id);
            type.primitives |= Primitive::LiteralSymbol;
        } else
            type.primitives |= isSymbolKind(key.kind) ? Primitive::Symbol : Primitive::Symbol | Primitive::String;
        return { };
    }
    case ValueFlowObjectKind::Closure: {
        FunctionID function = universe.closureFunction(member);
        const RuntimeFunctionEntry* entry = findRuntimeFunction(program, program.at(function).enclosingRuntimeFunction);
        if (entry && entry->function == function && !m_inputs.isReached(function))
            type.anyFunction = true;
        else
            type.functions.add(function);
        return { };
    }
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        // What stands for all that a native binding answers is no object of the realm, and no type names it. Neither is what stands above an object whose prototype link nobody knows.
        BuiltinID builtin = builtins.canonicalID(universe.builtinOf(member));
        if (isObjectKind(builtins.at(builtin).kind) && builtin != Builtins::UndeclaredNativeCode)
            type.builtins.add(builtin);
        else
            type.anyObject = type.anyFunction = true;
        return { };
    }
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::Site: {
        if (LayoutID layout = universe.layoutOf(member); layout && !m_inputs.isLayoutOfDictionaries(layout)) {
            addObjectWithLayout(type, layouts, member, layout);
            return { };
        }
        BuiltinClassID builtinClass = m_inputs.classOf(member);
        if (!builtinClass)
            type.anyObject = type.anyFunction = true;
        else if (builtins.at(builtinClass).hasClosureInstances)
            type.anyFunction = true;
        else
            return builtinClass;
        return { };
    }
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
        type.anyObject = true;
        return { };
    case ValueFlowObjectKind::Namespace:
        // The namespace object of a module of any other kind gets its names when it is linked.
        if (LayoutID layout = program.layoutOfWholeNamespace(universe.moduleOf(member))) {
            layouts.append(layout);
            return { };
        }
        type.anyObject = type.anyFunction = true;
        return { };
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
        type.anyObject = type.anyFunction = true;
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type ValueFlowPositions::finish(Type&& type, std::span<const LayoutID> layouts)
{
    type.canonicalize();
    if (!layouts.empty() && !type.anyObject)
        type.layouts = m_types.completeLayoutSet(layouts);
    return WTF::move(type);
}

Type ValueFlowPositions::typeOfMember(ValueFlowObjectID member)
{
    Type type;
    Vector<LayoutID, 8> layouts;
    add(type, layouts, member);
    return finish(WTF::move(type), layouts.span());
}

// Whoever asks has dealt with the placeholders. The untracked object is passed over: what it stands for depends on the family.
Type ValueFlowPositions::typeOf(ValueSetID set)
{
    auto found = m_indexOfSet.find(set.index());
    if (found != m_indexOfSet.end())
        return m_typesOfSets[found->value];

    auto make = [&] {
        if (!sharesTypeOf(set))
            return typeOfMembers(set);
        const ValueFlowSharedTypeOfSet* shared = m_counts.findTypeOfSet(set);
        return shared ? typeFrom(*shared) : typeOfMembersToShare(set);
    };
    Type type = make();
    m_indexOfSet.add(set.index(), m_typesOfSets.size());
    m_typesOfSets.append(type);
    return type;
}

// What a switch turns off for one subject is left out of the types of that subject alone.
bool ValueFlowPositions::sharesTypeOf(ValueSetID set) const
{
    constexpr uint32_t fewestMembersOfSharedSet = 32;
    return m_counts.sharesTypesOfSets && m_inputs.sets().size(set) >= fewestMembersOfSharedSet
        && isEnabled(FlowResultKind::ProvenContents) && isEnabled(FlowResultKind::ArrayStorageKinds) && isEnabled(FlowResultKind::PristineInstances);
}

// More texts than a type lists make a string: see withinLimits(). They need no order for that.
void ValueFlowPositions::listTexts(Type& type, std::span<const StringID> texts)
{
    if (texts.size() > m_inputs.build.limit(LimitID::MaxLiteralsInType))
        type.primitives |= Primitive::String;
    else if (!texts.empty())
        type.stringLiterals = SortedSet<StringID>(texts);
}

Type ValueFlowPositions::typeOfMembers(ValueSetID set)
{
    Type type;
    Vector<LayoutID, 8> layouts;
    // A set may hold thousands of keys, in the order of their numbers and not of their texts. They are sorted once.
    const ValueFlowUniverse& universe = m_inputs.universe();
    Vector<StringID, 16> texts;
    m_inputs.sets().forEachMember(set, [&](ValueFlowObjectID member) {
        if (member == ValueFlowUniverse::untrackedObject())
            return;
        if (universe.kindOf(member) == ValueFlowObjectKind::Key) {
            const PropertyKey& key = m_inputs.program.at(universe.keyOf(member));
            if (key.kind == PropertyKeyKind::Text) {
                texts.append(key.text);
                return;
            }
        }
        add(type, layouts, member);
    });
    listTexts(type, texts.span());
    return finish(WTF::move(type), layouts.span());
}

// The instances are united by themselves, in a type that holds nothing else.
Type ValueFlowPositions::typeOfMembersToShare(ValueSetID set)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    auto shared = makeUnique<ValueFlowSharedTypeOfSet>();
    Type& type = shared->withoutLayoutsAndInstances;
    Type instances;
    Vector<LayoutID, 8> layouts;
    Vector<StringID, 16> texts;
    bool hasMetInstance = false;
    // The primitives have the lowest numbers and the keys the highest. An object adds nothing to any object, once no classes are left that a union could ask the arena about.
    m_inputs.sets().anyMember(maker().intersect(set, ValueSets::complement(universe.keys())), [&](ValueFlowObjectID member) {
        if (member == ValueFlowUniverse::untrackedObject())
            return false;
        BuiltinClassID builtinClass = addUnlessInstanceOfClass(type, layouts, member);
        if (!builtinClass)
            return type.anyObject && instances.classes.isEmpty();
        if (!std::exchange(hasMetInstance, true) && !type.isNever()) {
            shared->firstInstanceMeetsOtherMembers = true;
            instances.primitives = Primitive::Undefined;
        }
        if (type.anyObject && !instances.anyObject) {
            shared->numberOfInstancesBeforeAnyObject = shared->instances.size();
            instances.anyObject = true;
        }
        if (addInstanceOfClass(instances, member, builtinClass))
            shared->instances.append({ member, builtinClass });
        return instances.anyObject && instances.classes.isEmpty();
    });
    m_inputs.sets().forEachMember(maker().intersect(set, universe.keys()), [&](ValueFlowObjectID member) {
        const PropertyKey& key = m_inputs.program.at(universe.keyOf(member));
        if (key.kind != PropertyKeyKind::Text)
            addUnlessInstanceOfClass(type, layouts, member);
        else if (!(type.primitives & Primitive::String))
            texts.append(key.text);
    });
    listTexts(type, texts.span());
    type.canonicalize();
    if (!type.anyObject) {
        std::ranges::sort(layouts);
        shared->layouts.append(layouts.span().first(layouts.size() - std::ranges::unique(layouts).size()));
    }

    Type result = type;
    result.classes = WTF::move(instances.classes);
    result.regExpLiteralSet = WTF::move(instances.regExpLiteralSet);
    result = finish(WTF::move(result), shared->layouts.span());
    m_counts.addTypeOfSet(set, WTF::move(shared));
    return result;
}

Type ValueFlowPositions::typeFrom(const ValueFlowSharedTypeOfSet& shared)
{
    Type instances;
    if (shared.firstInstanceMeetsOtherMembers)
        instances.primitives = Primitive::Undefined;
    for (uint32_t index = 0; index < shared.instances.size(); ++index) {
        if (index == shared.numberOfInstancesBeforeAnyObject)
            instances.anyObject = true;
        addInstanceOfClass(instances, shared.instances[index].object, shared.instances[index].builtinClass);
    }
    Type type = shared.withoutLayoutsAndInstances;
    type.classes = WTF::move(instances.classes);
    type.regExpLiteralSet = WTF::move(instances.regExpLiteralSet);
    return finish(WTF::move(type), shared.layouts.span());
}

// A part that is left out is widened, so that the meet with the family leaves that part of the family as it is.
Type ValueFlowPositions::withinLimits(Type&& type)
{
    BuildContext& build = m_inputs.build;
    bool namesLayouts = isEnabled(FlowResultKind::ProvenLayoutSets) && membersOf(m_inputs.program, type).size() <= build.limit(LimitID::MaxLayoutsInType);
    if (type.layouts && !namesLayouts)
        type.anyObject = type.anyFunction = true;

    bool listsLiterals = isEnabled(FlowResultKind::LiteralValues);
    if (!type.stringLiterals.isEmpty() && (!listsLiterals || type.stringLiterals.size() > build.limit(LimitID::MaxLiteralsInType)))
        type.primitives |= Primitive::String;
    if (!type.symbolLiterals.isEmpty() && !listsLiterals)
        type.primitives |= Primitive::Symbol;
    if (!listsLiterals || type.regExpLiteralSet.size() > build.limit(LimitID::MaxRegExpLiteralsInType))
        type.regExpLiteralSet.clear();
    if (!type.functions.isEmpty() && (!isEnabled(FlowResultKind::ExactFunctions) || type.functions.size() > build.limit(LimitID::MaxFunctionsInType)))
        type.anyFunction = true;
    if (type.builtins.size() > build.limit(LimitID::MaxBuiltinsInType))
        type.anyObject = type.anyFunction = true;
    type.canonicalize();
    return WTF::move(type);
}

ValueSetID ValueFlowPositions::expand(ValueSetID set, const Type& family)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (!holds(set, ValueFlowUniverse::untrackedObject()) || !universe.namesProgramObjectsAlone(m_inputs.program, family))
        return set;
    ValueSetID listed = maker().intersect(set, ValueSets::complement(universe.untrackedObjectAlone()));
    return maker().unite(listed, universe.familyObjectsOf(maker(), m_inputs.program, family));
}

// A member that lies outside whatever it is comes before one of which only a part may lie outside.
ValueFlowObjectID ValueFlowPositions::findStranger(ValueSetID set, const Type& family, Trust trust)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueFlowObjectID outside;
    ValueFlowObjectID undecided;
    // Thousands of objects share a few layouts, and the type of an object that has its layout for sure is that layout.
    BitVector layoutsInside;
    auto sureLayoutOf = [&](ValueFlowObjectID member) {
        if (universe.kindOf(member) != ValueFlowObjectKind::Site && universe.kindOf(member) != ValueFlowObjectKind::Instance)
            return LayoutID();
        LayoutID layout = universe.layoutOf(member);
        if (!layout || m_inputs.isLayoutOfDictionaries(layout) || findLayoutsPickedBy(member))
            return LayoutID();
        const ValueFlowSite* site = universe.siteOf(member);
        return site && site->origin == ValueFlowSiteOrigin::BuiltinResult ? LayoutID() : layout;
    };
    m_inputs.sets().forEachMember(trust >= Trust::Primitives ? maker().intersect(set, universe.objects()) : set, [&](ValueFlowObjectID member) {
        if (outside || member == ValueFlowUniverse::untrackedObject())
            return;
        LayoutID sureLayout = sureLayoutOf(member);
        if (sureLayout && layoutsInside.get(sureLayout.index()))
            return;
        Type type = typeOfMember(member);
        if (type.isSubsetOf(family, program)) {
            if (sureLayout)
                layoutsInside.set(sureLayout.index());
            return;
        }
        if (!findLayoutsPickedBy(member) || !type.overlaps(family, program))
            outside = member;
        else if (!undecided)
            undecided = member;
    });
    return outside ? outside : undecided;
}

// What held.withOnlyObjects().isSubsetOf(family, program) answers. The lists of literals may be long, and this is asked of every position: they are set aside, not copied.
static bool holdsOnlyObjectsOf(Type& held, const Type& family, const Program& program)
{
    uint16_t primitives = std::exchange(held.primitives, 0);
    Range integers = std::exchange(held.integers, Range::empty());
    SortedSet<StringID> stringLiterals = std::exchange(held.stringLiterals, { });
    SortedSet<PropertyKeyID> symbolLiterals = std::exchange(held.symbolLiterals, { });
    bool isSubset = held.isSubsetOf(family, program);
    held.primitives = primitives;
    held.integers = integers;
    held.stringLiterals = WTF::move(stringLiterals);
    held.symbolLiterals = WTF::move(symbolLiterals);
    return isSubset;
}

// What the type as read becomes when the results are applied.
std::optional<Type> ValueFlowPositions::familyBehindDictionaries(const Type& family)
{
    if (m_inputs.typesBehindDictionaries.empty())
        return std::nullopt;
    // A family may list hundreds of layouts, and this is asked of every position.
    auto namesDictionaries = [&](LayoutSetID set) {
        if (!TypeTable::isProvisional(set) && set.index() < m_inputs.layoutSetsNamingDictionaries->size())
            return m_inputs.layoutSetsNamingDictionaries->quickGet(set.index());
        return std::ranges::any_of(m_inputs.program.at(set).members, [&](LayoutID member) { return m_inputs.isLayoutOfDictionaries(member); });
    };
    bool isChanged = family.layouts && namesDictionaries(family.layouts);
    for (const ClassPart& part : family.classes) {
        for (TypeID contents : part.contents)
            isChanged |= typeBehindDictionaries(m_inputs.typesBehindDictionaries, contents) != contents;
    }
    if (!isChanged)
        return std::nullopt;
    return typeBehindDictionaries(m_inputs.program, *m_inputs.layoutsOfDictionaries, m_inputs.typesBehindDictionaries, family, m_types);
}

ValueFlowPositionResult ValueFlowPositions::decide(const Type& familyAsRead, ValueSetID set, DeadZone deadZone, Trust trust, const ValueFlowNumbers* numbers, NOESCAPE const Invocable<String()> auto& place)
{
    const Program& program = m_inputs.program;
    std::optional<Type> familyBehind = familyBehindDictionaries(familyAsRead);
    const Type& family = familyBehind ? *familyBehind : familyAsRead;
    // An environment and an opaque word are no values of the language, and no set stands for one.
    if (!family.isJavaScriptValue() || m_inputs.sets().isEmpty(set))
        return { };
    if (!isEnabled(FlowResultKind::NarrowedTypes))
        return { .proof = ValueFlowProof::Disabled };
    bool holdsUnknownValue = ValueSets::isComplement(set) || holds(set, ValueFlowUniverse::unknownObject());
    bool holdsUntrackedObject = holds(set, ValueFlowUniverse::untrackedObject());
    // Whatever a placeholder stands for is a value of the language, so it lies inside a family that takes them all. Beyond them such a family holds `absent` alone, which no placeholder stands for.
    bool hasDeadZoneToRemove = family.primitives & Primitive::Absent && deadZone != DeadZone::MayBeObserved && isEnabled(FlowResultKind::Initialization);
    if ((holdsUnknownValue || holdsUntrackedObject) && family.isAnything() && hasDeadZoneToRemove)
        return { .proof = ValueFlowProof::Proven, .narrowed = family.withoutPrimitives(Primitive::Absent) };
    if (holdsUnknownValue)
        return { .proof = ValueFlowProof::IllTyped };
    // Nothing keeps an object that nobody follows inside a type that is only expected.
    if (holdsUntrackedObject && (trust == Trust::Nothing || !m_inputs.universe().namesProgramObjectsAlone(program, family)))
        return { .proof = ValueFlowProof::Untracked };

    Type held = typeOf(set);
    if (numbers)
        numbers->narrow(held);
    if (deadZone == DeadZone::UndefinedAtStartIsNeverObserved)
        held.primitives &= ~Primitive::Undefined;
    if (deadZone == DeadZone::UndefinedAtStartMayBeObserved)
        held.primitives |= Primitive::Undefined;
    if (!isEnabled(FlowResultKind::RemovedNullish))
        held.primitives |= family.primitives & Primitive::UndefinedOrNull;
    if (deadZone != DeadZone::MayBeObserved)
        held.primitives &= ~Primitive::Absent;
    if (!isEnabled(FlowResultKind::Initialization))
        held.primitives |= family.primitives & Primitive::Absent;

    // The meet would drop a value that is there.
    bool isInside = trust == Trust::PassedTest || (trust >= Trust::Primitives ? holdsOnlyObjectsOf(held, family, program) : held.isSubsetOf(family, program));
    // The range of an expected number is a guess. Where integers arrive, alone or beside what the family takes as it is, the position holds those, and the check behind it tests one of them against the guess.
    auto liesOutsideGuessedRangeAlone = [&] {
        if (held.isInteger() && family.isNumber())
            return true;
        if (!(held.primitives & Primitive::Integer) || !(family.primitives & Primitive::Integer))
            return false;
        Type withinGuessedRange = held;
        withinGuessedRange.integers = family.integers;
        return withinGuessedRange.isSubsetOf(family, program);
    };
    if (!isInside && trust == Trust::Nothing && liesOutsideGuessedRangeAlone())
        return { .proof = ValueFlowProof::Proven, .narrowed = WTF::move(held) };
    if (!isInside) {
        ValueFlowObjectID stranger = findStranger(set, family, trust);
        bool isUndecided = stranger && findLayoutsPickedBy(stranger) && typeOfMember(stranger).overlaps(family, program);
        if (!isUndecided) {
            m_counts.count.fetch_add(1, std::memory_order_relaxed);
            if (m_counts.listsThem)
                m_counts.note(program, place(), held, family);
            return { .proof = ValueFlowProof::OutsideFamily, .stranger = stranger };
        }
        // Of all that such an allocation makes only what lies inside gets into a position that is checked.
        if (trust < Trust::Position)
            return { .proof = ValueFlowProof::UndecidedLayout, .stranger = stranger };
    }

    // Inside such a family the untracked object is one of the objects that the family lists, whichever.
    if (holdsUntrackedObject)
        held = held.unionWith(family.withOnlyObjects(), m_types);
    Type narrowed = withinLimits(WTF::move(held)).intersectWith(family, m_types);
    if (narrowed.isNever() || (narrowed == family && !familyBehind))
        return { .proof = ValueFlowProof::Proven };
    return { .proof = ValueFlowProof::Proven, .narrowed = WTF::move(narrowed) };
}

ValueFlowPositionResult ValueFlowPositions::resultAt(const Type& family, ValueSetID set, DeadZone deadZone, const ValueFlowNumbers* numbers)
{
    return decide(family, set, deadZone, Trust::Primitives, numbers, [&] {
        return makeString("subject "_s, m_switchSubject);
    });
}

ValueFlowPositionResult ValueFlowPositions::resultAt(const DeclaredType& declared, ValueSetID set, DeadZone deadZone)
{
    return decide(m_inputs.program.at(declared.type), set, deadZone, declared.flags.contains(PositionFlags::IsExpected) ? Trust::Nothing : Trust::Position, nullptr, [&] {
        return makeString("subject "_s, m_switchSubject);
    });
}

FlowPositionUpdate ValueFlowPositions::updateFor(const DeclaredType& declared, ValueFlowNodeID node, DeadZone deadZone, const ValueFlowNumbers* numbers)
{
    const ValueFlowSolver& solver = m_inputs.solver();
    const Type& family = m_inputs.program.at(declared.type);
    Trust trust = declared.flags.contains(PositionFlags::IsExpected) ? Trust::Nothing : Trust::Position;
    if (trust == Trust::Position && solver.graph().maskKindOf(node) == ValueFlowMaskKind::Checked)
        trust = Trust::PassedTest;
    ValueFlowPositionResult result = decide(family, solver.valueOf(node), deadZone, trust, numbers, [&] {
        return placeOf(node);
    });
    // The solver compares with the mask of the node, which is made from the same type.
    if (result.proof == ValueFlowProof::Proven && solver.holdsObjectOutsideFamily(node))
        result = { .proof = ValueFlowProof::OutsideFamily };

    FlowPositionUpdate update;
    update.isProven = result.proof == ValueFlowProof::Proven;
    if (family.hasObjects() && result.proof != ValueFlowProof::NothingArrives)
        (update.isProven ? m_counts.provenPositions : m_counts.unprovenPositions).fetch_add(1, std::memory_order_relaxed);
    // What bounds the numbers holds whatever else arrives, and nobody doubts the type of a position that is more than expected.
    bool isInDoubt = result.proof == ValueFlowProof::IllTyped || result.proof == ValueFlowProof::Untracked || result.proof == ValueFlowProof::OutsideFamily || result.proof == ValueFlowProof::UndecidedLayout;
    if (isInDoubt && numbers && trust != Trust::Nothing) {
        Type narrowed = family;
        numbers->narrow(narrowed);
        if (narrowed != family) {
            m_counts.narrowedPositions.fetch_add(1, std::memory_order_relaxed);
            update.isProven = true;
            update.type = m_types.intern(narrowed);
            return update;
        }
    }
    if (!update.isProven) {
        FlowSiteLoss loss = lossOf(result, node);
        update.cause = loss.cause;
        update.witness = loss.witness;
        if (family.isJavaScriptValue() && family.primitives & Primitive::Absent && deadZone != DeadZone::MayBeObserved && isEnabled(FlowResultKind::Initialization))
            update.type = m_types.intern(family.withoutPrimitives(Primitive::Absent));
        return update;
    }
    if (result.narrowed) {
        m_counts.narrowedPositions.fetch_add(1, std::memory_order_relaxed);
        update.type = m_types.intern(*result.narrowed);
    }
    return update;
}

std::optional<LayoutSet> ValueFlowPositions::receiversAt(const Type& family, ValueSetID set)
{
    const Program& program = m_inputs.program;
    if (ValueSets::isComplement(set) || holds(set, ValueFlowUniverse::unknownObject()))
        return std::nullopt;
    bool holdsUntrackedObject = holds(set, ValueFlowUniverse::untrackedObject());
    if (holdsUntrackedObject && !m_inputs.universe().namesProgramObjectsAlone(program, family))
        return std::nullopt;

    Type held = typeOf(set);
    if (holdsUntrackedObject)
        held = held.unionWith(family.withOnlyObjects(), m_types);
    if (held.anyObject || held.anyFunction || !held.classes.isEmpty() || !held.functions.isEmpty() || !held.builtins.isEmpty())
        return std::nullopt;
    LayoutSet receivers;
    receivers.members.append(membersOf(program, held));
    receivers.isComplete = true;
    canonicalize(receivers);
    return receivers;
}

static bool throwsBeforeEntry(const Function& callee, bool isConstruction)
{
    if (isConstruction)
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    return callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
}

// A bound function, a proxy and whatever else can be called enter code that nobody lists, so an object that is neither a closure nor a built-in function leaves the set open.
ValueSetID ValueFlowPositions::withoutWhatPlaceholdersCover(ValueSetID set)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    auto leaveOut = [&](ValueFlowObjectID placeholder, ValueSetID placeholderAlone, ValueFlowNodeID covered) {
        if (holds(set, placeholder))
            set = maker().unite(maker().intersect(set, ValueSets::complement(m_inputs.solver().valueOf(covered))), placeholderAlone);
    };
    leaveOut(ValueFlowUniverse::unknownObject(), universe.unknownObjectAlone(), ValueFlowProgramNodes::escaped());
    leaveOut(ValueFlowUniverse::untrackedObject(), universe.untrackedObjectAlone(), ValueFlowProgramNodes::lost());
    return set;
}

std::optional<CalleeSet> ValueFlowPositions::calleesAt(const Type& family, ValueSetID set, bool isConstruction)
{
    if (ValueSets::isComplement(set))
        return std::nullopt;

    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID arriving = expand(set, family);
    ValueSetID listed = withoutWhatPlaceholdersCover(arriving);
    // The pass that hands the object it has allocated to the constructor above tests the link to that constructor at run time, and it finds in the list which version of the constructor is entered.
    if (isConstruction && m_inputs.sets().size(arriving) - m_inputs.sets().size(listed) <= m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
        listed = arriving;

    CalleeSet callees;
    callees.isComplete = true;
    m_inputs.sets().forEachMember(listed, [&](ValueFlowObjectID member) {
        if (!universe.isObject(member))
            return;
        if (FunctionID function = universe.closureFunction(member)) {
            if (!throwsBeforeEntry(program.at(function), isConstruction))
                callees.functions.append(function);
            return;
        }
        if (holds(universe.builtinFunctions(), member)) {
            callees.builtins.append(program.builtins().canonicalID(universe.builtinOf(member)));
            return;
        }
        callees.isComplete = false;
    });
    canonicalize(callees);
    return callees;
}

// What a call of JSON.parse or its like has made: a built-in whose row answers plain data is among the callees of the call that the object stands for.
static bool isDataFromOutside(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowSite* site = universe.isObject(object) ? universe.siteOf(object) : nullptr;
    FunctionID function = site ? universe.functionOf(object) : FunctionID();
    if (!function || site->origin != ValueFlowSiteOrigin::BuiltinResult || !site->value)
        return false;
    bool answersPlainData = false;
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(function.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.site != site->value)
            continue;
        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
        if (operands.empty() || !operands[0].node)
            continue;
        ValueSetID callees = inputs.solver().valueOf(operands[0].node);
        if (ValueSets::isComplement(callees))
            continue;
        inputs.sets().forEachMember(callees, [&](ValueFlowObjectID callee) {
            BuiltinID builtin = universe.isObject(callee) ? universe.builtinOf(callee) : BuiltinID();
            if (!builtin)
                return;
            for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(builtin)))
                answersPlainData |= overload.result.type && builtins.at(overload.result.type).flags.contains(BuiltinTypeFlag::HasPlainData);
        });
    }
    return answersPlainData;
}

Witness ValueFlowPositions::witnessOfStranger(const ValueFlowPositionResult& result) const
{
    return result.stranger && m_inputs.universe().isObject(result.stranger) ? witnessOfAllocation(m_inputs, result.stranger) : noWitness;
}

FlowSiteLoss ValueFlowPositions::lossOf(const ValueFlowPositionResult& result, ValueFlowNodeID node)
{
    switch (result.proof) {
    case ValueFlowProof::Proven:
    case ValueFlowProof::NothingArrives:
        return { };
    case ValueFlowProof::Disabled:
        return { .cause = UnprovenCause::ResultDisabled };
    case ValueFlowProof::IllTyped: {
        FlowSiteLoss loss { .cause = UnprovenCause::UnknownObject };
        if (!node)
            return loss;
        if (std::optional<ValueFlowUnknownSource> source = m_inputs.solver().unknownSourceOf(node))
            loss.cause = unprovenCauseOf(*source);
        if (const ValueFlowBirthsOfUnknownObject* births = m_counts.births(m_inputs))
            loss.witness = births->witnessOf(node);
        return loss;
    }
    case ValueFlowProof::Untracked:
        return { .cause = UnprovenCause::Untracked };
    case ValueFlowProof::OutsideFamily:
        return { .cause = result.stranger && isDataFromOutside(m_inputs, result.stranger) ? UnprovenCause::DataFromOutside : UnprovenCause::LayoutOutsideFamily, .witness = witnessOfStranger(result) };
    case ValueFlowProof::UndecidedLayout:
        return { .cause = UnprovenCause::OtherNewTarget, .witness = witnessOfStranger(result) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// In the words of the IR text.
String ValueFlowPositions::placeOf(ValueFlowNodeID node) const
{
    const Program& program = m_inputs.program;
    const ValueFlowProgramNodes& nodes = m_inputs.bodies().nodes();
    if (FunctionID function = nodes.functionOf(node); function && function.index() < program.functions.size()) {
        uint32_t firstResult = nodes.result(function, 0).index();
        if (node.index() < firstResult)
            return makeString('f', function.index(), " entry parameter "_s, node.index() - nodes.firstNodeOf(function));
        return makeString('f', function.index(), " result "_s, node.index() - firstResult);
    }
    uint32_t firstVariable = nodes.variable(VariableID(0u)).index();
    if (node.index() >= firstVariable && node.index() - firstVariable < program.variables.size())
        return makeString('g', node.index() - firstVariable);
    for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
        uint32_t firstField = nodes.field(LayoutID(layout), 0).index();
        if (node.index() >= firstField && node.index() - firstField < program.layouts[layout].fields.size())
            return makeString('L', layout, '.', node.index() - firstField);
    }
    return makeString("node "_s, node.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
