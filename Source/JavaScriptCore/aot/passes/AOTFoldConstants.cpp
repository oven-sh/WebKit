/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTDecidedBranches.h"
#include "AOTFunctionAnalyses.h"
#include "AOTKnownValue.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResultRules.h"
#include "AOTStringArena.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "TypeofType.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t maxPrimitiveTextBytes = 24;

static std::optional<TypeofType> typeofTypeNamed(std::span<const uint8_t> text)
{
    static constexpr std::pair<ASCIILiteral, TypeofType> names[] = {
        { "undefined"_s, TypeofType::Undefined },
        { "bigint"_s, TypeofType::BigInt },
        { "boolean"_s, TypeofType::Boolean },
        { "number"_s, TypeofType::Number },
        { "string"_s, TypeofType::String },
        { "symbol"_s, TypeofType::Symbol },
        { "object"_s, TypeofType::Object },
        { "function"_s, TypeofType::Function },
    };
    for (auto& [name, type] : names) {
        if (equalSpans(name.span8(), text))
            return type;
    }
    return std::nullopt;
}

static std::optional<Type> typeTestedByTypeOf(const Program& program, TypeofType result, const Type& source)
{
    switch (result) {
    case TypeofType::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case TypeofType::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    case TypeofType::Boolean:
        return Type::fromPrimitives(Primitive::Boolean);
    case TypeofType::Number:
        return Type::fromPrimitives(Primitive::Number);
    case TypeofType::String:
        return Type::fromPrimitives(Primitive::String);
    case TypeofType::Symbol:
        return Type::fromPrimitives(Primitive::Symbol);
    case TypeofType::Function:
        return Type::function();
    case TypeofType::Object:
        break;
    }

    // Type::object() includes the callable objects, whose typeof is "function", and no type stands for the others.
    if (!source.overlaps(Type::function(), program))
        return Type::object().withPrimitives(Primitive::Null);
    if (source.withOnlyObjects().isSubsetOf(Type::function(), program))
        return Type::fromPrimitives(Primitive::Null);
    return std::nullopt;
}

namespace {

class ConstantFolder {
public:
    explicit ConstantFolder(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_useLists(context.analyses.useLists())
    {
    }

    PassOutcome run()
    {
        Vector<ValueID, 16> foldedValues;
        for (BlockID blockID : m_context.analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(blockID);
            ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
            for (ValueID id : block.values) {
                if (id != tailCall && foldValue(id))
                    foldedValues.append(id);
            }
        }
        if (foldedValues.isEmpty())
            return PassOutcome::Unchanged;

        m_context.analyses.didChangeValues();
        narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build }, foldedValues.span());
        return PassOutcome::Changed;
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    const Value& definitionOf(ValueID id) const { return m_function.at(definitionThroughUpcasts(m_function, id)); }
    KnownValue knownValueOf(ValueID id) const { return Typed::knownValueOf(m_program, m_function, id); }

    bool foldValue(ValueID id)
    {
        bool didFold = false;
        while (foldOnce(id))
            didFold = true;
        return didFold;
    }

    bool foldOnce(ValueID id)
    {
        Value& value = m_function.at(id);
        Proof proof = provesOf(value.opcode);
        if (value.opcode == Opcode::Parameter || resultRuleOf(value.opcode) == ResultRule::Constant || isCast(value.opcode) || provesForStretch(proof))
            return false;
        if (numberOfResultsOf(m_program, m_function, value))
            return false;
        if (proof != Proof::None && isConditionOfBranchWithProofs(m_function, m_useLists, id))
            return false;
        if (foldKnownOperands(id, value) || foldSingleValueType(id, value) || foldKnownString(id, value))
            return true;

        switch (value.opcode) {
        case Opcode::TypeTest:
        case Opcode::IsClosureOf:
            return becomeBoolean(id, evaluateTestByTypes(m_program, m_function, value));
        case Opcode::StrictEq:
        case Opcode::Eq:
            return becomeBoolean(id, evaluateTestByTypes(m_program, m_function, value)) || foldTypeOfComparison(id, value);
        case Opcode::ToBoolean:
            return foldToBoolean(id, value);
        case Opcode::ToString:
            return foldToString(id, value);
        case Opcode::ElemGet:
        case Opcode::ElemSet:
        case Opcode::ElemHas:
        case Opcode::ElemDelete:
        case Opcode::ElemDefine:
            return convertToNamedAccess(value);
        case Opcode::SuperGet:
        case Opcode::SuperSet:
            return convertToNamedSuperAccess(value);
        case Opcode::CopyDataProperties:
            return foldCopyDataProperties(id, value);
        default:
            return false;
        }
    }

    static bool isPrimitive(KnownValue value) { return value.isKnown() && !value.isObject(); }

    bool foldKnownOperands(ValueID id, const Value& value)
    {
        KnownValue result = evaluateWithKnownOperands(m_program, m_function, value, [&](ValueID operand) {
            return knownValueOf(operand);
        });
        return isPrimitive(result) && becomeConstant(id, result);
    }

    bool foldSingleValueType(ValueID id, const Value& value)
    {
        if (resultRuleOf(value.opcode) == ResultRule::None)
            return false;
        KnownValue result = knownValueOf(id);
        if (!isPrimitive(result) || !m_context.analyses.effectsOf(id).onAnyEdge().isRemovableWhenUnused())
            return false;
        return becomeConstant(id, result);
    }

    bool foldKnownString(ValueID id, const Value& value)
    {
        if (!opcodesCreatingKnownStrings.contains(value.opcode))
            return false;

        uint64_t bytes = 0;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            KnownValue operand = knownValueOf(value.operand(index));
            if (!isPrimitive(operand))
                return false;
            bytes += operand.kind() == KnownValue::Kind::String ? m_program.string(operand.asString()).size() : maxPrimitiveTextBytes;
        }
        if (m_context.exceeds(LimitID::MaxFoldedStringBytes, bytes, id))
            return false;

        KnownValue result = evaluateStringOperation(m_program, m_context.strings, m_function, value, [&](ValueID operand) {
            return knownValueOf(operand);
        });
        return result.kind() == KnownValue::Kind::String && becomeConstant(id, result);
    }

    bool foldToBoolean(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        if (!typeOf(operand).isBoolean())
            return becomeBoolean(id, evaluateTestByTypes(m_program, m_function, value));

        becomeUpcast(m_function, id, operand);
        return true;
    }

    bool foldToString(ValueID id, const Value& value)
    {
        ValueID operand = value.valueOperand();
        if (!typeOf(operand).isAnyString())
            return false;

        becomeUpcast(m_function, id, operand);
        return true;
    }

    bool foldTypeOfComparison(ValueID id, Value& value)
    {
        for (unsigned i = 0; i < 2; ++i) {
            const Value& typeOfOperation = definitionOf(value.operand(i));
            const Value& string = definitionOf(value.operand(1 - i));
            if (typeOfOperation.opcode != Opcode::TypeOf || string.opcode != Opcode::String)
                continue;
            auto expectedTypeof = typeofTypeNamed(m_program.string(string.string()));
            if (!expectedTypeof)
                return becomeConstant(id, KnownValue::boolean(false));

            ValueID operand = typeOfOperation.valueOperand();
            std::optional<Type> tested = typeTestedByTypeOf(m_program, *expectedTypeof, typeOf(operand));
            if (!tested)
                return false;
            value.become(Opcode::TypeTest);
            value.setValueOperand(operand);
            value.setTestedType(m_context.types.intern(*tested));
            return true;
        }
        return false;
    }

    PropertyKeyID findConstantPropertyKey(ValueID key)
    {
        return internKey(m_context.types, constantKeyOf(m_program, typeOf(key)));
    }

    bool convertToNamedSuperAccess(Value& value)
    {
        ValueID computedKey = value.computedKeyOperand();
        PropertyKeyID key = computedKey ? findConstantPropertyKey(computedKey) : PropertyKeyID();
        if (!key)
            return false;
        value.setComputedKeyOperand({ });
        value.setKey(key);
        return true;
    }

    bool convertToNamedAccess(Value& value)
    {
        PropertyKeyID key = findConstantPropertyKey(value.propertyOperand());
        if (!key)
            return false;

        TypeID predicted = value.predictionOrType();
        becomeNamedAccess(value, key);
        retypeKeepingPrediction(value, predicted);
        return true;
    }

    void retypeKeepingPrediction(Value& value, TypeID predicted)
    {
        TypeArena& types = m_context.types;
        value.type = resultTypeOf(m_program, m_function, value, types);
        value.setWritten({ });
        TypeID narrowed = types.intersectionOf(value.type, predicted);
        value.setPrediction(narrowed == TypeTable::never || narrowed == value.type ? TypeID() : narrowed);
    }

    bool foldCopyDataProperties(ValueID id, const Value& value)
    {
        static constexpr uint16_t primitivesWithoutProperties = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;
        if (!typeOf(value.sourceOperand()).hasOnlyPrimitives(primitivesWithoutProperties))
            return false;
        becomeUndefined(m_function, id);
        return true;
    }

    bool becomeBoolean(ValueID id, KnownBoolean result)
    {
        switch (result) {
        case KnownBoolean::Unknown:
            return false;
        case KnownBoolean::False:
            return becomeConstant(id, KnownValue::boolean(false));
        case KnownBoolean::True:
            return becomeConstant(id, KnownValue::boolean(true));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool becomeConstant(ValueID id, KnownValue constant)
    {
        if (evaluateTypeTest(m_program, constant, typeOf(id)) != KnownBoolean::True)
            return false;

        Value& value = m_function.at(id);
        switch (constant.kind()) {
        case KnownValue::Kind::Undefined:
            value.become(Opcode::Undefined);
            break;
        case KnownValue::Kind::Null:
            value.become(Opcode::Null);
            break;
        case KnownValue::Kind::Boolean:
            value.become(Opcode::Boolean);
            value.setBoolean(constant.asBoolean());
            break;
        case KnownValue::Kind::Integer:
            value.become(Opcode::Integer);
            value.setInteger(constant.asInteger());
            break;
        case KnownValue::Kind::Double:
            value.become(Opcode::Double);
            value.setNumber(constant.asNumber());
            break;
        case KnownValue::Kind::String:
            value.become(Opcode::String);
            value.setString(constant.asString());
            break;
        case KnownValue::Kind::Unknown:
        case KnownValue::Kind::Object:
        case KnownValue::Kind::Function:
            RELEASE_ASSERT_NOT_REACHED();
        }
        value.type = resultTypeOf(m_program, m_function, value, m_context.types);
        value.setWritten({ });
        value.setPrediction({ });
        return true;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    const UseLists& m_useLists;
};

} // anonymous namespace

PassOutcome foldConstants(FunctionPassContext& context)
{
    return ConstantFolder(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
