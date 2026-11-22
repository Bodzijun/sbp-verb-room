import { readFile } from 'node:fs/promises';
import { loadContract } from '../src/bridge/contract.js';
export const readFixture = async name => JSON.parse(await readFile(new URL(`../fixtures/${name}.json`, import.meta.url), 'utf8'));
export const validator = await loadContract(async name => JSON.parse(await readFile(new URL(`../schema/${name}.schema.json`, import.meta.url), 'utf8')));
