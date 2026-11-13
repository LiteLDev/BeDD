#!/usr/bin/env node
// Publishes the AsyncAPI document of the WebSocket API with the site: copies ../spec/bedd.asyncapi.yaml (the one
// source, next to the code it describes) to public/, which Vite serves at <base>bedd.asyncapi.yaml. The copy is
// git-ignored; `npm run build` and `npm run dev` run this first.
import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const site = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const from = resolve(site, '..', 'spec', 'bedd.asyncapi.yaml');
const to = join(site, 'public', 'bedd.asyncapi.yaml');
mkdirSync(dirname(to), { recursive: true });
copyFileSync(from, to);
console.log(`spec: ${from} → ${to}`);
