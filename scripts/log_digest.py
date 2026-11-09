#!/usr/bin/env python3
"""A short digest of an Android run log: what went wrong, without reading the whole log. Runs dm/tools/log_digest.py (its options: -h).

    python3 scripts/log_digest.py <run.log | folder of logs | bug-report .zip> [--logcat FILE] [--exit FILE] [--json]
    python3 scripts/log_digest.py --filter <run.log> [--context N] [--max-lines N]     (the log without its routine lines, with line numbers)

Exit code: 0 (also for a log with problems), 2 the input cannot be read."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import log_digest  # noqa: E402

if __name__ == '__main__':
    sys.exit(log_digest.main())
