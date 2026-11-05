"""The prospective model must use the actual frozen assembly and unroll."""
import json
import unittest

from port_predictions import body, templates


class Predictions(unittest.TestCase):
    def test_decode_actual_memory_and_prefix_categories(self):
        self.assertEqual(body('ext 0\nld.w %r10,[%r7]\nld.w [%r8],%r10\nnop\nld.w %r10,5'),
                         ['ext', 'ldw', 'st', 'alu', 'imm'])
        with self.assertRaises(ValueError):
            body('unexpected %r10,%r11')

    def test_unroll_and_native_layout_must_match(self):
        manifest = {'probes': {'ldh_u1': {'unroll': 1, 'bytes': 28},
                               'ldh_u8': {'unroll': 8, 'bytes': 42}}}
        assembly = json.dumps('ld.h %r10,[%r7]')
        source = f'PROBE_N(ldh_u1, 1, {assembly});\nPROBE_N(ldh_u8, 8, {assembly});'
        self.assertEqual(templates(source, manifest), {'ldh_u1': ['ldh'], 'ldh_u8': ['ldh']})
        for changed in (source.replace('ldh_u8, 8', 'ldh_u8, 4'),
                        source.splitlines()[0], source + '\n' + source.splitlines()[0],
                        source.replace('ld.h %r10,[%r7]', 'ld.h %r10,[%r7]\\nnop')):
            with self.subTest(source=changed), self.assertRaises(ValueError):
                templates(changed, manifest)


if __name__ == '__main__':
    unittest.main()
