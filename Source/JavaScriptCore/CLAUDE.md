# CLAUDE.md - JavaScriptCore

This file provides guidance to Claude Code (claude.ai/code) when working with JavaScriptCore in the Bun WebKit fork.

## Quick Start

### Building just JSC
```bash
# From repository root - fastest way to test changes
bun build.ts debug  # or: release, lto
./WebKitBuild/Debug/bin/jsc your-test.js
```

### Finding code
```bash
# Where is this JavaScript function implemented?
grep -r "functionName" --include="*.cpp" --include="*.h" runtime/

# Where is this bytecode op handled?
grep -r "op_get_by_id" --include="*.cpp" --include="*.asm" llint/ jit/ dfg/ ftl/

# Find Bun-specific code
grep -r "USE(BUN" --include="*.cpp" --include="*.h"
```

## Key Things to Know

### The compilation tiers are sequential
1. Everything starts in **LLInt** (interpreter in `llint/LowLevelInterpreter.asm`)
2. Hot functions → **Baseline JIT** (`jit/JIT.cpp`)
3. Very hot → **DFG** (`dfg/DFGSpeculativeJIT.cpp`)
4. Extremely hot → **FTL** (`ftl/FTLLowerDFGToB3.cpp`)

Each tier has its own way of handling the same bytecode operation. If you're changing behavior, you likely need to update all four.

### Most JavaScript built-ins are in JavaScript
Check `builtins/*.js` first before diving into C++. These get compiled to bytecode at build time.

### The object model is based on Structures
- `Structure` = hidden class/shape
- When you see `structure()->`, that's accessing the object's shape
- Property access goes through inline caches that check structure

### Values are NaN-boxed
- `JSValue` uses NaN-boxing on 64-bit (see `runtime/JSCJSValue.h`)
- Small integers are stored inline, objects are pointers
- Check `if (value.isCell())` before treating as pointer

## Common Tasks

### Adding/modifying a JavaScript global function
1. Look in `runtime/JSGlobalObjectFunctions.cpp`
2. Find similar function for reference
3. Use `JSC_DEFINE_HOST_FUNCTION` macro
4. Register it in `runtime/JSGlobalObject.cpp`

### Debugging a crash
1. Build with `bun build.ts debug`
2. Run with lldb/gdb: `lldb ./WebKitBuild/Debug/bin/jsc`
3. Useful breakpoints:
   - `jsDynamicCast` - type casting issues
   - `JSC::throwException` - where exceptions originate
   - `WTFCrash` - assertion failures

### Understanding a performance issue
```bash
# See what tier code is in
JSC_dumpDisassembly=true ./jsc script.js

# Watch tier transitions
JSC_verboseOSR=true ./jsc script.js

# Disable tiers to isolate issues
JSC_useDFGJIT=false ./jsc script.js
JSC_useFTLJIT=false ./jsc script.js
```

### Finding where a JS operation is implemented
1. Find the bytecode op in `bytecode/BytecodeList.rb`
2. Search for `llint_op_<name>` in `llint/LowLevelInterpreter.asm`
3. Search for `emit_op_<name>` in `jit/JIT.cpp`
4. Search for case `op_<name>` in `dfg/DFGByteCodeParser.cpp`

## Bun-Specific Code

### Key Bun additions
- **V8 heap snapshots**: `heap/BunV8HeapSnapshotBuilder.cpp`
- **Internal fields**: `runtime/InternalFieldTuple.h`
- **Inspector extensions**: `inspector/protocol/BunFrontendDevServer.json`

These are controlled by:
- `USE(BUN_JSC_ADDITIONS)` - Bun-specific features
- `USE(BUN_EVENT_LOOP)` - Bun's event loop integration

### Finding Bun modifications
```bash
# Find all Bun-specific code
grep -r "BUN" --include="*.h" --include="*.cpp" .

# Find Bun-specific cmake flags
grep -r "BUN" CMakeLists.txt
```

## Important Files

### If you're working on...

**JavaScript built-in functions**
- `builtins/*.js` - JavaScript implementations
- `runtime/JSGlobalObjectFunctions.cpp` - C++ global functions
- `runtime/CommonIdentifiers.h` - Well-known property names

**Performance/JIT issues**
- `dfg/DFGSpeculativeJIT.cpp` - DFG code generation
- `ftl/FTLLowerDFGToB3.cpp` - FTL code generation
- `jit/JITOpcodes.cpp` - Baseline JIT opcodes

**Memory/GC issues**
- `heap/Heap.cpp` - Main GC implementation
- `heap/SlotVisitor.cpp` - GC marking
- `runtime/JSCell.h` - Base class for all heap objects

**Parser/syntax issues**
- `parser/Parser.cpp` - JavaScript parser
- `parser/Lexer.cpp` - Tokenization
- `bytecompiler/BytecodeGenerator.cpp` - AST to bytecode

**Module loading**
- `runtime/JSModuleLoader.cpp` - Module loading logic
- `builtins/ModuleLoader.js` - JavaScript side of modules

## Debugging Tips

### Use dataLog liberally
```cpp
#include "DataLog.h"
dataLog("Value: ", value, " at ", RawPointer(ptr), "\n");
```

### The most useful JSC options
```bash
# Dump all options
./jsc --options

# Most useful for debugging
--validateGraph=true     # Validate DFG IR
--dumpDisassembly=true   # See generated code
--dumpBytecodeAtDFGTime=true  # See bytecode when compiling
--gcAtEnd=true          # Force GC to find memory issues
--useConcurrentJIT=false  # Simplify debugging
```

### Watch values in lldb
```lldb
# Print JSValue
p value.dump()

# Print Structure  
p structure->dump()

# Print CodeBlock
p codeBlock->dumpBytecode()
```

## Build System

### Regenerate after changing BytecodeList.rb
The build system should handle this, but if not:
```bash
cd WebKitBuild/Debug
ninja JSCBuiltins
```

### Speed up builds
- Use `ninja` not `make`
- Only build jsc target: `ninja jsc`
- Use ccache if available
- Consider `--no-webkit2` for faster builds

## Testing

### Quick testing
```bash
# Run single test
./jsc test.js

# With validation
./jsc --validateOptions --validateGraph test.js

# Force tier
for i in {1..10000}; do ./jsc test.js; done  # Force JIT compilation
```

### Test directories
- `../../JSTests/stress/` - Regression tests (good examples!)
- `../../JSTests/microbenchmarks/` - Performance tests
- `API/tests/` - API tests

## Remember

1. **Check all tiers** - A change in one tier usually needs changes in others
2. **Use existing code as reference** - Find similar functionality and copy the pattern
3. **Build debug for development** - Release builds hide many helpful assertions
4. **Test with JSC shell first** - Faster iteration than full Bun builds
5. **dataLog is your friend** - Printf debugging works well in JSC