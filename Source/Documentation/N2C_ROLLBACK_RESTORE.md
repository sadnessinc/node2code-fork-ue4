# Rollback and deferred restore — Version 173

## Normal apply

1. strict dry semantic gate;
2. save dirty current Blueprint;
3. copy the current package to a pre-apply backup;
4. build a stable structural snapshot/hash;
5. enter `FScopedTransaction`, call `Modify()` on Blueprint/package and apply;
6. compile and save;
7. commit only after success.

## Failure after mutation

The transaction is allowed to close. The importer then requests a real Editor Undo, recompiles and builds a second structural snapshot. Rollback is reported as successful only when the post-Undo hash equals the pre-apply hash.

`FScopedTransaction::Cancel()` is deliberately not used as rollback because it can discard the transaction record while leaving mutated UObject state.

## Fallback

When Undo or structural equality cannot be verified, the importer copies the pre-apply backup into a pending folder and writes a `.restore` manifest. The user is told not to save the asset. On the next Editor startup, plugin initialization copies the pending backup over the target package, rescans Asset Registry state and moves the manifest to an applied `.done` record.

## Automation proof

- `RollbackAfterMutation` forces failure only after a real member-variable mutation and requires the mutation to disappear plus a rollback PASS marker.
- `RollbackStructuralEquality` additionally requires identical before/after snapshot hashes.
- Restore first pass writes a physically different target `.uasset` and queues the baseline.
- Restore second pass runs in a new UE process and requires exact target/baseline byte equality, successful Blueprint compile and absence of the mutation.

All fault injection and modal-bypass entry points are compiled only under `WITH_DEV_AUTOMATION_TESTS`.

## Version 183 startup reporting

Queued restore processing now runs immediately and retries once after editor startup. Interactive sessions receive a delayed modal summary, so the result cannot disappear before Slate is ready. The latest persistent status is written to `Saved/NodeToCode/Backups/PendingRestoreApplied/N2C_LAST_PENDING_RESTORE_STATUS.txt`. Commandlets do not open UI; they log the same summary. A remaining manifest keeps the asset locked.
