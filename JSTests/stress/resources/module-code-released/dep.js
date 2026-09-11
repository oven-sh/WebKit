export let counter = 0;
export function bump() { return ++counter; }
export const tag = (strings) => strings;
export function taggedOnce() { return tag`a${1}b`; }
