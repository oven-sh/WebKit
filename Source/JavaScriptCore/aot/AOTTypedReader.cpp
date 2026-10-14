/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedIR.h"
#include "AOTTypedThreads.h"
#include "Options.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class Reader {
public:
    Reader(Program& program)
        : m_program(program)
        , m_buffer(program.buffer.span())
    {
    }

    ASCIILiteral run()
    {
        if (m_buffer.size() < sizeof(Format::Header) || reinterpret_cast<uintptr_t>(m_buffer.data()) % alignof(uint32_t))
            return "the program is shorter than its header"_s;
        m_header = reinterpret_cast<const Format::Header*>(m_buffer.data());
        if (m_header->magic != Format::magic)
            return "this is not a program of the front end"_s;
        if (m_header->version != Format::version || m_header->opcodeNamesHash != Format::opcodeNamesHash || m_header->builtinsHash != builtinsHash)
            return "the front end and the engine are of different versions"_s;
        if (m_header->sizeInBytes != m_buffer.size())
            return "the program is not as long as it says"_s;
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
            return "a section lies outside the program"_s;

        for (auto& string : section<Format::String>(m_header->strings)) {
            if (static_cast<uint64_t>(string.offset) + string.length > bytes.size())
                return "a string lies outside the program"_s;
            m_program.strings.append(bytes.subspan(string.offset, string.length));
            m_program.noteString(StringID(m_program.strings.size() - 1));
        }
        for (auto& declared : section<Format::DeclaredType>(m_header->declaredTypes))
            m_types.append(m_program.types.intern(read(declared)));
        auto& types = m_program.types;
        types.never = types.intern(Type::never());
        types.undefined = types.intern(Type::ofPrimitives(Primitive::Undefined));
        types.boolean = types.intern(Type::ofPrimitives(Primitive::Boolean));
        types.int32 = types.intern(Type::integer(Range::int32()));
        types.number = types.intern(Type::ofPrimitives(Primitive::Number));
        types.string = types.intern(Type::ofPrimitives(Primitive::String));
        types.function = types.intern(Type::function());
        types.object = types.intern(Type::object());
        types.anything = types.intern(Type::anything());
        types.integer = types.intern(Type::integer(Range::full()));
        for (auto& shape : section<Format::Shape>(m_header->shapes)) {
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
                return "the path a shape is declared at is no string of the program"_s;
            layout.declaredPath = StringID(shape.declaredPath);
            layout.declaredLine = shape.declaredLine;
            layout.mayBeFrozen = shape.flags & Format::ShapeMayBeFrozen;
            layout.isLaidOut = shape.flags & Format::ShapeIsLaidOut;
            layout.isEvaluatedOnce = shape.flags & Format::ShapeIsEvaluatedOnce;
            layout.hasForeignRoot = shape.flags & Format::ShapeHasForeignRoot;
            layout.hasFixedMembers = shape.flags & Format::ShapeHasFixedMembers;
            layout.mayHaveOtherPrototype = shape.flags & Format::ShapeMayHaveOtherPrototype;
            layout.hasStaticMemberOfUnknownName = shape.flags & Format::ShapeHasStaticMemberOfUnknownName;
            layout.admitsForeignObjects = shape.flags & Format::ShapeAdmitsForeignObjects;
            for (uint32_t member : slice(m_words, shape.members))
                layout.members.append(LayoutID(member));
            for (uint32_t member : slice(m_words, shape.membersByFit))
                layout.membersByFit.append(LayoutID(member));
            for (uint32_t specialization : slice(m_words, shape.specializations))
                layout.specializations.append(LayoutID(specialization));
            for (auto& from : slice(m_fields, shape.fields)) {
                Field field;
                field.name = StringID(from.name);
                field.type = typeReliedOn(from.declaredType);
                field.flags = (from.flags & (fieldIsOptional | fieldIsImmutable)) | (from.flags & Format::FieldIsMethod ? fieldIsMethod : 0) | (from.flags & Format::FieldIsAnnotated ? fieldIsAnnotated : 0);
                if (layout.kind == LayoutKind::Environment && from.flags & Format::FieldIsNeverSeenUninitialized)
                    field.flags |= fieldIsNeverSeenUninitialized | (from.flags & Format::FieldIsStoredOnce ? fieldIsStoredOnce : 0);
                if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldIsNeverStored)
                    field.flags |= fieldIsNeverStored;
                if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldIsExpectedInt32)
                    field.flags |= fieldIsExpectedInt32;
                if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldIsSetAtCreation)
                    field.flags |= fieldIsSetAtCreation;
                if (layout.kind != LayoutKind::Environment && from.flags & Format::FieldMayBeSetAtCreation)
                    field.flags |= fieldMayBeSetAtCreation;
                if (from.flags & Format::FieldDoesNotAdmitUndefined)
                    field.flags |= fieldDoesNotAdmitUndefined;
                if (from.flags & Format::FieldIsAbsentAtBirth) {
                    if (layout.kind != LayoutKind::Instance || !(from.flags & Format::FieldIsOptional))
                        return "a field that is absent at birth is not an optional field of a class"_s;
                    field.flags |= fieldIsAbsentAtBirth;
                }
                if (from.flags & Format::FieldIsNeverSeenAbsent) {
                    if (!isObjectLayout(layout.kind) || from.flags & (Format::FieldIsOptional | Format::FieldIsNeverStored))
                        return "a field that is never seen absent is not a required field of an object"_s;
                    // All stores go by name through the run time then, which counts a field among the keys when it finds it absent.
                    if (!Options::auditAOTTypedAssertions())
                        field.flags |= fieldIsNeverSeenAbsent | fieldIsNeverSeenAbsentAsWritten;
                }
                // The stores that make an object what it is born as are carried out the general way then, like any other.
                if (layout.kind != LayoutKind::Environment && Options::auditAOTTypedAssertions()) [[unlikely]]
                    field.flags &= ~fieldIsImmutable;
                auto literals = slice(m_words, from.literals);
                if (literals.size() % 3)
                    return "the literals of a field are not whole"_s;
                for (size_t i = 0; i < literals.size(); i += 3) {
                    bool isNumber = literals[i] == static_cast<uint32_t>(Format::LiteralKind::Number);
                    if (!isNumber && (literals[i] != static_cast<uint32_t>(Format::LiteralKind::String) || literals[i + 1] >= m_program.strings.size()))
                        return "a literal of a field is neither a string of the program nor a number"_s;
                    field.literals.append(isNumber ? Literal { StringID(), std::bit_cast<double>(static_cast<uint64_t>(literals[i + 2]) << 32 | literals[i + 1]) } : Literal { StringID(literals[i + 1]), 0 });
                }
                if (from.typeText != Format::none && from.typeText >= m_program.strings.size())
                    return "the type text of a field is no string of the program"_s;
                field.typeText = StringID(from.typeText);
                if (from.onlyFunction != Format::none && from.onlyFunction >= numberOfFunctions)
                    return "the only function of a field is no function of the program"_s;
                field.onlyFunction = FunctionID(from.onlyFunction);
                field.onlyFunctionIsFact = field.onlyFunction && layout.kind == LayoutKind::Environment && from.onlyFunctionFlags & Format::OnlyFunctionIsFact;
                field.onlyFunctionOrNil = field.onlyFunction && layout.kind == LayoutKind::Environment && from.onlyFunctionFlags & Format::OnlyFunctionOrNil;
                layout.fields.append(WTF::move(field));
            }
            for (auto& method : slice(m_methods, shape.methods)) {
                bool isStatic = method.flags & Format::MethodIsStatic;
                if (method.name == Format::none) {
                    // The type of the variable vouches for that.
                    if (method.flags & Format::MethodKeyIsSymbol && !Options::auditAOTTypedAssertions())
                        m_keysThatAreSymbols.append({ LayoutID(m_program.layouts.size()), method.key });
                    else
                        (isStatic ? layout.hasStaticMemberOfUnknownName : layout.hasMethodOfUnknownName) = true;
                }
                (isStatic ? layout.staticMethods : layout.methods).append(Method { StringID(method.name), FunctionID(method.function), method.kind, method.key, !!(method.flags & Format::MethodMayBeReplaced) });
            }
            auto staticFields = slice(m_words, shape.staticFields);
            for (size_t i = 0; i + 1 < staticFields.size(); i += 2)
                layout.staticFields.append(StaticField { StringID(staticFields[i]), typeReliedOn(staticFields[i + 1]) });
            layout.indexFields();
            m_program.layouts.append(WTF::move(layout));
        }
        for (auto& layout : m_program.layouts)
            layout.mayGiveUp = layout.kind == LayoutKind::Record && layout.isAllocated && layout.admitsForeignObjects && !Options::auditAOTTypedAssertions();
        for (auto& view : m_program.layouts) {
            if (view.admitsForeignObjects)
                continue;
            for (LayoutID member : view.members) {
                if (member.index() < m_program.layouts.size())
                    m_program.layouts[member.index()].mayGiveUp = false;
            }
        }
        // Every store goes by name through the run time then, which holds the value to the field of the object's own layout. Typed code
        // stores through a view what the field of the VIEW takes. An ordinary image has room for that, because the fields share a slot that
        // is widened by what is stored through the view: see Placement::shareSlots() and collectTypes(). Here nothing is accessed through
        // a view. So the field of a member takes what the field by that name takes in each of its views.
        if (Options::auditAOTTypedAssertions()) [[unlikely]] {
            auto& layouts = m_program.layouts;
            for (auto& view : layouts) {
                for (LayoutID id : view.members) {
                    if (id.index() >= layouts.size() || &layouts[id.index()] == &view)
                        continue;
                    auto& member = layouts[id.index()];
                    for (auto& field : view.fields) {
                        if (auto found = member.fieldNamed(field.name))
                            member.fields[*found].type = unionOf(member.fields[*found].type, field.type);
                    }
                }
            }
        }
        for (auto& from : section<Format::Variable>(m_header->variables)) {
            Variable variable;
            variable.name = StringID(from.name);
            variable.module = ModuleID(from.module);
            variable.type = typeReliedOn(from.declaredType);
            variable.startsUndefined = from.initialization == Format::Initialization::Undefined;
            variable.isStoredOnce = from.initialization == Format::Initialization::NeverSeenAndStoredOnce;
            variable.isNeverSeenUninitialized = variable.isStoredOnce || from.initialization == Format::Initialization::NeverSeen;
            variable.startsZero = from.initialization == Format::Initialization::Zero;
            if (from.initialization > Format::Initialization::Zero)
                return "a variable starts out in a way that is not known"_s;
            if (from.onlyFunction != Format::none && from.onlyFunction >= numberOfFunctions)
                return "the only function of a variable is no function of the program"_s;
            variable.onlyFunction = FunctionID(from.onlyFunction);
            variable.onlyFunctionIsFact = variable.onlyFunction && from.onlyFunctionFlags & Format::OnlyFunctionIsFact;
            variable.onlyFunctionOrNil = variable.onlyFunction && from.onlyFunctionFlags & Format::OnlyFunctionOrNil;
            m_program.variables.append(WTF::move(variable));
        }
        for (auto& layout : m_program.layouts) {
            if (!layout.isEvaluatedOnce || !layout.hasFixedMembers || Options::auditAOTTypedAssertions() || layout.staticFields.size() + layout.staticMethods.size() > 100)
                continue;
            for (auto& field : layout.staticFields) {
                if (!field.type)
                    continue;
                if (layout.classObjectPlace == Format::none) {
                    layout.classObjectPlace = WTF::roundUpToMultipleOf<2>(m_program.numberOfStaticFieldPlaces);
                    m_program.numberOfStaticFieldPlaces = layout.classObjectPlace + 2;
                }
                field.place = m_program.numberOfStaticFieldPlaces++;
            }
        }
        for (auto& from : section<Format::Module>(m_header->modules)) {
            Module module;
            module.path = StringID(from.path);
            module.initializer = FunctionID(from.initializer);
            if (from.displayPath != Format::none && from.displayPath >= m_program.strings.size())
                return "the display path of a module is no string of the program"_s;
            module.displayPath = StringID(from.displayPath);
            module.lineTableSize = from.lineTable.count;
            if (!readLineTable(slice(bytes, from.lineTable), module))
                return "the line table of a module is malformed"_s;
            module.printed = slice(bytes, from.printed);
            m_program.modules.append(WTF::move(module));
        }
        auto functions = section<Format::Function>(m_header->functions);
        m_program.functions.reserveInitialCapacity(2 * functions.size());
        m_program.functions.grow(functions.size());
        Vector<Vector<std::pair<LayoutID, uint32_t>>> closuresMadeOnce(functions.size());
        forEachIndexInParallel(functions.size(), [&](unsigned i) {
            m_program.functions[i] = read(functions[i]);
            auto& function = m_program.functions[i];
            for (auto& value : function.values) {
                if (value.opcode != Opcode::NewClosureOnce)
                    continue;
                // In a block that is not reached the front end has nothing to name for the environment. What is reached is for the verifier.
                LayoutID environment = value.child(0).index() < function.values.size() ? m_program.typeOf(function, value.child(0)).onlyView() : LayoutID();
                if (environment && m_program.at(environment).kind == LayoutKind::Environment && value.field() < m_program.at(environment).fields.size())
                    closuresMadeOnce[i].append({ environment, value.field() });
            }
        });
        for (auto& ofFunction : closuresMadeOnce) {
            for (auto [environment, field] : ofFunction)
                m_program.at(environment).fields[field].flags |= fieldHoldsClosureMadeOnce;
        }
        for (uint32_t index : slice(m_words, m_header->entries)) {
            if (index >= m_program.functions.size())
                return "an entry is no function of the program"_s;
            if (m_program.isEntry(FunctionID(index)))
                return "an entry is listed twice"_s;
            m_program.entries.append(FunctionID(index));
        }
        if (m_program.entries.isEmpty())
            return "the program has no entry"_s;
        for (uint32_t string : slice(m_words, m_header->extendedGlobals)) {
            BuiltinName name = string < m_program.strings.size() ? m_program.builtinName(StringID(string)) : BuiltinName();
            if (Builtins::singleton().mayBeExtended(name))
                m_program.extendedGlobals.set(name.index());
        }
        for (uint32_t string : slice(m_words, m_header->prototypePropertyNames)) {
            if (string >= m_program.strings.size())
                return "a name defined on a prototype is no string of the program"_s;
            m_program.prototypePropertyNames.set(string);
        }
        auto regExpLiterals = slice(m_words, m_header->regExpLiterals);
        if (regExpLiterals.size() % 2)
            return "the list of regular expression literals is not one of pairs"_s;
        for (size_t i = 0; i < regExpLiterals.size(); i += 2) {
            if (regExpLiterals[i] >= m_program.strings.size() || regExpLiterals[i + 1] >= m_program.strings.size())
                return "a regular expression literal is no pair of strings of the program"_s;
            m_program.regExpLiterals.append({ StringID(regExpLiterals[i]), StringID(regExpLiterals[i + 1]) });
        }
        m_program.sites = Vector<uint32_t>(slice(m_words, m_header->sites));
        auto builtinModules = slice(m_words, m_header->builtinModules);
        if (builtinModules.size() % 2)
            return "the list of builtin modules is not one of pairs"_s;
        for (size_t i = 0; i < builtinModules.size(); i += 2) {
            uint32_t id = builtinModules[i];
            uint32_t index = builtinModules[i + 1];
            if (i && id <= builtinModules[i - 2])
                return "the builtin modules are not in the order of their numbers"_s;
            if (index >= m_program.functions.size())
                return "a builtin module is no function of the program"_s;
            auto& function = m_program.functions[index];
            if (function.kind != FunctionKind::Plain || function.declaredAs & (functionIsAsync | functionIsGenerator) || !function.parameterTypes.isEmpty() || !function.captureTypes.isEmpty())
                return "a builtin module is not a plain function without parameters and captures"_s;
            // The run time calls it as it calls any function value.
            function.flags |= Format::FunctionIsUsedAsValue;
            m_program.builtinModules.append({ id, FunctionID(index) });
        }
        auto builtinFunctions = slice(m_words, m_header->builtinFunctions);
        if (builtinFunctions.size() % 3)
            return "the list of builtin functions is not one of triples"_s;
        for (size_t i = 0; i < builtinFunctions.size(); i += 3) {
            uint32_t id = builtinFunctions[i];
            uint32_t index = builtinFunctions[i + 1];
            uint32_t visibility = builtinFunctions[i + 2];
            if (i && id <= builtinFunctions[i - 3])
                return "the builtin functions are not in the order of their numbers"_s;
            if (index >= m_program.functions.size() || !m_program.functions[index].captureTypes.isEmpty())
                return "a builtin function is no function of the program that captures nothing"_s;
            if (visibility > static_cast<uint32_t>(ImplementationVisibility::PrivateRecursive))
                return "a builtin function has no such visibility"_s;
            m_program.functions[index].flags |= Format::FunctionIsUsedAsValue;
            m_program.builtinFunctions.append({ id, FunctionID(index), visibility });
        }
        for (auto& function : m_program.functions) {
            if (function.has(Format::FunctionIsOfBuiltinModule) && function.module && !m_program.modules[function.module.index()].initializer)
                m_program.filesOfBuiltinFunctions.set(function.module.index());
        }
        auto functionsByKey = slice(m_words, m_header->functionsByKey);
        if (functionsByKey.size() % 3)
            return "the list of functions by key is not one of triples"_s;
        for (size_t i = 0; i < functionsByKey.size(); i += 3) {
            uint32_t property = functionsByKey[i];
            uint32_t key = functionsByKey[i + 1];
            uint32_t index = functionsByKey[i + 2];
            if (i && std::pair(property, key) <= std::pair(functionsByKey[i - 3], functionsByKey[i - 2]))
                return "the functions by key are not in the order of property and key"_s;
            if (property >= m_program.strings.size() || key >= m_program.strings.size() || index >= m_program.functions.size())
                return "a function by key names no string or no function of the program"_s;
            m_program.functionsByKey.append({ StringID(property), StringID(key), FunctionID(index) });
        }
        if (m_failed)
            return "a list lies outside its section"_s;
        return checkClasses();
    }

private:
    // The run time makes class objects, prototype objects and the objects of classes from what the shapes say, and reads and writes the
    // variables of a class by their numbers. So what they say has to hang together.
    ASCIILiteral checkClasses()
    {
        auto& layouts = m_program.layouts;
        auto& functions = m_program.functions;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (layout.reuseVariable && (layout.reuseVariable.index() >= m_program.variables.size() || layout.fields.size() < 2))
                return "an environment that may be reused has no variable to be kept in"_s;
            if (layout.kind != LayoutKind::Instance) {
                if (layout.constructor || layout.environment || !layout.staticMethods.isEmpty() || !layout.staticFields.isEmpty())
                    return "a shape that is no class has what only a class has"_s;
                // Of the type of an object literal: the accessors it declares. See Layout::methods.
                for (auto& method : layout.methods) {
                    if (layout.kind != LayoutKind::Record || method.kind == MethodKind::Method || !method.name || method.name.index() >= m_program.strings.size() || method.function.index() >= functions.size())
                        return "a shape that is no class has methods"_s;
                }
                continue;
            }
            // Only types name the class: its declaration is no part of the program, so there is no class object and there are no objects.
            if (!layout.constructor) {
                if (layout.isAllocated || layout.environment || !layout.methods.isEmpty() || !layout.staticMethods.isEmpty() || !layout.staticFields.isEmpty())
                    return "a class without a constructor has objects or members"_s;
                continue;
            }
            if (layout.constructor.index() >= functions.size() || functions[layout.constructor.index()].kind != FunctionKind::Constructor)
                return "what a class has for its constructor is none"_s;
            auto& constructor = functions[layout.constructor.index()];
            if (constructor.constructedClass)
                return "two classes have the same constructor"_s;
            constructor.constructedClass = LayoutID(i);
            if (!constructor.has(Format::FunctionHasThis) || (constructor.has(Format::FunctionHasNewTarget) && constructor.parameterTypes.isEmpty()))
                return "a constructor does not take what it is handed"_s;
            if (layout.hasForeignRoot && !constructor.has(Format::FunctionHasNewTarget))
                return "the constructor of a class with a foreign root does not take new.target"_s;
            if (!layout.isLaidOut && (!layout.fields.isEmpty() || layout.isAllocated || !layout.members.isEmpty() || !layout.specializations.isEmpty()))
                return "a class that is not laid out has fields or objects"_s;
            if (layout.isLaidOut && layout.hasForeignRoot)
                return "a class with a foreign root is laid out"_s;
            if (layout.base) {
                if (layout.base.index() >= i || layouts[layout.base.index()].kind != LayoutKind::Instance)
                    return "a class extends what is no class before it"_s;
                auto& base = layouts[layout.base.index()];
                if (!layout.isLaidOut || !base.isLaidOut || !base.constructor)
                    return "a class that is not laid out extends or is extended by a class of the program"_s;
                if (layout.isEvaluatedOnce && !base.isEvaluatedOnce)
                    return "a class that is evaluated once extends one that is not"_s;
            }
            uint32_t numberOfVariables = m_program.variables.size();
            if (layout.environment) {
                if (layout.isEvaluatedOnce || layout.environment.index() >= layouts.size() || layouts[layout.environment.index()].kind != LayoutKind::Environment)
                    return "a class closes over what is no environment"_s;
                numberOfVariables = layouts[layout.environment.index()].fields.size();
            } else if (!layout.isEvaluatedOnce)
                return "a class that is not evaluated once has no environment"_s;
            // The run time makes the closures of the members, with what the class object captures: the environment of the class.
            auto capturesSomethingElse = [&](const Function& function) {
                if (function.captureTypes.isEmpty())
                    return false;
                return function.captureTypes.size() != 1 || !layout.environment || m_program.at(function.captureTypes[0]) != Type::view(layout.environment);
            };
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods) {
                    if (method.function.index() >= functions.size() || (method.name ? method.name.index() >= m_program.strings.size() : method.key >= numberOfVariables))
                        return "a method has no function or no name"_s;
                    if (capturesSomethingElse(functions[method.function.index()]))
                        return "a method captures something else than the environment of its class"_s;
                }
            }
            if (capturesSomethingElse(constructor))
                return "a constructor captures something else than the environment of its class"_s;
            for (auto& field : layout.staticFields) {
                if (field.name.index() >= m_program.strings.size())
                    return "a static field has no name"_s;
            }
        }
        for (auto [id, key] : m_keysThatAreSymbols) {
            auto& layout = m_program.at(id);
            TypeID type = layout.environment ? m_program.at(layout.environment).fields[key].type : m_program.variables[key].type;
            if (!type || !m_program.at(type).hasOnlyPrimitives(Primitive::Symbol))
                return "the variable of a computed name that is said to be a symbol holds more than symbols"_s;
        }
        for (unsigned i = 0; i < m_program.types.size(); ++i) {
            for (LayoutID view : m_program.at(TypeID(i)).views) {
                if (view.index() >= layouts.size() || (m_program.at(view).kind == LayoutKind::Instance && !m_program.at(view).isLaidOut))
                    return "a type names a class that is not laid out"_s;
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
        for (uint32_t word : slice(m_words, span))
            result.append(ValueID(word));
    }

    // See Format::Module::lineTable.
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
                // Two bytes make one UTF-16 unit, three make one, four make two.
                excessBytes += length == 2 ? 1 : 2;
                module.columnAdjustments.append(Module::ColumnAdjustment { offset, line, excessBytes });
            }
        }
        return isValid && at == table.size();
    }

    // None: the front end says nothing.
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

    // Options::auditAOTTypedAssertions(): a value that fails a test is reported and goes on, and gets wherever a value of the type gets. So
    // the type says nothing any more, and anything takes its place: as the type of a value, a block parameter, a parameter, a result,
    // `this`, a field and a variable. They all agree as before, and every operation is carried out as in a function of JavaScript, the
    // general way. What stays:
    //   - a type that is kept as a bare number or boolean, with or without room for `undefined` and `null`. There is no room for
    //     anything else: what fails the test for such a type is converted. See State::assertionFailed(). Likewise a type that has
    //     `undefined` and `null` only, and `never`: what is of these goes where a bare number goes.
    //   - an environment, which a program never lays its hands on. Likewise what the passes make and type themselves: closures as
    //     general entries are handed them, the frames of coroutines.
    //   - what the operation itself vouches for: see prepareForAudit().
    bool isReliedOn(TypeID id)
    {
        auto& type = m_program.at(id);
        if (!holdsCells(repOf(type)) || type.isUndefinedOrNull() || type.isNever())
            return true;
        auto shapes = section<Format::Shape>(m_header->shapes);
        for (LayoutID view : type.views) {
            if (view.index() < shapes.size() && shapes[view.index()].kind == Format::ShapeKind::Environment)
                return true;
        }
        return false;
    }

    TypeID typeReliedOn(uint32_t index)
    {
        TypeID given = typeAt(index);
        if (!given || !Options::auditAOTTypedAssertions() || isReliedOn(given))
            return given;
        return m_program.types.anything;
    }

    // See Value::assertedType. None: there is nothing to test for.
    TypeID assertedTypeAt(uint32_t index)
    {
        TypeID given = typeAt(index);
        if (!given || !Options::auditAOTTypedAssertions() || isReliedOn(given) || given == m_program.types.anything)
            return { };
        return given;
    }

    // None stands for anything.
    TypeID unionOf(TypeID a, TypeID b)
    {
        if (!a || !b || a == b)
            return a == b ? a : TypeID();
        Type type = m_program.at(a);
        // A copy: interning below may move the table.
        Type other = m_program.at(b);
        uint16_t arrayClass = Builtins::singleton().arrayClass().index;
        if (other.engineClasses.contains(arrayClass))
            type.elements = type.engineClasses.contains(arrayClass) ? unionOf(type.elements, other.elements) : other.elements;
        type.primitives |= other.primitives;
        type.integers = type.integers.hull(other.integers);
        type.anyObject |= other.anyObject;
        type.anyFunction |= other.anyFunction || type.anyObject;
        type.views.appendVector(other.views);
        type.engineClasses.appendVector(other.engineClasses);
        if (type.anyObject) {
            type.views.clear();
            type.engineClasses.clear();
            type.elements = { };
        }
        std::ranges::sort(type.views);
        type.views.shrink(std::unique(type.views.begin(), type.views.end()) - type.views.begin());
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        return m_program.types.intern(type);
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
        };
        uint16_t bits = 0;
        for (auto [from, to] : primitives) {
            if (declared.primitives & from)
                bits |= to;
        }
        Type type = Type::ofPrimitives(bits);
        if (declared.flags & Format::DeclaredIntegers && !(bits & Primitive::Int)) {
            type.primitives |= Primitive::Int;
            type.integers.least = static_cast<int64_t>(static_cast<uint64_t>(declared.leastIntegerHigh) << 32 | declared.leastIntegerLow);
            type.integers.most = static_cast<int64_t>(static_cast<uint64_t>(declared.mostIntegerHigh) << 32 | declared.mostIntegerLow);
        }
        type.anyObject = declared.flags & Format::DeclaredAnyObject;
        type.anyFunction = declared.flags & Format::DeclaredAnyFunction;
        auto& builtins = Builtins::singleton();
        auto addPrimitiveOrItsObject = [&](uint16_t primitive) {
            type.primitives |= primitive;
            if (primitive & Primitive::Int)
                type.integers = Range::full();
            type.anyObject = true;
        };
        for (auto& part : slice(m_objectParts, declared.objects)) {
            switch (part.kind) {
            case Format::ObjectPartKind::Class:
                type.views.append(LayoutID(part.shapeOrName));
                break;
            case Format::ObjectPartKind::Array: {
                bool hasArrayPart = type.engineClasses.contains(builtins.arrayClass().index);
                type.engineClasses.append(builtins.arrayClass().index);
                auto arguments = slice(m_words, part.typeArguments);
                TypeID elements = arguments.size() == 1 ? typeAt(arguments[0]) : TypeID();
                // `A[] | B[]`: an element is an A or a B.
                type.elements = hasArrayPart ? unionOf(type.elements, elements) : elements;
                break;
            }
            case Format::ObjectPartKind::GlobalClass: {
                BuiltinName name = part.shapeOrName < m_program.strings.size() ? m_program.builtinName(StringID(part.shapeOrName)) : BuiltinName();
                switch (builtins.interfaceNamed(name)) {
                case GlobalInterface::Class:
                    // Only what can be told by its JSType is taken for the class. Anything else is an interface that any object may have.
                    if (builtins.classNamed(name)->type == CellType)
                        type.anyObject = true;
                    else
                        type.engineClasses.append(builtins.classNamed(name)->index);
                    break;
                case GlobalInterface::Function:
                    type.anyFunction = true;
                    break;
                case GlobalInterface::String:
                    addPrimitiveOrItsObject(Primitive::String);
                    break;
                case GlobalInterface::Number:
                    addPrimitiveOrItsObject(Primitive::Number);
                    break;
                case GlobalInterface::Boolean:
                    addPrimitiveOrItsObject(Primitive::Boolean);
                    break;
                case GlobalInterface::Symbol:
                    addPrimitiveOrItsObject(Primitive::Symbol);
                    break;
                case GlobalInterface::BigInt:
                    addPrimitiveOrItsObject(Primitive::BigInt);
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
            // Any object: a function is one.
            type.anyFunction = true;
            type.views.clear();
            type.engineClasses.clear();
            type.elements = { };
        }
        std::ranges::sort(type.views);
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        return type;
    }

    // Whether a read that is given the one type, of a place that is given the other, tests what it finds. Likewise a call and the result of the function.
    bool readIsTested(uint32_t typeOfRead, uint32_t typeOfPlace)
    {
        TypeID wanted = typeAt(typeOfRead);
        TypeID held = typeAt(typeOfPlace);
        return wanted && !(held ? m_program.at(held) : Type::anything()).isWithin(m_program.at(wanted), m_program);
    }

    // Options::auditAOTTypedAssertions(), see typeReliedOn(). The value is as it was read.
    void prepareForAudit(Value& value, const Format::Value& from, const Function& function)
    {
        // A call that goes by what is expected goes past the general entry, which has the tests for the parameters.
        value.predictedClosure = { };
        value.predictedByKey = { };
        switch (value.opcode) {
        // Whatever the checker has made of the program, these are what they are said to be.
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
        case Opcode::NewObjectWithShape:
            value.type = typeAt(from.type);
            return;
        case Opcode::GetModuleVariable: {
            auto variables = section<Format::Variable>(m_header->variables);
            if (from.immediates[0] < variables.size() && readIsTested(from.type, variables[from.immediates[0]].declaredType))
                value.assertedType = assertedTypeAt(from.type);
            return;
        }
        case Opcode::CallDirect: {
            auto functions = section<Format::Function>(m_header->functions);
            if (from.immediates[0] < functions.size() && readIsTested(from.type, functions[from.immediates[0]].resultType))
                value.assertedType = assertedTypeAt(from.type);
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
                if (value.opcode == Opcode::GetField && readIsTested(from.type, field.declaredType))
                    value.assertedType = assertedTypeAt(from.type);
                return;
            }
            // These go by where the field is, untested. That will do for an object this function has made. Of any other it is asked by the name.
            if (uint32_t object = from.children[0]; object < function.values.size() && function.values[object].opcode == Opcode::NewObjectWithShape)
                return;
            value.opcode = value.opcode == Opcode::GetField ? Opcode::GetById : Opcode::PutById;
            value.immediates = { field.name, Format::none };
            if (value.opcode == Opcode::GetById)
                value.assertedType = assertedTypeAt(from.type);
            return;
        }
        // Nothing but the operation vouches for what it yields: it holds that to the type of the value.
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
            value.assertedType = assertedTypeAt(from.type);
            return;
        case Opcode::Cast:
            if (value.castReason() != CastReason::Upcast)
                value.assertedType = assertedTypeAt(from.type);
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
        function.placeOfText = source.placeOfText;
        function.endOfText = source.endOfText;
        if (source.endOfText && (source.module >= m_program.modules.size() || source.placeOfText > source.endOfText || source.endOfText > m_program.modules[source.module].printed.size()))
            m_failed = true;
        if (source.kind > FunctionKind::Module)
            m_failed = true;
        function.kind = source.kind;
        function.flags = source.flags;
        function.declaredAs = (source.flags & Format::FunctionIsAsync ? functionIsAsync : 0) | (source.flags & Format::FunctionIsGenerator ? functionIsGenerator : 0)
            | (source.kind == FunctionKind::Plain && source.flags & Format::FunctionHasNewTarget ? functionReadsNewTarget : 0)
            | (source.flags & Format::FunctionIsOfBuiltinModule ? functionIsOfBuiltinModule : 0);
        function.length = source.length;
        // A closure has a byte for how many there are.
        if (source.captures.count > UINT8_MAX)
            m_failed = true;
        for (uint32_t word : slice(m_words, source.captures)) {
            if (word != Format::none && word & Format::ParameterIsOnlyCalled) {
                function.capturesOnlyCalled.set(function.captureTypes.size());
                word &= ~Format::ParameterIsOnlyCalled;
            }
            function.captureTypes.append(typeReliedOn(word));
            if (!function.captureTypes.last())
                m_failed = true;
        }
        function.thisType = typeReliedOn(source.thisType);
        function.assertedThisType = assertedTypeAt(source.thisType);
        function.resultType = typeReliedOn(source.resultType);
        for (uint32_t word : slice(m_words, source.parameterTypes)) {
            if (word == Format::untypedParameterIsOnlyCalled) {
                function.parametersOnlyCalled.set(function.parameterTypes.size());
                word = Format::none;
            } else if (word != Format::none && word & Format::ParameterIsOnlyCalled) {
                function.parametersOnlyCalled.set(function.parameterTypes.size());
                word &= ~Format::ParameterIsOnlyCalled;
            }
            if (word != Format::none && word & Format::ParameterIsAnnotated) {
                function.annotatedParameters.set(function.parameterTypes.size());
                word &= ~Format::ParameterIsAnnotated;
            }
            function.parameterTypes.append(typeReliedOn(word));
            if (Options::auditAOTTypedAssertions()) [[unlikely]]
                function.assertedParameterTypes.append(assertedTypeAt(word));
        }
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
            if ((value.opcode == Opcode::GetByVal || value.opcode == Opcode::IteratorValue) && value.immediates[0] != Format::none && value.immediates[0] != Value::elementIsKnownByTheStores) {
                m_failed = true;
                break;
            }
            if ((value.opcode == Opcode::GetById || value.opcode == Opcode::PutById) && value.immediates[1] == Format::ReceiverIsOfOneRow) {
                value.receiverIsExpectedOfOneRow = true;
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
            if (value.opcode == Opcode::GetModuleVariable && value.immediates[1] != Format::none && value.immediates[1] != Format::VariableIsInitialized) {
                m_failed = true;
                break;
            }
            if (value.opcode == Opcode::Call && value.immediates[0] != Format::none) {
                m_failed = true;
                break;
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
            value.type = typeReliedOn(from.type);
            if (!value.type) {
                m_failed = true;
                break;
            }
            if (from.prediction != Format::none && from.prediction & Format::PredictionIsClosure) {
                value.predictedClosure = FunctionID(from.prediction & ~Format::PredictionIsClosure);
                if (value.predictedClosure.index() >= m_header->functions.count) {
                    m_failed = true;
                    break;
                }
            } else if (from.prediction != Format::none && from.prediction & Format::PredictionIsByKey) {
                value.predictedByKey = StringID(from.prediction & ~Format::PredictionIsByKey);
                if (value.opcode != Opcode::GetByVal || value.predictedByKey.index() >= m_program.strings.size()) {
                    m_failed = true;
                    break;
                }
            } else
                value.prediction = typeAt(from.prediction);
            // The type a cast holds its operand to is the type of the cast, but for a coercion.
            if (value.opcode == Opcode::Cast) {
                if (uint32_t freshness = from.immediates[1] >> Format::freshnessShift; (from.immediates[1] & 0xff) > static_cast<uint32_t>(CastReason::Coerce)
                    || freshness > static_cast<uint32_t>(Format::Freshness::WholeGraph) || (freshness && value.castReason() != CastReason::Coerce && !(value.castReason() == CastReason::As && freshness == static_cast<uint32_t>(Format::Freshness::TopLevel))))
                    m_failed = true;
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
    // By the number the front end knows a type by.
    Vector<TypeID> m_types;
    std::span<const Format::Field> m_fields;
    std::span<const Format::Method> m_methods;
    std::span<const Format::Block> m_blocks;
    std::span<const Format::Value> m_values;
    std::span<const Format::Target> m_targets;
    // The class, and its variable.
    Vector<std::pair<LayoutID, uint32_t>> m_keysThatAreSymbols;
    std::atomic<bool> m_failed { false };
};

std::unique_ptr<Program> Program::tryRead(std::span<const uint8_t> bytes, ASCIILiteral& whyFailed)
{
    auto program = makeUnique<Program>();
    program->buffer.grow(bytes.size());
    constexpr size_t bytesAtATime = MB;
    forEachIndexInParallel((bytes.size() + bytesAtATime - 1) / bytesAtATime, [&](unsigned i) {
        size_t start = i * bytesAtATime;
        size_t size = std::min(bytesAtATime, bytes.size() - start);
        memcpySpan(program->buffer.mutableSpan().subspan(start, size), bytes.subspan(start, size));
    });
    whyFailed = Reader(*program).run();
    if (!whyFailed.isNull())
        return nullptr;
    return program;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
