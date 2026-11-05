# Frozen forecasts from 2026-11-05

These are byte-for-byte copies of predictions saved before physical portq
and fetchq collection. The original JSON timestamps, classification, paths,
model settings and hashes are unchanged. They are historical forecasts,
not newly fitted models or claims of qualification. `index.json` records
original paths and SHA-256 hashes; the corresponding raw hardware reports
are versioned in `../../../port_bench` and `../../../fetch_bench`.

The two port forecasts include their exact frontend and generator snapshots.
The fetch forecast includes all five models' raw samples and input hashes;
`fetch-model-0.txt` through `fetch-model-4.txt` preserve their original guest
reports. Executables, card images and complete build runs remain local under
`build/wr128`; the strict fetch analyzer also checks those original binaries.
To reproduce the port comparison, restore the six `port-prospective*` files
to the `build/wr128/frontend-lab` paths recorded in the index. No rerun or
refit of the declarative models is needed.

Do not edit these artifacts to match later implementations or hardware.
The current scripts and controller may differ from the frozen source/input
identities. The returned probes did not qualify a replacement timing model.
