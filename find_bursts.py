#!/usr/bin/env python3
"""
Find all flush bursts in the log (menu opens).
"""

import re
from collections import Counter

# Parse log and extract flush timestamps
flushes = []
with open('atfix.log', 'r') as f:
    for line in f:
        if ' Flush #' in line:
            ts_match = re.search(r'\[([0-9.]+)s\]', line)
            if ts_match:
                flushes.append(float(ts_match.group(1)))

print(f"Total flushes: {len(flushes)}")

# Find bursts: sliding window of 10 flushes
# If 10 flushes occur within 5 seconds, it's a burst
bursts = []
i = 0
while i < len(flushes) - 10:
    window_start = flushes[i]
    window_end = flushes[i + 9]
    window_duration = window_end - window_start

    # If 10 flushes in < 5 seconds, it's a burst
    if window_duration < 5.0:
        # Find the full extent of this burst
        burst_start = i
        burst_end = i + 9

        # Extend burst_end while flushes are close together
        while burst_end < len(flushes) - 1:
            gap = flushes[burst_end + 1] - flushes[burst_end]
            if gap < 0.1:  # Less than 100ms gap
                burst_end += 1
            else:
                break

        burst_duration = (flushes[burst_end] - flushes[burst_start]) * 1000
        burst_count = burst_end - burst_start + 1

        # Only count bursts with significant flush count
        if burst_count > 30:
            bursts.append((flushes[burst_start], flushes[burst_end], burst_count))

            print(f"\nBurst at {flushes[burst_start]:.2f}s:")
            print(f"  Duration:     {burst_duration:.0f}ms")
            print(f"  Flush count:  {burst_count}")

            # Skip past this burst
            i = burst_end + 1
        else:
            i += 1
    else:
        i += 1

print(f"\nTotal bursts detected: {len(bursts)}")
