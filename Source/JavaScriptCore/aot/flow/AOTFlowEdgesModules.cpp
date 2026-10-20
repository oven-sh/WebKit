/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static bool mayCaptureDefaultMember(SyntheticExportRule rule)
{
    switch (rule) {
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::RuntimeModule:
        return true;
    case SyntheticExportRule::None:
    case SyntheticExportRule::CommonJsIgnoringAnnotation:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addModuleOperation(ValueID id, const Value& value)
{
    FlowModuleOperation operation { .origin = id, .result = localOf(id), .thrown = thrownTarget() };

    switch (value.opcode) {
    case Opcode::ModuleNamespace:
        operation.kind = FlowModuleOperationKind::Namespace;
        operation.module = value.module();
        break;
    case Opcode::ImportModule:
        operation.kind = FlowModuleOperationKind::Import;
        operation.module = value.module();
        break;
    case Opcode::RequireModule:
        operation.kind = FlowModuleOperationKind::Require;
        operation.module = value.module();
        break;
    case Opcode::ImportDynamic:
        operation.kind = FlowModuleOperationKind::ImportDynamic;
        operation.specifier = localOf(value.specifierOperand());
        addOperandUsesFromSchema(id, value, BitVector());
        break;
    case Opcode::RequireDynamic:
        operation.kind = FlowModuleOperationKind::RequireDynamic;
        operation.specifier = localOf(value.specifierOperand());
        addOperandUsesFromSchema(id, value, BitVector());
        break;
    case Opcode::RequireBuiltinModule:
        operation.kind = FlowModuleOperationKind::RequireBuiltinModule;
        operation.runtimeModule = value.runtimeModule();
        break;
    case Opcode::ResolveDynamic:
        addFromSchema(id, value);
        // import.meta.resolve is native code all the way.
        if (value.isURL())
            return;
        operation.kind = FlowModuleOperationKind::ResolveDynamic;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    m_assembler.add(operation);
}

// An import alone does not evaluate a module without side effects. A function of the module needs what the body of the module stores, and so does whoever names a variable of it.
void FlowSummaryBuilder::addUsesOfModules()
{
    if (!m_hasModulesWithoutSideEffects)
        return;

    Vector<ModuleID, 4> modules;
    if (m_function.module && !m_function.isModuleBody() && m_program.at(m_function.module).flags.contains(ModuleFlags::HasNoSideEffects))
        modules.append(m_function.module);
    // A module with side effects may be imported by unused modules alone.
    for (const FlowExternalNode& external : m_assembler.externals()) {
        if (external.kind() != FlowExternalKind::Variable)
            continue;
        ModuleID module = m_program.at(external.variable()).module;
        if (module != m_function.module && !modules.contains(module))
            modules.append(module);
    }
    for (ModuleID module : modules)
        m_assembler.add(FlowModuleOperation { .kind = FlowModuleOperationKind::Use, .module = module });
}

void FlowSummaryBuilder::addDirectEval(ValueID id, const Value& value)
{
    m_assembler.add(FlowUnseenCodeEvent { id, UnseenCodeCause::EvaluatesText });

    FlowLocalID unknownJavaScript = m_assembler.external(FlowExternalNode::unknownJavaScript());
    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (mayHoldReference(operand))
            m_assembler.copy(localOf(operand), unknownJavaScript);
    });

    FlowLocalID unknownValue = m_assembler.addTemporary();
    m_assembler.seed(FlowSeed::unknown(unknownValue));
    for (const EvalBinding& binding : m_program.at(value.evalScope()).bindings) {
        if (!binding.variable)
            continue;
        FlowLocalID location = m_assembler.external(FlowExternalNode::variable(binding.variable));
        m_assembler.copy(location, unknownJavaScript);
        m_assembler.copy(unknownValue, location);
        m_assembler.add(FlowLocationAccess { FlowAccessKind::Load, FlowReadUse::Value, isInCycle(), id, location, unknownJavaScript });
        m_assembler.add(FlowLocationAccess { FlowAccessKind::Store, FlowReadUse::Value, isInCycle(), id, location, unknownValue });
    }

    m_assembler.copy(unknownValue, localOf(id));
    m_assembler.copy(unknownValue, thrownTarget());
}

void FlowSummaryBuilder::addNativeBinding(ValueID id, const Value&)
{
    m_assembler.add(FlowOperandUse { .origin = id, .result = localOf(id) });
}

void FlowSummaryBuilder::addSyntheticExportCaptures()
{
    const Module& module = m_program.at(m_function.module);
    if (module.body != m_id || module.syntheticExports.isEmpty())
        return;

    FlowLocalID source = addSyntheticExportSource(module);
    if (!source)
        return;

    // The loader reads every own enumerable property of the value, whichever names the program imports, so every getter runs.
    FlowLocalID anyName = m_assembler.addTemporary();
    m_assembler.seed(FlowSeed::typeAsRead(anyName, TypeTable::string));
    m_assembler.add(FlowElementAccess {
        .kind = FlowAccessKind::Load,
        .receiver = source,
        .key = anyName,
        .value = m_assembler.addTemporary(),
        .thrown = m_assembler.thrown(),
    });

    for (const SyntheticExport& entry : module.syntheticExports)
        addCapturedExport(source, module, entry);
}

FlowLocalID FlowSummaryBuilder::addSyntheticExportSource(const Module& module)
{
    switch (module.exportRule) {
    case SyntheticExportRule::None:
        return { };
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::CommonJsIgnoringAnnotation: {
        FlowLocalID moduleObject = m_assembler.addTemporary();
        FlowLocalID source = m_assembler.addTemporary();
        m_assembler.seed(FlowSeed::moduleBuiltin(moduleObject, Builtins::ModuleObject, m_function.module));
        m_assembler.add(FlowPropertyAccess {
            .kind = FlowAccessKind::Load,
            .receiver = moduleObject,
            .key = m_program.keyOf(BuiltinNames::exports),
            .value = source,
            .thrown = m_assembler.thrown(),
        });
        return source;
    }
    case SyntheticExportRule::RuntimeModule:
        return m_assembler.result(0);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addCapturedExport(FlowLocalID source, const Module& module, const SyntheticExport& entry)
{
    if (!entry.variable)
        return;

    FlowLocalID captured = m_assembler.addTemporary();
    bool isDefaultExport = entry.name == m_program.stringOf(BuiltinNames::defaultKeyword);
    if (isDefaultExport)
        m_assembler.copy(source, captured);

    if (!isDefaultExport || mayCaptureDefaultMember(module.exportRule)) {
        FlowLocalID key = m_assembler.addTemporary();
        m_assembler.seed(FlowSeed::string(key, entry.name));
        // A getter that throws and a known member that is not an own enumerable property are captured as undefined.
        seedPrimitives(captured, Primitive::Undefined);
        m_assembler.add(FlowElementAccess {
            .kind = FlowAccessKind::Load,
            .receiver = source,
            .key = key,
            .value = captured,
            .thrown = m_assembler.thrown(),
        });
    }

    FlowLocalID location = m_assembler.external(FlowExternalNode::variable(entry.variable));
    m_assembler.copy(captured, location);
    m_assembler.add(FlowLocationAccess { FlowAccessKind::Store, FlowReadUse::Value, false, ValueID(), location, captured });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
