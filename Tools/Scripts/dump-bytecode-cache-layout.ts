#!/usr/bin/env node
// Prints the memory layout of every record the bytecode cache (runtime/CachedTypes.cpp) writes into a payload, in a
// form that is textually identical across targets exactly when the layouts are. The release workflow diffs this file
// between platforms: a payload encoded on one must decode on the others, so their Cached* records must agree.
//
// Usage: (bun | node) dump-bytecode-cache-layout.ts <build dir>            recompiles the CachedTypes.cpp translation unit with -fdump-record-layouts
//        (bun | node) dump-bytecode-cache-layout.ts --dump <clang output>   parses an existing dump (for testing)

import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { basename, isAbsolute, join } from "node:path";

function fail(message: string): never {
  process.stderr.write(message + "\n");
  process.exit(1);
}

function splitCommand(command: string): string[] {
  const args: string[] = [];
  for (const m of command.matchAll(/"((?:\\.|[^"\\])*)"|'([^']*)'|(\S+)/g)) args.push(m[1]?.replace(/\\(["\\])/g, "$1") ?? m[2] ?? m[3]);
  return args;
}

function compileCommand(buildDir: string): { directory: string; args: string[] } {
  const commands: { directory: string; file: string; command?: string; arguments?: string[] }[] = JSON.parse(readFileSync(join(buildDir, "compile_commands.json"), "utf8"));
  for (const entry of commands) {
    if (!entry.file.includes("JavaScriptCore") || !entry.file.includes("UnifiedSource")) continue;
    const path = isAbsolute(entry.file) ? entry.file : join(entry.directory, entry.file);
    if (!existsSync(path) || !readFileSync(path, "utf8").includes('"runtime/CachedTypes.cpp"')) continue;
    return { directory: entry.directory, args: entry.arguments ?? splitCommand(entry.command!) };
  }
  fail("no translation unit in compile_commands.json includes runtime/CachedTypes.cpp");
}

function recordLayouts(buildDir: string): string {
  let { directory, args } = compileCommand(buildDir);
  if (["ccache", "sccache"].includes(basename(args[0]))) args = args.slice(1);
  // The same command minus its outputs: only the front end runs, and it prints every record layout it computes.
  const filtered: string[] = [];
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (["-o", "-MF", "-MT", "-MQ"].includes(arg)) i++;
    else if (["-c", "/c", "-MD", "-MMD", "/showIncludes"].includes(arg) || /^(-o|\/Fo|\/Fd|-MF|-MT)/.test(arg)) continue;
    else filtered.push(arg);
  }
  filtered.splice(1, 0, "-fsyntax-only", "-Xclang", "-fdump-record-layouts"); // ahead of any `--` (clang-cl commands end `-- <source>`)
  try {
    return execFileSync(filtered[0], filtered.slice(1), { cwd: directory, encoding: "utf8", stdio: ["ignore", "pipe", "inherit"], maxBuffer: 1 << 30 });
  } catch {
    fail("dumping record layouts failed: " + filtered.join(" "));
  }
}

function normalize(name: string): string {
  return name
    .replace(/\b(class|struct|union|enum) /g, "")
    .replace(/\(unnamed (?:\w+ )?at [^)]*?([^/\\:)]+:\d+:\d+)\)/g, "(unnamed at $1)")
    .replace(/\bunsigned long long\b|\bunsigned long\b/g, "ulong") // uint64_t's spelling differs (LP64 / LLP64); sizes are compared, not spellings
    .replace(/(?<!unsigned )\blong long\b|(?<![nu] )\blong\b/g, "long")
    .trim();
}

type Record = { name: string; size: number; align: number; rows: { offset: string; depth: number; text: string }[]; members: { type: string | null }[]; bases: string[] };

function parse(dump: string): Map<string, Record> {
  const records = new Map<string, Record>();
  for (const block of dump.split("*** Dumping AST Record Layout\n").slice(1)) {
    const lines = block.split("\n");
    const head = lines[0].match(/^\s*0 \| (.*)$/);
    const sizes = block.match(/\[sizeof=(\d+),(?:\s*dsize=\d+,)?\s*align=(\d+)/);
    if (!head || !sizes) continue;
    const record: Record = { name: normalize(head[1]), size: +sizes[1], align: +sizes[2], rows: [], members: [], bases: [] };
    for (const line of lines.slice(1)) {
      const row = line.match(/^\s*([\d:.-]+) \| ( +)(.*)$/);
      if (!row) continue;
      const depth = row[2].length / 2, text = normalize(row[3]);
      record.rows.push({ offset: row[1], depth, text });
      if (depth !== 1) continue;
      const base = / \((?:primary |virtual )?base\)(?: \(empty\))?$/;
      if (base.test(text)) {
        const type = text.replace(base, "");
        record.bases.push(type);
        record.members.push({ type });
      } else record.members.push({ type: text.match(/^(.+) [A-Za-z_]\w*(\[\d*\])*$/)?.[1] ?? null });
    }
    records.set(record.name, record);
  }
  return records;
}

function templateArguments(name: string): string[] {
  const out: string[] = [];
  let depth = 0, start = 0;
  for (let i = 0; i < name.length; i++) {
    const c = name[i];
    if (c === "<" && ++depth === 1) start = i + 1;
    else if (c === ">" && --depth === 0) out.push(name.slice(start, i));
    else if (c === "," && depth === 1) { out.push(name.slice(start, i)); start = i + 1; }
  }
  return out.map(a => a.trim());
}

function cacheRecords(records: Map<string, Record>): Record[] {
  // Roots: everything laid out in a payload derives from CachedObject<> / VariableLengthObjectBase, plus the entry headers.
  const isRoot = (record: Record, seen: string[] = []): boolean =>
    /^JSC::(\w*CacheEntry\b|CachedObject<|VariableLengthObjectBase$)/.test(record.name) ||
    record.bases.some(base => records.has(base) && !seen.includes(base) && isRoot(records.get(base)!, [...seen, record.name]));
  // T of every CachedObject<T> / VariableLengthObject<T>: the in-memory types the cache converts from, never laid out in a payload.
  const sourceTypes = new Set([...records.values()].filter(r => /^JSC::(CachedObject|VariableLengthObject)</.test(r.name)).map(r => templateArguments(r.name)[0]));
  const selected = new Map<string, Record>();
  const queue = [...records.values()].filter(r => isRoot(r));
  while (queue.length) {
    const record = queue.pop()!;
    if (selected.has(record.name)) continue;
    selected.set(record.name, record);
    const reached = record.members.map(m => m.type).filter((t): t is string => !!t);
    // A Cached container's element type is written into the payload too (in its variable-length tail), so follow
    // template arguments -- except CachedObject<T> / VariableLengthObject<T>, whose T is the in-memory source type.
    if (!/^JSC::(CachedObject|VariableLengthObject)</.test(record.name)) reached.push(...templateArguments(record.name).filter(t => !sourceTypes.has(t)));
    for (const name of reached) {
      if (/[*&]$/.test(name)) continue;
      const r = records.get(normalize(name));
      if (r && !selected.has(r.name)) queue.push(r);
    }
  }
  return [...selected.keys()].sort().map(name => selected.get(name)!);
}

const argv = process.argv.slice(2);
const dump = argv[0] === "--dump" ? readFileSync(argv[1], "utf8") : recordLayouts(argv[0] ?? fail("usage: dump-bytecode-cache-layout.ts <build dir>"));
const selected = cacheRecords(parse(dump));
if (selected.length < 50) fail(`found only ${selected.length} bytecode cache records; the dump or the selection is broken`);
const out: string[] = [];
for (const record of selected) {
  out.push(`${record.name} size=${record.size} align=${record.align}`);
  for (const { offset, depth, text } of record.rows) out.push(`  ${"  ".repeat(depth - 1)}${offset} | ${text}`);
}
process.stdout.write(out.join("\n") + "\n");
