#!/usr/bin/env python3
"""Pass each FIRRTL annotation to firtool once in the native simulation build."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def embedded_annotations(circuit):
    text = circuit.read_text()
    circuit_start = text.find('\ncircuit ')
    if circuit_start < 0 and text.startswith('circuit '):
        circuit_start = 0
    if circuit_start < 0:
        raise ValueError(f'no FIRRTL circuit header in {circuit}')
    header_end = text.index('\n', circuit_start + 1)
    marker = text.find('%[', circuit_start, header_end)
    if marker < 0:
        return []
    annotations, end = json.JSONDecoder().raw_decode(text, marker + 2)
    if text[end:end + 1] != ']' or not isinstance(annotations, list):
        raise ValueError(f'invalid embedded FIRRTL annotations in {circuit}')
    return annotations


def annotation_key(annotation):
    if not isinstance(annotation, dict):
        raise ValueError('FIRRTL annotations must be JSON objects')
    return json.dumps(annotation, sort_keys=True, separators=(',', ':'))


def run(arguments):
    tool = shlex.split(os.environ.get('DIFFTEST_S_FIRTOOL_BIN', 'firtool'))
    if not tool:
        raise ValueError('DIFFTEST_S_FIRTOOL_BIN is empty')
    circuits = [Path(arg) for arg in arguments if arg.endswith('.fir') and not arg.startswith('-')]
    annotations = [(index, Path(arg.split('=', 1)[1])) for index, arg in enumerate(arguments)
                   if arg.startswith('--annotation-file=')]
    if not circuits or not annotations:
        return subprocess.run(tool + arguments).returncode
    if len(circuits) != 1 or len(annotations) != 1:
        raise ValueError('native firtool launcher requires one circuit and one annotation file')
    index, annotation_file = annotations[0]
    embedded = {annotation_key(annotation) for annotation in embedded_annotations(circuits[0])}
    external = json.loads(annotation_file.read_text())
    if not isinstance(external, list):
        raise ValueError('external FIRRTL annotations must be a JSON array')
    remaining = [annotation for annotation in external if annotation_key(annotation) not in embedded]
    if len(remaining) == len(external):
        return subprocess.run(tool + arguments).returncode
    with tempfile.TemporaryDirectory(prefix='difftest-s-firtool-', dir=annotation_file.parent) as directory:
        filtered_file = Path(directory) / 'external.anno.json'
        filtered_file.write_text(json.dumps(remaining))
        arguments[index] = '--annotation-file=' + str(filtered_file)
        return subprocess.run(tool + arguments).returncode


def main():
    try:
        return run(sys.argv[1:])
    except (OSError, ValueError) as error:
        print(f'Difftest-S firtool input error: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
