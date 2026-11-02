// To read some fields is to take more for granted than the type of a value says: the word in the cell of a string is a StringImpl only while the string is flat,
// and a type says what the characters are, not which object holds them. Such a field is read by few functions, which test first or take a type that only a proof can make.
// This step of the build counts, file by file, who names the heap of such a field and who calls a function that trusts its caller, and holds the counts to
// aot/codegen/AOTReadsThatAssume.txt. A place that the list lacks stops the build: go through one of the functions named in the message, or add the place with the reason why it is right.
import { mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { parseArgs } from "node:util";

const compilerDirectories = ["builtins", "codegen", "image", "object", "passes", "place", "represent", "select", "stubs"];
const listFile = "aot/codegen/AOTReadsThatAssume.txt";
const stampFile = "AOTReadsThatAssumeChecked.h";

const watched: { name: string; pattern: RegExp; instead: string }[] = [
  {
    name: "JSString_value",
    pattern: /\b(?:m_)?heaps\.JSString_value\b/g,
    instead: "StringEmitter::viewOf(), lengthOf(), viewOfString(), isRopeString(), or implOfFlatString() with a FlatStringCell",
  },
  {
    name: "flatStringWrittenHere",
    pattern: /\bflatStringWrittenHere\(/g,
    instead: "StringEmitter::flatStringByType(): this one trusts its caller that the cell is the value's",
  },
  {
    name: "flatStringAssumedBy",
    pattern: /\bflatStringAssumedBy\(/g,
    instead: "StringEmitter::flatStringByType(), and another road for what is not flat: this one stops the build of a user's program",
  },
  {
    name: "viewOfFlatString",
    pattern: /\bviewOfFlatString\(/g,
    instead: "StringEmitter::viewOf(): this one trusts its caller that isRopeString() of the same cell has just answered no",
  },
  {
    name: "areBoth8Bit",
    pattern: /\bareBoth8Bit\(/g,
    instead: "StringEmitter::areEqual(): this one trusts its caller that both strings are flat",
  },
];

type Counts = Map<string, number>;

function keyOf(name: string, file: string): string {
  return `${name} | ${file}`;
}

function countPlaces(javascriptcore: string): Counts {
  const counts: Counts = new Map();
  for (const directory of compilerDirectories) {
    for (const entry of readdirSync(join(javascriptcore, "aot", directory), { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      if (!entry.isFile() || !/\.(?:cpp|h)$/.test(entry.name)) continue;
      const text = readFileSync(join(javascriptcore, "aot", directory, entry.name), "utf8");
      for (const { name, pattern } of watched) {
        const found = text.match(pattern)?.length ?? 0;
        if (found) counts.set(keyOf(name, `${directory}/${entry.name}`), found);
      }
    }
  }
  return counts;
}

function readList(text: string): { listed: Counts; problems: string[] } {
  const listed: Counts = new Map();
  const problems: string[] = [];
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const [name, file, count, reason] = line.split(" | ");
    if (!name || !file || !/^\d+$/.test(count ?? "") || !reason?.trim()) problems.push(`${listFile}: not "<name> | <file> | <count> | <reason>": ${line}`);
    else listed.set(keyOf(name, file), Number(count));
  }
  return { listed, problems };
}

function compare(found: Counts, listed: Counts): string[] {
  const problems: string[] = [];
  for (const [key, count] of found) {
    const allowed = listed.get(key) ?? 0;
    if (count === allowed) continue;
    const [name, file] = key.split(" | ");
    if (count > allowed)
      problems.push(
        `aot/${file} has ${count} of ${name}, the list has ${allowed}. It takes for granted what no type says. Use ${watched.find(entry => entry.name === name)!.instead}; or say in ${listFile} why the new place is right.`,
      );
    else problems.push(`aot/${file} has ${count} of ${name}, the list has ${allowed}: lower the count in ${listFile}, or the next place passes unseen.`);
  }
  for (const [key, allowed] of listed) {
    if (!found.has(key)) problems.push(`aot/${key.split(" | ")[1]} has none of ${key.split(" | ")[0]}, the list has ${allowed}: take the line out of ${listFile}.`);
  }
  return problems;
}

// A check that was never seen to fail proves nothing: every name must be found somewhere, and one place too many, one too few and one in a file of its own must each be noticed.
function hasTeeth(found: Counts): boolean {
  if (!watched.every(({ name }) => [...found.keys()].some(key => key.startsWith(`${name} | `)))) return false;
  const [key, count] = [...found][0] ?? [];
  if (key === undefined || count === undefined) return false;
  const more = new Map(found).set(key, count + 1);
  const fewer = new Map(found).set(key, count - 1);
  const elsewhere = new Map(found).set(keyOf(watched[0]!.name, "codegen/AOTNoSuchFile.cpp"), 1);
  return !compare(found, found).length && [more, fewer, elsewhere].every(changed => compare(changed, found).length === 1 && compare(found, changed).length === 1);
}

const { values } = parseArgs({
  strict: true,
  options: { "javascriptcore": { type: "string" }, "out": { type: "string" }, "print-counts": { type: "boolean" } },
});
if (values.javascriptcore === undefined || (values.out === undefined && !values["print-counts"])) {
  console.error("usage: <bun or node> aot/codegen/generator/reads-that-assume.ts --javascriptcore <Source/JavaScriptCore> (--out <dir> | --print-counts)");
  process.exit(1);
}
const javascriptcore = resolve(values.javascriptcore);
const found = countPlaces(javascriptcore);
if (values["print-counts"]) {
  for (const [key, count] of found) console.log(`${key} | ${count}`);
  process.exit(0);
}
const { listed, problems } = readList(readFileSync(join(javascriptcore, listFile), "utf8"));
if (!hasTeeth(found)) problems.push("aot/codegen/generator/reads-that-assume.ts does not notice a place too many or too few: it checks nothing.");
problems.push(...compare(found, listed));
if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
mkdirSync(values.out!, { recursive: true });
writeFileSync(join(values.out!, stampFile), "// Written by aot/codegen/generator/reads-that-assume.ts when the sources of the compiler have passed it. Do not edit.\n");
