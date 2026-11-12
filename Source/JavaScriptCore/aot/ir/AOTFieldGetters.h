/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

inline PropertyKeyID fieldReturnedByGetter(const Program& program, const Function& getter)
{
    if (getter.kind != FunctionKind::Getter || !getter.hasBody() || getter.isCoroutine() || getter.body.numberOfBlocks() != 1)
        return { };
    BodyBlock block = getter.body.entry();
    if (block.hasHandler() || block.numberOfValues() != 1)
        return { };
    BodyValue read = *block.values().begin();
    if (read.opcode() != Opcode::PropGet || read.isFreeName())
        return { };
    std::optional<uint32_t> parameter = block.indexOfParameter(read.objectOperand());
    if (!parameter || *parameter >= getter.numberOfEntryParameters() || getter.entryParameterKind(*parameter) != EntryParameter::This)
        return { };
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Return || terminator.as<TerminatorKind::Return>().valueOperand() != read.id())
        return { };
    return program.at(read.key()).kind == PropertyKeyKind::PrivateName ? PropertyKeyID() : read.key();
}

struct CapturedVariable {
    enum class Storage : uint8_t { None, Capture, EnvironmentField, ModuleVariable };

    explicit operator bool() const { return storage != Storage::None; }

    Storage storage { Storage::None };
    uint32_t capture { 0 };
    LayoutID layout;
    uint32_t field { 0 };
    VariableID variable;
};

struct ReturnedCapture {
    explicit operator bool() const { return !!variable; }

    CapturedVariable variable;
    CapturedVariable key;
};

class ReturnedCaptureMatcher {
public:
    explicit ReturnedCaptureMatcher(const Function& function)
        : m_function(function)
        , m_block(function.body.entry())
    {
    }

    ReturnedCapture match()
    {
        ValueID returned = listValues();
        if (!returned)
            return { };
        ReturnedCapture result;
        const BodyValue* read = find(returned);
        if (read && read->opcode() == Opcode::ElemGet) {
            ++m_numberOfValuesRead;
            result.variable = resolve(read->objectOperand());
            result.key = resolve(read->propertyOperand());
            if (!result.key)
                return { };
        } else
            result.variable = resolve(returned);
        return m_numberOfValuesRead == m_values.size() ? result : ReturnedCapture();
    }

    CapturedVariable matchKeyedLookup()
    {
        ValueID returned = listValues();
        const BodyValue* read = returned ? find(returned) : nullptr;
        if (!read || read->opcode() != Opcode::ElemGet)
            return { };
        std::optional<uint32_t> key = m_block.indexOfParameter(read->propertyOperand());
        if (!key || *key != m_function.firstParameterIndex() || *key >= m_function.numberOfEntryParameters() || m_function.entryParameterKind(*key) != EntryParameter::Parameter)
            return { };
        ++m_numberOfValuesRead;
        CapturedVariable object = resolve(read->objectOperand());
        return m_numberOfValuesRead == m_values.size() ? object : CapturedVariable();
    }

private:
    ValueID listValues()
    {
        constexpr uint32_t maxNumberOfValues = 5;
        BodyTerminator terminator = m_block.terminator();
        if (m_block.hasHandler() || m_block.numberOfValues() > maxNumberOfValues || terminator.kind() != TerminatorKind::Return)
            return { };
        for (BodyValue value : m_block.values())
            m_values.append(value);
        return terminator.as<TerminatorKind::Return>().valueOperand();
    }

    const BodyValue* find(ValueID id) const
    {
        auto found = std::ranges::find_if(m_values, [&](const BodyValue& value) { return value.id() == id; });
        return found == m_values.end() ? nullptr : &*found;
    }

    std::optional<uint32_t> captureOf(ValueID id) const
    {
        std::optional<uint32_t> parameter = m_block.indexOfParameter(id);
        if (!parameter || *parameter >= m_function.numberOfEntryParameters() || m_function.entryParameterKind(*parameter) != EntryParameter::Capture)
            return std::nullopt;
        return *parameter - m_function.numberOfEntryParametersBefore(EntryParameter::Capture);
    }

    CapturedVariable resolve(ValueID id)
    {
        using Storage = CapturedVariable::Storage;
        if (std::optional<uint32_t> capture = captureOf(id))
            return { .storage = Storage::Capture, .capture = *capture };
        const BodyValue* value = find(id);
        if (!value)
            return { };
        ++m_numberOfValuesRead;
        switch (value->opcode()) {
        case Opcode::CheckInitialized:
            return resolve(value->valueOperand());
        case Opcode::GetModuleVariable:
            return { .storage = Storage::ModuleVariable, .variable = value->variable() };
        case Opcode::GetEnvironmentField: {
            std::optional<uint32_t> environment = captureOf(value->environmentOperand());
            return environment ? CapturedVariable { .storage = Storage::EnvironmentField, .capture = *environment, .layout = value->layout(), .field = value->field() } : CapturedVariable();
        }
        default:
            return { };
        }
    }

    const Function& m_function;
    const BodyBlock m_block;
    Vector<BodyValue, 5> m_values;
    size_t m_numberOfValuesRead { 0 };
};

inline bool hasNameOfGetter(const Program& program, const Function& function)
{
    auto isNamedGet = [&] {
        PropertyKeyID get = program.keyOf(BuiltinNames::get);
        return get && function.name == program.at(get).text;
    };
    switch (function.kind) {
    case FunctionKind::Getter:
        return true;
    case FunctionKind::Plain:
        return function.name && (!function.has(FunctionFlags::HasExplicitName) || isNamedGet());
    case FunctionKind::Arrow:
        return !!function.name;
    case FunctionKind::Method:
        return isNamedGet();
    default:
        return false;
    }
}

inline ReturnedCapture findReturnedCapture(const Program& program, const Function& function)
{
    if (!hasNameOfGetter(program, function))
        return { };
    if (!function.hasBody() || function.isCoroutine() || function.body.numberOfBlocks() != 1 || !function.parameters.isEmpty() || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return { };
    return ReturnedCaptureMatcher(function).match();
}

inline CapturedVariable findKeyedLookup(const Function& function)
{
    bool isCallable = function.kind == FunctionKind::Plain || function.kind == FunctionKind::Arrow || function.kind == FunctionKind::Method;
    if (!isCallable || !function.hasBody() || function.isCoroutine() || function.body.numberOfBlocks() != 1 || function.parameters.size() != 1 || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return { };
    return ReturnedCaptureMatcher(function).matchKeyedLookup();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
