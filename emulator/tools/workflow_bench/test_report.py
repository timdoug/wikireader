import unittest

from report import validate, summarize, startup, compare


MANIFEST = {'version': 1, 'entries': [{'archive': 'simple', 'title': 'New York'}],
            'passes': 3, 'scroll_moves': [104, 104, -104, -104]}


def sample():
    lines = ['ZCAL v1 entries=1 passes=3 warmup=0 ticks_per_us=60 scrolls=4 '
             'sdram_ctl=00001243 refresh=03ff01c0 clkcntl=0a7b050e pll=00101a95']
    for p in range(1, 4):
        lines.append(f'ZC pass={p} entry=0 archive=1:/simple.zim title=New York '
                     'index=10 lookup_ticks=60 retrieve_ticks=120 open_ticks=180 '
                     'paint_ticks=240 screen_ticks=300 raw=20 text=10 article=10 '
                     'lookup_reads=1 lookup_sectors=1 lookup_read_ticks=20 '
                     'retrieve_reads=1 retrieve_sectors=2 retrieve_read_ticks=40 '
                     'images=0 frame=abcdef01 ok=1')
        position = 0
        for step, delta in enumerate(MANIFEST['scroll_moves']):
            lines.append(f'ZCS pass={p} entry=0 step={step} delta={delta} before={position} '
                         f'after={position + delta} ticks=60 direct=1 frame=abcdef02 ok=1')
            position += delta
    lines.append('END ZCAL cases=3 failures=0')
    return '\n'.join(lines) + '\n'


class Report(unittest.TestCase):
    def test_full_coverage_and_units(self):
        articles, scrolls = validate(sample(), MANIFEST)
        self.assertEqual((len(articles), len(scrolls)), (3, 12))
        result = summarize(sample(), MANIFEST)
        self.assertEqual(len(result), 9)
        self.assertEqual(result[0]['title'], 'New York')
        self.assertEqual(result[0]['median_us'], 1)
        self.assertEqual(result[0]['ticks'], [60, 60, 60])

    def test_truncated_and_duplicate_runs_are_rejected(self):
        text = sample()
        for bad in (text.split('END ZCAL')[0], text + text,
                    text.replace('END ZCAL', text.splitlines()[1] + '\nEND ZCAL')):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    validate(bad, MANIFEST)

    def test_missing_repetition_or_viewport_is_rejected(self):
        for prefix in ('ZC pass=2', 'ZCS pass=3 entry=0 step=2'):
            text = '\n'.join(line for line in sample().splitlines() if not line.startswith(prefix)) + '\n'
            with self.assertRaises(ValueError):
                validate(text, MANIFEST)

    def test_noop_or_wrong_direction_is_not_a_scroll_sample(self):
        for after in ('0', '-104'):
            text = sample().replace('before=0 after=104', 'before=0 after=' + after, 1)
            with self.assertRaises(ValueError):
                validate(text, MANIFEST)
        with self.assertRaises(ValueError):
            validate(sample().replace('direct=1', 'direct=0'), MANIFEST)

    def test_wrong_work_or_pixels_cannot_improve_timing_score(self):
        for before, after in (('frame=abcdef01', 'frame=deadbeef'), ('index=10', 'index=11'),
                              ('raw=20', 'raw=21'), ('title=New York', 'title=Paris')):
            with self.subTest(before=before):
                with self.assertRaises(ValueError):
                    validate(sample().replace(before, after, 1), MANIFEST)

    def test_empty_or_single_sample_is_not_qualification(self):
        for m in ({**MANIFEST, 'entries': []}, {**MANIFEST, 'passes': 1}):
            with self.assertRaises(ValueError):
                validate(sample(), m)

    def test_io_time_cannot_exceed_the_phase_it_is_part_of(self):
        with self.assertRaises(ValueError):
            validate(sample().replace('lookup_read_ticks=20', 'lookup_read_ticks=61', 1), MANIFEST)

    def test_comparison_keeps_each_repetition_and_phase(self):
        result = compare(sample(), sample().replace('screen_ticks=300', 'screen_ticks=600'), MANIFEST)
        screen = next(r for r in result['phases'] if r['phase'] == 'screen')
        self.assertEqual(screen['median_relative_error'], -.5)
        self.assertEqual(screen['pass_relative_errors'], [-.5, -.5, -.5])
        self.assertFalse(result['all_phase_medians_within_5_percent'])
        self.assertFalse(compare(sample(), sample(), MANIFEST)['full_workflow_qualification'])

    def test_comparison_rejects_different_work_pixels_or_clocks(self):
        for old, new in (('lookup_sectors=1', 'lookup_sectors=2'),
                         ('frame=abcdef01', 'frame=deadbeef'),
                         ('pll=00101a95', 'pll=00101a96')):
            with self.subTest(old=old):
                with self.assertRaises(ValueError):
                    compare(sample(), sample().replace(old, new), MANIFEST)
        # The read-only refresh status bit does not change the programmed timings.
        self.assertTrue(compare(sample(), sample().replace('refresh=03ff01c0', 'refresh=01ff01c0'),
                                MANIFEST)['all_phase_medians_within_5_percent'])

    def test_startup_is_healthy_and_explicitly_one_cold_boot(self):
        text = ('ZIMBOOT v1 dma_bits=32\narchive=1:/simple.zim\n'
                'open_to_keyboard_us=30 app_to_keyboard_us=40 kernel_to_app_us=50\n')
        for name in ('file', 'indexes', 'ui'):
            text += (name + ' elapsed_us=10 read_calls=1 read_sectors=1 read_us=5 '
                     'read_errors=0 timeouts=0 dma_errors=0\n')
        for i in range(4):
            text += f'KERNELBOOT seq={i} kind=1 read_errors=0 timeouts=0 dma_errors=0\n'
        text += 'dma_disabled_at_start=0 dma_disabled_at_end=0\nEND ZIMBOOT\n'
        self.assertEqual(startup(text)['cold_boot_repetitions'], 1)
        for bad in (text.replace('timeouts=0', 'timeouts=1', 1),
                    text.replace('dma_disabled_at_end=0', 'dma_disabled_at_end=1'),
                    text.replace('open_to_keyboard_us=30', 'open_to_keyboard_us=100'),
                    text.split('END ZIMBOOT')[0]):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    startup(bad)


if __name__ == '__main__':
    unittest.main()
