/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDefinitions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCallGraph.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTKnownClosures.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t noDefinition = UINT32_MAX;

// An operation that does no more than this with an operand needs no object: a call is direct, a test has its answer in the description, a member is read from the text while the object does not exist.
constexpr OperandUses usesThatNeedNoObject { OperandUse::Inspects, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::ReadsContents };

// The class object is the closure of the constructor. Here the class is a definition of its own, which holds that closure.
ValueID aliasedOperandOf(const Value& value)
{
    return value.opcode == Opcode::NewClass ? ValueID() : sameValueOperandOf(value);
}

struct DefinitionUseCounts {
    uint32_t definition { noDefinition };
    uint32_t past { 0 };
    uint32_t observing { 0 };
};

struct Bindings {
    // noDefinition for a variable that is stored more than once, never, or with something else.
    Vector<uint32_t> definitionOfVariable;
    Vector<uint8_t> numberOfStores;
};

class UseClassifier {
public:
    UseClassifier(const Program& program, FunctionAnalyses& analyses)
        : m_program(program)
        , m_function(analyses.function())
        , m_useLists(analyses.useLists())
    {
    }

    template<typename Functor>
    void forEachUseThroughSameValues(ValueID id, const Functor& functor) const
    {
        for (const Use& use : m_useLists.usesOf(id)) {
            if (use.kind == Use::Kind::Operand && aliasedOperandOf(m_function.at(use.user)) == id) {
                forEachUseThroughSameValues(use.user, functor);
                continue;
            }
            functor(use);
        }
    }

    bool needsObject(const Use& use) const
    {
        switch (use.kind) {
        case Use::Kind::Operand: {
            const Value& user = m_function.at(use.user);
            if (!usesThatNeedNoObject.containsAll(declaredBehaviorOf(user.opcode, use.operandIndex).uses) || isWrittenOperand(user.opcode, use.operandIndex))
                return true;
            // The object under `prototype` names the function that it belongs to.
            return user.opcode == Opcode::PropGet && m_program.nameClassOf(user.key()) == NameClass::Prototype;
        }
        case Use::Kind::TerminatorOperand:
            return !usesThatNeedNoObject.containsAll(declaredBehaviorOf(m_function.at(use.block).terminator.kind, use.operandIndex).uses);
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    const Program& m_program;
    const Function& m_function;
    const UseLists& m_useLists;
};

class ModuleBodyScan {
public:
    ModuleBodyScan(const Program& program, BuildContext& build, FunctionID id, Bindings& bindings, Vector<Definition>& definitions)
        : m_program(program)
        , m_id(id)
        , m_function(program.at(id))
        , m_analyses(AnalysisContext { build, program, nullptr, nullptr, id }, m_function)
        , m_bindings(bindings)
        , m_definitions(definitions)
    {
        m_definitionOfValue.fill(noDefinition, m_function.values.size());
    }

    // True when a binding became known that was not.
    bool findDefinitions(Vector<uint32_t>& definitionOfAllocation, Vector<uint32_t>* unknownOperands = nullptr)
    {
        bool didBind = false;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            bool runsOnce = !m_analyses.isInCycle(block);
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (ValueID same = aliasedOperandOf(value)) {
                    m_definitionOfValue[id.index()] = m_definitionOfValue[same.index()];
                    continue;
                }
                if (value.opcode == Opcode::GetModuleVariable) {
                    m_definitionOfValue[id.index()] = m_bindings.definitionOfVariable[value.variable().index()];
                    continue;
                }
                if (value.opcode == Opcode::GetClassPrototype) {
                    m_definitionOfValue[id.index()] = m_definitionOfValue[value.operand(OperandIndex::GetClassPrototype::classObject).index()];
                    continue;
                }
                if (value.opcode == Opcode::PutModuleVariable) {
                    didBind |= bind(value);
                    continue;
                }
                if (!runsOnce)
                    continue;
                std::optional<DefinitionKind> kind = kindOf(value);
                if (!kind) {
                    if (unknownOperands)
                        countUnknownOperand(value, *unknownOperands);
                    continue;
                }
                uint32_t& known = definitionOfAllocation[id.index()];
                if (known == noDefinition) {
                    known = m_definitions.size();
                    m_definitions.append(Definition { .kind = *kind, .moduleBody = m_id, .allocation = id });
                }
                m_definitionOfValue[id.index()] = known;
            }
        }
        return didBind;
    }

    void countUses()
    {
        UseClassifier classifier(m_program, m_analyses);
        for (const Block& block : m_function.blocks) {
            for (ValueID id : block.values) {
                uint32_t index = m_definitionOfValue[id.index()];
                if (index == noDefinition || aliasedOperandOf(m_function.at(id)))
                    continue;
                classifier.forEachUseThroughSameValues(id, [&](const Use& use) {
                    countUse(classifier, index, use);
                });
            }
        }
    }

private:
    bool isKnown(ValueID id) const
    {
        return !id || m_definitionOfValue[id.index()] != noDefinition || constantThroughCasts(m_program, m_function, id);
    }

    void countUnknownOperand(const Value& value, Vector<uint32_t>& unknownOperands) const
    {
        switch (value.opcode) {
        case Opcode::NewClosure:
        case Opcode::NewObject:
        case Opcode::NewArray:
        case Opcode::NewClass:
            break;
        default:
            return;
        }
        bool isCounted = false;
        value.forEachChild([&](ValueID operand) {
            if (isCounted || isKnown(operand))
                return;
            isCounted = true;
            ++unknownOperands[static_cast<unsigned>(m_function.at(definitionThroughSameValues(m_function, operand)).opcode)];
        });
    }

    bool isNull(ValueID id) const
    {
        std::optional<ValueID> constant = constantThroughCasts(m_program, m_function, id);
        return constant && m_function.at(*constant).opcode == Opcode::Null;
    }

    bool isDefinitionOfKind(ValueID id, std::initializer_list<DefinitionKind> kinds) const
    {
        uint32_t index = m_definitionOfValue[id.index()];
        return index != noDefinition && std::ranges::find(kinds, m_definitions[index].kind) != kinds.end();
    }

    std::optional<DefinitionKind> kindOf(const Value& value) const
    {
        auto allAreKnown = [&](std::span<const ValueID> operands) {
            return std::ranges::all_of(operands, [&](ValueID operand) {
                return isKnown(operand);
            });
        };
        switch (value.opcode) {
        case Opcode::NewClosure:
            return allAreKnown(value.variadicOperands()) ? std::optional(DefinitionKind::Function) : std::nullopt;
        case Opcode::NewRegExp:
            return DefinitionKind::RegExp;
        case Opcode::NewTemplateObject:
            return DefinitionKind::TemplateObject;
        case Opcode::NewClassToken:
            return DefinitionKind::ClassToken;
        case Opcode::NewObject:
            return isKnown(value.operand(OperandIndex::NewObject::prototype)) && allAreKnown(value.variadicOperands()) ? std::optional(DefinitionKind::Object) : std::nullopt;
        case Opcode::NewArray:
            return !value.operand(OperandIndex::NewArray::capacity) && allAreKnown(value.variadicOperands()) ? std::optional(DefinitionKind::Array) : std::nullopt;
        case Opcode::NewClass: {
            if (!isDefinitionOfKind(value.operand(OperandIndex::NewClass::constructor), { DefinitionKind::Function }))
                return std::nullopt;
            ValueID heritage = value.operand(OperandIndex::NewClass::heritage);
            // The `prototype` of a class cannot be replaced. That of a function can, between the definition below it and the first look at that.
            if (heritage && !isDefinitionOfKind(heritage, { DefinitionKind::Class }) && !isNull(heritage))
                return std::nullopt;
            return DefinitionKind::Class;
        }
        default:
            return std::nullopt;
        }
    }

    bool bind(const Value& store)
    {
        uint32_t variable = store.variable().index();
        uint32_t index = m_definitionOfValue[store.operand(OperandIndex::PutModuleVariable::value).index()];
        if (index == noDefinition || m_bindings.numberOfStores[variable] != 1 || m_bindings.definitionOfVariable[variable] == index)
            return false;
        m_bindings.definitionOfVariable[variable] = index;
        if (!m_definitions[index].binding)
            m_definitions[index].binding = store.variable();
        return true;
    }

    // An operation that builds a definition out of this one, or this one out of others, is part of the text of the definition.
    bool isPartOfDefinition(uint32_t index, const Use& use)
    {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = m_function.at(use.user);
        uint32_t holder = m_definitionOfValue[use.user.index()];
        auto isMemberOf = [&](ValueID object, ValueID member, uint32_t memberOperandIndex) {
            uint32_t objectDefinition = m_definitionOfValue[object.index()];
            if (objectDefinition == noDefinition || !isKnown(member))
                return false;
            if (use.operandIndex == memberOperandIndex)
                m_definitions[index].holder = objectDefinition;
            return true;
        };
        switch (user.opcode) {
        case Opcode::NewObject:
        case Opcode::NewArray:
            if (holder == noDefinition)
                return false;
            m_definitions[index].holder = holder;
            return true;
        case Opcode::NewClosure:
            // A method that uses `super` captures the object it is defined on.
            return holder != noDefinition;
        case Opcode::DefineGetter:
            return !user.operand(OperandIndex::DefineGetter::computedKey) && isMemberOf(user.operand(OperandIndex::DefineGetter::object), user.operand(OperandIndex::DefineGetter::function), OperandIndex::DefineGetter::function);
        case Opcode::DefineSetter:
            return !user.operand(OperandIndex::DefineSetter::computedKey) && isMemberOf(user.operand(OperandIndex::DefineSetter::object), user.operand(OperandIndex::DefineSetter::function), OperandIndex::DefineSetter::function);
        case Opcode::SetLiteralPrototype: {
            ValueID object = user.operand(OperandIndex::SetLiteralPrototype::object);
            return m_function.at(object).opcode == Opcode::NewObject && isMemberOf(object, user.operand(OperandIndex::SetLiteralPrototype::prototype), OperandIndex::SetLiteralPrototype::prototype);
        }
        case Opcode::SetFunctionName:
            return use.operandIndex == OperandIndex::SetFunctionName::function && m_function.at(user.operand(OperandIndex::SetFunctionName::function)).opcode == Opcode::NewClosure
                && constantThroughCasts(m_program, m_function, user.operand(OperandIndex::SetFunctionName::computedKey));
        case Opcode::PropDefine: {
            // Behind FinishObject a definition of a property is a change like any other.
            ValueID object = user.operand(OperandIndex::PropDefine::object);
            return m_function.at(object).opcode == Opcode::NewObject && isMemberOf(object, user.operand(OperandIndex::PropDefine::value), OperandIndex::PropDefine::value);
        }
        case Opcode::NewClass:
            if (holder == noDefinition)
                return false;
            if (use.operandIndex == OperandIndex::NewClass::constructor)
                m_definitions[index].holder = holder;
            return true;
        case Opcode::ResolveHeritage:
            return std::ranges::all_of(m_analyses.useLists().usesOf(use.user), [&](const Use& useOfParent) {
                return useOfParent.kind == Use::Kind::Operand && m_function.at(useOfParent.user).opcode == Opcode::NewClass && m_definitionOfValue[useOfParent.user.index()] != noDefinition;
            });
        case Opcode::DefineMethod:
            return !user.operand(OperandIndex::DefineMethod::computedKey) && isMemberOf(user.operand(OperandIndex::DefineMethod::classObject), user.operand(OperandIndex::DefineMethod::function), OperandIndex::DefineMethod::function);
        case Opcode::PutModuleVariable:
            return m_bindings.definitionOfVariable[user.variable().index()] == index;
        default:
            return false;
        }
    }

    void countUse(const UseClassifier& classifier, uint32_t index, const Use& use)
    {
        if (isPartOfDefinition(index, use))
            return;
        if (classifier.needsObject(use)) {
            Definition& definition = m_definitions[index];
            if (!definition.observingUsesInModuleBodies++ && use.kind == Use::Kind::Operand)
                definition.observerInModuleBody = m_function.at(use.user).opcode;
        } else
            ++m_definitions[index].usesThatGoPast;
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    FunctionAnalyses m_analyses;
    Bindings& m_bindings;
    Vector<Definition>& m_definitions;
    Vector<uint32_t> m_definitionOfValue;
};

} // anonymous namespace

// The holder has a property of this name before the first member is defined.
static bool isCreatedWithHolder(const Program& program, ClassSide side, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    switch (side) {
    case ClassSide::Prototype:
        return nameClass == NameClass::Constructor;
    case ClassSide::Static:
        return nameClass == NameClass::Length || nameClass == NameClass::Prototype || key == program.keyOf(BuiltinNames::name);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool capturesAtMostClassEnvironment(const Program& program, const Function& constructor, const Function& function)
{
    if (function.captures.isEmpty())
        return true;
    if (function.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = function.captures[0].type;
    return captured && captured == constructor.captures[0].type && program.at(captured).environment;
}

bool canClassRecordCreate(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || !member.function || program.isIndexLike(member.key) || isCreatedWithHolder(program, member.side, member.key))
        return false;
    // SetFunctionName names the closure of the definition.
    const Function& function = program.at(member.function);
    return function.name && capturesAtMostClassEnvironment(program, constructor, function);
}

static bool isClassEvaluatedOnce(const Program& program, const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.isDerived() && !layout.has(LayoutFlags::IsConstructorFunction) && isEvaluatedOnce(program, layout);
}

// The prototype property of a built-in constructor of the engine never changes either, and a cell keeps the constructor that the realm was created with, whatever the program puts in its place.
BuiltinID builtinParentNamedByClassRecord(const Program& program, const Layout& layout)
{
    if (layout.flow.baseState != BaseState::BuiltinClass)
        return { };
    const BuiltinTable& builtins = program.builtins();
    const BuiltinClass& baseClass = builtins.at(layout.flow.baseClass);
    BuiltinID constructor = baseClass.constructor;
    if (!constructor || !baseClass.prototype)
        return { };
    const Builtin& builtin = builtins.at(constructor);
    bool isFoundAtAnyTime = builtin.origin == BuiltinOrigin::Engine && builtin.scope == BuiltinScope::Realm && BuiltinCellKey::object(constructor).hasCell(builtins);
    return isFoundAtAnyTime ? constructor : BuiltinID();
}

// What a constructor function has as its prototype property when the class below it is defined is known only then. The property of a class never changes.
static bool hasOnlyClassesAbove(const Program& program, LayoutID id)
{
    LayoutID current = id;
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (!isClassEvaluatedOnce(program, layout) || !program.at(layout.constructor).captures.isEmpty())
            return false;
        switch (layout.flow.baseState) {
        case BaseState::Layout:
            current = layout.flow.base;
            continue;
        case BaseState::None:
            return !layout.has(LayoutFlags::HasHeritage);
        case BaseState::BuiltinClass:
            return !!builtinParentNamedByClassRecord(program, layout);
        case BaseState::Unknown:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return false;
}

bool isDefinedByClassRecord(const Program& program, LayoutID id)
{
    if (!hasOnlyClassesAbove(program, id))
        return false;
    const Layout& layout = program.at(id);
    const Function& constructor = program.at(layout.constructor);
    bool recordCreatesEveryMember = std::ranges::all_of(layout.classFlow.members, [&](const ClassMember& member) {
        return canClassRecordCreate(program, constructor, member);
    });
    if (!recordCreatesEveryMember)
        return false;
    // The closure table has the one closure of each, whoever asks for it first.
    bool capturesNothing = true;
    forEachFunctionInClassRecord(program, layout, [&](FunctionID function, FunctionReferenceKind) {
        capturesNothing &= program.at(function).captures.isEmpty();
    });
    return capturesNothing;
}

static bool isOnlyClosureOperand(const Function& function, const UseLists& useLists, ValueID id, FunctionID closureFunction)
{
    const Value& closure = function.at(id);
    return closure.opcode == Opcode::NewClosure && closure.function() == closureFunction && closure.capturesOperands().empty() && !closure.isMadeOnDemand() && useLists.useCount(id) == 1;
}

std::optional<ClassDefinitionSite> findClassDefinitionSite(const Program& program, const Function& function, const UseLists& useLists, ValueID newClass)
{
    const Value& head = function.at(newClass);
    RELEASE_ASSERT(head.opcode == Opcode::NewClass);
    if (!isDefinedByClassRecord(program, head.layout()))
        return std::nullopt;

    const Layout& layout = program.at(head.layout());
    ClassDefinitionSite site { .layout = head.layout(), .classObject = newClass };
    if (!isOnlyClosureOperand(function, useLists, head.constructorOperand(), layout.constructor))
        return std::nullopt;
    site.describedByRecord.append(head.constructorOperand());

    // The heritage is proven to be a class, whose prototype property is a data property that holds an object: the look at it finds that and does nothing else.
    for (ValueID current = head.prototypeParentOperand(); current;) {
        const Value& value = function.at(current);
        bool isCastThatDoesNothing = value.opcode == Opcode::AssumeType || value.opcode == Opcode::Upcast;
        if (value.opcode != Opcode::ResolveHeritage && !isCastThatDoesNothing)
            return std::nullopt;
        unsigned otherUses = 0;
        for (const Use& use : useLists.usesOf(current)) {
            if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateType)
                site.describedByRecord.append(use.user);
            else
                ++otherUses;
        }
        if (otherUses != 1)
            return std::nullopt;
        site.describedByRecord.append(current);
        current = isCastThatDoesNothing ? value.valueOperand() : ValueID();
    }

    const Vector<ClassMember>& members = layout.classFlow.members;
    BitVector isDefinedHere(members.size());
    auto addDefinition = [&](ValueID id) {
        const Value& definition = function.at(id);
        const Value& closure = function.at(definition.functionOperand());
        if (definition.layout() != site.layout || !definition.key() || definition.computedKeyOperand() || closure.opcode != Opcode::NewClosure)
            return false;
        size_t position = members.findIf([&](const ClassMember& member) {
            return member.function == closure.function();
        });
        if (position == notFound)
            return false;
        const ClassMember& member = members[position];
        bool isAsListed = member.key == definition.key() && member.side == definition.side() && member.kind == definition.methodKind();
        if (!isAsListed || isDefinedHere.quickSet(position) || !isOnlyClosureOperand(function, useLists, definition.functionOperand(), member.function))
            return false;
        site.describedByRecord.append(definition.functionOperand());
        site.describedByRecord.append(id);
        return true;
    };

    for (size_t next = 0; next <= site.copies.size(); ++next) {
        ValueID holder = next ? site.copies[next - 1] : newClass;
        for (const Use& use : useLists.usesOf(holder)) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = function.at(use.user);
            if (sameValueOperandOf(user) == holder) {
                site.copies.append(use.user);
                continue;
            }
            if (user.opcode == Opcode::PutModuleVariable) {
                site.stores.append(use.user);
                continue;
            }
            if (user.opcode == Opcode::DefineMethod && use.operandIndex == OperandIndex::DefineMethod::classObject && !addDefinition(use.user))
                return std::nullopt;
        }
    }
    if (isDefinedHere.bitCount() != members.size())
        return std::nullopt;
    return site;
}

ASCIILiteral nameOf(DefinitionKind kind)
{
    switch (kind) {
    case DefinitionKind::Function:
        return "Function"_s;
    case DefinitionKind::Class:
        return "Class"_s;
    case DefinitionKind::RegExp:
        return "RegExp"_s;
    case DefinitionKind::TemplateObject:
        return "TemplateObject"_s;
    case DefinitionKind::ClassToken:
        return "ClassToken"_s;
    case DefinitionKind::Object:
        return "Object"_s;
    case DefinitionKind::Array:
        return "Array"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<uint8_t> countStores(const Program& program, const ParallelLoops& loops)
{
    Vector<uint32_t> stored = collectInParallel<uint32_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::PutModuleVariable)
                    found.append(function.at(id).variable().index());
            }
        }
    });
    Vector<uint8_t> counts(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
    for (uint32_t variable : stored)
        counts[variable] = std::min<unsigned>(counts[variable] + 1, 2);
    return counts;
}

static void countUsesInFunction(const Program& program, BuildContext& build, FunctionID id, const Bindings& bindings, Vector<DefinitionUseCounts>& found)
{
    const Function& function = program.at(id);
    std::optional<FunctionAnalyses> analyses;
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (value.opcode != Opcode::GetModuleVariable)
                continue;
            uint32_t index = bindings.definitionOfVariable[value.variable().index()];
            if (index == noDefinition)
                continue;
            if (!analyses)
                analyses.emplace(AnalysisContext { build, program, nullptr, nullptr, id }, function);
            UseClassifier classifier(program, *analyses);
            DefinitionUseCounts counts { index };
            classifier.forEachUseThroughSameValues(valueID, [&](const Use& use) {
                ++(classifier.needsObject(use) ? counts.observing : counts.past);
            });
            found.append(counts);
        }
    }
}

Definitions::Definitions(const Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Bindings bindings;
    bindings.definitionOfVariable.fill(noDefinition, program.variables.size());
    bindings.numberOfStores = countStores(program, loops);

    m_unknownOperands.fill(0, numberOfOpcodes);

    Vector<FunctionID> moduleBodies;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (program.functions[index].hasBody() && program.functions[index].isModuleBody())
            moduleBodies.append(FunctionID(index));
    }
    Vector<Vector<uint32_t>> definitionOfAllocation(moduleBodies.size());
    for (size_t index = 0; index < moduleBodies.size(); ++index)
        definitionOfAllocation[index].fill(noDefinition, program.at(moduleBodies[index]).values.size());

    // A class may extend, and a literal may hold, a binding of a module that comes later in this list.
    for (bool didBind = true; didBind;) {
        didBind = false;
        for (size_t index = 0; index < moduleBodies.size(); ++index)
            didBind |= ModuleBodyScan(program, build, moduleBodies[index], bindings, m_definitions).findDefinitions(definitionOfAllocation[index]);
    }
    for (size_t index = 0; index < moduleBodies.size(); ++index) {
        ModuleBodyScan scan(program, build, moduleBodies[index], bindings, m_definitions);
        scan.findDefinitions(definitionOfAllocation[index], &m_unknownOperands);
        scan.countUses();
    }

    Vector<DefinitionUseCounts> uses = collectInParallel<DefinitionUseCounts>(loops, program.functions.size(), [&](unsigned index, Vector<DefinitionUseCounts>& found) {
        const Function& function = program.functions[index];
        if (function.hasBody() && !function.isModuleBody())
            countUsesInFunction(program, build, FunctionID(index), bindings, found);
    });
    for (const DefinitionUseCounts& counts : uses) {
        m_definitions[counts.definition].usesThatGoPast += counts.past;
        m_definitions[counts.definition].observingUsesInFunctions += counts.observing;
    }
}

bool Definitions::isMadeAtStart(const Definition& definition) const
{
    for (const Definition* current = &definition;; current = &m_definitions[current->holder]) {
        if (current->observingUsesInModuleBodies)
            return true;
        if (current->holder == Definition::noHolder)
            return false;
    }
}

bool Definitions::isEverMade(const Definition& definition) const
{
    for (const Definition* current = &definition;; current = &m_definitions[current->holder]) {
        if (current->isObserved())
            return true;
        if (current->holder == Definition::noHolder)
            return false;
    }
}

void Definitions::dumpCounts(PrintStream& out) const
{
    struct Counts {
        uint32_t all { 0 };
        uint32_t madeAtStart { 0 };
        uint32_t madeOnDemand { 0 };
        uint32_t neverMade { 0 };
    };
    std::array<Counts, numberOfDefinitionKinds> byKind { };
    for (const Definition& definition : m_definitions) {
        Counts& counts = byKind[static_cast<unsigned>(definition.kind)];
        ++counts.all;
        ++(isMadeAtStart(definition) ? counts.madeAtStart : isEverMade(definition) ? counts.madeOnDemand : counts.neverMade);
    }
    for (unsigned kind = 0; kind < numberOfDefinitionKinds; ++kind) {
        const Counts& counts = byKind[kind];
        out.println("definitions ", nameOf(static_cast<DefinitionKind>(kind)), " ", counts.all, " made at start ", counts.madeAtStart, " made on demand ", counts.madeOnDemand, " never made ", counts.neverMade);
    }

    // Where the functions are held says which holders have to be able to hold a function that is not made.
    auto holderNameOf = [&](const Definition& definition) -> ASCIILiteral {
        if (definition.holder != Definition::noHolder)
            return nameOf(m_definitions[definition.holder].kind);
        return definition.binding ? "a binding"_s : "nothing"_s;
    };
    auto fateOf = [&](const Definition& definition) -> unsigned {
        return isMadeAtStart(definition) ? (definition.observingUsesInModuleBodies ? 0 : 1) : isEverMade(definition) ? 2 : 3;
    };
    static constexpr std::array<ASCIILiteral, 4> fates { "observed at start"_s, "made at start with its holder"_s, "made on demand"_s, "never made"_s };
    for (ASCIILiteral holder : { "a binding"_s, "nothing"_s, nameOf(DefinitionKind::Class), nameOf(DefinitionKind::Object), nameOf(DefinitionKind::Array), nameOf(DefinitionKind::Function) }) {
        std::array<uint32_t, 4> byFate { };
        for (const Definition& definition : m_definitions) {
            if (definition.kind == DefinitionKind::Function && holderNameOf(definition) == holder)
                ++byFate[fateOf(definition)];
        }
        out.print("functions held by ", holder);
        for (unsigned fate = 0; fate < fates.size(); ++fate)
            out.print(fate ? ", " : ": ", fates[fate], " ", byFate[fate]);
        out.println();
    }

    for (unsigned opcode = 0; opcode < numberOfOpcodes; ++opcode) {
        if (m_unknownOperands[opcode])
            out.println("no definition for an operand made by ", nameOf(static_cast<Opcode>(opcode)), " ", m_unknownOperands[opcode]);
    }

    std::array<uint32_t, numberOfOpcodes + 1> byObserver { };
    for (const Definition& definition : m_definitions) {
        if (definition.observingUsesInModuleBodies)
            ++byObserver[definition.observerInModuleBody ? static_cast<unsigned>(*definition.observerInModuleBody) : numberOfOpcodes];
    }
    for (unsigned observer = 0; observer <= numberOfOpcodes; ++observer) {
        if (byObserver[observer])
            out.println("observed at start by ", observer < numberOfOpcodes ? nameOf(static_cast<Opcode>(observer)) : "a terminator or an edge"_s, " ", byObserver[observer]);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
