/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedIR.h"
#include "AOTTypedThreads.h"
#include "ImplementationVisibility.h"
#include "Options.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class Reader {
public:
    Reader(Program& program)
        : m_program(program)
        , m_buffer(program.buffer)
    {
    }

    ASCIILiteral run()
    {
        if (m_buffer.size() < sizeof(Format::Header) || reinterpret_cast<uintptr_t>(m_buffer.data()) % alignof(uint32_t))
            return "the program is shorter than its header"_s;
        m_header = reinterpret_cast<const Format::Header*>(m_buffer.data());
        if (m_header->magic != Format::magic)
            return "this is not a program written by the front end"_s;
        if (m_header->version != Format::version || m_header->opcodeNamesHash != Format::opcodeNamesHash || m_header->builtinsHash != builtinsHash)
            return "the front end and the engine have different versions"_s;
        if (m_header->sizeInBytes != m_buffer.size())
            return "the program's length does not match its header"_s;
        for (unsigned i = 0; i < std::size(Format::recordSizes); ++i) {
            if (m_header->recordSizes[i] != Format::recordSizes[i])
                return "the front end and the engine lay out a record differently"_s;
        }
        if (m_header->flags & ~Format::programLogsTypeErrors)
            return "the program has a flag that the engine does not know"_s;
        m_program.logsTypeErrors = m_header->flags & Format::programLogsTypeErrors;

        auto bytes = section<uint8_t>(m_header->bytes);
        m_words = section<uint32_t>(m_header->words);
        m_objectParts = section<Format::ObjectPart>(m_header->objectParts);
        m_fields = section<Format::Field>(m_header->fields);
        size_t numberOfFunctions = section<Format::Function>(m_header->functions).size();
        m_methods = section<Format::Method>(m_header->methods);
        m_blocks = section<Format::Block>(m_header->blocks);
        m_values = section<Format::Value>(m_header->values);
        m_targets = section<Format::Target>(m_header->targets);
        if (m_failed)
            return "a section extends outside the program"_s;

        {
            auto strings = section<Format::String>(m_header->strings);
            constexpr size_t stringsInChunk = 1024;
            m_program.strings.grow(strings.size());
            m_program.m_builtinNames.grow(strings.size());
            std::atomic<bool> someStringExtendsOutside { false };
            std::atomic<bool> someStringIsModuleGraph { false };
            forEachIndexInParallel(static_cast<unsigned>((strings.size() + stringsInChunk - 1) / stringsInChunk), [&](unsigned chunk) {
                size_t end = std::min(strings.size(), (static_cast<size_t>(chunk) + 1) * stringsInChunk);
                for (size_t index = static_cast<size_t>(chunk) * stringsInChunk; index < end; ++index) {
                    auto& string = strings[index];
                    if (static_cast<uint64_t>(string.offset) + string.length > bytes.size()) {
                        someStringExtendsOutside = true;
                        return;
                    }
                    m_program.strings[index] = bytes.subspan(string.offset, string.length);
                    if (equalSpans(m_program.strings[index], asBytes("ModuleGraph"_s.span8())))
                        someStringIsModuleGraph = true;
                    m_program.m_builtinNames[index] = Builtins::singleton().nameOf(m_program.strings[index]);
                }
            });
            if (someStringExtendsOutside)
                return "a string extends outside the program"_s;
            m_program.mayRunInMultipleInstances |= someStringIsModuleGraph;
            for (size_t index = 0; index < strings.size(); ++index) {
                BuiltinName name = m_program.m_builtinNames[index];
                if (!name)
                    continue;
                if (m_program.m_builtinNameStrings.size() <= name.index())
                    m_program.m_builtinNameStrings.grow(name.index() + 1);
                m_program.m_builtinNameStrings[name.index()] = StringID(index);
            }
        }
        for (auto& declared : section<Format::DeclaredType>(m_header->declaredTypes)) {
            m_callSignatureRead = Format::none;
            m_types.append(m_program.types.intern(read(declared)));
            m_callSignatures.append(m_callSignatureRead);
        }
        for (TypeID type : m_types)
            m_predictedTypes.append(m_program.at(type).hasLiteralString() ? m_program.types.intern(m_program.at(type).withLiteralStringWidened()) : type);
        auto& types = m_program.types;
        types.never = types.intern(Type::never());
        types.undefined = types.intern(Type::fromPrimitives(Primitive::Undefined));
        types.boolean = types.intern(Type::fromPrimitives(Primitive::Boolean));
        types.int32 = types.intern(Type::integer(Range::int32()));
        types.number = types.intern(Type::fromPrimitives(Primitive::Number));
        types.string = types.intern(Type::fromPrimitives(Primitive::String));
        types.function = types.intern(Type::function());
        types.object = types.intern(Type::object());
        types.anything = types.intern(Type::anything());
        types.integer = types.intern(Type::integer(Range::full()));
        auto shapes = section<Format::Shape>(m_header->shapes);
        m_program.layouts.grow(shapes.size());
        Vector<ASCIILiteral> shapeFailureReason(shapes.size());
        Vector<Vector<std::pair<LayoutID, uint32_t>>> symbolKeysOfShapes(shapes.size());
        forEachIndexInParallel(shapes.size(), [&](unsigned index) {
            shapeFailureReason[index] = [&]() -> ASCIILiteral {
                auto& shape = shapes[index];
                Layout layout;
                layout.kind = static_cast<LayoutKind>(shape.kind);
                layout.name = StringID(shape.name);
                layout.base = LayoutID(shape.base);
                layout.constructor = FunctionID(shape.constructor);
                if (layout.kind == LayoutKind::Environment && shape.flags & Format::ShapeMayBeReused)
                    layout.reuseVariable = VariableID(shape.environment);
                else
                    layout.environment = LayoutID(shape.environment);
                layout.isAllocated = shape.flags & Format::ShapeIsAllocated;
                if (shape.declaredPath != Format::none && shape.declaredPath >= m_program.strings.size())
                    return "the declaration path of a shape is not a string of the program"_s;
                layout.declaredPath = StringID(shape.declaredPath);
                layout.declaredLine = shape.declaredLine;
                layout.mayBeFrozen = shape.flags & Format::ShapeMayBeFrozen;
                layout.hasFixedLayout = shape.flags & Format::ShapeHasFixedLayout;
                layout.isEvaluatedOnce = shape.flags & Format::ShapeIsEvaluatedOnce;
                layout.hasUntypedBase = shape.flags & Format::ShapeHasUntypedBase;
                layout.hasImmutableMembers = shape.flags & Format::ShapeHasFixedMembers;
                layout.mayHaveOtherPrototype = shape.flags & Format::ShapeMayHaveOtherPrototype;
                layout.prototypeIsSetByTypedCode = shape.flags & Format::ShapePrototypeIsSetByTypedCode;
                layout.hasComputedStaticMemberName = shape.flags & Format::ShapeHasComputedStaticMemberName;
                layout.constructorMayBeReplaced = shape.flags & Format::ShapeConstructorMayBeReplaced;
                layout.acceptsUntypedObjects = shape.flags & Format::ShapeAcceptsUntypedObjects;
                layout.isRelaxedForLogging = shape.flags & Format::ShapeIsRelaxedForLogging;
                if (layout.isRelaxedForLogging && (!layout.acceptsUntypedObjects || !m_program.logsTypeErrors))
                    return "a shape is relaxed for logging, but it does not accept untyped objects or the program does not log"_s;
            layout.acceptsOnlyOtherLayouts = shape.flags & Format::ShapeAcceptsOnlyOtherLayouts;
            if (layout.acceptsOnlyOtherLayouts && !layout.acceptsUntypedObjects)
                return "a shape accepts only other layouts, but does not accept untyped objects"_s;
                for (uint32_t subtype : slice(m_words, shape.members))
                    layout.subtypes.append(LayoutID(subtype));
                for (uint32_t subtype : slice(m_words, shape.structuralSubtypes))
                    layout.structuralSubtypes.append(LayoutID(subtype));
                for (uint32_t specialization : slice(m_words, shape.specializations))
                    layout.specializations.append(LayoutID(specialization));
                auto fields = slice(m_fields, shape.fields);
                layout.fields.reserveInitialCapacity(fields.size());
                for (auto& from : fields) {
                    Field field;
                    field.name = StringID(from.name);
                    field.type = trustedTypeAt(from.declaredType);
                    field.flags = (from.flags & (fieldIsOptional | fieldIsImmutable)) | (from.flags & Format::FieldIsMethod ? fieldIsMethod : 0) | (from.flags & Format::FieldIsAnnotated ? fieldIsAnnotated : 0);
                    if (layout.kind == LayoutKind::Environment && from.flags & Format::FieldIsNeverReadUninitialized)
                        field.flags |= fieldIsNeverReadUninitialized | (from.flags & Format::FieldIsStoredOnce ? fieldIsStoredOnce : 0);
                    if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldIsNeverStored)
                        field.flags |= fieldIsNeverStored;
                    if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldIsPredictedInt32)
                        field.flags |= fieldIsPredictedInt32;
                    if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldIsSetAtCreation)
                        field.flags |= fieldIsSetAtCreation;
                    if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldMayBeSetAtCreation)
                        field.flags |= fieldMayBeSetAtCreation;
                    if (from.flags & Format::FieldDisallowsUndefined)
                        field.flags |= fieldDisallowsUndefined;
                    if (from.flags & Format::FieldIsAbsentAtCreation) {
                        if (layout.kind != LayoutKind::Instance || !(from.flags & Format::FieldIsOptional))
                            return "a field that is absent at allocation is not an optional field of a class"_s;
                        field.flags |= fieldIsAbsentAtCreation;
                    }
                    if (from.flags & Format::FieldIsNeverObservedAbsent) {
                        if (!isObjectLayout(layout.kind) || from.flags & (Format::FieldIsOptional | Format::FieldIsNeverStored))
                            return "a field that is never observed absent is not a required field of an object"_s;
                        if (!Options::auditAOTTypedAssertions())
                            field.flags |= fieldIsNeverObservedAbsent | fieldIsNeverObservedAbsentAsDeclared;
                    }
                    if (layout.kind != LayoutKind::Environment && Options::auditAOTTypedAssertions()) [[unlikely]]
                        field.flags &= ~fieldIsImmutable;
                    auto literals = slice(m_words, from.literals);
                    if (literals.size() % 3)
                        return "the literals of a field are truncated"_s;
                    for (size_t i = 0; i < literals.size(); i += 3) {
                        bool isNumber = literals[i] == static_cast<uint32_t>(Format::LiteralKind::Number);
                        if (!isNumber && (literals[i] != static_cast<uint32_t>(Format::LiteralKind::String) || literals[i + 1] >= m_program.strings.size()))
                            return "a literal of a field is neither a string of the program nor a number"_s;
                        field.literals.append(isNumber ? Literal { StringID(), std::bit_cast<double>(static_cast<uint64_t>(literals[i + 2]) << 32 | literals[i + 1]) } : Literal { StringID(literals[i + 1]), 0 });
                    }
                    if (from.typeSource != Format::none && from.typeSource >= m_program.strings.size())
                        return "the type source of a field is not a string of the program"_s;
                    field.typeSource = StringID(from.typeSource);
                    if (from.singleFunction != Format::none && from.singleFunction >= numberOfFunctions)
                        return "the single function of a field is not a function of the program"_s;
                    field.singleFunction = FunctionID(from.singleFunction);
                    field.singleFunctionIsProven = field.singleFunction && layout.kind == LayoutKind::Environment && from.singleFunctionFlags & Format::SingleFunctionIsProven;
                    field.singleFunctionOrNullish = field.singleFunction && layout.kind == LayoutKind::Environment && from.singleFunctionFlags & Format::SingleFunctionOrNullish;
                    layout.fields.append(WTF::move(field));
                }
                for (auto& method : slice(m_methods, shape.methods)) {
                    bool isStatic = method.flags & Format::MethodIsStatic;
                    if (method.name == Format::none) {
                        if (method.flags & Format::MethodKeyIsSymbol && !Options::auditAOTTypedAssertions()) {
                            symbolKeysOfShapes[index].append({ LayoutID(index), method.key });
                            layout.hasMethodWithSymbolKey |= !isStatic;
                        } else
                            (isStatic ? layout.hasComputedStaticMemberName : layout.hasComputedMethodName) = true;
                    }
                    (isStatic ? layout.staticMethods : layout.methods).append(Method { StringID(method.name), FunctionID(method.function), method.kind, method.key, !!(method.flags & Format::MethodMayBeReplaced) });
                }
                auto staticFields = slice(m_words, shape.staticFields);
                for (size_t i = 0; i + 1 < staticFields.size(); i += 2)
                    layout.staticFields.append(StaticField { StringID(staticFields[i]), trustedTypeAt(staticFields[i + 1]) });
                layout.indexFields();
                m_program.layouts[index] = WTF::move(layout);
                return { };
            }();
        });
        for (ASCIILiteral failureReason : shapeFailureReason) {
            if (!failureReason.isNull())
                return failureReason;
        }
        for (auto& keys : symbolKeysOfShapes)
            m_symbolKeys.appendVector(keys);
        for (auto& layout : m_program.layouts) {
            if (isObjectLayout(layout.kind)) {
                for (auto& field : layout.fields)
                    m_program.hasFieldsWithSymbolKeys |= field.name && isConstantSymbol(m_program.string(field.name));
            }
            if (layout.kind != LayoutKind::Instance)
                continue;
            for (auto& field : layout.fields) {
                if (field.flags & fieldIsMethod)
                    m_program.methodFieldNames.set(field.name.index());
            }
        }
        for (auto& layout : m_program.layouts)
            layout.mayDespecialize = isObjectLayout(layout.kind) && layout.isAllocated && layout.acceptsUntypedObjects && !layout.checksUntypedObjects() && !Options::auditAOTTypedAssertions();
        for (auto& supertype : m_program.layouts) {
            if (supertype.acceptsUntypedObjects)
                continue;
            for (LayoutID subtype : supertype.subtypes) {
                if (subtype.index() < m_program.layouts.size())
                    m_program.layouts[subtype.index()].mayDespecialize = false;
            }
        }
        for (auto& layout : m_program.layouts) {
            if (layout.mayDespecialize)
                layout.mayBeFrozen = false;
        }
        if (Options::auditAOTTypedAssertions()) [[unlikely]] {
            auto& layouts = m_program.layouts;
            for (auto& supertype : layouts) {
                for (LayoutID id : supertype.subtypes) {
                    if (id.index() >= layouts.size() || &layouts[id.index()] == &supertype)
                        continue;
                    auto& subtype = layouts[id.index()];
                    for (auto& field : supertype.fields) {
                        if (auto found = subtype.fieldNamed(field.name))
                            subtype.fields[*found].type = unionOf(subtype.fields[*found].type, field.type);
                    }
                }
            }
        }
        for (auto& from : section<Format::Variable>(m_header->variables)) {
            Variable variable;
            variable.name = StringID(from.name);
            variable.module = ModuleID(from.module);
            variable.type = trustedTypeAt(from.declaredType);
            variable.startsUndefined = from.initialization == Format::Initialization::Undefined;
            variable.isStoredOnce = from.initialization == Format::Initialization::NeverReadUninitializedStoredOnce;
            variable.isNeverReadUninitialized = variable.isStoredOnce || from.initialization == Format::Initialization::NeverReadUninitialized;
            variable.startsZero = from.initialization == Format::Initialization::Zero;
            if (from.initialization > Format::Initialization::Zero)
                return "a variable has an unknown initial state"_s;
            if (from.singleFunction != Format::none && from.singleFunction >= numberOfFunctions)
                return "the single function of a variable is not a function of the program"_s;
            variable.singleFunction = FunctionID(from.singleFunction);
            variable.singleFunctionIsProven = variable.singleFunction && from.singleFunctionFlags & Format::SingleFunctionIsProven;
            variable.singleFunctionOrNullish = variable.singleFunction && from.singleFunctionFlags & Format::SingleFunctionOrNullish;
            m_program.variables.append(WTF::move(variable));
        }
        for (auto& layout : m_program.layouts) {
            if (!layout.isEvaluatedOnce || !layout.hasImmutableMembers || Options::auditAOTTypedAssertions() || layout.staticFields.size() + layout.staticMethods.size() > 100)
                continue;
            for (auto& field : layout.staticFields) {
                if (!field.type)
                    continue;
                if (layout.classObjectSlot == Format::none) {
                    layout.classObjectSlot = WTF::roundUpToMultipleOf<2>(m_program.numberOfStaticFieldSlots);
                    m_program.numberOfStaticFieldSlots = layout.classObjectSlot + 2;
                }
                field.place = m_program.numberOfStaticFieldSlots++;
            }
        }
        auto modules = section<Format::Module>(m_header->modules);
        m_program.modules.grow(modules.size());
        Vector<ASCIILiteral> moduleFailureReason(modules.size());
        forEachIndexInParallel(modules.size(), [&](unsigned index) {
            moduleFailureReason[index] = [&]() -> ASCIILiteral {
                auto& from = modules[index];
                Module module;
                module.path = StringID(from.path);
                module.initializer = FunctionID(from.initializer);
                if (from.displayPath != Format::none && from.displayPath >= m_program.strings.size())
                    return "the display path of a module is not a string of the program"_s;
                module.displayPath = StringID(from.displayPath);
                module.lineTableSize = from.lineTable.count;
                if (!readLineTable(slice(bytes, from.lineTable), module))
                    return "the line table of a module is malformed"_s;
                module.printed = slice(bytes, from.printed);
                m_program.modules[index] = WTF::move(module);
                return { };
            }();
        });
        for (ASCIILiteral failureReason : moduleFailureReason) {
            if (!failureReason.isNull())
                return failureReason;
        }
        auto functions = section<Format::Function>(m_header->functions);
        m_program.functions.reserveInitialCapacity(2 * functions.size());
#if !ASAN_ENABLED
        {
            constexpr size_t pageBytes = 4 * KB;
            constexpr size_t bytesInChunk = MB;
            auto* room = reinterpret_cast<volatile uint8_t*>(m_program.functions.begin());
            size_t size = functions.size() * sizeof(Function);
            forEachIndexInParallel(static_cast<unsigned>((size + bytesInChunk - 1) / bytesInChunk), [&](unsigned chunk) {
                size_t end = std::min(size, (static_cast<size_t>(chunk) + 1) * bytesInChunk);
                for (size_t at = static_cast<size_t>(chunk) * bytesInChunk; at < end; at += pageBytes)
                    room[at] = 0;
            });
        }
#endif
        m_program.functions.grow(functions.size());
        Vector<Vector<std::pair<LayoutID, uint32_t>>> cachedClosures(functions.size());
        constexpr size_t functionsPerChunk = 32;
        Vector<Functions::ValueIndexRows> valueIndexRows((functions.size() + functionsPerChunk - 1) / functionsPerChunk);
        forEachIndexInParallel(valueIndexRows.size(), [&](unsigned chunk) {
            size_t end = std::min(functions.size(), (chunk + 1) * functionsPerChunk);
            for (size_t i = chunk * functionsPerChunk; i < end; ++i) {
                m_program.functions[i] = read(functions[i]);
                auto& function = std::as_const(m_program).functions[i];
                for (auto& value : function.values) {
                    if (value.opcode != Opcode::NewClosureOnce)
                        continue;
                    LayoutID environment = value.child(0).index() < function.values.size() ? m_program.typeOf(function, value.child(0)).singleLayout() : LayoutID();
                    if (environment && m_program.at(environment).kind == LayoutKind::Environment && value.field() < m_program.at(environment).fields.size())
                        cachedClosures[i].append({ environment, value.field() });
                }
                valueIndexRows[chunk].append(i, function);
            }
        });
        m_program.functions.adoptValueIndexRows(valueIndexRows.span());
        destroyOnHelperThread(WTF::move(valueIndexRows));
        for (auto& ofFunction : cachedClosures) {
            for (auto [environment, field] : ofFunction)
                m_program.at(environment).fields[field].flags |= fieldHoldsCachedClosure;
        }
        for (uint32_t index : slice(m_words, m_header->entries)) {
            if (index >= m_program.functions.size())
                return "an entry point is not a function of the program"_s;
            if (m_program.isEntry(FunctionID(index)))
                return "an entry point is listed twice"_s;
            m_program.entries.append(FunctionID(index));
        }
        if (m_program.entries.isEmpty())
            return "the program has no entry point"_s;
        for (uint32_t string : slice(m_words, m_header->extendedGlobals)) {
            BuiltinName name = string < m_program.strings.size() ? m_program.builtinName(StringID(string)) : BuiltinName();
            if (Builtins::singleton().mayBeExtended(name))
                m_program.extendedGlobals.set(name.index());
        }
        for (uint32_t string : slice(m_words, m_header->prototypePropertyNames)) {
            if (string >= m_program.strings.size())
                return "a name defined on a prototype is not a string of the program"_s;
            m_program.prototypePropertyNames.set(string);
        }
        auto regExpLiterals = slice(m_words, m_header->regExpLiterals);
        if (regExpLiterals.size() % 2)
            return "the list of regular expression literals does not consist of pairs"_s;
        for (size_t i = 0; i < regExpLiterals.size(); i += 2) {
            if (regExpLiterals[i] >= m_program.strings.size() || regExpLiterals[i + 1] >= m_program.strings.size())
                return "a regular expression literal is not a pair of strings of the program"_s;
            m_program.regExpLiterals.append({ StringID(regExpLiterals[i]), StringID(regExpLiterals[i + 1]) });
        }
        m_program.sites = Vector<uint32_t>(slice(m_words, m_header->sites));
        auto webAssemblyModules = slice(m_words, m_header->webAssemblyModules);
        if (webAssemblyModules.size() % 2)
            return "the WebAssembly modules are not pairs"_s;
        for (size_t i = 0; i < webAssemblyModules.size(); i += 2) {
            if (static_cast<uint64_t>(webAssemblyModules[i]) + webAssemblyModules[i + 1] > bytes.size())
                return "a WebAssembly module extends outside the program"_s;
            m_program.webAssemblyModules.append(bytes.subspan(webAssemblyModules[i], webAssemblyModules[i + 1]));
        }
        auto builtinModules = slice(m_words, m_header->builtinModules);
        if (builtinModules.size() % 2)
            return "the list of builtin modules does not consist of pairs"_s;
        for (size_t i = 0; i < builtinModules.size(); i += 2) {
            uint32_t id = builtinModules[i];
            uint32_t index = builtinModules[i + 1];
            if (i && id <= builtinModules[i - 2])
                return "the builtin modules are not sorted by number"_s;
            if (index >= m_program.functions.size())
                return "a builtin module is not a function of the program"_s;
            auto& function = m_program.functions.atKeepingValueIndex(index);
            if (function.kind != FunctionKind::Plain || function.declaredAs & (functionIsAsync | functionIsGenerator) || !function.parameterTypes.isEmpty() || !function.captureTypes.isEmpty())
                return "a builtin module is not a plain function without parameters and captures"_s;
            function.flags |= Format::FunctionIsUsedAsValue;
            if (function.module)
                m_program.modules[function.module.index()].isBuiltin = true;
            m_program.builtinModules.append({ id, FunctionID(index) });
        }
        auto builtinFunctions = slice(m_words, m_header->builtinFunctions);
        if (builtinFunctions.size() % 3)
            return "the list of builtin functions does not consist of triples"_s;
        for (size_t i = 0; i < builtinFunctions.size(); i += 3) {
            uint32_t id = builtinFunctions[i];
            uint32_t index = builtinFunctions[i + 1];
            uint32_t visibility = builtinFunctions[i + 2];
            if (i && id <= builtinFunctions[i - 3])
                return "the builtin functions are not sorted by number"_s;
            if (index >= m_program.functions.size() || !std::as_const(m_program).functions[index].captureTypes.isEmpty())
                return "a builtin function is not a function of the program without captures"_s;
            if (visibility > static_cast<uint32_t>(ImplementationVisibility::PrivateRecursive))
                return "a builtin function has an unknown visibility"_s;
            m_program.functions.atKeepingValueIndex(index).flags |= Format::FunctionIsUsedAsValue;
            m_program.builtinFunctions.append({ id, FunctionID(index), visibility });
        }
        for (auto& function : std::as_const(m_program).functions) {
            if (function.has(Format::FunctionIsInBuiltinModule) && function.module && !m_program.modules[function.module.index()].initializer)
                m_program.builtinFunctionModules.set(function.module.index());
        }
        auto functionsByKey = slice(m_words, m_header->functionsByKey);
        if (functionsByKey.size() % 3)
            return "the list of functions by key does not consist of triples"_s;
        for (size_t i = 0; i < functionsByKey.size(); i += 3) {
            uint32_t property = functionsByKey[i];
            uint32_t key = functionsByKey[i + 1];
            uint32_t index = functionsByKey[i + 2];
            if (i && std::pair(property, key) <= std::pair(functionsByKey[i - 3], functionsByKey[i - 2]))
                return "the functions by key are not sorted by property and key"_s;
            if (property >= m_program.strings.size() || key >= m_program.strings.size() || index >= m_program.functions.size())
                return "a function by key does not name a string and a function of the program"_s;
            m_program.functionsByKey.append({ StringID(property), StringID(key), FunctionID(index) });
        }
        if (m_failed)
            return "a list extends outside its section"_s;
        return checkClasses();
    }

private:
    ASCIILiteral checkClasses()
    {
        auto& layouts = m_program.layouts;
        auto& functions = std::as_const(m_program).functions;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (layout.reuseVariable && (layout.reuseVariable.index() >= m_program.variables.size() || layout.fields.size() < 2))
                return "an environment that may be reused has no variable to be cached in"_s;
            if (layout.kind != LayoutKind::Instance) {
                if (layout.constructor || layout.environment || !layout.staticMethods.isEmpty() || !layout.staticFields.isEmpty())
                    return "a shape that is not a class has members that only a class can have"_s;
                for (auto& method : layout.methods) {
                    if (layout.kind != LayoutKind::Record || method.kind == MethodKind::Method || !method.name || method.name.index() >= m_program.strings.size() || method.function.index() >= functions.size())
                        return "a shape that is not a class has methods"_s;
                }
                continue;
            }
            if (!layout.constructor) {
                if (layout.isAllocated || layout.environment || !layout.methods.isEmpty() || !layout.staticMethods.isEmpty() || !layout.staticFields.isEmpty())
                    return "a class without a constructor has instances or members"_s;
                continue;
            }
            if (layout.constructor.index() >= functions.size() || functions[layout.constructor.index()].kind != FunctionKind::Constructor)
                return "the constructor of a class is not a constructor"_s;
            auto& constructor = m_program.functions.atKeepingValueIndex(layout.constructor.index());
            if (constructor.constructedClass)
                return "two classes have the same constructor"_s;
            constructor.constructedClass = LayoutID(i);
            if (!constructor.has(Format::FunctionHasThis) || (constructor.has(Format::FunctionHasNewTarget) && constructor.parameterTypes.isEmpty()))
                return "a constructor does not accept the arguments it is passed"_s;
            if (layout.hasUntypedBase && !constructor.has(Format::FunctionHasNewTarget))
                return "the constructor of a class with an untyped base does not accept new.target"_s;
            if (!layout.hasFixedLayout && (!layout.fields.isEmpty() || layout.isAllocated || !layout.subtypes.isEmpty() || !layout.specializations.isEmpty()))
                return "a class without a layout has fields or instances"_s;
            if (layout.hasFixedLayout && layout.hasUntypedBase)
                return "a class with an untyped base has a layout"_s;
            if (layout.base) {
                if (layout.base.index() >= i || layouts[layout.base.index()].kind != LayoutKind::Instance)
                    return "a class extends something that is not an earlier class"_s;
                auto& base = layouts[layout.base.index()];
                if (!layout.hasFixedLayout || !base.hasFixedLayout || !base.constructor)
                    return "a class without a layout extends or is extended by a class of the program"_s;
                if (layout.isEvaluatedOnce && !base.isEvaluatedOnce)
                    return "a class that is evaluated once extends one that is not"_s;
            }
            uint32_t numberOfVariables = m_program.variables.size();
            if (layout.environment) {
                if (layout.isEvaluatedOnce || layout.environment.index() >= layouts.size() || layouts[layout.environment.index()].kind != LayoutKind::Environment)
                    return "a class closes over something that is not an environment"_s;
                numberOfVariables = layouts[layout.environment.index()].fields.size();
            } else if (!layout.isEvaluatedOnce)
                return "a class that is not evaluated once has no environment"_s;
            auto hasOtherCaptures = [&](const Function& function) {
                if (function.captureTypes.isEmpty())
                    return false;
                return function.captureTypes.size() != 1 || !layout.environment || m_program.at(function.captureTypes[0]) != Type::forLayout(layout.environment);
            };
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods) {
                    if (method.function.index() >= functions.size() || (method.name ? method.name.index() >= m_program.strings.size() : method.key >= numberOfVariables))
                        return "a method has no function or no name"_s;
                    if (hasOtherCaptures(functions[method.function.index()]))
                        return "a method captures something other than the environment of its class"_s;
                }
            }
            if (hasOtherCaptures(constructor))
                return "a constructor captures something other than the environment of its class"_s;
            for (auto& field : layout.staticFields) {
                if (field.name.index() >= m_program.strings.size())
                    return "a static field has no name"_s;
            }
        }
        for (auto [id, key] : m_symbolKeys) {
            auto& layout = m_program.at(id);
            TypeID type = layout.environment ? m_program.at(layout.environment).fields[key].type : m_program.variables[key].type;
            if (!type || !m_program.at(type).hasOnlyPrimitives(Primitive::Symbol))
                return "the variable of a computed name that is declared to be a symbol holds other values too"_s;
        }
        for (unsigned i = 0; i < m_program.types.size(); ++i) {
            for (LayoutID supertype : m_program.at(TypeID(i)).objectLayouts) {
                if (supertype.index() >= layouts.size() || (m_program.at(supertype).kind == LayoutKind::Instance && !m_program.at(supertype).hasFixedLayout))
                    return "a type names a class without a layout"_s;
            }
        }
        return { };
    }

    template<typename T>
    std::span<const T> section(Format::Section section)
    {
        if (section.offsetInBytes % alignof(T) || static_cast<uint64_t>(section.offsetInBytes) + static_cast<uint64_t>(section.count) * sizeof(T) > m_buffer.size()) {
            m_failed = true;
            return { };
        }
        return { reinterpret_cast<const T*>(m_buffer.data() + section.offsetInBytes), section.count };
    }

    template<typename T>
    std::span<const T> slice(std::span<const T> all, Format::Span span)
    {
        if (static_cast<uint64_t>(span.start) + span.count > all.size()) {
            m_failed = true;
            return { };
        }
        return all.subspan(span.start, span.count);
    }

    template<typename Container>
    void readValues(Format::Span span, Container& result)
    {
        auto words = slice(m_words, span);
        result.reserveInitialCapacity(words.size());
        for (uint32_t word : words)
            result.append(ValueID(word));
    }

    static bool readLineTable(std::span<const uint8_t> table, Module& module)
    {
        size_t at = 0;
        bool isValid = true;
        auto next = [&]() -> uint32_t {
            uint64_t result = 0;
            for (unsigned shift = 0; shift < 35; shift += 7) {
                if (at >= table.size())
                    break;
                uint8_t byte = table[at++];
                result |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if (!(byte & 0x80)) {
                    isValid &= result <= UINT32_MAX;
                    return static_cast<uint32_t>(result);
                }
            }
            isValid = false;
            return 0;
        };
        module.lineStarts.append(0);
        if (table.empty())
            return true;
        uint32_t numberOfLines = next();
        for (uint32_t line = 1; isValid && line < numberOfLines; ++line)
            module.lineStarts.append(module.lineStarts.last() + next());
        uint32_t line = 0;
        for (uint32_t remaining = next(); isValid && remaining--;) {
            line += next();
            if (line >= module.lineStarts.size())
                return false;
            uint32_t offset = module.lineStarts[line];
            uint32_t excessBytes = 0;
            for (uint32_t count = next(); isValid && count--;) {
                uint32_t word = next();
                uint32_t length = (word & 3) + 2;
                offset += (word >> 2) + length;
                excessBytes += length == 2 ? 1 : 2;
                module.columnAdjustments.append(Module::ColumnAdjustment { offset, line, excessBytes });
            }
        }
        return isValid && at == table.size();
    }

    TypeID typeAt(uint32_t index)
    {
        if (index == Format::none)
            return { };
        if (index >= m_types.size()) {
            m_failed = true;
            return { };
        }
        return m_types[index];
    }

    bool isTrustedType(TypeID id)
    {
        auto& type = m_program.at(id);
        if (!holdsCells(repOf(type)) || type.isUndefinedOrNull() || type.isNever())
            return true;
        auto shapes = section<Format::Shape>(m_header->shapes);
        for (LayoutID supertype : type.objectLayouts) {
            if (supertype.index() < shapes.size() && shapes[supertype.index()].kind == Format::ShapeKind::Environment)
                return true;
        }
        return false;
    }

    TypeID trustedTypeAt(uint32_t index)
    {
        TypeID given = typeAt(index);
        if (!given || !Options::auditAOTTypedAssertions() || isTrustedType(given))
            return given;
        return m_program.types.anything;
    }

    TypeID assertedTypeAt(uint32_t index)
    {
        TypeID given = typeAt(index);
        if (!given || !Options::auditAOTTypedAssertions() || isTrustedType(given) || given == m_program.types.anything)
            return { };
        return given;
    }

    TypeID unionOf(TypeID a, TypeID b)
    {
        if (!a || !b || a == b)
            return a == b ? a : TypeID();
        Type type = m_program.at(a);
        Type other = m_program.at(b);
        uint16_t arrayClass = Builtins::singleton().arrayClass().index;
        if (other.engineClasses.contains(arrayClass))
            type.elements = type.engineClasses.contains(arrayClass) ? unionOf(type.elements, other.elements) : other.elements;
        type.primitives |= other.primitives;
        type.integers = type.integers.unionWith(other.integers);
        type.anyObject |= other.anyObject;
        type.anyFunction |= other.anyFunction || type.anyObject;
        type.objectLayouts.appendVector(other.objectLayouts);
        type.engineClasses.appendVector(other.engineClasses);
        type.normalizeLiteralString();
        if (type.anyObject) {
            type.objectLayouts.clear();
            type.engineClasses.clear();
            type.elements = { };
        }
        std::ranges::sort(type.objectLayouts);
        type.objectLayouts.shrink(std::unique(type.objectLayouts.begin(), type.objectLayouts.end()) - type.objectLayouts.begin());
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        return m_program.types.intern(type);
    }

    bool readCallSignature(const Format::ObjectPart& part)
    {
        if (part.shapeOrName >= m_program.strings.size())
            return false;
        auto text = m_program.string(StringID(part.shapeOrName));
        auto prefix = asBytes("(signature "_s.span8());
        if (text.size() < prefix.size() + 4 || !equalSpans(text.first(prefix.size()), prefix))
            return false;
        CallSignature signature;
        size_t at = prefix.size();
        for (; at < text.size() && isASCIIDigit(text[at]); ++at)
            signature.minArgumentCount = signature.minArgumentCount * 10 + (text[at] - '0');
        signature.hasRestParameter = at + 1 < text.size() && text[at + 1] == '1';
        auto arguments = slice(m_words, part.typeArguments);
        if (arguments.empty()) {
            m_failed = true;
            return true;
        }
        signature.result = typeAt(arguments[0]);
        for (uint32_t argument : arguments.subspan(1))
            signature.parameters.append(typeAt(argument));
        auto& alike = m_callSignaturesByHash.add(computeHash(signature.result, signature.parameters, signature.minArgumentCount, signature.hasRestParameter) >> 1, Vector<uint32_t, 1>()).iterator->value;
        for (uint32_t known : alike) {
            if (m_program.callSignatures[known] == signature) {
                m_callSignatureRead = known;
                return true;
            }
        }
        m_callSignatureRead = safeCast<uint32_t>(m_program.callSignatures.size());
        alike.append(m_callSignatureRead);
        m_program.callSignatures.append(WTF::move(signature));
        return true;
    }

    Type read(const Format::DeclaredType& declared)
    {
        static constexpr std::pair<uint32_t, uint16_t> primitives[] = {
            { Format::PrimitiveUndefined, Primitive::Undefined },
            { Format::PrimitiveNull, Primitive::Null },
            { Format::PrimitiveFalse, Primitive::False },
            { Format::PrimitiveTrue, Primitive::True },
            { Format::PrimitiveNumber, Primitive::Number },
            { Format::PrimitiveString, Primitive::String },
            { Format::PrimitiveSymbol, Primitive::Symbol },
            { Format::PrimitiveBigInt, Primitive::BigInt },
            { Format::PrimitiveLiteralString, Primitive::LiteralString },
        };
        uint16_t bits = 0;
        for (auto [from, to] : primitives) {
            if (declared.primitives & from)
                bits |= to;
        }
        Type type = Type::fromPrimitives(bits);
        if (declared.flags & Format::DeclaredIntegers && !(bits & Primitive::Int)) {
            type.primitives |= Primitive::Int;
            type.integers.minimum = static_cast<int64_t>(static_cast<uint64_t>(declared.minIntegerHigh) << 32 | declared.minIntegerLow);
            type.integers.maximum = static_cast<int64_t>(static_cast<uint64_t>(declared.maxIntegerHigh) << 32 | declared.maxIntegerLow);
        }
        type.anyObject = declared.flags & Format::DeclaredAnyObject;
        type.anyFunction = declared.flags & Format::DeclaredAnyFunction;
        auto& builtins = Builtins::singleton();
        auto addPrimitiveOrWrapperObject = [&](uint16_t primitive) {
            type.primitives |= primitive;
            if (primitive & Primitive::Int)
                type.integers = Range::full();
            type.anyObject = true;
        };
        for (auto& part : slice(m_objectParts, declared.objects)) {
            switch (part.kind) {
            case Format::ObjectPartKind::Class:
                type.objectLayouts.append(LayoutID(part.shapeOrName));
                break;
            case Format::ObjectPartKind::Array: {
                bool hasArrayPart = type.engineClasses.contains(builtins.arrayClass().index);
                type.engineClasses.append(builtins.arrayClass().index);
                auto arguments = slice(m_words, part.typeArguments);
                TypeID elements = arguments.size() == 1 ? typeAt(arguments[0]) : TypeID();
                type.elements = hasArrayPart ? unionOf(type.elements, elements) : elements;
                break;
            }
            case Format::ObjectPartKind::GlobalClass: {
                if (readCallSignature(part)) {
                    type.anyFunction = true;
                    break;
                }
                BuiltinName name = part.shapeOrName < m_program.strings.size() ? m_program.builtinName(StringID(part.shapeOrName)) : BuiltinName();
                switch (builtins.interfaceNamed(name)) {
                case GlobalInterface::Class:
                    if (builtins.classNamed(name)->type == CellType)
                        type.anyObject = true;
                    else
                        type.engineClasses.append(builtins.classNamed(name)->index);
                    break;
                case GlobalInterface::Function:
                    type.anyFunction = true;
                    break;
                case GlobalInterface::String:
                    addPrimitiveOrWrapperObject(Primitive::String);
                    break;
                case GlobalInterface::Number:
                    addPrimitiveOrWrapperObject(Primitive::Number);
                    break;
                case GlobalInterface::Boolean:
                    addPrimitiveOrWrapperObject(Primitive::Boolean);
                    break;
                case GlobalInterface::Symbol:
                    addPrimitiveOrWrapperObject(Primitive::Symbol);
                    break;
                case GlobalInterface::BigInt:
                    addPrimitiveOrWrapperObject(Primitive::BigInt);
                    break;
                case GlobalInterface::Unknown:
                    type.anyObject = true;
                    break;
                }
                break;
            }
            }
        }
        if (type.anyObject) {
            type.anyFunction = true;
            type.objectLayouts.clear();
            type.engineClasses.clear();
            type.elements = { };
        }
        std::ranges::sort(type.objectLayouts);
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        return type;
    }

    bool readNeedsCheck(uint32_t readType, uint32_t slotType)
    {
        TypeID wanted = typeAt(readType);
        TypeID held = typeAt(slotType);
        return wanted && !(held ? m_program.at(held) : Type::anything()).isSubsetOf(m_program.at(wanted), m_program);
    }

    void prepareForAudit(Value& value, const Format::Value& from, const Function& function)
    {
        value.setPredictedClosure({ });
        value.setPredictedByKey({ });
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::ConstantSymbol:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
        case Opcode::NewObjectWithShape:
            value.type = typeAt(from.type);
            return;
        case Opcode::GetModuleVariable: {
            auto variables = section<Format::Variable>(m_header->variables);
            if (from.immediates[0] < variables.size() && readNeedsCheck(from.type, variables[from.immediates[0]].declaredType))
                value.setAssertedType(assertedTypeAt(from.type));
            return;
        }
        case Opcode::CallDirect: {
            auto functions = section<Format::Function>(m_header->functions);
            if (from.immediates[0] < functions.size() && readNeedsCheck(from.type, functions[from.immediates[0]].resultType))
                value.setAssertedType(assertedTypeAt(from.type));
            return;
        }
        case Opcode::GetField:
        case Opcode::PutField: {
            auto shapes = section<Format::Shape>(m_header->shapes);
            auto fields = from.immediates[0] < shapes.size() ? slice(m_fields, shapes[from.immediates[0]].fields) : std::span<const Format::Field>();
            if (from.immediates[1] >= fields.size()) {
                m_failed = true;
                return;
            }
            auto& field = fields[from.immediates[1]];
            if (shapes[from.immediates[0]].kind == Format::ShapeKind::Environment) {
                if (value.opcode == Opcode::GetField && readNeedsCheck(from.type, field.declaredType))
                    value.setAssertedType(assertedTypeAt(from.type));
                return;
            }
            if (uint32_t object = from.children[0]; object < function.values.size() && function.values[object].opcode == Opcode::NewObjectWithShape)
                return;
            value.opcode = value.opcode == Opcode::GetField ? Opcode::GetById : Opcode::PutById;
            value.immediates = { field.name, Format::none };
            if (value.opcode == Opcode::GetById)
                value.setAssertedType(assertedTypeAt(from.type));
            return;
        }
        case Opcode::GetGlobal:
        case Opcode::GetById:
        case Opcode::GetByVal:
        case Opcode::IteratorValue:
        case Opcode::Call:
        case Opcode::Construct:
        case Opcode::CallMethod:
        case Opcode::CatchValue:
        case Opcode::Operation:
        case Opcode::Await:
        case Opcode::GetExternalModule:
        case Opcode::Yield:
            value.setAssertedType(assertedTypeAt(from.type));
            return;
        case Opcode::Cast:
            if (value.reportsMismatchAtCaller() && (!m_program.logsTypeErrors || value.castReason() != CastReason::Claim))
                m_failed = true;
            if (value.castReason() != CastReason::Upcast)
                value.setAssertedType(assertedTypeAt(from.type));
            return;
        default:
            return;
        }
    }

    Function read(const Format::Function& source)
    {
        Function function;
        function.name = StringID(source.name);
        function.module = ModuleID(source.module);
        function.position = source.position;
        function.sourceStart = source.sourceStart;
        function.sourceEnd = source.sourceEnd;
        if (source.sourceEnd && (source.module >= m_program.modules.size() || source.sourceStart > source.sourceEnd || source.sourceEnd > m_program.modules[source.module].printed.size()))
            m_failed = true;
        if (source.kind > FunctionKind::Module)
            m_failed = true;
        function.kind = source.kind;
        function.flags = source.flags;
        function.declaredAs = (source.flags & Format::FunctionIsAsync ? functionIsAsync : 0) | (source.flags & Format::FunctionIsGenerator ? functionIsGenerator : 0)
            | (source.kind == FunctionKind::Plain && source.flags & Format::FunctionHasNewTarget ? functionReadsNewTarget : 0)
            | (source.flags & Format::FunctionIsInBuiltinModule ? functionIsInBuiltinModule : 0);
        function.length = source.length;
        if (source.captures.count > UINT8_MAX)
            m_failed = true;
        for (uint32_t word : slice(m_words, source.captures)) {
            if (word != Format::none && word & Format::ParameterIsCalledOnly) {
                function.calledOnlyCaptures.set(function.captureTypes.size());
                word &= ~Format::ParameterIsCalledOnly;
            }
            function.captureTypes.append(trustedTypeAt(word));
            if (!function.captureTypes.last())
                m_failed = true;
        }
        function.thisType = trustedTypeAt(source.thisType);
        function.assertedThisType = assertedTypeAt(source.thisType);
        function.resultType = trustedTypeAt(source.resultType);
        for (uint32_t word : slice(m_words, source.parameterTypes)) {
            if (word == Format::untypedParameterIsCalledOnly) {
                function.calledOnlyParameters.set(function.parameterTypes.size());
                word = Format::none;
            } else if (word != Format::none && word & Format::ParameterIsCalledOnly) {
                function.calledOnlyParameters.set(function.parameterTypes.size());
                word &= ~Format::ParameterIsCalledOnly;
            }
            if (word != Format::none && word & Format::ParameterIsAnnotated) {
                function.annotatedParameters.set(function.parameterTypes.size());
                word &= ~Format::ParameterIsAnnotated;
            }
            function.parameterTypes.append(trustedTypeAt(word));
            if (Options::auditAOTTypedAssertions()) [[unlikely]]
                function.assertedParameterTypes.append(assertedTypeAt(word));
        }
        function.values.reserveInitialCapacity(slice(m_values, source.values).size());
        for (auto& from : slice(m_values, source.values)) {
            Value value;
            if (static_cast<uint32_t>(from.opcode) >= numberOfFormatOpcodes) {
                m_failed = true;
                break;
            }
            value.opcode = static_cast<Opcode>(from.opcode);
            value.position = from.position;
            for (unsigned i = 0; i < 3; ++i)
                value.children[i] = ValueID(from.children[i]);
            value.immediates = { from.immediates[0], from.immediates[1] };
            if (value.opcode == Opcode::ConstantSymbol && (value.immediates[0] >= m_program.strings.size() || !isConstantSymbol(m_program.strings[value.immediates[0]]))) {
                m_failed = true;
                break;
            }
            if ((value.opcode == Opcode::GetByVal || value.opcode == Opcode::IteratorValue) && value.immediates[0] != Format::none) {
                m_failed = true;
                break;
            }
            if ((value.opcode == Opcode::GetById || value.opcode == Opcode::PutById) && value.immediates[1] == Format::ReceiverHasSingleStructure) {
                value.receiverHasSingleStructure = true;
                value.immediates[1] = Format::none;
            }
            if (value.opcode == Opcode::Construct) {
                if (value.immediates[0] != Format::none || (value.immediates[1] != Format::none && value.immediates[1] != 1)) {
                    m_failed = true;
                    break;
                }
                value.hasStringKeysByType = value.immediates[1] == 1;
                value.immediates[1] = Format::none;
            }
            if (value.opcode == Opcode::GetModuleVariable && value.immediates[1] != Format::none && value.immediates[1] != Format::VariableIsKnownInitialized) {
                m_failed = true;
                break;
            }
            if (value.opcode == Opcode::Call && value.immediates[0] != Format::none) {
                m_failed = true;
                break;
            }
            if (value.opcode == Opcode::CallMethod && value.immediates[1] == Format::CallIsFusedWithMap) {
                value.isFusedWithMap = true;
                value.immediates[1] = Format::none;
            }
            if ((value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod) && value.immediates[1] != Format::none) {
                if (uint32_t written = value.immediates[1]; written != Format::CallIsInTailPosition) {
                    uint32_t type = (written - 2) >> 1;
                    if (written < 2 || type >= m_callSignatures.size() || m_callSignatures[type] == Format::none) {
                        m_failed = true;
                        break;
                    }
                    value.setCallSignature(m_callSignatures[type]);
                    value.immediates[1] = (written - 2) & 1 ? Format::CallIsInTailPosition : Format::none;
                }
            }
            if (value.opcode == Opcode::Call || value.opcode == Opcode::CallMethod || value.opcode == Opcode::CallDirect || value.opcode == Opcode::Operation) {
                if (value.immediates[1] != Format::none && value.immediates[1] != Format::CallIsInTailPosition) {
                    m_failed = true;
                    break;
                }
                value.isInTailPosition = value.immediates[1] == Format::CallIsInTailPosition;
                value.immediates[1] = Format::none;
            }
            if (value.opcode == Opcode::Operation) {
                uint32_t operation = value.immediates[0];
                if (operation >= numberOfBuiltins || Builtins::singleton().operationNamed(Builtins::singleton().at(BuiltinID(operation)).nameName) != BuiltinID(operation)) {
                    m_failed = true;
                    break;
                }
                if (!(WTF::atomicLoad(&m_program.m_usedOperations[operation / 32], std::memory_order_relaxed) >> operation % 32 & 1))
                    WTF::atomicExchangeOr(&m_program.m_usedOperations[operation / 32], 1u << operation % 32, std::memory_order_relaxed);
            }
            if (value.opcode == Opcode::StrictEq && value.immediates[0] != Format::none) {
                m_failed = true;
                break;
            }
            if (value.opcode == Opcode::GetById) {
                if (value.immediates[1] != Format::none && value.immediates[1] != 1) {
                    m_failed = true;
                    break;
                }
                value.hasStringKeysByType = value.immediates[1] == 1;
                value.immediates[1] = Format::none;
            }
            readValues(from.arguments, value.arguments);
            value.type = trustedTypeAt(from.type);
            if (!value.type) {
                m_failed = true;
                break;
            }
            if (from.prediction != Format::none && from.prediction & Format::PredictionIsClosure) {
                value.setPredictedClosure(FunctionID(from.prediction & ~Format::PredictionIsClosure));
                if (value.predictedClosure().index() >= m_header->functions.count) {
                    m_failed = true;
                    break;
                }
            } else if (from.prediction != Format::none && from.prediction & Format::PredictionIsByKey) {
                value.setPredictedByKey(StringID(from.prediction & ~Format::PredictionIsByKey));
                if (value.opcode != Opcode::GetByVal || value.predictedByKey().index() >= m_program.strings.size()) {
                    m_failed = true;
                    break;
                }
            } else if (typeAt(from.prediction))
                value.setPrediction(m_predictedTypes[from.prediction]);
            if (value.opcode == Opcode::Cast) {
                if (uint32_t freshness = from.immediates[1] >> Format::freshnessShift & 3; (from.immediates[1] & 0xff) > static_cast<uint32_t>(CastReason::Coerce)
                    || freshness > static_cast<uint32_t>(Format::Freshness::WholeGraph) || (freshness && value.castReason() != CastReason::Coerce && !(value.castReason() == CastReason::As && freshness == static_cast<uint32_t>(Format::Freshness::TopLevel))))
                    m_failed = true;
                if (value.keepsUnconvertibleObjects()) {
                    if (value.castReason() != CastReason::As)
                        m_failed = true;
                    else if (!m_program.at(value.type).acceptsOnlyOtherLayouts(m_program))
                        value.immediates[1] &= ~Format::CastKeepsUnconvertibleObjects;
                }
                TypeID coercedTo = value.castReason() == CastReason::Coerce ? typeAt(from.immediates[0]) : TypeID();
                if (value.castReason() == CastReason::Coerce && !coercedTo)
                    m_failed = true;
                value.immediates[0] = coercedTo ? coercedTo.index() : Format::none;
                if (value.castReason() == CastReason::NonNull && from.immediates[0] != Format::none) {
                    if (from.immediates[0] >= m_program.strings.size())
                        m_failed = true;
                    value.immediates[0] = from.immediates[0];
                }
            }
            if (value.opcode == Opcode::NewObjectWithShape && (!value.child(0) != (from.immediates[1] == Format::none) || (value.child(0) && from.immediates[1] >= m_program.strings.size())))
                m_failed = true;
            if (Options::auditAOTTypedAssertions()) [[unlikely]]
                prepareForAudit(value, from, function);
            value.rep = repOf(m_program.at(value.type));
            function.values.append(WTF::move(value));
        }
        function.blocks.reserveInitialCapacity(slice(m_blocks, source.blocks).size());
        for (auto& from : slice(m_blocks, source.blocks)) {
            Block block;
            readValues(from.parameters, block.parameters);
            readValues(from.values, block.values);
            block.terminator.kind = from.terminator;
            block.terminator.value = ValueID(from.value);
            for (auto& target : slice(m_targets, from.targets)) {
                Target result { BlockID(target.block), { } };
                readValues(target.arguments, result.arguments);
                block.terminator.targets.append(WTF::move(result));
            }
            auto cases = slice(m_words, from.cases);
            for (size_t i = 0; i + 1 < cases.size(); i += 2)
                block.terminator.cases.append(static_cast<int64_t>(static_cast<uint64_t>(cases[i + 1]) << 32 | cases[i]));
            function.blocks.append(WTF::move(block));
        }
        return function;
    }

    Program& m_program;
    std::span<const uint8_t> m_buffer;
    const Format::Header* m_header { nullptr };
    std::span<const uint32_t> m_words;
    std::span<const Format::ObjectPart> m_objectParts;
    Vector<TypeID> m_types;
    Vector<TypeID> m_predictedTypes;
    Vector<uint32_t> m_callSignatures;
    uint32_t m_callSignatureRead { Format::none };
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_callSignaturesByHash;
    std::span<const Format::Field> m_fields;
    std::span<const Format::Method> m_methods;
    std::span<const Format::Block> m_blocks;
    std::span<const Format::Value> m_values;
    std::span<const Format::Target> m_targets;
    Vector<std::pair<LayoutID, uint32_t>> m_symbolKeys;
    std::atomic<bool> m_failed { false };
};

std::unique_ptr<Program> Program::tryRead(std::span<const uint8_t> bytes, ASCIILiteral& failureReason)
{
    auto program = makeUnique<Program>();
    program->buffer = bytes;
    failureReason = Reader(*program).run();
    if (!failureReason.isNull())
        return nullptr;
    return program;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
