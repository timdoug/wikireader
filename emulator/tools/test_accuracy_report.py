import unittest
from accuracy_report import summarize


class Accuracy(unittest.TestCase):
    def test_large_miss_survives_a_good_median(self):
        result = summarize([(str(i), 1, 1) for i in range(9)] + [('large', 2, 1)])
        self.assertEqual(result['median_absolute_relative_error'], 0)
        self.assertEqual(result['p90_absolute_relative_error'], 0)
        self.assertTrue(result['representative_threshold_met'])
        self.assertEqual([r['case'] for r in result['unexplained_over_20_percent']], ['large'])

    def test_threshold_counts_cases(self):
        result = summarize([(str(i), 1.1, 1) for i in range(8)] + [('a', 1.11, 1), ('b', 1.3, 1)])
        self.assertEqual(result['within_10_percent'], 8)
        self.assertFalse(result['representative_threshold_met'])
        self.assertAlmostEqual(result['p90_absolute_relative_error'], .11)

    def test_signed_error_and_plain_relative_percent(self):
        result = summarize([('fast', .5, 1), ('slow', 1.5, 1)])
        self.assertEqual(result['median_absolute_relative_error'], .5)
        self.assertEqual([r['relative_error'] for r in result['case_errors']], [-.5, .5])

    def test_empty_and_nonpositive_are_not_qualified(self):
        for rows in ([], [('bad', 0, 1)], [('bad', 1, float('nan'))]):
            with self.assertRaises(ValueError):
                summarize(rows)


if __name__ == '__main__':
    unittest.main()
