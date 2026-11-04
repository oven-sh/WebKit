/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NOT COMPILED, AND INCLUDED BY NOBODY. This is what ir/generator/emit-cpp-cursors.ts writes as AOTSchemaBodyOperations.h from the schema as it stands at the tag before-one-walk-1101.
// It is here so that whoever writes against AOTBody.h can look a getter up. Where the schema changes, the generated header follows the schema and this copy does not.
// BodyValue and BodyTerminator have every getter below under the same name, for code that serves several entries (AOTSchemaBodyValueAccessors.inc, AOTSchemaBodyTerminatorAccessors.inc).
// There a getter whose field is optional in any entry answers with the optional type in all.
// The three BodyElement specializations at the head are written into AOTSchemaBodyElements.inc, which AOTBody.h includes in front of BodyList.

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

template<>
struct BodyElement<DeclaredType> {
    static constexpr uint32_t numberOfWords = 3;
    static DeclaredType decode(const uint32_t* words, const UnitIDRemap& unit)
    {
        std::array<uint32_t, PayloadWords<DeclaredType>::count> payload { programIDOf<TypeID>(unit, words[0]).bits(), optionalProgramIDOf<TypeSiteID>(unit, words[1]).bits(), words[2] };
        return PayloadWords<DeclaredType>::decode(payload);
    }
};

template<>
struct BodyElement<TailCast> {
    static constexpr uint32_t numberOfWords = 3;
    static TailCast decode(const uint32_t* words, const UnitIDRemap& unit)
    {
        TailCast record;
        record.declared = BodyElement<DeclaredType>::decode(words, unit);
        return record;
    }
};

template<>
struct BodyElement<TemplateString> {
    static constexpr uint32_t numberOfWords = 2;
    static TemplateString decode(const uint32_t* words, const UnitIDRemap& unit)
    {
        TemplateString record;
        record.cooked = optionalProgramIDOf<StringID>(unit, words[0]);
        record.raw = programIDOf<StringID>(unit, words[1]);
        return record;
    }
};

template<>
class BodyOperation<Opcode::Undefined> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::Null> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::Absent> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::Boolean> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    bool boolean() const { return plainAt<bool>(0); }
};

template<>
class BodyOperation<Opcode::Integer> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    int64_t integer() const { return int64At(0); }
};

template<>
class BodyOperation<Opcode::Double> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    double number() const { return doubleAt(0); }
};

template<>
class BodyOperation<Opcode::String> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    StringID string() const { return idAt<StringID>(0); }
};

template<>
class BodyOperation<Opcode::BigInt> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    StringID digits() const { return idAt<StringID>(0); }
};

template<>
class BodyOperation<Opcode::Builtin> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    BuiltinID builtin() const { return plainAt<BuiltinID>(0); }
    bool isNotReadFromHolder() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::ModuleBuiltin> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    BuiltinID builtin() const { return plainAt<BuiltinID>(0); }
    ModuleID module() const { return idAt<ModuleID>(1); }
};

template<>
class BodyOperation<Opcode::Data> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    DataID data() const { return idAt<DataID>(0); }
};

template<>
class BodyOperation<Opcode::NewClosure> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    FunctionID function() const { return idAt<FunctionID>(0); }
    std::span<const ValueID> capturesOperands() const { return operandsAt(1); }
};

template<>
class BodyOperation<Opcode::GetModuleVariable> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    VariableID variable() const { return idAt<VariableID>(0); }
};

template<>
class BodyOperation<Opcode::PutModuleVariable> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    VariableID variable() const { return idAt<VariableID>(0); }
    ValueID valueOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetEnvironmentField> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID environmentOperand() const { return operandAt(0); }
    LayoutID layout() const { return idAt<LayoutID>(1); }
    uint32_t field() const { return plainAt<uint32_t>(2); }
};

template<>
class BodyOperation<Opcode::PutEnvironmentField> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID environmentOperand() const { return operandAt(0); }
    LayoutID layout() const { return idAt<LayoutID>(1); }
    uint32_t field() const { return plainAt<uint32_t>(2); }
    ValueID valueOperand() const { return operandAt(3); }
};

template<>
class BodyOperation<Opcode::ScopeHasBinding> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID scopeOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
};

template<>
class BodyOperation<Opcode::NewObject> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    LayoutID layout() const { return idAt<LayoutID>(0); }
    std::span<const uint32_t> fields() const { return numbersAt(1); }
    ValueID prototypeOperand() const { return operandAt(2 + word(1)); }
    std::span<const ValueID> valuesOperands() const { return operandsAt(3 + word(1)); }
};

template<>
class BodyOperation<Opcode::NewDictionary> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::FinishObject> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::NewArray> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    TypeID contents() const { return idAt<TypeID>(0); }
    std::span<const uint32_t> holes() const { return numbersAt(1); }
    std::span<const ValueID> elementsOperands() const { return operandsAt(2 + word(1)); }
};

template<>
class BodyOperation<Opcode::AppendElement> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID valueOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::AppendSpread> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID iterableOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::AppendHole> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::NewRegExp> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    StringID pattern() const { return idAt<StringID>(0); }
    OptionSet<RegExpFlags> regExpFlags() const { return plainAt<OptionSet<RegExpFlags>>(1); }
};

template<>
class BodyOperation<Opcode::NewTemplateObject> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    BodyList<TemplateString> templateStrings() const { return listAt<TemplateString>(0); }
};

template<>
class BodyOperation<Opcode::NewRuntimeError> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    RuntimeErrorKind runtimeError() const { return plainAt<RuntimeErrorKind>(0); }
    StringID name() const { return optionalIDAt<StringID>(1); }
};

template<>
class BodyOperation<Opcode::NewFunctionThatThrows> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID getterOperand() const { return operandAt(0); }
    StringID name() const { return idAt<StringID>(1); }
};

template<>
class BodyOperation<Opcode::NewSuppressedError> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID errorOperand() const { return operandAt(0); }
    ValueID suppressedOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::CreateUnmappedArguments> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID countOperand() const { return operandAt(0); }
    ValueID restOperand() const { return operandAt(1); }
    std::span<const ValueID> parametersOperands() const { return operandsAt(2); }
};

template<>
class BodyOperation<Opcode::CreateMappedArguments> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID calleeOperand() const { return operandAt(0); }
    ValueID countOperand() const { return operandAt(1); }
    ValueID restOperand() const { return operandAt(2); }
    std::span<const ValueID> parametersOperands() const { return operandsAt(3); }
};

template<>
class BodyOperation<Opcode::GetMappedArgument> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID argumentsOperand() const { return operandAt(0); }
    uint32_t index() const { return plainAt<uint32_t>(1); }
};

template<>
class BodyOperation<Opcode::SetMappedArgument> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID argumentsOperand() const { return operandAt(0); }
    uint32_t index() const { return plainAt<uint32_t>(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::PropGet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
    bool isFreeName() const { return flags() & 1 << 0; }
    bool isForTypeOf() const { return flags() & 1 << 1; }
};

template<>
class BodyOperation<Opcode::PropSet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
    ECMAMode mode() const { return plainAt<ECMAMode>(2); }
    ValueID valueOperand() const { return operandAt(3); }
    bool isFreeName() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::PropDefine> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::PropHas> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
};

template<>
class BodyOperation<Opcode::PropDelete> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
    ECMAMode mode() const { return plainAt<ECMAMode>(2); }
    bool isFreeName() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::ElemGet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID propertyOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::ElemSet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID propertyOperand() const { return operandAt(1); }
    ECMAMode mode() const { return plainAt<ECMAMode>(2); }
    ValueID valueOperand() const { return operandAt(3); }
};

template<>
class BodyOperation<Opcode::ElemHas> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID propertyOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::ElemDelete> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID propertyOperand() const { return operandAt(1); }
    ECMAMode mode() const { return plainAt<ECMAMode>(2); }
};

template<>
class BodyOperation<Opcode::ElemDefine> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID propertyOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
    BuiltinID calledBuiltin() const { return plainAt<BuiltinID>(3); }
};

template<>
class BodyOperation<Opcode::DefineGetter> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID functionOperand() const { return operandAt(1); }
    ValueID computedKeyOperand() const { return operandAt(2); }
    PropertyKeyID key() const { return optionalIDAt<PropertyKeyID>(3); }
};

template<>
class BodyOperation<Opcode::DefineSetter> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID functionOperand() const { return operandAt(1); }
    ValueID computedKeyOperand() const { return operandAt(2); }
    PropertyKeyID key() const { return optionalIDAt<PropertyKeyID>(3); }
};

template<>
class BodyOperation<Opcode::TryDefineLiveBinding> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID targetOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
    ValueID sourceKeyOperand() const { return operandAt(2); }
    ValueID targetKeyOperand() const { return operandAt(3); }
    ValueID getterOperand() const { return operandAt(4); }
};

template<>
class BodyOperation<Opcode::TryReExportFields> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID targetOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
    FunctionID function() const { return idAt<FunctionID>(2); }
    bool reusesDescriptors() const { return flags() & 1 << 0; }
    bool keyIsFirstCapture() const { return flags() & 1 << 1; }
};

template<>
class BodyOperation<Opcode::SetFunctionName> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID functionOperand() const { return operandAt(0); }
    ValueID computedKeyOperand() const { return operandAt(1); }
    MethodKind methodKind() const { return plainAt<MethodKind>(2); }
};

template<>
class BodyOperation<Opcode::SetLiteralPrototype> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID prototypeOperand() const { return operandAt(1); }
    bool refusesOtherValues() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::CopyDataProperties> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID targetOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
    BodyList<PropertyKeyID> excludedKeys() const { return listAt<PropertyKeyID>(2); }
    std::span<const ValueID> excludedOperands() const { return operandsAt(3 + word(2)); }
};

template<>
class BodyOperation<Opcode::CopyObject> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID sourceOperand() const { return operandAt(0); }
    BodyList<PropertyKeyID> excludedKeys() const { return listAt<PropertyKeyID>(1); }
    std::span<const ValueID> excludedOperands() const { return operandsAt(2 + word(1)); }
    bool assigns() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::GetSuperBase> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    HomeObjectKind homeKind() const { return plainAt<HomeObjectKind>(0); }
    ValueID homeOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::SuperGet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID baseOperand() const { return operandAt(0); }
    ValueID receiverOperand() const { return operandAt(1); }
    ValueID computedKeyOperand() const { return operandAt(2); }
    PropertyKeyID key() const { return optionalIDAt<PropertyKeyID>(3); }
};

template<>
class BodyOperation<Opcode::SuperSet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ECMAMode mode() const { return plainAt<ECMAMode>(0); }
    ValueID baseOperand() const { return operandAt(1); }
    ValueID receiverOperand() const { return operandAt(2); }
    ValueID valueOperand() const { return operandAt(3); }
    ValueID computedKeyOperand() const { return operandAt(4); }
    PropertyKeyID key() const { return optionalIDAt<PropertyKeyID>(5); }
};

template<>
class BodyOperation<Opcode::Add> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Sub> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Mul> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Div> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Mod> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Pow> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::BitAnd> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::BitOr> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::BitXor> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::LShift> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::RShift> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::URShift> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Negate> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ToNumeric> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::Increment> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::Decrement> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::BitNot> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ToNumber> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::Less> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::LessEq> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Greater> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GreaterEq> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Eq> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::StrictEq> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID leftOperand() const { return operandAt(0); }
    ValueID rightOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::InstanceOf> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    ValueID constructorOperand() const { return operandAt(1); }
    StringID writtenExpression() const { return optionalIDAt<StringID>(2); }
};

template<>
class BodyOperation<Opcode::ToBoolean> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::LogicalNot> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::TypeOf> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ToString> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::Strcat> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    std::span<const ValueID> partsOperands() const { return operandsAt(0); }
};

template<>
class BodyOperation<Opcode::ToPropertyKey> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ToObject> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    StringID message() const { return optionalIDAt<StringID>(1); }
};

template<>
class BodyOperation<Opcode::ToThis> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::Upcast> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    TypeID castType() const { return idAt<TypeID>(1); }
};

template<>
class BodyOperation<Opcode::NarrowByField> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    ValueID readOperand() const { return operandAt(1); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(2); }
};

template<>
class BodyOperation<Opcode::Check> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    ExpectedType expectedType() const { return ExpectedType(idAt<TypeID>(1)); }
    TypeSiteID site() const { return optionalIDAt<TypeSiteID>(2); }
};

template<>
class BodyOperation<Opcode::Convert> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    DeclaredType declared() const { return recordAt<DeclaredType>(1); }
    IllTypedCause illTypedCause() const { return plainAt<IllTypedCause>(4); }
};

template<>
class BodyOperation<Opcode::CheckInitialized> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    StringID name() const { return idAt<StringID>(1); }
};

template<>
class BodyOperation<Opcode::RequireObjectCoercible> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    NullishAccess nullishAccess() const { return plainAt<NullishAccess>(1); }
    StringID name() const { return optionalIDAt<StringID>(2); }
};

template<>
class BodyOperation<Opcode::RequireObject> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    RuntimeErrorKind runtimeError() const { return plainAt<RuntimeErrorKind>(1); }
};

template<>
class BodyOperation<Opcode::TypeTest> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    TypeID testedType() const { return idAt<TypeID>(1); }
};

template<>
class BodyOperation<Opcode::Call> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID calleeOperand() const { return operandAt(0); }
    ValueID thisValueOperand() const { return operandAt(1); }
    std::span<const ValueID> argumentsOperands() const { return operandsAt(2); }
    PropertyKeyID calleeKey() const { return optionalIDAt<PropertyKeyID>(3 + word(2)); }
    StringID calleeName() const { return optionalIDAt<StringID>(4 + word(2)); }
    ExpectedResultID expectedResult() const { return optionalIDAt<ExpectedResultID>(5 + word(2)); }
    BuiltinID calledBuiltin() const { return plainAt<BuiltinID>(6 + word(2)); }
    std::optional<uint32_t> callNumber() const { return plainAt<std::optional<uint32_t>>(7 + word(2)); }
    bool isAnnotatedPure() const { return flags() & 1 << 0; }
    bool isNeverInlined() const { return flags() & 1 << 1; }
    bool isDoneInPlace() const { return flags() & 1 << 2; }
};

template<>
class BodyOperation<Opcode::Construct> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID calleeOperand() const { return operandAt(0); }
    ValueID newTargetOperand() const { return operandAt(1); }
    std::span<const ValueID> argumentsOperands() const { return operandsAt(2); }
    StringID calleeName() const { return optionalIDAt<StringID>(3 + word(2)); }
    bool isAnnotatedPure() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::CallList> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID calleeOperand() const { return operandAt(0); }
    ValueID thisValueOperand() const { return operandAt(1); }
    ValueID listOperand() const { return operandAt(2); }
    PropertyKeyID calleeKey() const { return optionalIDAt<PropertyKeyID>(3); }
    StringID calleeName() const { return optionalIDAt<StringID>(4); }
    ExpectedResultID expectedResult() const { return optionalIDAt<ExpectedResultID>(5); }
};

template<>
class BodyOperation<Opcode::ConstructList> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID calleeOperand() const { return operandAt(0); }
    ValueID newTargetOperand() const { return operandAt(1); }
    ValueID listOperand() const { return operandAt(2); }
    StringID calleeName() const { return optionalIDAt<StringID>(3); }
};

template<>
class BodyOperation<Opcode::CallDirect> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    FunctionID function() const { return idAt<FunctionID>(0); }
    std::span<const ValueID> argumentsOperands() const { return operandsAt(1); }
};

template<>
class BodyOperation<Opcode::Results> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    std::span<const ValueID> valuesOperands() const { return operandsAt(0); }
};

template<>
class BodyOperation<Opcode::ResultAt> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID resultsOperand() const { return operandAt(0); }
    uint32_t index() const { return plainAt<uint32_t>(1); }
};

template<>
class BodyOperation<Opcode::CatchValue> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::CatchException> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::LoopHint> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::ReleaseHint> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<TerminatorKind::Jump> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<TerminatorKind::Branch> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID conditionOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<TerminatorKind::Switch> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID subjectOperand() const { return operandAt(0); }
    BodyList<int64_t> cases() const { return listAt<int64_t>(1); }
};

template<>
class BodyOperation<TerminatorKind::SwitchString> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID subjectOperand() const { return operandAt(0); }
    BodyList<StringID> strings() const { return listAt<StringID>(1); }
};

template<>
class BodyOperation<TerminatorKind::Return> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<TerminatorKind::ReturnCall> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID callOperand() const { return operandAt(0); }
    BodyList<TailCast> casts() const { return listAt<TailCast>(1); }
};

template<>
class BodyOperation<TerminatorKind::Throw> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<TerminatorKind::Rethrow> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID exceptionOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<TerminatorKind::Unreachable> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::ResolveHeritage> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID heritageOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::NewClass> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    LayoutID layout() const { return idAt<LayoutID>(0); }
    ValueID constructorOperand() const { return operandAt(1); }
    ValueID heritageOperand() const { return operandAt(2); }
    ValueID prototypeParentOperand() const { return operandAt(3); }
};

template<>
class BodyOperation<Opcode::GetClassPrototype> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID classObjectOperand() const { return operandAt(0); }
    LayoutID layout() const { return idAt<LayoutID>(1); }
};

template<>
class BodyOperation<Opcode::DefineMethod> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID classObjectOperand() const { return operandAt(0); }
    ValueID functionOperand() const { return operandAt(1); }
    ValueID computedKeyOperand() const { return operandAt(2); }
    LayoutID layout() const { return idAt<LayoutID>(3); }
    ClassSide side() const { return plainAt<ClassSide>(4); }
    MethodKind methodKind() const { return plainAt<MethodKind>(5); }
    uint32_t index() const { return plainAt<uint32_t>(6); }
    PropertyKeyID key() const { return optionalIDAt<PropertyKeyID>(7); }
};

template<>
class BodyOperation<Opcode::FinishClass> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID classObjectOperand() const { return operandAt(0); }
    LayoutID layout() const { return idAt<LayoutID>(1); }
};

template<>
class BodyOperation<Opcode::NewClassToken> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    LayoutID layout() const { return idAt<LayoutID>(0); }
};

template<>
class BodyOperation<Opcode::GetConstructPrototype> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID newTargetOperand() const { return operandAt(0); }
    LayoutID layout() const { return idAt<LayoutID>(1); }
};

template<>
class BodyOperation<Opcode::SuperConstruct> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    LayoutID layout() const { return idAt<LayoutID>(0); }
    ValueID parentOperand() const { return operandAt(1); }
    ValueID newTargetOperand() const { return operandAt(2); }
    std::span<const ValueID> argumentsOperands() const { return operandsAt(3); }
};

template<>
class BodyOperation<Opcode::SuperConstructList> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    LayoutID layout() const { return idAt<LayoutID>(0); }
    ValueID parentOperand() const { return operandAt(1); }
    ValueID newTargetOperand() const { return operandAt(2); }
    ValueID listOperand() const { return operandAt(3); }
};

template<>
class BodyOperation<Opcode::PrivateGet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID tokenOperand() const { return operandAt(1); }
    LayoutID layout() const { return idAt<LayoutID>(2); }
    uint32_t field() const { return plainAt<uint32_t>(3); }
};

template<>
class BodyOperation<Opcode::PrivateSet> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID tokenOperand() const { return operandAt(1); }
    LayoutID layout() const { return idAt<LayoutID>(2); }
    uint32_t field() const { return plainAt<uint32_t>(3); }
    ValueID valueOperand() const { return operandAt(4); }
};

template<>
class BodyOperation<Opcode::PrivateDefine> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID tokenOperand() const { return operandAt(1); }
    LayoutID layout() const { return idAt<LayoutID>(2); }
    uint32_t field() const { return plainAt<uint32_t>(3); }
    ValueID valueOperand() const { return operandAt(4); }
};

template<>
class BodyOperation<Opcode::PrivateHas> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID tokenOperand() const { return operandAt(1); }
    LayoutID layout() const { return idAt<LayoutID>(2); }
    uint32_t field() const { return plainAt<uint32_t>(3); }
};

template<>
class BodyOperation<Opcode::IteratorOpen> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID iterableOperand() const { return operandAt(0); }
    IteratorKind iteratorKind() const { return plainAt<IteratorKind>(1); }
    std::optional<uint32_t> elementCount() const { return plainAt<std::optional<uint32_t>>(2); }
    bool isDestructuring() const { return flags() & 1 << 0; }
    bool hasRest() const { return flags() & 1 << 1; }
    bool isDelegating() const { return flags() & 1 << 2; }
};

template<>
class BodyOperation<Opcode::IteratorResume> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID iteratorOperand() const { return operandAt(0); }
    ValueID sentOperand() const { return operandAt(1); }
    IteratorKind iteratorKind() const { return plainAt<IteratorKind>(2); }
    ResumeMode resumeMode() const { return plainAt<ResumeMode>(3); }
    std::optional<uint32_t> stepIndex() const { return plainAt<std::optional<uint32_t>>(4); }
    std::span<const ValueID> heldOperands() const { return operandsAt(5); }
    bool readsValueWhenDone() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::IteratorClose> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID iteratorOperand() const { return operandAt(0); }
    IteratorKind iteratorKind() const { return plainAt<IteratorKind>(1); }
    IteratorCompletion completion() const { return plainAt<IteratorCompletion>(2); }
    std::span<const ValueID> heldOperands() const { return operandsAt(3); }
};

template<>
class BodyOperation<Opcode::ForInOpen> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ForInStep> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID enumeratorOperand() const { return operandAt(0); }
    ValueID objectOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::Await> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    std::span<const ValueID> heldOperands() const { return operandsAt(1); }
    bool takesExtraTurn() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::Yield> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    std::span<const ValueID> heldOperands() const { return operandsAt(1); }
};

template<>
class BodyOperation<Opcode::InitialYield> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    std::span<const ValueID> heldOperands() const { return operandsAt(0); }
};

template<>
class BodyOperation<Opcode::ReadScope> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    std::span<const ValueID> valuesOperands() const { return operandsAt(0); }
};

template<>
class BodyOperation<Opcode::ModuleNamespace> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ImportPhase phase() const { return plainAt<ImportPhase>(1); }
    bool ofModuleItself() const { return flags() & 1 << 0; }
    bool asksAnew() const { return flags() & 1 << 1; }
};

template<>
class BodyOperation<Opcode::ImportModule> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ValueID optionsOperand() const { return operandAt(1); }
    ModuleID referrer() const { return idAt<ModuleID>(2); }
};

template<>
class BodyOperation<Opcode::RequireModule> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
    StringID specifier() const { return idAt<StringID>(2); }
    bool ifHeldForImport() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::EvaluateModule> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
    bool isBare() const { return flags() & 1 << 0; }
    bool isCallOfRequire() const { return flags() & 1 << 1; }
};

template<>
class BodyOperation<Opcode::RecordRequire> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
};

template<>
class BodyOperation<Opcode::WasLoadedSynchronously> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
};

template<>
class BodyOperation<Opcode::SeesMockOf> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
};

template<>
class BodyOperation<Opcode::MockSeenBy> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
};

template<>
class BodyOperation<Opcode::AreLoaderFunctionsStored> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::CountExecution> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    StringID description() const { return idAt<StringID>(1); }
    uint32_t counter() const { return plainAt<uint32_t>(2); }
};

template<>
class BodyOperation<Opcode::ResolveModule> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleID module() const { return idAt<ModuleID>(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
    StringID specifier() const { return idAt<StringID>(2); }
    bool isURL() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::ThrowModuleFailure> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ModuleFailureID failure() const { return idAt<ModuleFailureID>(0); }
    bool isForRequire() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::ImportDynamic> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID specifierOperand() const { return operandAt(0); }
    ValueID optionsOperand() const { return operandAt(1); }
    ModuleID referrer() const { return idAt<ModuleID>(2); }
    bool allNamesAreKnown() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::RequireDynamic> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID specifierOperand() const { return operandAt(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
    bool allNamesAreKnown() const { return flags() & 1 << 0; }
};

template<>
class BodyOperation<Opcode::ResolveDynamic> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID specifierOperand() const { return operandAt(0); }
    ModuleID referrer() const { return idAt<ModuleID>(1); }
    bool isURL() const { return flags() & 1 << 0; }
    bool isForRequire() const { return flags() & 1 << 1; }
};

template<>
class BodyOperation<Opcode::DirectEval> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    EvalScopeID evalScope() const { return idAt<EvalScopeID>(0); }
    ValueID textOperand() const { return operandAt(1); }
    ValueID thisValueOperand() const { return operandAt(2); }
    ValueID newTargetOperand() const { return operandAt(3); }
    std::span<const ValueID> environmentsOperands() const { return operandsAt(4); }
};

template<>
class BodyOperation<Opcode::IsConstructor> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ReadsIteratorOfListUnseen> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ArraySpeciesCreate> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID lengthOperand() const { return operandAt(0); }
    ValueID arrayOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetByValWithThis> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID thisValueOperand() const { return operandAt(1); }
    ValueID propertyOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::GetOwnProperty> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
};

template<>
class BodyOperation<Opcode::ToLength> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ThrowNewError> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ErrorKind errorKind() const { return plainAt<ErrorKind>(0); }
    StringID message() const { return idAt<StringID>(1); }
};

template<>
class BodyOperation<Opcode::ResumeGenerator> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID generatorOperand() const { return operandAt(0); }
    ResumeMode resumeMode() const { return plainAt<ResumeMode>(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::StepGenerator> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID generatorOperand() const { return operandAt(0); }
    ResumeMode resumeMode() const { return plainAt<ResumeMode>(1); }
    ValueID valueOperand() const { return operandAt(2); }
    GeneratorStepCheck check() const { return plainAt<GeneratorStepCheck>(3); }
};

template<>
class BodyOperation<Opcode::IsGeneratorSuspendedAtStart> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID generatorOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::IsUnmodifiedGenerator> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::CreateIteratorResult> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    ValueID doneOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::StepArrayIterator> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID iteratorOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::StepMapIterator> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID iteratorOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::StepSetIterator> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID iteratorOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::RequireBuiltinModule> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    RuntimeModuleID runtimeModule() const { return plainAt<RuntimeModuleID>(0); }
};

template<>
class BodyOperation<Opcode::NativeBinding> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    BuiltinID builtin() const { return plainAt<BuiltinID>(0); }
};

template<>
class BodyOperation<Opcode::LinkTimeConstant> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    LinkTimeConstantID constant() const { return plainAt<LinkTimeConstantID>(0); }
};

template<>
class BodyOperation<Opcode::HasJSType> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
    CheckedJSType first() const { return plainAt<CheckedJSType>(1); }
    CheckedJSType last() const { return plainAt<CheckedJSType>(2); }
};

template<>
class BodyOperation<Opcode::FindsDeclaredMember> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    PropertyKeyID key() const { return idAt<PropertyKeyID>(1); }
};

template<>
class BodyOperation<Opcode::GetInternalField> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    uint32_t index() const { return plainAt<uint32_t>(1); }
};

template<>
class BodyOperation<Opcode::PutInternalField> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    uint32_t index() const { return plainAt<uint32_t>(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::NewPromise> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::CreatePromise> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID constructorOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ResolvePromise> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID promiseOperand() const { return operandAt(0); }
    ValueID valueOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::RejectPromise> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID promiseOperand() const { return operandAt(0); }
    ValueID valueOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::IsPristineArray> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID valueOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::ArrayStorageKind> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::HasContiguousIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetContiguousIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetDoubleIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::PutContiguousIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::PutDoubleIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::GetOwnContiguousIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetOwnDoubleIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetOwnInt32Index> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::PutOwnContiguousIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::PutOwnDoubleIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::PutOwnInt32Index> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::NewContiguousArray> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID lengthOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::NewDoubleArray> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID lengthOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::NewArrayLike> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID lengthOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::NewArrayWithCapacity> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID lengthOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::TryPutByValDirect> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::TryPutIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::TryCopyFilledElements> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
    ValueID countOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::TryPutElements> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID sourceOperand() const { return operandAt(1); }
    ValueID countOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::TryPutDoubleIndex> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID positionOperand() const { return operandAt(1); }
    ValueID valueOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::GrowNewArray> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID lengthOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::TruncateNewArray> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID arrayOperand() const { return operandAt(0); }
    ValueID lengthOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::CollectionTable> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID collectionOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::CollectionIteratorClosedMark> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }
};

template<>
class BodyOperation<Opcode::GetCollectionIteratorTable> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::PutCollectionIteratorTable> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID objectOperand() const { return operandAt(0); }
    ValueID valueOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::IsCollectionIteratorClosedMark> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID tableOperand() const { return operandAt(0); }
};

template<>
class BodyOperation<Opcode::MapNext> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID mapOperand() const { return operandAt(0); }
    ValueID tableOperand() const { return operandAt(1); }
    ValueID entryOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::SetNext> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    ValueID setOperand() const { return operandAt(0); }
    ValueID tableOperand() const { return operandAt(1); }
    ValueID entryOperand() const { return operandAt(2); }
};

template<>
class BodyOperation<Opcode::IsClosureOf> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    FunctionID function() const { return idAt<FunctionID>(0); }
    ValueID valueOperand() const { return operandAt(1); }
};

template<>
class BodyOperation<Opcode::GetCapture> : public BodyFields {
public:
    explicit BodyOperation(BodyFields fields) : BodyFields(fields) { }

    FunctionID function() const { return idAt<FunctionID>(0); }
    uint32_t index() const { return plainAt<uint32_t>(1); }
    ValueID closureOperand() const { return operandAt(2); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
