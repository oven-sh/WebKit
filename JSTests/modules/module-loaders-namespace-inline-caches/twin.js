// The same exported names as dep.js, so its namespace object has the same export layout, and other values.
export let id = -1;
export const tag = "twin";
export default "twin default";
export function bump() { return --id; }
export function declared() { return "twin declared"; }
