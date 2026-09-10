# Journal recovery (storage, not consensus)

The node must have one writer per data directory; concurrent processes or external
modification while open are not supported. `Load()` is read-only. `Open()` and the
first append after a reported write failure must complete recovery before writes.

Complete prepare/commit pairs are retained in commit order. An incomplete final
length, payload or commit, or complete uncommitted prepare records, require repair.
Unknown tags, a complete zero/oversized length, invalid serialized blocks, unmatched
commits, duplicate pending prepares and I/O errors fail closed. Recovery does not
scan for plausible records beyond the first torn boundary or salvage their blocks.

Repair copies the original bytes to a new exclusive `blocks.dat.recovery-N` file,
syncs that copy and its directory, writes normalized committed pairs to an exclusive
`.tmp` file, flushes and syncs the file, then atomically replaces `blocks.dat` and
syncs the parent directory. Windows uses `MoveFileExW` with write-through. Open
syncs the active file/directory before authorizing writes, including after a prior
interruption just after replacement. A crash exposes either the old recoverable
journal or the new committed journal, never a partially rewritten active file.

Quarantine and incomplete temporary files are never overwritten or automatically
deleted. At most 1024 suffix slots are tried; exhausted slots, disk-full, sync or
replacement failures prevent opening/writing. Operators must archive preserved
files privately before freeing space. Recovery uses the existing bounded block
parser and its in-memory replay list; rebuilding costs one pass over committed
blocks and extra disk for the original and replacement. No extra replay pass is
added to ordinary successful appends.

This does not establish disk hardware guarantees, multi-writer locking or independent
approval. It does not change consensus serialization, network parameters or genesis.
The previously faulty staging directory must remain preserved; only a disposable
copy may be repaired for testing. Blocks beyond its torn boundary were not valid
journal commits and must not silently be treated as recovered chain state.
