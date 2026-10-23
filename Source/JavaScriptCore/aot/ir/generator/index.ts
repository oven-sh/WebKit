import { parseArgs } from "node:util";
import { findUnusedNames } from "./check-uses.ts";
import { loadSchemaModel, type LoadedSchema } from "./load.ts";
import { engineOutputs, findFileListProblems, rustOutputs, writeOutputs } from "./outputs.ts";
import { SchemaError, SchemaErrors } from "./source.ts";

const usage = `usage: <bun or node> aot/ir/generator/index.ts --schema <AOTSchema.def> [options]
  --engine-out <dir>     write the C++ outputs, and fail when AOTSchemaFiles.cmake beside the schema does not list them
  --rust-out <dir>       write what Bun compiles: the Rust files and AOTSchemaHash.h
  --check-uses <path>    fail when a name of the schema that needs a use has none in the file <path>, or in the .cpp, .h and .rs
                         files under the folder <path>, folders named "generated" left out (may be repeated)
  --hash                 print the schema hash
  --list                 print the names of all outputs`;

interface Options {
  readonly schema: string;
  readonly engineOut: string | undefined;
  readonly rustOut: string | undefined;
  readonly checkUses: readonly string[];
  readonly hash: boolean;
  readonly list: boolean;
}

function readOptions(argv: readonly string[]): Options {
  const { values } = parseArgs({
    args: [...argv],
    strict: true,
    options: {
      "schema": { type: "string" },
      "engine-out": { type: "string" },
      "rust-out": { type: "string" },
      "check-uses": { type: "string", multiple: true, default: [] },
      "hash": { type: "boolean", default: false },
      "list": { type: "boolean", default: false },
    },
  });
  if (values.schema === undefined) throw new Error(usage);
  return {
    schema: values.schema,
    engineOut: values["engine-out"],
    rustOut: values["rust-out"],
    checkUses: values["check-uses"],
    hash: values.hash,
    list: values.list,
  };
}

function updateOutputs(options: Options, loaded: LoadedSchema): string[] {
  if (options.rustOut !== undefined) writeOutputs(options.rustOut, rustOutputs(loaded));
  if (options.engineOut === undefined) return [];
  const files = engineOutputs(loaded);
  writeOutputs(options.engineOut, files);
  return findFileListProblems(loaded, files);
}

function run(options: Options, loaded: LoadedSchema): string[] {
  if (options.hash) console.log(`0x${loaded.hash.toString(16).padStart(16, "0")}`);
  if (options.list) {
    for (const file of engineOutputs(loaded)) console.log(`engine/${file.name}`);
    for (const file of rustOutputs(loaded)) console.log(`rust/${file.name}`);
  }
  const problems = updateOutputs(options, loaded);
  if (options.checkUses.length > 0) {
    for (const unused of findUnusedNames(loaded.model, options.checkUses)) {
      const { path: file, line, column } = unused.location;
      problems.push(`${file}:${line}:${column}: ${unused.spelling} has no use`);
    }
  }
  return problems;
}

function tryReadOptions(argv: readonly string[]): Options | null {
  try {
    return readOptions(argv);
  } catch (error) {
    if (!(error instanceof Error)) throw error;
    console.error(error.message);
    return null;
  }
}

function main(argv: readonly string[]): number {
  const options = tryReadOptions(argv);
  if (options === null) return 1;
  try {
    const problems = run(options, loadSchemaModel(options.schema));
    for (const problem of problems) console.error(problem);
    return problems.length > 0 ? 1 : 0;
  } catch (error) {
    if (!(error instanceof SchemaError) && !(error instanceof SchemaErrors)) throw error;
    console.error(error.message);
    return 1;
  }
}

// Under an older node a generator would stop at the first type, or run and write nothing. It says what it needs instead.
function requireRuntime(): void {
  if ("Bun" in globalThis) return;
  const [major, minor] = process.versions.node.split(".").map(Number);
  if (major > 24 || (major === 24 && minor >= 2) || (major === 22 && minor >= 18)) return;
  console.error(
    `error: the generators of aot/ need node 24.2 or later, or 22.18 or later of 22: this is ${process.versions.node}`,
  );
  process.exit(1);
}

requireRuntime();
process.exit(main(process.argv.slice(2)));
