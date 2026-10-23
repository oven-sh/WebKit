import fs from "node:fs";
import path from "node:path";
import { emitCppAccessors } from "./emit-cpp-accessors.ts";
import { emitCppDispatch } from "./emit-cpp-dispatch.ts";
import { emitCppEffects } from "./emit-cpp-effects.ts";
import { emitCppEnums } from "./emit-cpp-enums.ts";
import { emitCppInlineBuiltins } from "./emit-cpp-inline-builtins.ts";
import { emitCppOperations } from "./emit-cpp-operations.ts";
import { emitCppParser } from "./emit-cpp-parser.ts";
import { emitCppPrinter } from "./emit-cpp-printer.ts";
import { emitCppReader } from "./emit-cpp-reader.ts";
import { emitCppRecords } from "./emit-cpp-records.ts";
import { emitCppResultRules } from "./emit-cpp-result-rules.ts";
import { emitCppRuntimeOperations } from "./emit-cpp-runtime-operations.ts";
import { emitCppVerifier } from "./emit-cpp-verifier.ts";
import { emitCppWalkers } from "./emit-cpp-walkers.ts";
import { emitCppWire } from "./emit-cpp-wire.ts";
import { emitRustAbi } from "./emit-rust-abi.ts";
import { emitRustBuilder } from "./emit-rust-builder.ts";
import { emitRustChecks } from "./emit-rust-checks.ts";
import { emitRustEntries } from "./emit-rust-entries.ts";
import { emitRustEntryText } from "./emit-rust-entry-text.ts";
import { emitRustEnums } from "./emit-rust-enums.ts";
import { emitRustRecords } from "./emit-rust-records.ts";
import { emitRustReferences } from "./emit-rust-references.ts";
import { emitRustText } from "./emit-rust-text.ts";
import { emitRustWriter } from "./emit-rust-writer.ts";
import { cppMarker, rustMarker, toText, type Lines } from "./lines.ts";
import type { LoadedSchema } from "./load.ts";
import type { Schema } from "./model.ts";
import { screamingCase } from "./names.ts";

export interface OutputFile {
  readonly name: string;
  readonly lines: Lines;
  /** A source that only testaot links. */
  readonly isTestSource?: true;
}

export interface EmitContext {
  readonly schema: Schema;
  readonly hash: bigint;
}

export type Emitter = (context: EmitContext) => OutputFile[];

export const fileListName = "AOTSchemaFiles.cmake";
export const hashHeaderName = "AOTSchemaHash.h";
export const maxInterfaceLines = 1000;
export const maxInterfaceBytes = 160000;

const engineEmitters: readonly Emitter[] = [
  emitCppEnums,
  emitCppWire,
  emitCppRecords,
  emitCppOperations,
  emitCppAccessors,
  emitCppEffects,
  emitCppReader,
  emitCppPrinter,
  emitCppParser,
  emitCppWalkers,
  emitCppVerifier,
  emitCppResultRules,
  emitCppRuntimeOperations,
  emitCppInlineBuiltins,
  emitCppDispatch,
];

const rustEmitters: readonly Emitter[] = [
  emitRustEnums,
  emitRustEntries,
  emitRustAbi,
  emitRustRecords,
  emitRustReferences,
  emitRustWriter,
  emitRustText,
  emitRustEntryText,
  emitRustBuilder,
  emitRustChecks,
];

function byName(left: OutputFile, right: OutputFile): number {
  return left.name < right.name ? -1 : left.name > right.name ? 1 : 0;
}

export function isInterfaceOutput(name: string): boolean {
  if (name.endsWith(".rs")) return !name.startsWith("record_") && !name.startsWith("entry_") && name !== "checks.rs";
  return (name.endsWith(".h") && !name.endsWith("Inlines.h")) || name.endsWith(".inc");
}

function checkSize(file: OutputFile): void {
  if (!isInterfaceOutput(file.name)) return;
  const bytes = Buffer.byteLength(toText(file.lines));
  if (file.lines.length > maxInterfaceLines || bytes > maxInterfaceBytes) {
    throw new Error(
      `${file.name} has ${file.lines.length} lines and ${bytes} bytes: people read it, so its emitter has to split it`,
    );
  }
}

export function runEmitters(emitters: readonly Emitter[], context: EmitContext): OutputFile[] {
  const files = emitters.flatMap(emitter => emitter(context)).sort(byName);
  for (let index = 1; index < files.length; index += 1) {
    if (files[index].name === files[index - 1].name) throw new Error(`two emitters write ${files[index].name}`);
  }
  files.forEach(checkSize);
  return files;
}

export function engineOutputs(loaded: LoadedSchema): OutputFile[] {
  return runEmitters(engineEmitters, { schema: loaded.model, hash: loaded.hash });
}

function listsOfCMakeFile(text: string): Map<string, string[]> {
  const lists = new Map<string, string[]>();
  for (const [, name, body] of text.matchAll(/^set\((\w+)\n((?:[ \t]+\S.*\n)*)\)$/gm))
    lists.set(
      name,
      body.split("\n").flatMap(line => (line.trim() === "" ? [] : [line.trim()])),
    );
  return lists;
}

/** Both builds of the engine read that file before anything is generated, so people keep it. */
export function findFileListProblems(loaded: LoadedSchema, files: readonly OutputFile[]): string[] {
  const listFile = path.join(loaded.directory, fileListName);
  const text = readIfPresent(listFile);
  if (text === null) return [`${listFile}: there is no such file`];
  const lists = listsOfCMakeFile(text);
  const written = (suffixes: readonly string[], isTestSource = false) =>
    files
      .filter(file => suffixes.some(suffix => file.name.endsWith(suffix)) && (file.isTestSource ?? false) === isTestSource)
      .map(file => `"\${JavaScriptCore_AOT_GENERATED_DIR}/${file.name}"`);
  const expected: readonly (readonly [string, readonly string[]])[] = [
    ["JavaScriptCore_AOT_SCHEMA_INPUTS", loaded.inputs.map(input => `aot/ir/${input}`)],
    ["JavaScriptCore_AOT_SCHEMA_GENERATED_HEADERS", written([".h", ".inc"])],
    ["JavaScriptCore_AOT_SCHEMA_GENERATED_SOURCES", written([".cpp"])],
    ["JavaScriptCore_AOT_SCHEMA_GENERATED_TEST_SOURCES", written([".cpp"], true)],
  ];
  return expected.flatMap(([name, entries]) => {
    const listed = lists.get(name);
    if (listed === undefined) return [`${listFile}: no list ${name}`];
    return [
      ...entries.filter(entry => !listed.includes(entry)).map(entry => `${listFile}: ${name} lacks ${entry}`),
      ...listed
        .filter(entry => !entries.includes(entry))
        .map(entry => `${listFile}: ${name} has ${entry}, and there is no such file`),
    ];
  });
}

interface RustCrateFile {
  readonly name: string;
  readonly parent: string;
  readonly modules: readonly string[];
}

const filesOfOtherCrates: readonly RustCrateFile[] = [
  {
    name: "aot_schema_builtins.rs",
    parent: "bun_aot_builtins",
    modules: ["entries", "enums"],
  },
  {
    name: "aot_schema_interface.rs",
    parent: "bun_aot_interface::ir::generated",
    modules: ["compile_call_enums", "diagnostics", "limits", "statistics"],
  },
];
const rustFileName = "aot_schema.rs";

function moduleNameOf(file: OutputFile): string {
  return file.name.slice(0, -".rs".length);
}

/** Bun writes the program as bytes. Only tests read text. */
function attributesOf(module: string): string[] {
  if (module === "checks") return ["#[cfg(test)]"];
  return module.endsWith("_text") ? ['#[cfg(any(test, feature = "ir_text"))]'] : [];
}

function rustModule(file: OutputFile, visibility: "pub " | ""): string[] {
  if (file.lines[0] !== rustMarker) throw new Error(`${file.name} does not start with the marker`);
  return [...attributesOf(moduleNameOf(file)), `${visibility}mod ${moduleNameOf(file)} {`, ...file.lines.slice(1), "}"];
}

function fileOfOtherCrate(crate: RustCrateFile, files: readonly OutputFile[]): OutputFile {
  const modules = crate.modules.map(module => {
    const file = files.find(candidate => moduleNameOf(candidate) === module);
    if (file === undefined) throw new Error(`no emitter writes ${module}.rs, which ${crate.name} holds`);
    return rustModule(file, "pub ");
  });
  return { name: crate.name, lines: [rustMarker, ...modules.flatMap(module => ["", ...module])] };
}

function useOfOtherCrate(crate: RustCrateFile): string[] {
  return [
    "#[cfg(test)]",
    `include!(concat!(env!("BUN_CODEGEN_DIR"), "/${crate.name}"));`,
    "#[cfg(not(test))]",
    `use ${crate.parent}::{${crate.modules.join(", ")}};`,
  ];
}

function rustModuleFile(context: EmitContext, files: readonly OutputFile[]): OutputFile {
  const modules = files.map(moduleNameOf);
  const elsewhere = new Set(filesOfOtherCrates.flatMap(crate => crate.modules));
  const isReexported = (module: string) =>
    module !== "checks" && !module.startsWith("builder_") && !module.startsWith("record_");
  const constants = context.schema.constants.map(constant => {
    const isWord = constant.value >= 0n && constant.value <= 0xffffffffn;
    const value =
      isWord && constant.value > 0xffffn
        ? `0x${constant.value.toString(16).padStart(8, "0")}`
        : constant.value.toString();
    return `pub const ${screamingCase(constant.name)}: ${isWord ? "u32" : "i64"} = ${value};`;
  });
  return {
    name: rustFileName,
    lines: [
      rustMarker,
      "",
      ...filesOfOtherCrates.flatMap(useOfOtherCrate),
      "",
      ...modules.filter(isReexported).flatMap(module => [...attributesOf(module), `pub use ${module}::*;`]),
      "#[cfg(test)]",
      "pub use checks::*;",
      "",
      `pub const SCHEMA_HASH: u64 = 0x${context.hash.toString(16).padStart(16, "0")};`,
      ...constants,
      ...files.filter(file => !elsewhere.has(moduleNameOf(file))).flatMap(file => ["", ...rustModule(file, "")]),
    ],
  };
}

/** One file a crate, under names that no change to the schema moves: Bun's build declares them before it has the engine. */
export function rustOutputs(loaded: LoadedSchema): OutputFile[] {
  const context = { schema: loaded.model, hash: loaded.hash };
  const files = runEmitters(rustEmitters, context);
  return [
    rustModuleFile(context, files),
    ...filesOfOtherCrates.map(crate => fileOfOtherCrate(crate, files)),
    hashHeader(loaded.hash),
  ].sort(byName);
}

function hashHeader(hash: bigint): OutputFile {
  const lines = [
    rustMarker,
    "#pragma once",
    "",
    `#define BUN_AOT_SCHEMA_HASH 0x${hash.toString(16).padStart(16, "0")}ull`,
  ];
  return { name: hashHeaderName, lines };
}

function readIfPresent(file: string): string | null {
  try {
    return fs.readFileSync(file, "utf8");
  } catch {
    return null;
  }
}

function isGenerated(text: string): boolean {
  return text.startsWith(cppMarker) || text.startsWith(rustMarker);
}

/** Other generators write into the same folders. */
const ownFileName = /^(?:AOTSchema|AOTWire|aot_schema)/;

function leftoverFiles(directory: string, files: readonly OutputFile[]): string[] {
  const names = new Set(files.map(file => file.name));
  let present: string[];
  try {
    present = fs.readdirSync(directory).sort();
  } catch {
    return [];
  }
  return present.filter(
    name => ownFileName.test(name) && !names.has(name) && isGenerated(readIfPresent(path.join(directory, name)) ?? ""),
  );
}

export function writeOutputs(directory: string, files: readonly OutputFile[]): string[] {
  for (const file of files)
    if (!ownFileName.test(file.name)) throw new Error(`${file.name}: not a name of this generator`);
  fs.mkdirSync(directory, { recursive: true });
  const changed: string[] = [];
  for (const file of files) {
    const target = path.join(directory, file.name);
    const text = toText(file.lines);
    if (readIfPresent(target) === text) continue;
    fs.writeFileSync(target, text);
    changed.push(file.name);
  }
  for (const name of leftoverFiles(directory, files)) {
    fs.rmSync(path.join(directory, name));
    changed.push(name);
  }
  return changed;
}
