/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTDerivedFunctions.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <functional>
#include <initializer_list>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

struct ResultNode;

struct DerivedFunctionRequests {
    FunctionID ensure(const DerivedFunctionKey& key) const { return pass ? pass->derivedFunction(key) : functions->ensure(key); }
    DerivedFunctionKey keyOf(FunctionID provisional) const { return functions->keyOf(provisional); }

    FunctionPassContext* pass;
    DerivedFunctions* functions;
};

struct EmissionInputs {
    explicit EmissionInputs(FunctionPassContext&);
    EmissionInputs(DerivedFunctionInputs&, FunctionID subject);

    AnalysisContext analysisContext() const { return { build, program, &flow, nullptr, function }; }
    TypeID outcomeType() const { return types.intern(Type::integer({ 0, numberOfEnumerators<JSONOutcome> - 1 })); }
    TypeID opaqueType(OpaqueKind kind) const { return types.intern(Type::forOpaque(kind)); }

    const Program& program;
    const FlowResults& flow;
    TypeArena& types;
    StringArena& strings;
    BuildContext& build;
    DerivedFunctionRequests functions;
    FunctionID function;
    // The subject of a limit hit: `function` may be provisional, which means nothing outside the pass.
    FunctionID functionAsRead;
};

struct DerivedSignature {
    Vector<TypeID, 2> parameters;
    Vector<TypeID, 3> results;
};

struct DerivedFunctionHead {
    Function function;
    BlockID entry;
    Vector<ValueID, 2> parameters;
};

DerivedFunctionHead createDerivedFunction(const EmissionInputs&, FunctionRole, CreatorRecord&&, const DerivedSignature&);
DerivedSignature signatureOfSerializer(const EmissionInputs&);
DerivedSignature signatureOfParser(const EmissionInputs&, const ResultNode&);

class Emission {
    WTF_MAKE_NONCOPYABLE(Emission);
public:
    Emission(IRBuilder& builder, const EmissionInputs& inputs)
        : m_builder(builder)
        , m_inputs(inputs)
    {
    }

    IRBuilder& builder() { return m_builder; }
    const EmissionInputs& inputs() const { return m_inputs; }
    const Program& program() const { return m_inputs.program; }
    const Function& function() const { return m_builder.function(); }
    const Type& typeOf(ValueID value) const { return program().typeOf(function(), value); }
    TypeID outcomeType() { return m_inputs.outcomeType(); }

    // The operation with each attribute handed to its setter.
    template<auto... setters>
    static Value make(Opcode opcode, std::initializer_list<ValueID> operands, const auto&... attributes)
    {
        Value value = IRBuilder::make(opcode, operands);
        (std::invoke(setters, value, attributes), ...);
        return value;
    }

    ValueID appendOutcome(JSONOutcome outcome) { return m_builder.appendInteger(static_cast<int64_t>(outcome)); }
    ValueID appendString(StringID string) { return m_builder.append(make<&Value::setString>(Opcode::String, { }, string)); }
    ValueID appendString(StringView text) { return appendString(m_inputs.strings.intern(text)); }
    ValueID appendNull() { return m_builder.append(Opcode::Null, { }); }
    ValueID appendResultAt(ValueID results, uint32_t index);
    void emitReturnOf(std::initializer_list<ValueID>);

    ValueID appendCallOf(FunctionID, std::initializer_list<ValueID> arguments);
    ValueID appendHelperCall(BuiltinID helper, std::initializer_list<ValueID> arguments);

    struct TestedObject {
        ValueID object;
        ValueID guard;
    };
    TestedObject emitShapeTest(ValueID, std::span<const LayoutID>, BlockID notTaken, BlockFrequency whenTaken = BlockFrequency::Normal);
    ValueID emitKeyOrderTest(const TestedObject&, LayoutID, std::span<const uint32_t> fields, BlockID notTaken);
    ValueID appendFieldRead(const TestedObject&, LayoutID, uint32_t field, Opcode = Opcode::GetSlot);

    void emitBranchOnOutcome(ValueID outcome, BlockID parsed, BlockID rejected, BlockID unknown);

    // The block that jumps to the named block takes that block's frequency, and the code goes on along the other edge.
    ValueID emitProvenBranch(ValueID test, BlockID notTaken, BlockFrequency whenTaken = BlockFrequency::Normal);
    ValueID emitTypeTest(ValueID, TypeID tested, BlockID notTaken);
    // Tests the parts of the field's type that a TypeTest can establish for this value: it proves no contents, storage or pristine state.
    ValueID emitRequireStoredType(ValueID, TypeID fieldType, BlockID notTaken);
    bool passesOnlyUndefinedIntoStoredType(const Type& value, TypeID fieldType);

    // What the type of a field proves about built-in classes and holds for a value because of what created the value.
    enum class ProofsByOrigin : uint8_t {
        None,
        AboutArrays,
        AboutEveryClass,
    };
    TypeID fieldTypeToTest(TypeID fieldType, ProofsByOrigin);
    // The result goes straight into the field: an assumption is checked against the position it feeds.
    ValueID emitRequireFieldType(ValueID, TypeID fieldType, ProofsByOrigin, BlockID notTaken);
    void emitJumpUnless(ValueID condition, BlockID otherwise);
    void emitJumpIf(ValueID condition, BlockID target);

    LayoutSetID layoutSetOf(std::span<const LayoutID> layouts) { return m_inputs.types.completeLayoutSet(layouts); }

private:
    IRBuilder& m_builder;
    const EmissionInputs& m_inputs;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
