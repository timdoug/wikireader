#!/usr/bin/env python3
"""Compare the native streaming scheduler against the reference timing model."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / 'experiments'))
from frontend import BODIES, simulate

DRIVER = Path(sys.argv.pop(1)).resolve()
TAGS = {'ext': 'e', 'alu': 'a', 'imm': 'i', 'ldh': 'h', 'ldw': 'w', 'st': 's', 'branch': 'b'}


class NativeFrontend(unittest.TestCase):
    def test_reference_timing_and_operation_order(self):
        for name, body in BODIES.items():
            for depth in (4, 8):
                for shared in (False, True):
                    for offset in (0, 2):
                        for unroll in (1, 3, 8, 16):
                            with self.subTest(name=name, depth=depth, shared=shared, offset=offset, unroll=unroll):
                                args = [depth, offset, int(shared), 1, 1, unroll, 8, ''.join(TAGS[kind] for kind in body)]
                                native = json.loads(subprocess.check_output([str(DRIVER), *map(str, args)], text=True))
                                events = []
                                config = dict(depth=depth, shared=shared, offset=offset, unroll=unroll,
                                              passes=8, trailing_ext=True, prefix_gate=True,
                                              port_turns=(2, 2), stage_edge=4, events=events)
                                if not shared: config['services'] = (8, 8, 16)
                                reference = simulate(body, **config)
                                self.assertEqual(native['cycles_per_pass'], reference)
                                self.assertEqual(native['events'], events)
                                self.assertEqual(native['words'], (len(body) * unroll + 2) * 8)

    def test_invalid_capacity_or_alignment_is_rejected(self):
        for depth, offset in ((1, 0), (9, 0), (4, 1)):
            result = subprocess.run([str(DRIVER), str(depth), str(offset), '1', '1', '1', '1', '8', 'a'],
                                    capture_output=True)
            self.assertEqual(result.returncode, 2)

    def test_reset_redirect_and_code_mapping_flush(self):
        result = subprocess.run([str(DRIVER), '--self-test'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('in-flight port checks pass', result.stdout)

    def test_optional_decode_rules_and_adjacent_accesses(self):
        for body in (['ldh', 'ldh'], ['ldw', 'ldw'], ['ldh', 'ext', 'alu'],
                     ['alu', 'ext', 'alu'], BODIES['mixed']):
            for trailing in (False, True):
                for gate in (False, True):
                    for shared in (False, True):
                        for unroll in (1, 8, 16):
                            args = [8, 2, int(shared), int(trailing), int(gate), unroll, 8,
                                    ''.join(TAGS[kind] for kind in body)]
                            native = json.loads(subprocess.check_output([str(DRIVER), *map(str, args)], text=True))
                            events = []
                            parameters = dict(depth=8, offset=2, shared=shared, unroll=unroll,
                                              passes=8, trailing_ext=trailing, prefix_gate=gate,
                                              port_turns=(2, 2), stage_edge=4, events=events)
                            if not shared: parameters['services'] = (8, 8, 16)
                            self.assertEqual(native['cycles_per_pass'], simulate(body, **parameters))
                            self.assertEqual(native['events'], events)

    def test_directional_turnaround_configurations(self):
        for body in (['ldh'], ['ldw', 'alu'], BODIES['mixed']):
            for turns in ((0, 2), (4, 0), (0, 0)):
                for shared in (False, True):
                    args = [8, 2, int(shared), 1, 1, 8, 8,
                            ''.join(TAGS[kind] for kind in body), *turns]
                    native = json.loads(subprocess.check_output([str(DRIVER), *map(str, args)], text=True))
                    events = []
                    parameters = dict(depth=8, offset=2, shared=shared, passes=8,
                                      trailing_ext=True, prefix_gate=True,
                                      port_turns=turns, stage_edge=4, events=events)
                    if not shared: parameters['services'] = (8, 8, 16)
                    self.assertEqual(native['cycles_per_pass'], simulate(body, **parameters))
                    self.assertEqual(native['events'], events)


if __name__ == '__main__':
    unittest.main()
