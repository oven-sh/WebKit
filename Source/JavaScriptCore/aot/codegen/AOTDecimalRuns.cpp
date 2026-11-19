/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDecimalRuns.h"

#if ENABLE(AOT)

#include "AOTClosuresLent.h"
#include "AOTFacts.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTReusedDecimals.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet otherNamesOfDecimals { Opcode::Box, Opcode::Unbox, Opcode::Decode, Opcode::Narrowed, Opcode::CheckInitialized, Opcode::RequireObjectCoercible };

DecimalRuns::DecimalRuns(const Program& program, const Plan& plan, const Facts& facts, const Code& code, const TypeArena& types)
    : m_program(program)
    , m_plan(plan)
    , m_facts(facts)
    , m_code(code)
    , m_types(types)
{
    Vector<std::pair<ValueID, Evidence>, 16> found;
    Vector<std::pair<ValueID, ValueID>, 16> alike;
    for (uint32_t number = 0; number < code.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!code.isReached(block))
            continue;
        for (ValueID value : code.valuesOf(block)) {
            if (std::optional<DecimalCall> call = callAt(value)) {
                found.append({ call->receiver, evidenceOf(value, *call) });
                if (call->otherDecimal)
                    alike.append({ call->receiver, call->otherDecimal });
                if (call->givesDecimal())
                    alike.append({ call->receiver, value });
            } else if (mayConstructDecimal(value) && readsConstructor(code.at(value).calleeOperand()))
                found.append({ value, Evidence::Strong });
        }
    }
    if (std::ranges::all_of(found, [](auto& each) { return each.second == Evidence::None; }))
        return;

    m_classes = Vector<uint32_t>(code.numberOfValues(), [](size_t index) { return static_cast<uint32_t>(index); });
    for (auto [value, other] : alike)
        unite(value, other);
    for (uint32_t number = 0; number < code.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!code.isReached(block))
            continue;
        const Terminator& terminator = code.at(block).terminator;
        for (uint32_t target = 0; target < terminator.numberOfEdges; ++target) {
            const Edge& edge = code.at(terminator.edge(target));
            std::span<const ValueID> parameters = code.at(edge.to).parameters();
            for (size_t index = 0; index < edge.arguments().size() && index < parameters.size(); ++index) {
                if (ValueID argument = edge.arguments()[index]; argument && !code.isConstant(argument))
                    unite(argument, parameters[index]);
            }
        }
    }
    m_evidence.fill(Evidence::None, code.numberOfValues());
    for (auto [value, evidence] : found) {
        Evidence& known = m_evidence[classOf(value)];
        known = std::max(known, evidence);
    }
}

ValueID DecimalRuns::originOf(ValueID value) const
{
    while (otherNamesOfDecimals.contains(m_code.at(value).opcode))
        value = m_code.at(value).valueOperand();
    return value;
}

uint32_t DecimalRuns::classOf(ValueID value) const
{
    uint32_t index = originOf(value).index();
    while (m_classes[index] != index) {
        m_classes[index] = m_classes[m_classes[index]];
        index = m_classes[index];
    }
    return index;
}

void DecimalRuns::unite(ValueID value, ValueID other)
{
    m_classes[classOf(value)] = classOf(other);
}

StringID DecimalRuns::textOf(PropertyKeyID key) const
{
    if (!key || liesInThread(key) || m_program.at(key).kind != PropertyKeyKind::Text)
        return { };
    return m_program.at(key).text;
}

std::optional<DecimalOperation> DecimalRuns::operationNamed(PropertyKeyID key) const
{
    StringID text = textOf(key);
    if (!text)
        return std::nullopt;
    auto& methods = m_program.namesOfDecimals().methods;
    auto found = std::ranges::find(methods, text);
    if (found == methods.end())
        return std::nullopt;
    return static_cast<DecimalOperation>(found - methods.begin());
}

bool DecimalRuns::findOptionsOfToFixed(ValueID argument, DecimalCall& call) const
{
    const Operation& box = m_code.at(argument);
    if (box.opcode != Opcode::Box || m_code.useCountOf(argument) != 1)
        return false;
    const Operation& made = m_code.at(box.valueOperand());
    if (made.opcode != Opcode::NewObject || m_code.useCountOf(box.valueOperand()) != 1 || made.prototypeOperand())
        return false;
    const Layout& layout = m_program.at(made.layout());
    if (layout.kind != LayoutKind::Record || layout.base || layout.computedKeyCount || layout.has(LayoutFlags::IsOfCreatedObject) || made.fields().size() != made.valuesOperands().size())
        return false;
    for (size_t index = 0; index < made.fields().size(); ++index) {
        const Field& field = layout.fields[made.fields()[index]];
        ValueID held = made.valuesOperands()[index];
        StringID name = textOf(field.key);
        if (!name || !held || field.form.contains(FieldForm::IsAccessor) || (typeOf(held).primitives & Primitive::Absent) || m_code.at(held).representation == Representation::Cell32)
            return false;
        if (name == m_program.namesOfDecimals().digits)
            call.count = held;
        else if (name == m_program.namesOfDecimals().roundingMode)
            call.modeInOptions = held;
        else
            return false;
    }
    call.options = argument;
    return !!call.count;
}

std::optional<DecimalCall> DecimalRuns::callAt(ValueID value) const
{
    const Operation& operation = m_code.at(value);
    if (operation.opcode != Opcode::Call || !operation.thisValueOperand() || operation.representation != Representation::JSValue || !typeOf(value).isAnything())
        return std::nullopt;
    std::optional<DecimalOperation> named = operationNamed(operation.calleeKey());
    if (!named || !mayBeDecimal(operation.thisValueOperand()) || argumentsLentBy(m_code, operation))
        return std::nullopt;

    DecimalCall call { *named, operation.thisValueOperand() };
    std::span<const ValueID> arguments = operation.argumentsOperands();
    switch (*named) {
    case DecimalOperation::Add:
    case DecimalOperation::Subtract:
    case DecimalOperation::Multiply:
    case DecimalOperation::Divide:
    case DecimalOperation::Remainder:
    case DecimalOperation::Equals:
    case DecimalOperation::NotEquals:
    case DecimalOperation::LessThan:
    case DecimalOperation::LessThanOrEqual:
    case DecimalOperation::GreaterThan:
    case DecimalOperation::GreaterThanOrEqual:
    case DecimalOperation::Compare:
        if (arguments.size() != 1 || !mayBeDecimal(arguments[0]))
            return std::nullopt;
        call.otherDecimal = arguments[0];
        return call;
    case DecimalOperation::Abs:
    case DecimalOperation::Negate:
    case DecimalOperation::Mantissa:
    case DecimalOperation::IsNaN:
    case DecimalOperation::IsFinite:
    case DecimalOperation::Exponent:
    case DecimalOperation::ToString:
    case DecimalOperation::ToExponential:
        if (!arguments.empty())
            return std::nullopt;
        return call;
    case DecimalOperation::Round:
    case DecimalOperation::Scale10: {
        if (arguments.empty() || arguments.size() > (*named == DecimalOperation::Round ? 2 : 1) || !(typeOf(arguments[0]).primitives & Primitive::Number))
            return std::nullopt;
        call.count = arguments[0];
        if (arguments.size() == 1 || m_code.at(arguments[1]).opcode == Opcode::Undefined)
            return call;
        const Operation& name = m_code.at(originOf(arguments[1]));
        if (name.opcode != Opcode::String)
            return std::nullopt;
        Vector<char16_t> buffer;
        std::optional<Decimal128::RoundingMode> mode = Decimal128::roundingModeNamed(m_types.viewOf(name.string(), buffer));
        if (!mode)
            return std::nullopt;
        call.mode = *mode;
        return call;
    }
    case DecimalOperation::ToFixed:
        if (arguments.size() > 1 || (arguments.size() == 1 && !findOptionsOfToFixed(arguments[0], call)))
            return std::nullopt;
        return call;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<DecimalOperation> DecimalRuns::testOfOrderAt(ValueID value) const
{
    static constexpr OpcodeSet tests { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::Eq, Opcode::StrictEq };
    const Operation& test = m_code.at(value);
    if (!tests.contains(test.opcode) || test.representation != Representation::Boolean || m_code.useCountOf(test.leftOperand()) != 1)
        return std::nullopt;
    const Operation& constant = m_code.at(originOf(test.rightOperand()));
    std::optional<DecimalCall> call = callAt(test.leftOperand());
    if (constant.opcode != Opcode::Integer || !call || call->operation != DecimalOperation::Compare)
        return std::nullopt;

    // By whether -1, 0 and 1 pass. NaN does not.
    auto passes = [&](int64_t order) {
        if (test.opcode == Opcode::Less)
            return order < constant.integer();
        if (test.opcode == Opcode::LessEq)
            return order <= constant.integer();
        if (test.opcode == Opcode::Greater)
            return order > constant.integer();
        if (test.opcode == Opcode::GreaterEq)
            return order >= constant.integer();
        return order == constant.integer();
    };
    if (passes(-1) == passes(1))
        return passes(0) && !passes(1) ? std::optional { DecimalOperation::Equals } : std::nullopt;
    if (passes(-1))
        return passes(0) ? DecimalOperation::LessThanOrEqual : DecimalOperation::LessThan;
    return passes(0) ? DecimalOperation::GreaterThanOrEqual : DecimalOperation::GreaterThan;
}

std::optional<Decimal128> DecimalRuns::constantAt(ValueID value) const
{
    const Operation& constant = m_code.at(originOf(m_code.at(value).argumentsOperands()[0]));
    if (constant.opcode == Opcode::Integer)
        return Decimal128::fromDouble(static_cast<double>(constant.integer()));
    if (constant.opcode == Opcode::Double)
        return Decimal128::fromDouble(constant.number());
    if (constant.opcode != Opcode::String)
        return std::nullopt;
    Vector<char16_t> buffer;
    return Decimal128::parse(m_types.viewOf(constant.string(), buffer));
}

ValueID DecimalRuns::callOfMethodReadAt(ValueID value) const
{
    const Operation& read = m_code.at(value);
    if (read.opcode != Opcode::PropGet || read.isFreeName() || read.representation != Representation::JSValue || m_code.useCountOf(value) != 1)
        return { };
    ValueID user = m_code.onlyUserOf(value);
    if (!user || !callAt(user))
        return { };
    const Operation& call = m_code.at(user);
    return call.calleeOperand() == value && call.calleeKey() == read.key() && originOf(call.thisValueOperand()) == originOf(read.objectOperand()) ? user : ValueID();
}

bool DecimalRuns::readsConstructor(ValueID value) const
{
    const Operation& read = m_code.at(value);
    if (read.opcode != Opcode::GetGlobalBinding || read.isForTypeOf() || read.representation != Representation::JSValue)
        return false;
    StringID name = textOf(read.key());
    return name && name == m_program.namesOfDecimals().constructor && !m_plan.globalName(read.key()).isStoredThroughBinding;
}

bool DecimalRuns::mayConstructDecimal(ValueID value) const
{
    const Operation& construction = m_code.at(value);
    if (construction.opcode != Opcode::Construct || construction.newTargetOperand() || construction.argumentsOperands().size() != 1)
        return false;
    if (construction.representation != Representation::JSValue || !mayBeDecimal(value) || !typeOf(construction.calleeOperand()).anyFunction)
        return false;
    ValueID argument = construction.argumentsOperands()[0];
    if (m_code.isConstant(originOf(argument)))
        return !!constantAt(value);
    return typeOf(argument).primitives & (Primitive::Number | Primitive::AnyString | Primitive::BigInt);
}

auto DecimalRuns::evidenceOf(ValueID value, const DecimalCall& call) const -> Evidence
{
    const Operation& operation = m_code.at(value);
    PropertyKeyID key = operation.calleeKey();
    const Operation& callee = m_code.at(operation.calleeOperand());
    bool isNameOfWrittenMethod = m_facts.linked().keysOfMembers[static_cast<size_t>(ClassSide::Prototype)].get(key.index()) || m_plan.slots().someLayoutHasField(key);
    if (isNameOfWrittenMethod && !(callee.opcode == Opcode::PropGet && callee.receiverIsNoObjectOfCompiledCode()))
        return Evidence::None;
    // What does not read the result means another method: add() of a Set.
    if (!m_code.useCountOf(value))
        return Evidence::None;
    // Of buffers, and of numbers.
    bool otherClassesAnswer = call.operation == DecimalOperation::Equals || call.operation == DecimalOperation::Compare || (call.operation >= DecimalOperation::ToString && !call.options);
    return otherClassesAnswer ? Evidence::Weak : Evidence::Strong;
}

bool DecimalRuns::isLikelyDecimal(ValueID value) const
{
    return !m_evidence.isEmpty() && m_code.at(value).representation == Representation::JSValue && mayBeDecimal(value) && evidenceFor(value) == Evidence::Strong;
}

bool DecimalRuns::makesDecimal(ValueID value) const
{
    if (!isLikelyDecimal(value))
        return false;
    std::optional<DecimalCall> call = callAt(value);
    return call ? call->givesDecimal() : mayConstructDecimal(value);
}

bool DecimalRuns::onlyReadsWordsOf(ValueID reader, ValueID decimal) const
{
    if (callOfMethodReadAt(reader))
        return true;
    std::optional<DecimalCall> call = callAt(reader);
    return call && call->count != decimal && m_code.at(reader).calleeOperand() != decimal;
}

bool DecimalRuns::mayBeMadeTwice(const Operation& operation) const
{
    static constexpr OpcodeSet reads { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::WidenInteger, Opcode::LogicalNot };
    EffectTraits traits = operation.traits();
    if (traits.mayRunOtherCode() || traits.mayWriteObservableState() || isNeverType(operation.type))
        return false;
    if (operation.opcode == Opcode::GetSlot)
        return operation.level() == KnowledgeLevel::Proven;
    // A sum may be left to it.
    if (operation.opcode == Opcode::Box && m_code.at(operation.valueOperand()).representation == Representation::Double)
        return mayBeMadeTwice(m_code.at(operation.valueOperand()));
    return reads.contains(operation.opcode) || otherNamesOfDecimals.contains(operation.opcode);
}

bool DecimalRuns::mayBeMadeAhead(const Operation& operation) const
{
    static constexpr OpcodeSet arithmetic {
        Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Negate, Opcode::Increment, Opcode::Decrement, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor,
        Opcode::Box, Opcode::IntegerToNumber, Opcode::WidenInteger,
    };
    return arithmetic.contains(operation.opcode) && operation.traits().isPure() && m_types.at(operation.type).isNumber();
}

std::optional<DecimalRun> DecimalRuns::find(BlockID block, std::span<const ValueID> values, ValueID returnedCall, const ReusedDecimals& reused) const
{
    using Step = DecimalRun::Step;
    if (m_evidence.isEmpty())
        return std::nullopt;

    DecimalRun run;
    std::optional<uint32_t> classOfRun;
    // One that is no Decimal would take the others along.
    auto belongsToRun = [&](ValueID decimal) {
        if (evidenceFor(decimal) == Evidence::None)
            return false;
        if (!classOfRun)
            classOfRun = classOf(decimal);
        return *classOfRun == classOf(decimal);
    };
    auto isMadeInRun = [&](ValueID value) {
        const DecimalRun::Part* part = run.find(value);
        return part && part->step != Step::IsMadeAhead;
    };
    auto isDecimalOfRun = [&](ValueID value) {
        return run.has(value, Step::Constructs) || (run.has(value, Step::CallsMethod) && callAt(value)->givesDecimal());
    };
    auto isOptionsOfCall = [&](ValueID box) {
        ValueID user = m_code.onlyUserOf(box);
        std::optional<DecimalCall> call = user ? callAt(user) : std::nullopt;
        return call && call->options == box && run.has(m_code.at(user).calleeOperand(), Step::ReadsMethod);
    };
    auto stepOf = [&](ValueID value) -> std::optional<Step> {
        const Operation& operation = m_code.at(value);
        ValueID user = m_code.useCountOf(value) == 1 ? m_code.onlyUserOf(value) : ValueID();
        if (ValueID call = callOfMethodReadAt(value))
            return belongsToRun(operation.objectOperand()) ? std::optional { Step::ReadsMethod } : std::nullopt;
        if (std::optional<DecimalCall> call = callAt(value)) {
            bool hasArgumentsAtHand = call->options ? run.has(call->options, Step::IsReadThrough) : !call->count || !isDecimalOfRun(call->count);
            return hasArgumentsAtHand && belongsToRun(call->receiver) ? std::optional { Step::CallsMethod } : std::nullopt;
        }
        if (readsConstructor(value))
            return user && mayConstructDecimal(user) && m_code.at(user).calleeOperand() == value && belongsToRun(user) ? std::optional { Step::ReadsConstructor } : std::nullopt;
        if (mayConstructDecimal(value))
            return evidenceFor(value) == Evidence::Strong && !isDecimalOfRun(operation.argumentsOperands()[0]) && belongsToRun(value) ? std::optional { Step::Constructs } : std::nullopt;
        if (run.parts.isEmpty())
            return std::nullopt;

        if (operation.opcode == Opcode::ToBoolean && run.has(operation.valueOperand(), Step::CallsMethod) && callAt(operation.valueOperand())->givesBoolean())
            return Step::AsksTruth;
        if (testOfOrderAt(value) && run.has(operation.leftOperand(), Step::CallsMethod) && m_code.isConstant(operation.rightOperand()))
            return Step::AsksOrder;
        bool readsDecimalOfRun = false;
        bool readsWhatRunMakes = false;
        operation.forEachChild([&](ValueID operand) {
            readsDecimalOfRun |= isDecimalOfRun(operand);
            readsWhatRunMakes |= isMadeInRun(operand);
        });
        if (readsDecimalOfRun)
            return std::nullopt;
        if (operation.opcode == Opcode::NewObject && user && m_code.at(user).opcode == Opcode::Box && isOptionsOfCall(user))
            return Step::IsReadThrough;
        if (operation.opcode == Opcode::Box && user) {
            if (isOptionsOfCall(value))
                return run.has(operation.valueOperand(), Step::IsReadThrough) ? std::optional { Step::IsReadThrough } : std::nullopt;
            const Operation& boxed = m_code.at(operation.valueOperand());
            bool isInteger = boxed.representation == Representation::Int32 || boxed.representation == Representation::Int64;
            std::optional<DecimalCall> call = callAt(user);
            if (isInteger && (mayConstructDecimal(user) || (call && call->count == value && !call->options)))
                return Step::IsReadThrough;
        }
        if (!readsWhatRunMakes && mayBeMadeAhead(operation))
            return Step::IsMadeAhead;
        if (mayBeMadeTwice(operation))
            return Step::IsMadeTwice;
        return std::nullopt;
    };

    // The parts are searched one by one.
    constexpr size_t mostParts = 128;
    for (ValueID value : values.first(std::min(values.size(), mostParts))) {
        std::optional<Step> step = stepOf(value);
        if (!step)
            break;
        run.parts.append({ value, *step });
    }
    if (returnedCall && run.parts.size() == values.size() && stepOf(returnedCall) == Step::CallsMethod) {
        run.parts.append({ returnedCall, Step::CallsMethod });
        run.endsInReturnedCall = true;
    }
    auto endsRun = [&](const DecimalRun::Part& part) {
        return part.step < Step::IsReadThrough && part.step != Step::ReadsConstructor;
    };
    auto lacksItsReader = [&](const DecimalRun::Part& part) {
        return part.step == Step::IsReadThrough && !run.find(m_code.onlyUserOf(part.value));
    };
    while (true) {
        while (!run.parts.isEmpty() && !endsRun(run.parts.last()))
            run.parts.removeLast();
        size_t place = run.parts.findIf(lacksItsReader);
        if (place == notFound)
            break;
        run.parts.shrink(place);
        run.endsInReturnedCall = false;
    }
    if (run.parts.isEmpty())
        return std::nullopt;

    if (EdgeID handler = m_code.handlerOf(block); handler && std::ranges::any_of(m_code.at(handler).arguments(), [&](ValueID argument) { return argument && run.find(argument); }))
        return std::nullopt;
    run.isLikely = m_evidence[*classOfRun] == Evidence::Strong;
    noteReads(run, reused);
    return run;
}

DecimalOperation DecimalRuns::operationAsComputed(const DecimalRun& run, ValueID call) const
{
    ValueID user = m_code.onlyUserOf(call);
    return user && run.has(user, DecimalRun::Step::AsksOrder) ? *testOfOrderAt(user) : callAt(call)->operation;
}

void DecimalRuns::noteReads(DecimalRun& run, const ReusedDecimals& reused) const
{
    using Step = DecimalRun::Step;
    Vector<uint32_t, 16> readsInRun;
    readsInRun.fill(0, run.parts.size());
    auto placeOf = [&](ValueID value) {
        return static_cast<size_t>(std::ranges::find(run.parts, value, &DecimalRun::Part::value) - run.parts.begin());
    };
    for (const DecimalRun::Part& reader : run.parts) {
        const Operation& operation = m_code.at(reader.value);
        auto noteRead = [&](ValueID operand) {
            if (size_t place = placeOf(operand); place < run.parts.size())
                ++readsInRun[place];
        };
        if (operation.opcode == Opcode::Narrowed)
            noteRead(operation.valueOperand());
        else
            operation.forEachChild(noteRead);

        if (reader.step != Step::CallsMethod)
            continue;
        DecimalCall call = *callAt(reader.value);
        DecimalOperation asComputed = operationAsComputed(run, reader.value);
        for (ValueID operand : { call.receiver, call.otherDecimal }) {
            if (size_t place = operand ? placeOf(operand) : run.parts.size(); place < run.parts.size()) {
                run.parts[place].mustLackTrailingZeros |= !takesTrailingZeros(asComputed);
                // The other use is the read of the method.
                run.parts[place].isOnlyRounded = asComputed == DecimalOperation::Round && m_code.useCountOf(operand) == 2;
            }
        }
    }
    for (size_t place = 0; place < run.parts.size(); ++place) {
        DecimalRun::Part& part = run.parts[place];
        uint8_t uses = m_code.useCountOf(part.value);
        part.isReadBehindRun = run.endsInReturnedCall ? place + 1 == run.parts.size() : readsInRun[place] < uses || uses == std::numeric_limits<uint8_t>::max();
        part.mustLackTrailingZeros |= part.isReadBehindRun && !reused.contains(part.value);
    }
    // A conversion may be left to what reads it.
    for (size_t place = run.parts.size(); place--;) {
        const Operation& conversion = m_code.at(run.parts[place].value);
        if (run.parts[place].step != Step::IsMadeTwice || !run.parts[place].isReadBehindRun || (conversion.opcode != Opcode::Box && conversion.opcode != Opcode::Unbox))
            continue;
        if (size_t converted = placeOf(conversion.valueOperand()); converted < place)
            run.parts[converted].isReadBehindRun = true;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
