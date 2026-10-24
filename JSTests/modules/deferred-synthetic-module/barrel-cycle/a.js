import { helper } from "./index.js";
(globalThis.barrelLog ??= []).push("a:" + typeof helper);
export const useHelper = () => helper();
