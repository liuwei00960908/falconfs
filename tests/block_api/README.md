# Block API Benchmark

`BlockApiBench` is a standalone benchmark for the Key/Data block API. It does not use `local_run` workloads.

Build:

```bash
cmake --build build --target BlockApiBench
```

Common environment:

```bash
source deploy/falcon_env.sh
```

By default, `BlockApiBench` reads `tests/block_api/block_api_bench.conf`. Command-line arguments override config values.

Default config values include:

```text
size=2097152
capacity=107374182400
block_data_dir=/tmp/opencode/falcon-block-data
```

Run with default config:

```bash
build/tests/block_api/BlockApiBench
```

Default output follows the `local_run` final-stat style first, so scripts can read `Throughput` as successful ops/s:

```text
[FINISH] BlockOp put, Time 0.650436, OPs 100, Throughput 153.743, Average Latency 6503310
```

It also prints a readable summary with MiB/s and percentile latency. Use `--csv` or `--csv-header` when collecting data for plotting.

The timed loop reuses one write/read buffer per worker thread. Buffer allocation and data pattern initialization are outside the measured operation latency. `OPs`, `Throughput`, `Average Latency`, and percentile latency are based on successful requests only; `attempt_ops` and `errors` are printed separately.

Use another config file:

```bash
build/tests/block_api/BlockApiBench --config /path/to/block_api_bench.conf
```

Append-only put benchmark:

```bash
build/tests/block_api/BlockApiBench --host 127.0.0.1 --port 55510 --op put --size 16777216 --keys 100 --threads 4 --csv-header
```

Read benchmark. The benchmark prepares keys before the timed section:

```bash
build/tests/block_api/BlockApiBench --host 127.0.0.1 --port 55510 --op get --size 16777216 --keys 100 --threads 4 --verify --csv-header
```

Delete benchmark. The benchmark prepares keys before the timed section:

```bash
build/tests/block_api/BlockApiBench --host 127.0.0.1 --port 55510 --op del --size 16777216 --keys 100 --threads 4 --csv-header
```

Delete keys written by a previous put run:

```bash
build/tests/block_api/BlockApiBench --op del --prefix <same-prefix> --phase put --no-prepare --size 16777216 --keys 100 --threads 4
```

Reclaim benchmark. Set capacity to `size * keys` for the tested size so the timed writes must reuse deleted slots:

```bash
build/tests/block_api/BlockApiBench --host 127.0.0.1 --port 55510 --op reclaim --size 16777216 --keys 100 --threads 4 --capacity $((16777216 * 100)) --csv-header
```

Output columns:

```text
op,size,threads,keys,attempt_ops,success_ops,total_bytes,seconds,success_ops_sec,mb_sec,avg_us,p50_us,p95_us,p99_us,errors,error_codes,prefix
```

## Block vs POSIX Baseline

Run the default comparison from `tests/block_api/block_vs_baseline.conf`:

```bash
tests/block_api/run_block_vs_baseline.sh
```

Default comparison:

```text
ops=put get del
size=2097152
threads=1 4 8
rounds=1
```

The runner is scoped to the 20GiB mode. `--preset full` is disabled because the
current Block allocator keeps size-file state in metadata; deleting only the
physical `data_<size>.dat` file would make metadata and storage inconsistent.

Outputs:

```text
/tmp/opencode/block-vs-baseline-results/block_vs_baseline.csv
/tmp/opencode/block-vs-baseline-results/block_vs_baseline.md
/tmp/opencode/block-vs-baseline-results/logs/
```

The baseline is POSIX `local_run` (`test_posix`), not Falcon's old file API. `test_falcon` currently cannot measure write/read data paths because its `dfs_write`/`dfs_read` are not implemented.

For each round/thread group, the runner writes one shared dataset and measures
selected operations against that dataset in order: `put`, `get`, `del`. It does
not remove the Block physical size file during the run; it deletes Block key
metadata with `FalconBlockDel`. POSIX files are removed from `/tmp` after each
group.
