// What the generator needs of its surroundings. It runs under node in WebKit's build and under Bun in Bun's.
import { mkdirSync, readFileSync, realpathSync, renameSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";
import { fileURLToPath } from "node:url";

// Under an older node a generator would stop at the first type, or run and write nothing. It says what it needs instead.
export function requireRuntime(): void {
  if ("Bun" in globalThis) return;
  const [major, minor] = process.versions.node.split(".").map(Number);
  if (major > 24 || (major === 24 && minor >= 2) || (major === 22 && minor >= 18)) return;
  console.error(
    `error: the generators of aot/ need node 24.2 or later, or 22.18 or later of 22: this is ${process.versions.node}`,
  );
  process.exit(1);
}

/** Whether the module at `url` is the program that was started, not a module that another one imports. */
export function isProgram(url: string): boolean {
  const started = process.argv[1];
  return started !== undefined && realpathSync(started) === realpathSync(fileURLToPath(url));
}

export function cap(text: string): string {
  return text[0].toUpperCase() + text.slice(1);
}

/** `--name=value` and `--name`, which reads as true. */
export function readFlags(names: readonly string[]): Record<string, string | boolean | undefined> {
  const flags: Record<string, string | boolean | undefined> = {};
  for (const argument of process.argv.slice(2)) {
    const separator = argument.indexOf("=");
    const name = argument.slice(2, separator === -1 ? undefined : separator);
    if (!argument.startsWith("--") || !names.includes(name)) {
      console.error(`error: unknown argument: ${argument}`);
      process.exit(1);
    }
    flags[name] = separator === -1 ? true : argument.slice(separator + 1);
  }
  return flags;
}

// A file that stays as it is keeps its time, so nothing that depends on it is built again. Through a rename, so that nobody reads half a file.
export function writeIfChanged(file: string, written: string): void {
  const text = `${written.replaceAll("\r\n", "\n").trim()}\n`;
  try {
    if (readFileSync(file, "utf8") === text) return;
  } catch {}
  mkdirSync(dirname(file), { recursive: true });
  const temporary = `${file}.${process.pid}.tmp`;
  writeFileSync(temporary, text);
  renameSync(temporary, file);
}

const setPattern = (name: string): RegExp => new RegExp(String.raw`^set\(${name}\s([^)]*)\)`, "m");

/** The words of `set(<name> ..)` in a CMake file that writes the list out in one piece, with `${variable}` replaced. */
export function readCMakeList(file: string, name: string, variables: Readonly<Record<string, string>> = {}): string[] {
  const words = setPattern(name).exec(readFileSync(file, "utf8"))?.[1];
  if (words === undefined) return [];
  return words
    .split(/\s+/)
    .filter(word => word !== "")
    .map(word => word.replace(/\$\{(\w+)\}/g, (reference, variable: string) => variables[variable] ?? reference));
}
