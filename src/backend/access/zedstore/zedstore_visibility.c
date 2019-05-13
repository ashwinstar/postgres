/*
 * zedstore_visibility.c
 *		Routines for MVCC in Zedstore
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstore_visibility.c
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/xact.h"
#include "access/zedstore_internal.h"
#include "access/zedstore_undo.h"
#include "storage/procarray.h"

static bool
zs_tuplelock_compatible(LockTupleMode mode, LockTupleMode newmode)
{
	switch (newmode)
	{
		case LockTupleKeyShare:
			return mode == LockTupleKeyShare ||
				mode == LockTupleShare ||
				mode == LockTupleNoKeyExclusive;

		case LockTupleShare:
			return mode == LockTupleKeyShare ||
				mode == LockTupleShare;

		case LockTupleNoKeyExclusive:
			return mode == LockTupleKeyShare;
		case LockTupleExclusive:
			return false;

		default:
			elog(ERROR, "unknown tuple lock mode %d", newmode);
	}
}

/*
 * Like HeapTupleSatisfiesUpdate.
 *
 * When returns TM_Ok, this also returns a flag in *undo_record_needed, to indicate
 * whether the old UNDO record is still of interest to anyone. If the old record
 * belonged to an aborted deleting transaction, for example, it can be ignored.
 *
 * This does more than HeapTupleSatisfiesUpdate. If HeapTupleSatisfiesUpdate sees
 * an updated or locked tuple, it returns TM_BeingUpdated, and the caller has to
 * check if the tuple lock is compatible with the update. zs_SatisfiesUpdate
 * checks if the new lock mode is compatible with the old one, and returns TM_Ok
 * if so. Waiting for conflicting locks is left to the caller.
 *
 * If the tuple was UPDATEd, *next_tid is set to the TID of the new row version.
 */
TM_Result
zs_SatisfiesUpdate(Relation rel, Snapshot snapshot,
				   ZSUndoRecPtr recent_oldest_undo, ZSBtreeItem *item,
				   LockTupleMode mode,
				   bool *undo_record_needed, TM_FailureData *tmfd, zstid *next_tid)
{
	ZSUndoRecPtr undo_ptr;
	bool		is_deleted;
	ZSUndoRec  *undorec;
	int			chain_depth = 0;

	Assert((item->t_flags & ZSBT_COMPRESSED) == 0);

	*undo_record_needed = true;

	is_deleted = (item->t_flags & (ZSBT_UPDATED | ZSBT_DELETED)) != 0;
	undo_ptr = zsbt_item_undoptr(item);

fetch_undo_record:
	chain_depth++;

	/* Is it visible? */
	if (undo_ptr.counter < recent_oldest_undo.counter)
	{
		if (is_deleted)
		{
			/* this probably shouldn't happen.. */
			return TM_Invisible;
		}
		else
		{
			/*
			 * the old UNDO record is no longer visible to anyone, so we don't
			 * need to keep it.
			 */
			if (chain_depth == 1)
				*undo_record_needed = false;
			return TM_Ok;
		}
	}

	/* have to fetch the UNDO record */
	undorec = zsundo_fetch(rel, undo_ptr);

	if (!is_deleted)
	{
		/* Inserted tuple */
		if (undorec->type == ZSUNDO_TYPE_INSERT)
		{
			if (TransactionIdIsCurrentTransactionId(undorec->xid))
			{
				if (undorec->cid >= snapshot->curcid)
					return TM_Invisible;	/* inserted after scan started */
				*undo_record_needed = true;
				return TM_Ok;
			}

			if (TransactionIdIsInProgress(undorec->xid))
				return TM_Invisible;		/* inserter has not committed yet */

			if (TransactionIdDidCommit(undorec->xid))
			{
				*undo_record_needed = true;
				return TM_Ok;
			}

			/* it must have aborted or crashed */
			return TM_Invisible;
		}
		else if (undorec->type == ZSUNDO_TYPE_TUPLE_LOCK)
		{
			ZSUndoRec_TupleLock *lock_undorec = (ZSUndoRec_TupleLock *) undorec;

			/*
			 * If any subtransaction of the current top transaction already holds
			 * a lock as strong as or stronger than what we're requesting, we
			 * effectively hold the desired lock already.  We *must* succeed
			 * without trying to take the tuple lock, else we will deadlock
			 * against anyone wanting to acquire a stronger lock.
			 */
			if (TransactionIdIsCurrentTransactionId(undorec->xid))
			{
				if (lock_undorec->lockmode >= mode)
				{
					*undo_record_needed = true;
					return TM_Ok;
				}
			}
			else if (!zs_tuplelock_compatible(lock_undorec->lockmode, mode) &&
					 TransactionIdIsInProgress(undorec->xid))
			{
				tmfd->ctid = ItemPointerFromZSTid(item->t_tid);
				tmfd->xmax = undorec->xid;
				tmfd->cmax = InvalidCommandId;
				return TM_BeingModified;
			}

			/*
			 * No conflict with this lock. Look at the previous UNDO record, there
			 * might be more locks.
			 *
			 * FIXME: Shouldn't we drill down to the INSERT record and check if
			 * that's visible to us first, before looking at the lockers?
			 */
			undo_ptr = ((ZSUndoRec_TupleLock *) undorec)->prevundorec;
			goto fetch_undo_record;
		}
		else
			elog(ERROR, "unexpected UNDO record type: %d", undorec->type);
	}
	else
	{
		LockTupleMode old_lockmode;

		/* deleted or updated-away tuple */
		Assert(undorec->type == ZSUNDO_TYPE_DELETE ||
			   undorec->type == ZSUNDO_TYPE_UPDATE);

		if (undorec->type == ZSUNDO_TYPE_DELETE)
		{
			old_lockmode = LockTupleExclusive;
		}
		else if (undorec->type == ZSUNDO_TYPE_UPDATE)
		{
			ZSUndoRec_Update *updaterec = (ZSUndoRec_Update *) undorec;

			*next_tid = updaterec->newtid;
			old_lockmode = updaterec->key_update ? LockTupleExclusive : LockTupleNoKeyExclusive;
		}
		else
			elog(ERROR, "unexpected UNDO record type for updated/deleted item: %d", undorec->type);

		if (TransactionIdIsCurrentTransactionId(undorec->xid))
		{
			if (zs_tuplelock_compatible(old_lockmode, mode))
				return TM_Ok;

			if (undorec->cid >= snapshot->curcid)
			{
				tmfd->ctid = ItemPointerFromZSTid(item->t_tid);
				tmfd->xmax = undorec->xid;
				tmfd->cmax = undorec->cid;
				return TM_SelfModified;	/* deleted/updated after scan started */
			}
			else
				return TM_Invisible;	/* deleted before scan started */
		}

		if (TransactionIdIsInProgress(undorec->xid))
		{
			if (zs_tuplelock_compatible(old_lockmode, mode))
				return TM_Ok;

			tmfd->ctid = ItemPointerFromZSTid(item->t_tid);
			tmfd->xmax = undorec->xid;
			tmfd->cmax = InvalidCommandId;

			return TM_BeingModified;
		}

		if (!TransactionIdDidCommit(undorec->xid))
		{
			/* deleter must have aborted or crashed */
			*undo_record_needed = false;
			return TM_Ok;
		}

		if (undorec->type == ZSUNDO_TYPE_DELETE)
		{
			tmfd->ctid = ItemPointerFromZSTid(item->t_tid);
			tmfd->xmax = undorec->xid;
			tmfd->cmax = InvalidCommandId;
			return TM_Deleted;
		}
		else
		{
			if (zs_tuplelock_compatible(old_lockmode, mode))
				return TM_Ok;

			tmfd->ctid = ItemPointerFromZSTid(((ZSUndoRec_Update *) undorec)->newtid);
			tmfd->xmax = undorec->xid;
			tmfd->cmax = InvalidCommandId;
			return TM_Updated;
		}
	}
}


/*
 * Like HeapTupleSatisfiesAny
 */
static bool
zs_SatisfiesAny(ZSBtreeScan *scan, ZSBtreeItem *item)
{
	return true;
}

/*
 * helper function to zs_SatisfiesMVCC(), to check if the given XID
 * is visible to the snapshot.
 */
static bool
xid_is_visible(Snapshot snapshot, TransactionId xid, CommandId cid, bool *aborted)
{
	*aborted = false;
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		if (cid >= snapshot->curcid)
			return false;
		else
			return true;
	}
	else if (XidInMVCCSnapshot(xid, snapshot))
		return false;
	else if (TransactionIdDidCommit(xid))
	{
		return true;
	}
	else
	{
		/* it must have aborted or crashed */
		*aborted = true;
		return false;
	}
}

/*
 * Like HeapTupleSatisfiesMVCC
 */
static bool
zs_SatisfiesMVCC(ZSBtreeScan *scan, ZSBtreeItem *item, TransactionId *obsoleting_xid)
{
	Relation	rel = scan->rel;
	Snapshot	snapshot = scan->snapshot;
	ZSUndoRecPtr recent_oldest_undo = scan->recent_oldest_undo;
	ZSUndoRecPtr undo_ptr;
	ZSUndoRec  *undorec;
	bool		is_deleted;
	bool		aborted;

	Assert((item->t_flags & ZSBT_COMPRESSED) == 0);
	Assert (snapshot->snapshot_type == SNAPSHOT_MVCC);

	is_deleted = (item->t_flags & (ZSBT_UPDATED | ZSBT_DELETED)) != 0;
	undo_ptr = zsbt_item_undoptr(item);

fetch_undo_record:
	if (undo_ptr.counter < recent_oldest_undo.counter)
	{
		if (!is_deleted)
			return true;
		else
			return false;
	}

	/* have to fetch the UNDO record */
	undorec = zsundo_fetch(rel, undo_ptr);

	if (!is_deleted)
	{
		/* Inserted tuple */
		if (undorec->type == ZSUNDO_TYPE_INSERT)
		{
			bool		result;

			result = xid_is_visible(snapshot, undorec->xid, undorec->cid, &aborted);
			if (!result && !aborted)
				*obsoleting_xid = undorec->xid;
			return result;
		}
		else if (undorec->type == ZSUNDO_TYPE_TUPLE_LOCK)
		{
			/* we don't care about tuple locks here. Follow the link to the
			 * previous UNDO record for this tuple. */
			undo_ptr = ((ZSUndoRec_TupleLock *) undorec)->prevundorec;
			goto fetch_undo_record;
		}
		else
			elog(ERROR, "unexpected UNDO record type: %d", undorec->type);
	}
	else
	{
		/* deleted or updated-away tuple */
		Assert(undorec->type == ZSUNDO_TYPE_DELETE ||
			   undorec->type == ZSUNDO_TYPE_UPDATE);

		if (xid_is_visible(snapshot, undorec->xid, undorec->cid, &aborted))
		{
			/* we can see the deletion */
			return false;
		}
		else
		{
			/*
			 * The deleting XID is not visible to us. But before concluding
			 * that the tuple is visible, we have to check if the inserting
			 * XID is visible to us.
			 */
			ZSUndoRecPtr	prevptr;

			if (!aborted)
				*obsoleting_xid = undorec->xid;

			do {
				if (undorec->type == ZSUNDO_TYPE_DELETE)
					prevptr = ((ZSUndoRec_Delete *) undorec)->prevundorec;
				else if (undorec->type == ZSUNDO_TYPE_UPDATE)
					prevptr = ((ZSUndoRec_Update *) undorec)->prevundorec;
				else if (undorec->type == ZSUNDO_TYPE_TUPLE_LOCK)
					prevptr = ((ZSUndoRec_TupleLock *) undorec)->prevundorec;
				else
					elog(ERROR, "unexpected UNDO record type: %d", undorec->type);

				if (prevptr.counter < recent_oldest_undo.counter)
					return true;

				undorec = zsundo_fetch(rel, prevptr);
			} while(undorec->type == ZSUNDO_TYPE_TUPLE_LOCK);

			Assert(undorec->type == ZSUNDO_TYPE_INSERT);
			if (xid_is_visible(snapshot, undorec->xid, undorec->cid, &aborted))
				return true;	/* we can see the insert, but not the delete */
			else
			{
				if (!aborted)
					*obsoleting_xid = undorec->xid;
				return false;	/* we cannot see the insert */
			}
		}
	}
}

/*
 * Like HeapTupleSatisfiesSelf
 */
static bool
zs_SatisfiesSelf(ZSBtreeScan *scan, ZSBtreeItem *item)
{
	Relation	rel = scan->rel;
	ZSUndoRecPtr recent_oldest_undo = scan->recent_oldest_undo;
	ZSUndoRec  *undorec;
	bool		is_deleted;
	ZSUndoRecPtr undoptr = zsbt_item_undoptr(item);

	Assert((item->t_flags & ZSBT_COMPRESSED) == 0);
	Assert (scan->snapshot->snapshot_type == SNAPSHOT_SELF);

	is_deleted = (item->t_flags & (ZSBT_UPDATED | ZSBT_DELETED)) != 0;

	if (undoptr.counter < recent_oldest_undo.counter)
	{
		if (!is_deleted)
			return true;
		else
			return false;
	}

	/* have to fetch the UNDO record */
	undorec = zsundo_fetch(rel, undoptr);

	if (!is_deleted)
	{
		/* Inserted tuple */
		Assert(undorec->type == ZSUNDO_TYPE_INSERT);

		if (TransactionIdIsCurrentTransactionId(undorec->xid))
			return true;		/* inserted by me */
		else if (TransactionIdIsInProgress(undorec->xid))
			return false;
		else if (TransactionIdDidCommit(undorec->xid))
			return true;
		else
		{
			/* it must have aborted or crashed */
			return false;
		}
	}
	else
	{
		/* deleted or updated-away tuple */
		Assert(undorec->type == ZSUNDO_TYPE_DELETE ||
			   undorec->type == ZSUNDO_TYPE_UPDATE);

		if (TransactionIdIsCurrentTransactionId(undorec->xid))
		{
			/* deleted by me */
			return false;
		}

		if (TransactionIdIsInProgress(undorec->xid))
			return true;

		if (!TransactionIdDidCommit(undorec->xid))
		{
			/* deleter aborted or crashed */
			return true;
		}

		return false;
	}
}

/*
 * Like HeapTupleSatisfiesDirty
 */
static bool
zs_SatisfiesDirty(ZSBtreeScan *scan, ZSBtreeItem *item)
{
	Relation	rel = scan->rel;
	Snapshot	snapshot = scan->snapshot;
	ZSUndoRecPtr recent_oldest_undo = scan->recent_oldest_undo;
	ZSUndoRecPtr undo_ptr;
	ZSUndoRec  *undorec;
	bool		is_deleted;

	Assert((item->t_flags & ZSBT_COMPRESSED) == 0);
	Assert (snapshot->snapshot_type == SNAPSHOT_DIRTY);

	snapshot->xmin = snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;

	is_deleted = (item->t_flags & (ZSBT_UPDATED | ZSBT_DELETED)) != 0;
	undo_ptr = zsbt_item_undoptr(item);

fetch_undo_record:
	if (undo_ptr.counter < recent_oldest_undo.counter)
	{
		if (!is_deleted)
			return true;
		else
			return false;
	}

	/* have to fetch the UNDO record */
	undorec = zsundo_fetch(rel, undo_ptr);

	if (!is_deleted)
	{
		/* Inserted tuple */
		if (undorec->type == ZSUNDO_TYPE_INSERT)
		{
			if (TransactionIdIsCurrentTransactionId(undorec->xid))
				return true;		/* inserted by me */
			else if (TransactionIdIsInProgress(undorec->xid))
			{
				snapshot->xmin = undorec->xid;
				return true;
			}
			else if (TransactionIdDidCommit(undorec->xid))
			{
				return true;
			}
			else
			{
				/* it must have aborted or crashed */
				return false;
			}
		}
		else if (undorec->type == ZSUNDO_TYPE_TUPLE_LOCK)
		{
			/* locked tuple. */
			/* look at the previous UNDO record to find the insert record */
			undo_ptr = ((ZSUndoRec_TupleLock *) undorec)->prevundorec;
			goto fetch_undo_record;
		}
		else
			elog(ERROR, "unexpected UNDO record type: %d", undorec->type);
	}
	else
	{
		/* deleted or updated-away tuple */
		Assert(undorec->type == ZSUNDO_TYPE_DELETE ||
			   undorec->type == ZSUNDO_TYPE_UPDATE);

		if (TransactionIdIsCurrentTransactionId(undorec->xid))
		{
			/* deleted by me */
			return false;
		}

		if (TransactionIdIsInProgress(undorec->xid))
		{
			snapshot->xmax = undorec->xid;
			return true;
		}

		if (!TransactionIdDidCommit(undorec->xid))
		{
			/* deleter aborted or crashed */
			return true;
		}

		return false;
	}
}

/*
 * True if tuple might be visible to some transaction; false if it's
 * surely dead to everyone, ie, vacuumable.
 */
static bool
zs_SatisfiesNonVacuumable(ZSBtreeScan *scan, ZSBtreeItem *item)
{
	Relation	rel = scan->rel;
	TransactionId OldestXmin = scan->snapshot->xmin;
	bool		is_deleted;
	ZSUndoRecPtr recent_oldest_undo = scan->recent_oldest_undo;
	ZSUndoRecPtr undo_ptr;
	ZSUndoRec  *undorec;

	Assert (scan->snapshot->snapshot_type == SNAPSHOT_NON_VACUUMABLE);
	Assert(TransactionIdIsValid(OldestXmin));

	is_deleted = (item->t_flags & (ZSBT_UPDATED | ZSBT_DELETED)) != 0;
	undo_ptr = zsbt_item_undoptr(item);

fetch_undo_record:

	/* Is it visible? */
	if (undo_ptr.counter < recent_oldest_undo.counter)
	{
		if (!is_deleted)
			return true;
		else
			return false;
	}


	/* have to fetch the UNDO record */
	undorec = zsundo_fetch(rel, undo_ptr);

	if (!is_deleted)
	{
		/* Inserted tuple */
		if (undorec->type == ZSUNDO_TYPE_INSERT)
		{
			if (TransactionIdIsInProgress(undorec->xid))
				return true;		/* inserter has not committed yet */

			if (TransactionIdDidCommit(undorec->xid))
				return true;

			/* it must have aborted or crashed */
			return false;
		}
		else if (undorec->type == ZSUNDO_TYPE_TUPLE_LOCK)
		{
			/* look at the previous UNDO record, to find the Insert record */
			undo_ptr = ((ZSUndoRec_TupleLock *) undorec)->prevundorec;
			goto fetch_undo_record;
		}
		else
			elog(ERROR, "unexpected UNDO record type: %d", undorec->type);
	}
	else
	{
		/* deleted or updated-away tuple */
		ZSUndoRecPtr	prevptr;

		Assert(undorec->type == ZSUNDO_TYPE_DELETE ||
			   undorec->type == ZSUNDO_TYPE_UPDATE);

		if (TransactionIdIsInProgress(undorec->xid))
			return true;	/* delete-in-progress */
		else if (TransactionIdDidCommit(undorec->xid))
		{
			/*
			 * Deleter committed. But perhaps it was recent enough that some open
			 * transactions could still see the tuple.
			 */
			if (!TransactionIdPrecedes(undorec->xid, OldestXmin))
				return true;

			return false;
		}

		/*
		 * The deleting transaction did not commit. But before concluding
		 * that the tuple is live, we have to check if the inserting
		 * XID is live.
		 */
		do {
			if (undorec->type == ZSUNDO_TYPE_DELETE)
				prevptr = ((ZSUndoRec_Delete *) undorec)->prevundorec;
			else if (undorec->type == ZSUNDO_TYPE_UPDATE)
				prevptr = ((ZSUndoRec_Update *) undorec)->prevundorec;
			else if (undorec->type == ZSUNDO_TYPE_TUPLE_LOCK)
				prevptr = ((ZSUndoRec_TupleLock *) undorec)->prevundorec;
			else
				elog(ERROR, "unexpected UNDO record type: %d", undorec->type);

			if (prevptr.counter < recent_oldest_undo.counter)
				return true;
			undorec = zsundo_fetch(rel, prevptr);
		} while(undorec->type == ZSUNDO_TYPE_TUPLE_LOCK);

		Assert(undorec->type == ZSUNDO_TYPE_INSERT);

		if (TransactionIdIsInProgress(undorec->xid))
			return true;	/* insert-in-progress */
		else if (TransactionIdDidCommit(undorec->xid))
			return true;	/* inserted committed */

		/* inserter must have aborted or crashed */
		return false;
	}
}

/*
 * Like HeapTupleSatisfiesVisibility
 */
bool
zs_SatisfiesVisibility(ZSBtreeScan *scan, ZSBtreeItem *item, TransactionId *obsoleting_xid)
{
	ZSUndoRecPtr undo_ptr;

	/*
	 * This works on a single or array item. Compressed items don't have
	 * visibility information (the items inside the compressed container
	 * do)
	 */
	Assert((item->t_flags & ZSBT_COMPRESSED) == 0);

	/* The caller should've filled in the recent_oldest_undo pointer */
	Assert(scan->recent_oldest_undo.counter != 0);

	*obsoleting_xid = InvalidTransactionId;

	/* dead items are never considered visible. */
	if ((item->t_flags & ZSBT_DEAD) != 0)
		return false;

	/*
	 * Items with invalid undo record are considered visible. Mostly META
	 * column stores the valid undo record, all other columns stores invalid
	 * undo pointer. Visibility check is performed based on META column and
	 * only if visible rest of columns are fetched. For in-place updates,
	 * columns other than META column may have valid undo record, in which
	 * case the visibility check needs to be performed for the same. META
	 * column can sometime also have items with invalid undo, see
	 * zsbt_undo_item_deletion().
	 */
	undo_ptr = zsbt_item_undoptr(item);
	if (!IsZSUndoRecPtrValid(&undo_ptr))
		return true;

	switch (scan->snapshot->snapshot_type)
	{
		case SNAPSHOT_MVCC:
			return zs_SatisfiesMVCC(scan, item, obsoleting_xid);

		case SNAPSHOT_SELF:
			return zs_SatisfiesSelf(scan, item);

		case SNAPSHOT_ANY:
			return zs_SatisfiesAny(scan, item);

		case SNAPSHOT_TOAST:
			elog(ERROR, "SnapshotToast not implemented in zedstore");
			break;

		case SNAPSHOT_DIRTY:
			return zs_SatisfiesDirty(scan, item);

		case SNAPSHOT_HISTORIC_MVCC:
			elog(ERROR, "SnapshotHistoricMVCC not implemented in zedstore yet");
			break;

		case SNAPSHOT_NON_VACUUMABLE:
			return zs_SatisfiesNonVacuumable(scan, item);
	}

	return false;				/* keep compiler quiet */
}
