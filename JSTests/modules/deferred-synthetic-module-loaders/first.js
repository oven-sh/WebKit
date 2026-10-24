import { read } from "./main.js";
(globalThis.loadersLog ??= []).push("first:" + read());
