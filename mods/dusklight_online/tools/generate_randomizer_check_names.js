// Refresh the display-name table from the official Dusklight Randomizer source.
// Usage: node tools/generate_randomizer_check_names.js <dusklight checkout>
const fs = require('fs');
const path = require('path');

const host = process.argv[2];
if (!host) throw new Error('Pass the official Dusklight checkout path');
const read = relative => fs.readFileSync(path.join(host, relative), 'utf8');
const stagesSource = read('mods/randomizer/src/stages.cpp');
const stagesBlock = stagesSource.match(/const char allStages\[.*?\]\[.*?\] = \{([\s\S]*?)\};/);
if (!stagesBlock) throw new Error('Cannot read Randomizer stage IDs');
const stages = [...stagesBlock[1].matchAll(/"([A-Z0-9_]+)"/g)].map(match => match[1]);
const itemsHeader = read('sdk/include/mods/items.h');
const macros = new Map([...itemsHeader.matchAll(/^#define (ITEM_CHECK_[A-Z0-9_]+) "([^"]+)"/gm)]
    .map(match => [match[1], match[2]]));
const toolsSource = read('mods/randomizer/src/tools.cpp');
const overridesBlock = toolsSource.match(/static std::unordered_map<std::string, std::string> nameLookup = \{([\s\S]*?)\};/);
if (!overridesBlock) throw new Error('Cannot read Randomizer name overrides');
const overrides = new Map();
for (const match of overridesBlock[1].matchAll(/\{"([^"]+)",\s*("[^"]+"|ITEM_CHECK_[A-Z0-9_]+)\}/g)) {
    const key = match[2].startsWith('"') ? match[2].slice(1, -1) : macros.get(match[2]);
    if (!key) throw new Error(`Unknown check macro ${match[2]}`);
    overrides.set(match[1], key);
}

const names = new Map();
const add = (key, display) => {
    if (names.has(key) && names.get(key) !== display)
        throw new Error(`Ambiguous Randomizer check ${key}: ${names.get(key)} / ${display}`);
    names.set(key, display);
};
const locations = read('mods/randomizer/generator/data/locations.yaml');
for (const block of locations.split(/(?=^- Name: )/m)) {
    const name = block.match(/^- Name: ([^\r\n]+)/m)?.[1];
    if (!name) continue;
    for (const [category, prefix, flagField] of [
        ['Chest', 'chest', 'Tbox Id'],
        ['Freestanding Item', 'freestanding', 'Flag'],
        ['Poe', 'poe', 'Flag'],
    ]) {
        const metadata = block.match(new RegExp(`^    ${category}:\\r?\\n((?: {6,}[^\\r\\n]*\\r?\\n)*)`, 'm'))?.[1];
        if (!metadata) continue;
        for (const entry of metadata.split(/(?=^      - Stage:)/m)) {
            const stageId = entry.match(/^      - Stage:\s*(\d+)/m)?.[1];
            const flag = entry.match(new RegExp(`^        ${flagField}:\\s*(0x[0-9a-fA-F]+|\\d+)`, 'm'))?.[1];
            if (stageId === undefined || flag === undefined) continue;
            const stage = stages[Number(stageId)];
            if (!stage) throw new Error(`Unknown Randomizer stage ${stageId}`);
            add(`${prefix}:${stage}:${Number(flag)}`, name);
        }
    }
    for (const [category, prefix, fields] of [
        ['Shop', 'shop', ['Room', 'Item']],
        ['Sky Character', 'sky', ['Room']],
    ]) {
        const metadata = block.match(new RegExp(`^    ${category}:\\r?\\n((?: {6,}[^\\r\\n]*\\r?\\n)*)`, 'm'))?.[1];
        if (!metadata) continue;
        for (const entry of metadata.split(/(?=^      - Stage:)/m)) {
            const stageId = entry.match(/^      - Stage:\s*(\d+)/m)?.[1];
            if (stageId === undefined) continue;
            const stage = stages[Number(stageId)];
            if (!stage) throw new Error(`Unknown Randomizer stage ${stageId}`);
            const values = fields.map(field => entry.match(
                new RegExp(`^        ${field}:\\s*(0x[0-9a-fA-F]+|\\d+)`, 'm'))?.[1]);
            if (values.some(value => value === undefined)) continue;
            add(`${prefix}:${stage}:${values.map(Number).join(':')}`, name);
        }
    }
    for (const [category, prefix, field] of [
        ['Bug Reward', 'bug', 'Item Id'],
        ['Golden Wolf', 'golden_wolf', 'Flag'],
    ]) {
        const metadata = block.match(new RegExp(`^    ${category}:\\r?\\n((?: {6,}[^\\r\\n]*\\r?\\n)*)`, 'm'))?.[1];
        if (!metadata) continue;
        for (const match of metadata.matchAll(new RegExp(`^      - ${field}:\\s*(0x[0-9a-fA-F]+|\\d+)`, 'gm')))
            add(`${prefix}:${Number(match[1])}`, name);
    }
    const nameLookup = block.match(/^    Name Lookup:\r?\n((?: {6,}[^\r\n]*\r?\n)*)/m)?.[1];
    if (nameLookup) {
        for (const match of nameLookup.matchAll(/^      - ([^\r\n]+)/gm))
            add(overrides.get(match[1]) ?? match[1], name);
    }
}

const cppString = value => JSON.stringify(value);
const entries = [...names].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)
    .map(([key, name]) => `    {${cppString(key)}, ${cppString(name)}},`).join('\n');
const output = `// Generated from official Dusklight Randomizer location metadata.\n` +
`// Refresh with: node tools/generate_randomizer_check_names.js <dusklight checkout>\n` +
`#include "dusklight_online/game/randomizer_check_names.hpp"\n` +
`#include <algorithm>\n#include <iterator>\n#include <string_view>\n#include <utility>\n\n` +
`namespace dusklight_online::game {\nnamespace {\n` +
`constexpr std::pair<std::string_view, std::string_view> kCheckNames[] = {\n${entries}\n};\n` +
`}  // namespace\n\n` +
`std::string_view randomizer_check_display_name(std::string_view checkName) {\n` +
`    const auto found = std::lower_bound(std::begin(kCheckNames), std::end(kCheckNames), checkName,\n` +
`        [](const auto& entry, std::string_view name) { return entry.first < name; });\n` +
`    if (found != std::end(kCheckNames) && found->first == checkName) return found->second;\n` +
`    // Future Randomizer checks without a known display name should not show raw stage IDs.\n` +
`    return checkName.find(':') == std::string_view::npos &&\n` +
`        checkName.find('_') == std::string_view::npos ? checkName : std::string_view{};\n` +
`}\n\n}  // namespace dusklight_online::game\n`;
const outputPath = path.join(__dirname, '../src/game/randomizer_check_names.cpp');
fs.writeFileSync(outputPath, output);
process.stdout.write(`Wrote ${names.size} check names to ${outputPath}\n`);
