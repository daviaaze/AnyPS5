import json
import subprocess
import sys

result = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
lines = result.stderr.splitlines()
records = []
ordinary = 0
for line in lines:
    if line.startswith("{"):
        value = json.loads(line)
        assert isinstance(value, dict)
        records.append(value)
    else:
        assert line.startswith("[capture] concurrent profiler record "), repr(line)
        ordinary += 1
assert ordinary > 0
begins = {}
ends = {}
for record in records:
    kind = record.get("type")
    if kind == "aps5_p0_snapshot":
        assert type(record.get("snapshot_id")) is int
        assert type(record.get("monotonic_ns")) is int
        if record.get("phase") == "begin":
            begins[record["snapshot_id"]] = record
        else:
            assert record.get("phase") == "end"
            assert type(record.get("row_count")) is int
            ends[record["snapshot_id"]] = record
    elif kind == "aps5_p0_table_summary":
        assert type(record.get("version")) is int and record["version"] == 1
        assert type(record.get("cumulative")) is bool and record["cumulative"] is True
        for key in ("reused", "replayed", "resolved", "writes_kept", "writes_made"):
            assert type(record.get(key)) is int and record[key] >= 0
        for key, length in (("denied_by_reason", 14), ("miss_by_reason", 8),
                            ("stale_by_reason", 4), ("current_fail_by_reason", 9),
                            ("proof_fail_by_reason", 7)):
            value = record.get(key)
            assert isinstance(value, list) and len(value) == length
            assert all(type(item) is int and item >= 0 for item in value)
    elif kind == "aps5_p0_operation":
        for key in ("count", "elapsed_ns", "thread_cpu_ns", "cpu_sample_count", "cpu_unavailable_count"):
            assert type(record.get(key)) is int and record[key] >= 0
    elif kind == "aps5_p0_guest_read":
        for key in ("count", "requested_bytes"):
            assert type(record.get(key)) is int and record[key] >= 0
    elif kind == "aps5_p0_guest_read_reason":
        for key in ("count", "requested_bytes"):
            assert type(record.get(key)) is int and record[key] >= 0
    elif kind == "aps5_p0_texture_lookup":
        assert type(record.get("count")) is int and record["count"] >= 0
    else:
        raise AssertionError(f"unexpected or incomplete JSON record: {record!r}")
assert len(begins) == 64 and len(ends) == 64
assert begins.keys() == ends.keys()
for snapshot_id, end in ends.items():
    # Reporter serializes complete snapshots under its report mutex; delimit by matching IDs.
    begin_index = next(i for i, row in enumerate(records) if row is begins[snapshot_id])
    end_index = next(i for i, row in enumerate(records) if row is end)
    body = records[begin_index + 1:end_index]
    assert len(body) == end["row_count"], (snapshot_id, len(body), end["row_count"])
    assert sum(row.get("type") == "aps5_p0_table_summary" for row in body) == 1
