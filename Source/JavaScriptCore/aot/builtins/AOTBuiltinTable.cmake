set(JavaScriptCore_AOT_BUILTIN_TABLE_DECLARATIONS
    aot/builtins/array-callbacks.builtins.ts
    aot/builtins/array.builtins.ts
    aot/builtins/buffer.builtins.ts
    aot/builtins/date.builtins.ts
    aot/builtins/engine-syntax.builtins.ts
    aot/builtins/error.builtins.ts
    aot/builtins/features.builtins.ts
    aot/builtins/function.builtins.ts
    aot/builtins/global.builtins.ts
    aot/builtins/holders.builtins.ts
    aot/builtins/host-roles.builtins.ts
    aot/builtins/instances-without-own-names.builtins.ts
    aot/builtins/intl.builtins.ts
    aot/builtins/iterator.builtins.ts
    aot/builtins/json.builtins.ts
    aot/builtins/layouts.builtins.ts
    aot/builtins/link-time-constants.builtins.ts
    aot/builtins/map-set.builtins.ts
    aot/builtins/math.builtins.ts
    aot/builtins/number.builtins.ts
    aot/builtins/object.builtins.ts
    aot/builtins/promise.builtins.ts
    aot/builtins/reflect.builtins.ts
    aot/builtins/regexp.builtins.ts
    aot/builtins/string.builtins.ts
    aot/builtins/symbol.builtins.ts
    aot/builtins/typedarray.builtins.ts
)

# The host of the programs that WebKit's own build links.
set(JavaScriptCore_AOT_BUILTIN_TABLE_STAND_IN_HOST_DECLARATIONS
    aot/tests/host/classes.builtins.ts
    aot/tests/host/compiler-helpers.builtins.ts
    aot/tests/host/modules.builtins.ts
    aot/tests/host/stand-in.builtins.ts
)
set(JavaScriptCore_AOT_BUILTIN_TABLE_STAND_IN_HOST_OWN_PROPERTIES aot/tests/host/own-properties.txt)

set(JavaScriptCore_AOT_BUILTIN_TABLE_HANDWRITTEN_FUNCTIONS
    aot/builtins/handwritten/ArrayIteratorPrototype.ts
    aot/builtins/handwritten/AsyncDisposableStackPrototype.ts
    aot/builtins/handwritten/DisposableStackPrototype.ts
    aot/builtins/handwritten/FunctionPrototype.ts
    aot/builtins/handwritten/GeneratorPrototype.ts
    aot/builtins/handwritten/IteratorOperations.ts
    aot/builtins/handwritten/JSIteratorHelperPrototype.ts
    aot/builtins/handwritten/MapPrototype.ts
    aot/builtins/handwritten/ObjectConstructor.ts
    aot/builtins/handwritten/SetPrototype.ts
    aot/builtins/handwritten/TypedArrayPrototype.ts
)

set(JavaScriptCore_AOT_BUILTIN_TABLE_MODEL_FUNCTIONS
    aot/builtins/models/Arrays.ts
    aot/builtins/models/Collections.ts
    aot/builtins/models/Objects.ts
)

# What aot/builtins/generator/engine.ts writes. A build makes these files: none is checked in.
set(JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_HEADERS
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinFeatures.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinIDs.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableEnums.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableWords.h"
)

set(JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_SOURCES
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableEngineBindings.cpp"
)

# What a release of WebKit ships of them, in Source/JavaScriptCore/aot/builtins/generated. Bun's generator compares its own output with these files.
set(JavaScriptCore_AOT_BUILTIN_TABLE_RELEASED_FILES
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinEngineDeclarations.json"
    ${JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_HEADERS}
    ${JavaScriptCore_AOT_BUILTIN_TABLE_GENERATED_SOURCES}
)

# The table itself. A static JavaScriptCore names its symbols and does not define them: whoever links the library compiles a table.
# Bun compiles its own, from its own declarations. These are for the programs that WebKit's build links: the engine's entries and the stand-in host's.
set(JavaScriptCore_AOT_BUILTIN_TABLE_DATA_HEADERS
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableArrays.h"
)

set(JavaScriptCore_AOT_BUILTIN_TABLE_DATA_SOURCES
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableAttachments.cpp"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableBuiltins.cpp"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableNames.cpp"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableOverloads.cpp"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableTypes.cpp"
)

# With what the table's generated sources include: whoever links the library compiles them.
set(JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_HEADERS
    aot/builtins/AOTBuiltinIDTypes.h
    aot/builtins/AOTBuiltinNameSlots.h
    aot/builtins/AOTBuiltinTable.h
    aot/builtins/AOTBuiltinTableBindings.h
    aot/builtins/AOTBuiltinTableGuards.h
    aot/builtins/AOTBuiltinTableRealm.h
)

set(JavaScriptCore_AOT_BUILTIN_TABLE_EXPORTED_GENERATED_HEADERS
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinFeatures.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinIDs.h"
    "${JavaScriptCore_AOT_GENERATED_DIR}/AOTBuiltinTableEnums.h"
)
