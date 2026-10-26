#!/usr/bin/env python3
"""Generate the native snapshot registry from elaboration layout metadata."""
import argparse
import json
from pathlib import Path


def bounded_integer(record, name, lower, upper):
    value = record[name]
    if type(value) is not int or not lower <= value <= upper:
        raise ValueError(f'invalid native layout {name}: {value}')
    return value


def validate_layout(layout):
    if layout['abi'] != 1:
        raise ValueError('unsupported native layout ABI')
    kind = bounded_integer(layout, 'kind', 1, 0xffffffff)
    harts = bounded_integer(layout, 'harts', 1, 256)
    hart = bounded_integer(layout, 'hart', 0, harts - 1)
    bits = bounded_integer(layout, 'bits', 1, 1 << 20)
    certificate = layout.get('sram_capture_boundary', False)
    if type(certificate) is not bool or (certificate and kind != 2):
        raise ValueError('invalid BOOM SRAM capture-boundary certificate')
    if not isinstance(layout['fields'], list) or not layout['fields']:
        raise ValueError('native layout has no fields')
    offset = 0
    paths = set()
    for field in layout['fields']:
        path = field['path']
        width = bounded_integer(field, 'width', 1, 64)
        field_offset = bounded_integer(field, 'offset', 0, bits - 1)
        if not isinstance(path, str) or not path or path in paths or field_offset != offset:
            raise ValueError(f'duplicated, overlapping or invalid native field {field}')
        paths.add(path)
        offset += width
    if offset != bits:
        raise ValueError(f'native payload width differs from field sum {(kind, hart)}')
    return kind, hart


def validate_rosters(layouts):
    rosters = {}
    keys = set()
    for layout in layouts:
        kind, hart = validate_layout(layout)
        if (kind, hart) in keys:
            raise ValueError(f'duplicate native probe layout {(kind, hart)}')
        keys.add((kind, hart))
        expected, observed = rosters.setdefault(kind, (layout['harts'], set()))
        if expected != layout['harts']:
            raise ValueError(f'inconsistent native hart roster for kind {kind}')
        observed.add(hart)
    for kind, (expected, observed) in rosters.items():
        if observed != set(range(expected)):
            raise ValueError(f'incomplete native hart roster for kind {kind}')
    return keys


def generate(layouts):
    keys = validate_rosters(layouts)
    lines = ['#include "native_probe.h"', 'namespace difftest_s::native {', 'namespace {']
    for layout in sorted(layouts, key=lambda entry: (entry['kind'], entry['hart'])):
        label = f"{layout['kind']}_{layout['hart']}"
        lines.append(f'static constexpr NativeFieldSpec fields_{label}[] = {{')
        for field in layout['fields']:
            path = json.dumps(field['path'])
            lines.append(f'  {{{path}, {field["offset"]}, {field["width"]}}},')
        lines += ['};', f'static constexpr NativeProbeLayout layout_{label} = {{',
                  f'  1, {layout["kind"]}, {layout["hart"]}, {layout["harts"]}, {layout["bits"]},',
                  f'  sizeof(fields_{label}) / sizeof(fields_{label}[0]), fields_{label},',
                  '  ' + str(layout.get('sram_capture_boundary', False)).lower(), '};']
    lines += ['} // namespace', '',
              'const NativeProbeLayout *find_native_layout(uint32_t kind, uint32_t hart) {']
    for kind, hart in sorted(keys):
        lines.append(f'  if (kind == {kind} && hart == {hart}) return &layout_{kind}_{hart};')
    lines += ['  return nullptr;', '}', '} // namespace difftest_s::native', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('layouts', nargs='+', type=Path)
    args = parser.parse_args()
    contents = generate([json.loads(path.read_text()) for path in args.layouts])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(contents)


if __name__ == '__main__':
    main()
