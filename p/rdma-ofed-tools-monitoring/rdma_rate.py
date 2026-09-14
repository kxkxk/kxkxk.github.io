#!/usr/bin/env python3
"""Read-only RDMA port-rate sampler. Standard port_*_data units are 4 octets.

Outputs device-reported counter rates, not per-process/application throughput.
Missing optional hardware counters are NA; decreasing counters reset baseline.
"""

import argparse
import csv
from datetime import datetime, timezone
import math
from pathlib import Path
import sys
import time


DATA_COUNTERS = ("port_xmit_data", "port_rcv_data")
HW_COUNTERS = (
    "np_ecn_marked_roce_packets",
    "np_cnp_sent",
    "rp_cnp_handled",
    "rp_cnp_ignored",
    "local_ack_timeout_err",
    "rnr_nak_retry_err",
    "out_of_buffer",
)


def read_value(path, required=False):
    try:
        value = int(path.read_text().strip())
        if value < 0:
            raise ValueError("negative counter")
        return value
    except (OSError, ValueError) as exc:
        if required:
            raise RuntimeError("Cannot read {}: {}".format(path, exc)) from exc
        return None


def snapshot(base):
    before = time.monotonic()
    values = {name: read_value(base / "counters" / name, required=True)
              for name in DATA_COUNTERS}
    values.update({name: read_value(base / "hw_counters" / name)
                   for name in HW_COUNTERS})
    after = time.monotonic()
    return (before + after) / 2.0, values


def counter_delta(old, new):
    if old is None or new is None:
        return None, "missing"
    if new < old:
        return None, "reset"
    return new - old, "ok"


def make_row(previous, current, timestamp):
    old_time, old_values = previous
    new_time, new_values = current
    elapsed = new_time - old_time
    if elapsed <= 0:
        raise RuntimeError("Non-positive monotonic sampling interval")
    fields = [timestamp, "{:.6f}".format(elapsed)]
    flags = []
    for name in DATA_COUNTERS + HW_COUNTERS:
        delta, status = counter_delta(old_values[name], new_values[name])
        if status != "ok":
            fields.append("NA")
            flags.append(status + ":" + name)
        elif name in DATA_COUNTERS:
            fields.append("{:.6f}".format(delta * 4 * 8 / elapsed / 1e9))
        else:
            fields.append(delta)
    fields.append(";".join(flags) if flags else "ok")
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="RDMA device, e.g. mlx5_0")
    parser.add_argument("--port", type=int, default=1)
    parser.add_argument("--interval", type=float, default=1.0, help="Seconds")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--sysfs-root", type=Path,
                        default=Path("/sys/class/infiniband"),
                        help="InfiniBand sysfs root; may point to a test fixture")
    args = parser.parse_args()
    if (not args.device or args.device in (".", "..") or "/" in args.device
            or args.port < 1 or args.samples < 1
            or not math.isfinite(args.interval) or args.interval <= 0):
        parser.error("Use a device name, positive port, sample count and interval")

    base = args.sysfs_root / args.device / "ports" / str(args.port)
    previous = snapshot(base)
    writer = csv.writer(sys.stdout)
    writer.writerow(["timestamp_utc", "interval_s", "tx_Gbit_s", "rx_Gbit_s"]
                    + [name + "_delta" for name in HW_COUNTERS] + ["status"])
    sys.stdout.flush()
    for _ in range(args.samples):
        time.sleep(args.interval)
        current = snapshot(base)
        stamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        writer.writerow(make_row(previous, current, stamp))
        sys.stdout.flush()
        previous = current


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(2)
