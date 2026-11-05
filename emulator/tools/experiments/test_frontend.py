"""Timing invariants for the offline frontend hypothesis, not guest execution."""
import unittest

from frontend import simulate, BODIES


class Frontend(unittest.TestCase):
    def test_idle_data_port_adds_no_dispatch_delay(self):
        # Eight transfers, one decrement, three-cycle branch, one fetch
        # restart. Servicing a request only on the next quarter tick made
        # every result two MCLK too long, regardless of the read latency.
        for latency in (4, 8, 12):
            for offset in (0, 2):
                with self.subTest(latency=latency, offset=offset):
                    self.assertEqual(simulate(['ldh'], shared=False, offset=offset,
                                              services=(latency, latency, 16), passes=40),
                                     8 * latency / 4 + 5)

    def test_result_is_independent_of_warmup_length(self):
        for body in (['ext', 'alu'], ['ldh', 'alu'], ['ext', 'ldw'], BODIES['mixed']):
            for shared in (True, False):
                for trailing in (False, True):
                    self.assertEqual(simulate(body, passes=40, shared=shared, trailing_ext=trailing),
                                     simulate(body, passes=80, shared=shared, trailing_ext=trailing))

    def test_variable_unroll_counts_each_access(self):
        for unroll in (1, 2, 3, 4, 8, 12, 16):
            for offset in (0, 2):
                with self.subTest(unroll=unroll, offset=offset):
                    self.assertEqual(simulate(['ldh'], shared=False, offset=offset,
                                              services=(12, 12, 16), passes=8, unroll=unroll),
                                     unroll * 3 + 5)

    def test_lookahead_preserves_operation_and_extension_sequence(self):
        for body in BODIES.values():
            for offset in (0, 2):
                for shared in (False, True):
                    traces = []
                    for trailing in (False, True):
                        events = []
                        simulate(body, offset=offset, shared=shared, passes=4,
                                 trailing_ext=trailing, events=events)
                        traces.append([(e['loop'], e['kind'], e['address'], e['prefixes']) for e in events])
                    self.assertEqual(traces[0], traces[1], (body, offset, shared))

    def test_queue_refill_does_not_add_a_numerical_tick(self):
        # Every modeled delay is a half-clock multiple. Refining the
        # simulation from half to quarter clocks must change no event.
        for name, body in BODIES.items():
            for offset in (0, 2):
                for gate in (False, True):
                    outputs = []
                    for quantum in (1, 2):
                        events, ports = [], []
                        cost = simulate(body, offset=offset, passes=8, port_turns=(2, 2),
                                        trailing_ext=True, prefix_gate=gate, quantum=quantum,
                                        events=events, port_events=ports)
                        outputs.append((cost, events, ports))
                    self.assertEqual(outputs[0], outputs[1], (name, offset, gate))

    def test_port_ownership_and_cpu_clock_edges(self):
        for body in (['ldh'], ['ldh', 'alu'], ['ext', 'ldw'], BODIES['mixed']):
            for turns in ((0, 0), (2, 2), (4, 0), (0, 6)):
                events, ports = [], []
                simulate(body, passes=8, port_turns=turns, response_delay=2,
                         stage_edge=4, trailing_ext=True, prefix_gate=True,
                         events=events, port_events=ports)
                self.assertTrue(all(e['quarter'] % 4 == 0 for e in events))
                for previous, current in zip(ports, ports[1:]):
                    turnaround = 0 if previous['owner'] == current['owner'] else turns[
                        previous['owner'] == 'data']
                    self.assertGreaterEqual(current['quarter'], previous['end'] + turnaround)

    def test_shared_port_rules_leave_separate_data_port_alone(self):
        for name, body in BODIES.items():
            for offset in (0, 2):
                base = dict(offset=offset, passes=8, shared=False, services=(8, 8, 16), trailing_ext=True)
                self.assertEqual(simulate(body, **base),
                                 simulate(body, port_turns=(2, 2), stage_edge=4,
                                          response_delay=2, prefix_gate=True, **base), name)

    def test_invalid_physical_delays_are_rejected(self):
        for config in ({'port_turns': (-1, 2)}, {'stage_edge': 3},
                       {'response_delay': 4}, {'quantum': 2}):
            with self.assertRaises(ValueError):
                simulate(['ldh'], **config)


if __name__ == '__main__':
    unittest.main()
