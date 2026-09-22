#!/usr/bin/env python3
"""Run the native matcher harness against expected PE/CLI fixture outcomes.

The optional v2 report cross-checks the independently established accepted
statuses and operand offsets. Native malformed cases have no v2 counterpart.
"""
from __future__ import annotations
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, action='append', required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--sanitized', action='store_true')
    args = parser.parse_args()
    cases = []
    for manifest_path in args.manifest:
        manifest = json.loads(manifest_path.read_text())
        for case in manifest['cases']:
            cases.append(dict(case, path=manifest_path.parent / case['file']))
    assert len({case['name'] for case in cases}) == len(cases), 'Duplicate fixture names'
    env = dict(os.environ)
    if args.sanitized:
        # LeakSanitizer cannot run under this execution environment's tracing.
        # Address and undefined-behavior checking remain enabled.
        env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    proc = subprocess.run([str(args.harness.resolve()), *[str(case['path'].resolve()) for case in cases]],
                          text=True, capture_output=True, timeout=180, env=env)
    if proc.returncode or proc.stderr:
        raise AssertionError(f'Harness exit={proc.returncode}\n{proc.stdout}\n{proc.stderr}')
    output = proc.stdout.splitlines()
    assert len(output) == len(cases), f'Expected {len(cases)} output records; got {len(output)}'
    baseline = {}
    if args.baseline:
        baseline = {case['name']: case for case in json.loads(args.baseline.read_text())}
    results = []
    for case, line in zip(cases, output):
        name, status, offset_text, upper_text, lower_text, roundtrip_text, reason = line.split('\t', 6)
        offset = int(offset_text)
        assert name == case['path'].name, (name, case['path'])
        passed = status == case['expect']
        if case['upper_offset'] is not None:
            passed &= offset == case['upper_offset']
        if case['name'] in baseline:
            prior = baseline[case['name']]
            passed &= status == prior['actual']
            if status != 'Unsupported':
                passed &= offset == prior['UpperOffset']
        passed &= roundtrip_text == '1'
        result = dict(name=case['name'], passed=passed, expected=case['expect'],
                      actual=status, upper_offset=offset, original_upper=float(upper_text),
                      lower=float(lower_text), roundtrip_unchanged_outside_operand=roundtrip_text == '1',
                      reason=reason, v2_crosscheck=case['name'] in baseline)
        results.append(result)
        print(('PASS: ' if passed else 'FAIL: ') + case['name'] + ': ' + status, flush=True)
    report = dict(checks=len(results), all_passed=all(result['passed'] for result in results),
                  platform=sys.platform, address_and_undefined_behavior_sanitizers=args.sanitized,
                  leak_sanitizer=False,
                  source='One supplied Arena release plus synthetic mutations; no other real releases tested.',
                  results=results)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    assert report['all_passed'], 'Fixture mismatch; see report'
    print(f'{len(results)} native matcher fixtures passed.')


if __name__ == '__main__':
    main()
