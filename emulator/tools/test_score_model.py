#!/usr/bin/env python3
"""Protect calibration against partial, failed or mismatched workloads."""
import math
import unittest

import score_model as score


class Scores(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.spi = (score.ROOT / 'emulator/tools/spi_dma_bench/device-2026-11-05.txt').read_text()
        cls.mem = (score.ROOT / 'emulator/tools/mem_dma_bench/device-2026-10-02.txt').read_text()
        cls.ub = (score.UBTOOLS / 'ubench-device.txt').read_text()
        cls.legacy_rv = {v: (score.ROOT / f'riscv/{stem}-device.txt').read_text()
                         for v, stem in score.VARIANTS.items()}
        # A synthetic current-format report exercises parsing without
        # presenting the historical device tables as valid calibration.
        console = '\nrv32: console output: 0 cycles, left out\n'
        console += '\n'.join(f'{k} 0x00000000' for k in score.KERNELS) + '\n'
        cls.rv = {v: text + console for v, text in cls.legacy_rv.items()}
        cls.pipeline = (score.ROOT / 'emulator/tools/pipeline_bench/device-2026-11-05.txt').read_text()

    def test_identical_reports(self):
        for fn, report in ((score.score_spi, self.spi), (score.score_mem, self.mem),
                           (score.score_ubench, self.ub), (score.score_riscv, self.rv),
                           (score.score_pipeline, self.pipeline)):
            with self.subTest(suite=fn.__name__):
                metrics = fn(report, report)
                self.assertGreater(metrics.pop('cases'), 0)
                self.assertTrue(all(v == 0 for v in metrics.values()))

    def test_missing_and_duplicate_rows(self):
        for fn, report, prefix in ((score.score_spi, self.spi, 'RESULT '),
                                    (score.score_mem, self.mem, 'RESULT '),
                                    (score.score_ubench, self.ub, 'UB '),
                                    (score.score_pipeline, self.pipeline, 'RESULT ')):
            row = next(line for line in report.splitlines() if line.startswith(prefix))
            for bad in (report.replace(row + '\n', '', 1), report + '\n' + row):
                with self.subTest(suite=fn.__name__), self.assertRaises(ValueError):
                    fn(bad, report)
        bad = dict(self.rv)
        row = next(line for line in bad['asm'].splitlines() if line.startswith('rv32: alu '))
        bad['asm'] = bad['asm'].replace(row, '')
        with self.assertRaises(ValueError):
            score.score_riscv(bad, self.rv)

    def test_guest_failures_and_truncation(self):
        for report in (self.spi.replace('ok=1', 'ok=0', 1),
                       self.spi.replace('END SPIBENCH', 'TRUNCATED')):
            with self.assertRaises(ValueError):
                score.score_spi(report, self.spi)
        for report in (self.mem.replace('verified=1', 'verified=0', 1),
                       self.mem.replace('state_restored=1', 'state_restored=0')):
            with self.assertRaises(ValueError):
                score.score_mem(report, self.mem)

    def test_invalid_times(self):
        for invalid in (0, -1, math.nan, math.inf):
            with self.subTest(value=invalid), self.assertRaises(ValueError):
                score.log_score({'loop': invalid}, {'loop': 1})

    def test_historical_riscv_is_not_calibration(self):
        with self.assertRaises(ValueError):
            score.score_riscv(self.legacy_rv, self.legacy_rv)

    def test_changed_ubench_body_is_not_calibration(self):
        changed = self.ub.replace('26    11', '28    12', 1)
        self.assertNotEqual(changed, self.ub)
        with self.assertRaises(ValueError):
            score.score_ubench(changed, self.ub)

    def test_pipeline_workload_identity(self):
        for bad in (self.pipeline.replace('ok=1', 'ok=0', 1),
                    self.pipeline.replace('passes=20000 ticks=', 'passes=19999 ticks=', 1),
                    self.pipeline.replace('END PIPELINE', 'TRUNCATED')):
            with self.assertRaises(ValueError):
                score.score_pipeline(bad, self.pipeline)

    def test_log_error_is_symmetric(self):
        self.assertAlmostEqual(score.log_score({'a': 2}, {'a': 1}),
                               score.log_score({'a': 1}, {'a': 2}))

    def test_spi_loss_is_separate_from_solo(self):
        # Delay solo and concurrent CPU work equally: solo error changes,
        # contention error does not. Transfer time remains independent.
        report = ('RESULT tx=hsdma width=32 dst=sdram cpu=load1 '
                  'solo_pass_x100=2000 pass_x100=2400 unit_x100=13600 ok=1\n'
                  'END SPIBENCH cases=1 failures=0\n')
        slower = report.replace('=2000', '=2200').replace('=2400', '=2600')
        result = score.score_spi(slower, report)
        self.assertEqual(result['solo_cycles_rms'], 2)
        self.assertEqual(result['dma_loss_cycles_rms'], 0)
        self.assertEqual(result['transfer_log_rms'], 0)


if __name__ == '__main__':
    unittest.main()
