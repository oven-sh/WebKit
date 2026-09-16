import { value as imported, bump as importedBump } from "./self.js";
import * as self from "./self.js";
export let value = 1;
export function bump() { return ++value; }
export function read() { return [imported, self.value, importedBump === self.bump && importedBump === bump]; }
