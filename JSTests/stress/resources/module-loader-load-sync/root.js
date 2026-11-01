import { dep } from "./dep.js";
globalThis.loadSyncLog.push("root");
export const value = dep + 1;
