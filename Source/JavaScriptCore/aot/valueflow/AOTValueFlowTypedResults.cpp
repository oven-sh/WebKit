/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTypedResults.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueFlowRules.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

TypeID typedElementTypeOf(const Program& program, const ClassPart& array)
{
    return array.contentsAt(program.builtins(), ContentsSlot::Element);
}

// A validator allocates a layout wherever a node of its plan names it. Here every such node stands where the types lead to it: at the root, in a field, in an element of an array.
bool placesLayoutsWhereTypesNameThem(const ExpectedResult& expected)
{
    return std::ranges::all_of(expected.validation, [](const ValidationNode& node) {
        switch (node.shape.kind()) {
        case ValidationShape::Kind::Tuple:
        case ValidationShape::Kind::Dictionary:
            return false;
        case ValidationShape::Kind::Object: {
            const auto& object = node.shape.as<ValidationShape::Object>();
            return object.layout && !object.undeclaredValue && std::ranges::all_of(object.members, [](const ValidationMember& member) {
                return member.field.has_value();
            });
        }
        default:
            return true;
        }
    });
}

// Follows the types that TypedResultAllocation::valueOf() and fill() follow.
void followTypes(const Program& program, ValueFlowTypedResult& result, bool layoutsStandOnlyWhereTypesNameThem)
{
    HashSet<TypeID> visitedTypes;
    BitVector isNamed(result.layouts.size());
    Vector<TypeID, 8> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };

    visit(result.type);
    while (!worklist.isEmpty()) {
        TypeID id = worklist.takeLast();
        const Type& type = program.at(id);
        if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array)) {
            result.typesWithArrays.append(id);
            visit(typedElementTypeOf(program, *array));
        }
        std::span<const LayoutID> members = membersOf(program, type);
        for (size_t index = 0; index < result.layouts.size() && !members.empty(); ++index) {
            LayoutID layout = result.layouts[index];
            if (isNamed.get(index) || !std::ranges::binary_search(members, layout))
                continue;
            isNamed.set(index);
            for (const Field& field : program.at(layout).fields) {
                if (field.key)
                    visit(expectationOf(field.declared));
            }
        }
    }
    AOT::sort(result.typesWithArrays);

    for (size_t index = 0; index < result.layouts.size(); ++index) {
        if (!layoutsStandOnlyWhereTypesNameThem || !isNamed.get(index))
            result.layoutsAmongPlainData.append(result.layouts[index]);
    }
}

class TypedResultAllocation {
    WTF_MAKE_NONCOPYABLE(TypedResultAllocation);
public:
    TypedResultAllocation(const ValueFlowTypedResult& result, const ValueFlowStatement& call, ValueSetID plainDataOfCall, ValueFlowRuleContext& context)
        : m_result(result)
        , m_call(call)
        , m_plainDataOfCall(plainDataOfCall)
        , m_context(context)
        , m_program(context.program())
        , m_universe(context.universe())
    {
    }

    // What a parser or a validator puts where the type stands.
    ValueSetID valueOf(TypeID id)
    {
        if (!id)
            return plainData();
        const Type& type = m_program.at(id);
        if (!type.isJavaScriptValue() || type.isAnything())
            return plainData();

        Vector<ValueFlowObjectID, 16> members;
        for (uint16_t rest = type.primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol); rest; rest &= rest - 1)
            members.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
        if (type.primitives & Primitive::AnyString)
            members.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        if (type.primitives & Primitive::AnySymbol)
            members.append(ValueFlowUniverse::primitive(PrimitiveSet::Symbol));

        // An ordinary object has no layout, so it stands only where the type admits an object without one.
        bool admitsPlainObjects = type.anyObject || type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty() || (type.layouts && m_program.at(type.layouts).hasMembersWithoutLayout);
        std::span<const LayoutID> layoutsOfType = membersOf(m_program, type);
        for (size_t index = 0; index < m_result.layouts.size() && !layoutsOfType.empty(); ++index) {
            if (std::ranges::binary_search(layoutsOfType, m_result.layouts[index]))
                members.append(objectAt(m_result.ordinalOfLayout(index)));
        }
        for (const ClassPart& part : type.classes) {
            auto listed = std::ranges::lower_bound(m_result.typesWithArrays, id);
            if (part.builtinClass != BuiltinClasses::Array || listed == m_result.typesWithArrays.end() || *listed != id) {
                admitsPlainObjects = true;
                continue;
            }
            members.append(objectAt(m_result.ordinalOfArray(listed - m_result.typesWithArrays.begin())));
            members.append(objectAt(m_result.ordinalOfPlainArray()));
        }

        ValueSetID value = m_context.maker().of(members.span());
        return admitsPlainObjects ? m_context.maker().unite(value, m_context.maker().intersect(plainData(), m_universe.objects())) : value;
    }

    void fill()
    {
        for (size_t index = 0; index < m_result.layouts.size(); ++index)
            fillObject(only(m_result.ordinalOfLayout(index)), m_result.layouts[index]);
        for (size_t index = 0; index < m_result.typesWithArrays.size(); ++index) {
            const ClassPart* array = m_program.at(m_result.typesWithArrays[index]).findClassPart(BuiltinClasses::Array);
            m_context.add(m_context.written(only(m_result.ordinalOfArray(index)), elementsName), valueOf(typedElementTypeOf(m_program, *array)));
        }
        m_context.add(m_context.written(only(m_result.ordinalOfPlainArray()), elementsName), plainData());

        // What lies in plain data lies in the objects that stand for plain data. Where that is the unknown object, it is what the unknown object stands for.
        ClimbingSet plainObjects = m_plainDataOfCall ? m_context.unite(ClimbingSet(m_context.maker().intersect(m_plainDataOfCall, m_universe.objects())), only(m_result.ordinalOfPlainArray())) : ClimbingSet();
        for (LayoutID layout : m_result.layoutsAmongPlainData) {
            size_t index = std::ranges::lower_bound(m_result.layouts, layout) - m_result.layouts.begin();
            if (!m_plainDataOfCall) {
                m_context.add(ValueFlowProgramNodes::escaped(), only(m_result.ordinalOfLayout(index)));
                continue;
            }
            for (ValueFlowName name : { unknownKeyName, elementsName })
                m_context.add(m_context.written(plainObjects, name), only(m_result.ordinalOfLayout(index)));
        }
    }

private:
    ValueFlowObjectID objectAt(uint32_t ordinal) const
    {
        ValueFlowObjectID object = m_universe.siteObject(m_call, ordinal);
        RELEASE_ASSERT(object);
        return object;
    }

    ClimbingSet only(uint32_t ordinal) { return ClimbingSet(m_context.maker().of(objectAt(ordinal))); }
    ValueSetID plainData() { return m_plainDataOfCall ? m_plainDataOfCall : unknownValue(m_context, ValueFlowUnknownSource::DataFromOutside); }

    void fillField(ClimbingSet object, const Field& field)
    {
        ValueFlowNodeID node = m_context.written(object, m_program.isIndexLike(field.key) ? elementsName : ValueFlowName::property(field.key));
        m_context.add(node, valueOf(expectationOf(field.declared)));
        // The text may leave the member out.
        if (!field.declared.type || m_program.at(field.declared.type).primitives & Primitive::Undefined)
            m_context.add(node, m_universe.undefinedValue());
    }

    void fillObject(ClimbingSet object, LayoutID id)
    {
        const Layout& layout = m_program.at(id);
        for (const Field& field : layout.fields) {
            if (field.key)
                fillField(object, field);
        }
        HashSet<LayoutID> visited;
        for (LayoutID base = predictedHeritageOf(layout); base && visited.add(base).isNewEntry; base = predictedHeritageOf(m_program.at(base))) {
            for (const Field& field : m_program.at(base).fields) {
                if (field.key && !layout.findField(field.key))
                    fillField(object, field);
            }
        }
        // A member that the layout does not declare becomes a property of the object.
        m_context.add(m_context.written(object, unknownKeyName), plainData());
        m_context.mark(object, ObjectMutation::AddProperty);
    }

    const ValueFlowTypedResult& m_result;
    const ValueFlowStatement& m_call;
    ValueSetID m_plainDataOfCall;
    ValueFlowRuleContext& m_context;
    const Program& m_program;
    const ValueFlowUniverse& m_universe;
};

bool answersPromise(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    return std::ranges::any_of(builtins.at(overload.result.type).parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && part.builtinClass == BuiltinClasses::Promise;
    });
}

} // anonymous namespace

ValueFlowTypedResult findTypedResult(const Program& program, const ValueFlowBuiltinTables& tables, const Function& function, const UseLists& uses, ValueID call)
{
    const Value& operation = function.at(call);
    if (operation.opcode != Opcode::Call)
        return { };

    ValueFlowTypedResult result;
    bool layoutsStandOnlyWhereTypesNameThem = true;
    if (ExpectedResultID id = operation.expectedResult()) {
        const ExpectedResult& expected = program.at(id);
        result.type = expected.type;
        result.isValidated = !expected.validation.isEmpty();
        result.layouts.appendRange(expected.layouts.begin(), expected.layouts.end());
        AOT::sort(result.layouts);
        result.layouts.shrink(std::ranges::unique(result.layouts).begin() - result.layouts.begin());
        layoutsStandOnlyWhereTypesNameThem = placesLayoutsWhereTypesNameThem(expected);
    } else {
        TypedResultTarget target = findTypedResultTarget(program, function, uses, call, maxObjectsOfTypedResult, &tables.headLayoutsOfFamilies());
        result.type = target.type;
        result.layouts = WTF::move(target.layouts);
    }
    if (!result.type || result.numberOfObjects() > maxObjectsOfTypedResult)
        return { };

    followTypes(program, result, layoutsStandOnlyWhereTypesNameThem);
    if (result.numberOfObjects() > maxObjectsOfTypedResult)
        return { };
    return result;
}

ValueFlowTypedResult findTypedResult(const Program& program, const ValueFlowBuiltinTables& tables, FunctionID id, ValueID call)
{
    const Function& function = program.at(id);
    CFG cfg(function);
    UseLists uses(function, cfg);
    return findTypedResult(program, tables, function, uses, call);
}

// Before the solve only the text of a call says what it may call: the name that its callee was read under, or the built-in that it names.
static bool mayCallFunctionWithTypedResult(const Program& program, const ValueFlowBuiltinTables& tables, const Function& function, const Value& call)
{
    if (PropertyKeyID key = call.calleeKey()) {
        return std::ranges::any_of(tables.namesOfFunctionsWithTypedResults(), [&](BuiltinName name) {
            return program.keyOf(name) == key;
        });
    }
    const Value& callee = function.at(sameObjectThroughCasts(function, call.operand(0)));
    return callee.opcode == Opcode::Builtin && tables.hasTypedResult(callee.builtin());
}

// Whatever is called, the result is validated, and the validator allocates. Type arguments alone allocate nothing: `append<Node>(to, value)` calls a function of the program.
static bool isValidated(const Program& program, const Value& call)
{
    ExpectedResultID expected = call.expectedResult();
    return expected && !program.at(expected).validation.isEmpty();
}

void appendSitesOfTypedResults(const Program& program, const ValueFlowBuiltinTables& tables, const Function& function, const CFG& cfg, Vector<ValueFlowSite>& sites)
{
    // Nearly every call of typed code is followed by a type with layouts, so the callee is looked at first: where the type of a result comes from, a cast, a test, a parameter or a tail call, takes the uses of the function.
    BitVector candidates;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        if (value.opcode == Opcode::Call && (isValidated(program, value) || mayCallFunctionWithTypedResult(program, tables, function, value)))
            candidates.set(index);
    }
    if (candidates.isEmpty())
        return;

    UseLists uses(function, cfg);
    for (size_t index : candidates) {
        ValueID call(static_cast<uint32_t>(index));
        ValueFlowTypedResult result = findTypedResult(program, tables, function, uses, call);
        if (!result)
            continue;
        for (size_t layout = 0; layout < result.layouts.size(); ++layout)
            sites.append({ ValueFlowSiteOrigin::TypedResult, call, result.ordinalOfLayout(layout), result.layouts[layout], { } });
        for (size_t array = 0; array <= result.typesWithArrays.size(); ++array)
            sites.append({ ValueFlowSiteOrigin::TypedResult, call, result.ordinalOfArray(array), { }, BuiltinClasses::Array });
    }
}

void allocateTypedResult(const BuiltinOverload& overload, const ValueFlowStatement& call, const ValueFlowBuiltinCall& description, ValueFlowObjectID promise, ValueSetID plainData, ValueFlowRuleContext& context)
{
    if (call.kind != ValueFlowStatementKind::Operation || call.opcode != Opcode::Call || description.kind != ValueFlowCallKind::Call)
        return;
    if (!context.universe().siteObject(call, ValueFlowTypedResult::firstOrdinal))
        return;

    const Program& program = context.program();
    ExpectedResultID expected = program.at(call.function).at(call.site).expectedResult();
    bool isValidated = expected && !program.at(expected).validation.isEmpty();
    TypedResultForm form = overload.result.typedResultForm;
    bool parses = form == TypedResultForm::ParsedText || form == TypedResultForm::ParsedBody || form == TypedResultForm::Rows;
    if (!isValidated && !parses)
        return;

    // What SelectTypedResults puts in the place of the call hands whatever the compiled code gives up on to helpers in the library.
    for (BuiltinID helper : { Builtins::CompilerHelpersValidateByPlan, Builtins::CompilerHelpersInterpretNode, Builtins::CompilerHelpersHoldsRule, Builtins::CompilerHelpersCopyOwnProperties })
        context.use(helper, BuiltinUse::Called);

    ValueFlowNodeID target = description.result;
    if (!expected && form != TypedResultForm::ParsedText && answersPromise(program.builtins(), overload)) {
        if (!promise)
            return;
        target = context.written(ClimbingSet(context.maker().of(promise)), { ValueFlowNameKind::Slot, BuiltinSlots::PromiseValue.index() });
    }

    ValueFlowTypedResult result = findTypedResult(program, context.universe().builtinTables(), call.function, call.site);
    RELEASE_ASSERT(result);
    TypedResultAllocation allocation(result, call, parses ? plainData : ValueSetID(), context);
    allocation.fill();
    if (target)
        context.add(target, allocation.valueOf(result.type));

    // Where the plan takes any object or any function, the copy holds the very value that its source held.
    if (!isValidated || parses)
        return;
    for (const ValueFlowOperand& argument : description.arguments)
        context.connect(argument, ValueFlowProgramNodes::escaped());
    context.connect(description.laterArguments, ValueFlowProgramNodes::escaped());
    context.connect(description.otherArguments, ValueFlowProgramNodes::escaped());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
