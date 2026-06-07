# module: status

**Responsibility:** define `qas_status`, the single result type returned by every
fallible function in qas, and `qas_status_str` to name a status for logs/tests.

**Public interface:** `status/status.h` (`qas_status`, `qas_status_str`).

**Key invariants:** `QAS_OK == 0`; no failure code is 0; codes are distinct.
Relied on across the codebase per
[error-handling.md](../../../Quicks-Meta/docs/standards/error-handling.md).

**Dependencies:** none.
