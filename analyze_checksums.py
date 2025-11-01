#!/usr/bin/env python3
import re
from collections import defaultdict

# Parse the trace file
writes = {}  # resource -> (timestamp, checksum)
reads = []   # (timestamp, checksum)
copies = []  # (timestamp, src_resource, dst_resource)

with open('trace_with_checksums.md', 'r') as f:
    for line in f:
        if line.startswith('#'):
            continue

        # Parse Unmap with checksum (WRITE completion)
        match = re.search(r'\[(\d+)\] Unmap res=(0x\w+) sub=\d+ checksum=(0x\w+)', line)
        if match:
            timestamp, resource, checksum = match.groups()
            writes[resource] = (int(timestamp), checksum)

        # Parse Map READ with checksum
        match = re.search(r'\[(\d+)\] Map type=READ res=(0x\w+) .* checksum=(0x\w+)', line)
        if match:
            timestamp, resource, checksum = match.groups()
            reads.append((int(timestamp), resource, checksum))

        # Parse CopySubresourceRegion
        match = re.search(r'\[(\d+)\] CopySubresourceRegion src=(0x\w+) dst=(0x\w+)', line)
        if match:
            timestamp, src, dst = match.groups()
            copies.append((int(timestamp), src, dst))

print("=" * 80)
print("CHECKSUM ANALYSIS")
print("=" * 80)

# Check if reads match the most recent write
print("\nChecking if READ checksums match WRITE checksums:")
print("-" * 80)

matches = 0
mismatches = 0
for read_time, read_res, read_checksum in reads:
    # Find the most recent copy operation before this read
    recent_copy = None
    for copy_time, src, dst in copies:
        if copy_time < read_time and dst == read_res:
            if recent_copy is None or copy_time > recent_copy[0]:
                recent_copy = (copy_time, src, dst)

    if recent_copy:
        src_res = recent_copy[1]
        # Find the write to that source resource
        if src_res in writes:
            write_time, write_checksum = writes[src_res]
            if write_checksum == read_checksum:
                matches += 1
                print(f"✓ MATCH: Write {src_res} -> Read {read_res}: {write_checksum}")
            else:
                mismatches += 1
                print(f"✗ MISMATCH: Write {src_res}={write_checksum} vs Read {read_res}={read_checksum}")

print(f"\nMatches: {matches}")
print(f"Mismatches: {mismatches}")

# Look for repeated checksums (unchanging data)
print("\n" + "=" * 80)
print("REPEATED CHECKSUMS (Potentially redundant reads)")
print("=" * 80)

checksum_counts = defaultdict(int)
for _, _, checksum in reads:
    checksum_counts[checksum] += 1

print(f"\nTotal unique checksums: {len(checksum_counts)}")
print(f"Total READ operations: {len(reads)}")
print("\nMost frequently read checksums:")
for checksum, count in sorted(checksum_counts.items(), key=lambda x: -x[1])[:10]:
    print(f"  {checksum}: {count} times")

# Analyze timing patterns
print("\n" + "=" * 80)
print("TIMING ANALYSIS")
print("=" * 80)

read_intervals = []
for i in range(1, len(reads)):
    interval = reads[i][0] - reads[i-1][0]
    read_intervals.append(interval)

if read_intervals:
    avg_interval = sum(read_intervals) / len(read_intervals)
    print(f"\nAverage time between READs: {avg_interval:.0f} microseconds ({avg_interval/1000:.2f} ms)")
    print(f"Min interval: {min(read_intervals)} µs")
    print(f"Max interval: {max(read_intervals)} µs")
