/*
 * zedstore_btree.c
 *		Routines for handling B-trees structures in ZedStore
 *
 * A Zedstore table consists of multiple B-trees, one for each attribute. The
 * functions in this file deal with one B-tree at a time, it is the caller's
 * responsibility to tie together the scans of each btree.
 *
 * Operations:
 *
 * - Sequential scan in TID order
 *  - must be efficient with scanning multiple trees in sync
 *
 * - random lookups, by TID (for index scan)
 *
 * - range scans by TID (for bitmap index scan)
 *
 * NOTES:
 * - Locking order: child before parent, left before right
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstore_btree.c
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/tupdesc_details.h"
#include "access/xact.h"
#include "access/zedstore_compression.h"
#include "access/zedstore_internal.h"
#include "access/zedstore_undo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/rel.h"

/* prototypes for local functions */
static Buffer zsbt_descend(Relation rel, BlockNumber rootblk, zstid key);
static Buffer zsbt_find_downlink(Relation rel, AttrNumber attno,
								 zstid key, BlockNumber childblk, int level,
								 int *itemno);
static void zsbt_recompress_replace(Relation rel, AttrNumber attno,
									Buffer oldbuf, List *items);
static void zsbt_insert_downlink(Relation rel, AttrNumber attno, Buffer leftbuf,
								 zstid rightlokey, BlockNumber rightblkno);
static void zsbt_split_internal_page(Relation rel, AttrNumber attno,
									 Buffer leftbuf, Buffer childbuf,
									 OffsetNumber newoff, zstid newkey, BlockNumber childblk);
static void zsbt_newroot(Relation rel, AttrNumber attno, int level,
						 zstid key1, BlockNumber blk1,
						 zstid key2, BlockNumber blk2,
						 Buffer leftchildbuf);
static ZSUncompressedBtreeItem *zsbt_scan_next_internal(ZSBtreeScan *scan);
static void zsbt_replace_item(Relation rel, AttrNumber attno, Buffer buf,
							  ZSBtreeItem *olditem, ZSBtreeItem *replacementitem,
							  ZSBtreeItem *newitem, List *newitems);

static int zsbt_binsrch_internal(zstid key, ZSBtreeInternalPageItem *arr, int arr_elems);

static TM_Result zsbt_update_lock_old(Relation rel, AttrNumber attno, zstid otid,
					 TransactionId xid, CommandId cid, Snapshot snapshot,
					 Snapshot crosscheck, bool wait, TM_FailureData *hufd);
static void zsbt_update_insert_new(Relation rel, AttrNumber attno,
					   Datum newdatum, bool newisnull, zstid *newtid,
					   TransactionId xid, CommandId cid);
static void zsbt_mark_old_updated(Relation rel, AttrNumber attno, zstid otid, zstid newtid,
					  TransactionId xid, CommandId cid, Snapshot snapshot);

/* ----------------------------------------------------------------
 *						 Public interface
 * ----------------------------------------------------------------
 */

/*
 * Begin a scan of the btree.
 */
void
zsbt_begin_scan(Relation rel, AttrNumber attno, zstid starttid, Snapshot snapshot, ZSBtreeScan *scan)
{
	BlockNumber	rootblk;
	int16		attlen;
	bool		attbyval;
	Buffer		buf;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, false, &attlen, &attbyval);

	scan->rel = rel;
	scan->attno = attno;
	scan->attlen = attlen;
	scan->attbyval = attbyval;
	scan->snapshot = snapshot;
	scan->for_update = false;		/* caller can change this */
	scan->atthasmissing = rel->rd_att->attrs[attno-1].atthasmissing;
	scan->context = CurrentMemoryContext;
	scan->lastbuf_is_locked = false;
	scan->lastoff = InvalidOffsetNumber;
	scan->has_decompressed = false;
	scan->nexttid = starttid;
	memset(&scan->recent_oldest_undo, 0, sizeof(scan->recent_oldest_undo));

	if (rootblk == InvalidBlockNumber)
	{
		/* completely empty tree */
		scan->active = false;
		scan->lastbuf = InvalidBuffer;
		return;
	}

	buf = zsbt_descend(rel, rootblk, starttid);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	scan->active = true;
	scan->lastbuf = buf;

	zs_decompress_init(&scan->decompressor);
}

void
zsbt_end_scan(ZSBtreeScan *scan)
{
	if (!scan->active)
		return;

	if (scan->lastbuf != InvalidBuffer)
	{
		if (scan->lastbuf_is_locked)
			LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(scan->lastbuf);
	}
	zs_decompress_free(&scan->decompressor);

	scan->active = false;
}

static void
zsbt_fill_missing_attribute_value(ZSBtreeScan *scan, Datum *datum, bool *isnull)
{
	int attno = scan->attno - 1;
	AttrMissing *attrmiss = NULL;
	TupleDesc tupleDesc = scan->rel->rd_att;
	Form_pg_attribute attr = TupleDescAttr(tupleDesc, attno);

	*isnull = true;
	if (tupleDesc->constr &&
		tupleDesc->constr->missing)
	{
		/*
		 * If there are missing values we want to put them into the
		 * tuple.
		 */
		attrmiss = tupleDesc->constr->missing;

		if (attrmiss[attno].am_present)
		{
			*isnull = false;
			if (attr->attbyval)
				*datum = fetch_att(&attrmiss[attno].am_value, attr->attbyval, attr->attlen);
			else
				*datum = zs_datumCopy(attrmiss[attno].am_value, attr->attbyval, attr->attlen);
		}
	}
}

/*
 * Return true if there was another tuple. The datum is returned in *datum,
 * and its TID in *tid. For a pass-by-ref datum, it's a palloc'd copy.
 */
bool
zsbt_scan_next(ZSBtreeScan *scan, Datum *datum, bool *isnull, zstid *tid, bool *isvaluemissing)
{
	ZSUncompressedBtreeItem *item;

	*isvaluemissing = false;

	if (!scan->active)
	{
		/*
		 * If btree is not present for this attribute, active will be false
		 * and atthasmissing will be true. In this case the table doesn't have
		 * the datum value but instead catalog has the value for it. Hence,
		 * fill the value from the catalog. Important note: we don't know the
		 * TID for this attributes in such case hence caller needs to not
		 * interpret the TID value.
		 */
		if (scan->atthasmissing)
		{
			zsbt_fill_missing_attribute_value(scan, datum, isnull);
			*tid = InvalidZSTid;
			*isvaluemissing = true;
			return true;
		}

		return false;
	}

	while ((item = zsbt_scan_next_internal(scan)) != NULL)
	{
		if (zs_SatisfiesVisibility(scan, item))
		{
			char		*ptr = item->t_payload;

			*tid = item->t_tid;
			if (item->t_flags & ZSBT_NULL)
				*isnull = true;
			else
			{
				*isnull = false;
				*datum = fetch_att(ptr, scan->attbyval, scan->attlen);
				*datum = zs_datumCopy(*datum, scan->attbyval, scan->attlen);
			}

			if (scan->lastbuf_is_locked)
			{
				LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
				scan->lastbuf_is_locked = false;
			}

			return true;
		}
	}
	return false;
}

/*
 * Get the last tid (plus one) in the tree.
 */
zstid
zsbt_get_last_tid(Relation rel, AttrNumber attno)
{
	BlockNumber	rootblk;
	zstid		rightmostkey;
	zstid		tid;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber maxoff;
	int16		attlen;
	bool		attbyval;

	/* Find the rightmost leaf */
	rootblk = zsmeta_get_root_for_attribute(rel, attno, true, &attlen, &attbyval);
	rightmostkey = MaxZSTid;
	buf = zsbt_descend(rel, rootblk, rightmostkey);
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);

	/*
	 * Look at the last item, for its tid.
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff >= FirstOffsetNumber)
	{
		ItemId		iid = PageGetItemId(page, maxoff);
		ZSBtreeItem	*hitup = (ZSBtreeItem *) PageGetItem(page, iid);

		/* COMPRESSED items cover a range of TIDs */
		if ((hitup->t_flags & ZSBT_COMPRESSED) != 0)
			tid = ((ZSCompressedBtreeItem *) hitup)->t_lasttid;
		else
			tid = hitup->t_tid;
		tid = ZSTidIncrementForInsert(tid);
	}
	else
	{
		tid = opaque->zs_lokey;
	}
	UnlockReleaseBuffer(buf);

	return tid;
}

static ZSUncompressedBtreeItem *
zsbt_create_item(int16 attlen, bool attbyval, zstid tid, Datum datum, bool isnull)
{
	Size		datumsz;
	Size		itemsz;
	ZSUncompressedBtreeItem *newitem;
	char	   *dataptr;

	/*
	 * Form a ZSBtreeItem to insert.
	 */
	if (isnull)
		datumsz = 0;
	else
		datumsz = zs_datumGetSize(datum, attbyval, attlen);
	itemsz = offsetof(ZSUncompressedBtreeItem, t_payload) + datumsz;

	newitem = palloc(itemsz);
	memset(newitem, 0, offsetof(ZSUncompressedBtreeItem, t_payload)); /* zero padding */
	newitem->t_tid = tid;
	newitem->t_flags = 0;
	newitem->t_size = itemsz;
	memset(&newitem->t_undo_ptr, 0, sizeof(ZSUndoRecPtr));

	if (isnull)
		newitem->t_flags |= ZSBT_NULL;
	else
	{
		dataptr = ((char *) newitem) + offsetof(ZSUncompressedBtreeItem, t_payload);
		if (attbyval)
			store_att_byval(dataptr, datum, attlen);
		else
			memcpy(dataptr, DatumGetPointer(datum), datumsz);
	}

	return newitem;
}

/*
 * Insert a multiple items to the given attribute's btree.
 *
 * Populates the TIDs of the new tuples.
 *
 * If 'tid' in list is valid, then that TID is used. It better not be in use already. If
 * it's invalid, then a new TID is allocated, as we see best. (When inserting the
 * first column of the row, pass invalid, and for other columns, pass the TID
 * you got for the first column.)
 */
void
zsbt_multi_insert(Relation rel, AttrNumber attno,
				  Datum *datums, bool *isnulls, zstid *tids, int nitems,
				  TransactionId xid, CommandId cid, ZSUndoRecPtr *undorecptr)
{
	Form_pg_attribute attr = &rel->rd_att->attrs[attno - 1];
	int16		attlen;
	bool		attbyval;
	bool		assign_tids;
	zstid		tid = tids[0];
	BlockNumber	rootblk;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber maxoff;
	zstid		lasttid;
	zstid		insert_target_key;
	ZSUndoRec_Insert undorec;
	int			i;
	List	   *newitems;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, true, &attlen, &attbyval);

	if (attr->attbyval != attbyval || attr->attlen != attlen)
		elog(ERROR, "attribute information stored in root dir doesn't match with rel");

	/*
	 * If TID was given, find the right place for it. Otherwise, insert to
	 * the rightmost leaf.
	 *
	 * TODO: use a Free Space Map to find suitable target.
	 */
	assign_tids = (tid == InvalidZSTid);

	if (!assign_tids)
		insert_target_key = tid;
	else
		insert_target_key = MaxZSTid;

	buf = zsbt_descend(rel, rootblk, insert_target_key);
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);

	/*
	 * Look at the last item, for its tid.
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff >= FirstOffsetNumber)
	{
		ItemId		iid = PageGetItemId(page, maxoff);
		ZSBtreeItem	*hitup = (ZSBtreeItem *) PageGetItem(page, iid);

		if ((hitup->t_flags & ZSBT_COMPRESSED) != 0)
			lasttid = ((ZSCompressedBtreeItem *) hitup)->t_lasttid;
		else
			lasttid = hitup->t_tid;

		if (assign_tids)
		{
			tid = lasttid;
			tid = ZSTidIncrementForInsert(tid);
		}
	}
	else
	{
		lasttid = opaque->zs_lokey;
		if (assign_tids)
			tid = lasttid;
	}

	/* assign TIDS for each item, if needed */
	if (assign_tids)
	{
		for (i = 0; i < nitems; i++)
		{
			tids[i] = tid;
			tid = ZSTidIncrementForInsert(tid);
		}
	}

	/* Form an undo record */
	if (!IsZSUndoRecPtrValid(undorecptr))
	{
		undorec.rec.size = sizeof(ZSUndoRec_Insert);
		undorec.rec.type = ZSUNDO_TYPE_INSERT;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tids[0];
		undorec.endtid = tids[nitems - 1];
		*undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Create items to insert */
	newitems = NIL;
	for (i = 0; i < nitems; i++)
	{
		ZSUncompressedBtreeItem *newitem;

		newitem = zsbt_create_item(attlen, attbyval, tid, datums[i], isnulls[i]);

		/* fill in the remaining fields in the item */
		newitem->t_undo_ptr = *undorecptr;
		newitem->t_tid = tids[i];

		newitems = lappend(newitems, newitem);
	}

	while (list_length(newitems))
	{
		ZSUncompressedBtreeItem *newitem = (ZSUncompressedBtreeItem *) linitial(newitems);

		/*
		 * If there's enough space on the page, insert it directly. Otherwise, try to
		 * compress all existing items. If that still doesn't create enough space, we
		 * have to split the page.
		 *
		 * TODO: We also resort to the slow way, if the new TID is not at the end of
		 * the page. Things get difficult, if the new TID is covered by the range of
		 * an existing compressed item.
		 */
		if (PageGetFreeSpace(page) >= MAXALIGN(newitem->t_size) &&
			(maxoff > FirstOffsetNumber || tid > lasttid))
		{
			OffsetNumber off;

			off = PageAddItemExtended(page, (Item) newitem, newitem->t_size,
									  maxoff + 1, PAI_OVERWRITE);
			if (off == InvalidOffsetNumber)
				elog(ERROR, "didn't fit, after all?");

			maxoff = PageGetMaxOffsetNumber(page);
			newitems = list_delete_first(newitems);
		}
		else
			break;
	}

	if (list_length(newitems))
	{
		/* recompress and possibly split the page */
		zsbt_replace_item(rel, attno, buf,
						  NULL, NULL,
						  NULL, newitems);
		/* zsbt_replace_item unlocked 'buf' */
		ReleaseBuffer(buf);
	}
	else
	{
		MarkBufferDirty(buf);
		/* TODO: WAL-log */
		UnlockReleaseBuffer(buf);
	}
}

TM_Result
zsbt_delete(Relation rel, AttrNumber attno, zstid tid,
			TransactionId xid, CommandId cid,
			Snapshot snapshot, Snapshot crosscheck, bool wait,
			TM_FailureData *hufd, bool changingPart)
{
	ZSBtreeScan scan;
	ZSUncompressedBtreeItem *item;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	ZSUndoRecPtr undorecptr;
	ZSUncompressedBtreeItem *deleteditem;

	zsbt_begin_scan(rel, attno, tid, snapshot, &scan);
	scan.for_update = true;

	/* Find the item to delete. (It could be compressed) */
	item = zsbt_scan_next_internal(&scan);
	if (item->t_tid != tid)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find tuple to delete with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), attno);
	}
	result = zs_SatisfiesUpdate(&scan, item, &keep_old_undo_ptr, hufd);
	if (result != TM_Ok)
	{
		zsbt_end_scan(&scan);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_Delete undorec;

		undorec.rec.size = sizeof(ZSUndoRec_Delete);
		undorec.rec.type = ZSUNDO_TYPE_DELETE;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;

		if (keep_old_undo_ptr)
			undorec.prevundorec = item->t_undo_ptr;
		else
			ZSUndoRecPtrInitialize(&undorec.prevundorec);

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the ZSBreeItem with a DELETED item. */
	deleteditem = palloc(item->t_size);
	memcpy(deleteditem, item, item->t_size);
	deleteditem->t_flags |= ZSBT_DELETED;
	deleteditem->t_undo_ptr = undorecptr;

	zsbt_replace_item(rel, attno, scan.lastbuf,
					  (ZSBtreeItem *) item, (ZSBtreeItem *) deleteditem,
					  NULL, NIL);
	scan.lastbuf_is_locked = false;	/* zsbt_replace_item released */
	zsbt_end_scan(&scan);

	pfree(deleteditem);

	return TM_Ok;
}

/*
 * If 'newtid' is valid, then that TID is used for the new item. It better not
 * be in use already. If it's invalid, then a new TID is allocated, as we see
 * best. (When inserting the first column of the row, pass invalid, and for
 * other columns, pass the TID you got for the first column.)
 */
TM_Result
zsbt_update(Relation rel, AttrNumber attno, zstid otid, Datum newdatum,
			bool newisnull, TransactionId xid, CommandId cid, Snapshot snapshot,
			Snapshot crosscheck, bool wait, TM_FailureData *hufd,
			zstid *newtid_p)
{
	TM_Result	result;

	/*
	 * Find and lock the old item.
	 *
	 * TODO: If there's free TID space left on the same page, we should keep the
	 * buffer locked, and use the same page for the new tuple.
	 */
	result = zsbt_update_lock_old(rel, attno, otid,
								  xid, cid, snapshot,
								  crosscheck, wait, hufd);

	if (result != TM_Ok)
		return result;

	/* insert new version */
	zsbt_update_insert_new(rel, attno, newdatum, newisnull, newtid_p, xid, cid);

	/* update the old item with the "t_ctid pointer" for the new item */
	zsbt_mark_old_updated(rel, attno, otid, *newtid_p, xid, cid, snapshot);

	return TM_Ok;
}

/*
 * Subroutine of zsbt_update(): locks the old item for update.
 */
static TM_Result
zsbt_update_lock_old(Relation rel, AttrNumber attno, zstid otid,
					 TransactionId xid, CommandId cid, Snapshot snapshot,
					 Snapshot crosscheck, bool wait, TM_FailureData *hufd)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Form_pg_attribute attr = &desc->attrs[attno - 1];
	ZSBtreeScan scan;
	ZSUncompressedBtreeItem *olditem;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;

	/*
	 * Find the item to delete.  It could be part of a compressed item,
	 * we let zsbt_scan_next_internal() handle that.
	 */
	zsbt_begin_scan(rel, attno, otid, snapshot, &scan);
	scan.for_update = true;

	if (attr->attbyval != scan.attbyval || attr->attlen != scan.attlen)
		elog(ERROR, "attribute information stored in root dir doesn't match with rel");

	olditem = zsbt_scan_next_internal(&scan);
	if (olditem->t_tid != otid)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find old tuple to update with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(otid), ZSTidGetOffsetNumber(otid), attno);
	}

	/*
	 * Is it visible to us?
	 */
	result = zs_SatisfiesUpdate(&scan, olditem, &keep_old_undo_ptr, hufd);
	if (result != TM_Ok)
	{
		zsbt_end_scan(&scan);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	/*
	 * TODO: tuple-locking not implemented. Pray that there is no competing
	 * concurrent update!
	 */

	/* transfer ownership of the buffer, and free the scan. */
	zsbt_end_scan(&scan);

	return TM_Ok;
}

/*
 * Subroutine of zsbt_update(): inserts the new, updated, item.
 */
static void
zsbt_update_insert_new(Relation rel, AttrNumber attno,
					   Datum newdatum, bool newisnull, zstid *newtid,
					   TransactionId xid, CommandId cid)
{
	ZSUndoRecPtr undorecptr;

	ZSUndoRecPtrInitialize(&undorecptr);
	zsbt_multi_insert(rel, attno, &newdatum, &newisnull, newtid, 1,
					  xid, cid, &undorecptr);
}

/*
 * Subroutine of zsbt_update(): mark old item as updated.
 */
static void
zsbt_mark_old_updated(Relation rel, AttrNumber attno, zstid otid, zstid newtid,
					  TransactionId xid, CommandId cid, Snapshot snapshot)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Form_pg_attribute attr = &desc->attrs[attno - 1];
	ZSBtreeScan scan;
	ZSUncompressedBtreeItem *olditem;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	TM_FailureData tmfd;
	ZSUndoRecPtr undorecptr;
	ZSUncompressedBtreeItem *deleteditem;

	/*
	 * Find the item to delete.  It could be part of a compressed item,
	 * we let zsbt_scan_next_internal() handle that.
	 */
	zsbt_begin_scan(rel, attno, otid, snapshot, &scan);
	scan.for_update = true;

	if (attr->attbyval != scan.attbyval || attr->attlen != scan.attlen)
		elog(ERROR, "attribute information stored in root dir doesn't match with rel");

	olditem = zsbt_scan_next_internal(&scan);
	if (olditem->t_tid != otid)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find old tuple to update with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(otid), ZSTidGetOffsetNumber(otid), attno);
	}

	/*
	 * Is it visible to us?
	 */
	result = zs_SatisfiesUpdate(&scan, olditem, &keep_old_undo_ptr, &tmfd);
	if (result != TM_Ok)
	{
		zsbt_end_scan(&scan);
		elog(ERROR, "tuple concurrently updated - not implemented");
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_Update undorec;

		undorec.rec.size = sizeof(ZSUndoRec_Update);
		undorec.rec.type = ZSUNDO_TYPE_UPDATE;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = otid;
		if (keep_old_undo_ptr)
			undorec.prevundorec = olditem->t_undo_ptr;
		else
			ZSUndoRecPtrInitialize(&undorec.prevundorec);
		undorec.newtid = newtid;

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the ZSBreeItem with an UPDATED item. */
	deleteditem = palloc(olditem->t_size);
	memcpy(deleteditem, olditem, olditem->t_size);
	deleteditem->t_flags |= ZSBT_UPDATED;
	deleteditem->t_undo_ptr = undorecptr;

	zsbt_replace_item(rel, attno, scan.lastbuf,
					  (ZSBtreeItem *) olditem, (ZSBtreeItem *) deleteditem,
					  NULL, NIL);
	scan.lastbuf_is_locked = false;	/* zsbt_recompress_replace released */
	zsbt_end_scan(&scan);

	pfree(deleteditem);
}

TM_Result
zsbt_lock_item(Relation rel, AttrNumber attno, zstid tid,
			   TransactionId xid, CommandId cid, Snapshot snapshot,
			   LockTupleMode lockmode, LockWaitPolicy wait_policy,
			   TM_FailureData *hufd)
{
	ZSBtreeScan scan;
	ZSUncompressedBtreeItem *item;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	ZSUndoRecPtr undorecptr;
	ZSUncompressedBtreeItem *newitem;

	zsbt_begin_scan(rel, attno, tid, snapshot, &scan);
	scan.for_update = true;

	/* Find the item to delete. (It could be compressed) */
	item = zsbt_scan_next_internal(&scan);
	if (item->t_tid != tid)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find tuple to delete with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), attno);
	}
	result = zs_SatisfiesUpdate(&scan, item, &keep_old_undo_ptr, hufd);
	if (result != TM_Ok)
	{
		zsbt_end_scan(&scan);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	if ((item->t_flags & ZSBT_DELETED) != 0)
		elog(ERROR, "cannot lock deleted tuple");

	if ((item->t_flags & ZSBT_UPDATED) != 0)
		elog(ERROR, "cannot lock updated tuple");

	/* Create UNDO record. */
	{
		ZSUndoRec_TupleLock undorec;

		undorec.rec.size = sizeof(ZSUndoRec_TupleLock);
		undorec.rec.type = ZSUNDO_TYPE_TUPLE_LOCK;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;
		undorec.lockmode = lockmode;
		if (keep_old_undo_ptr)
			undorec.prevundorec = item->t_undo_ptr;
		else
			ZSUndoRecPtrInitialize(&undorec.prevundorec);

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the item with an identical one, but with updated undo pointer. */
	newitem = palloc(item->t_size);
	memcpy(newitem, item, item->t_size);
	newitem->t_undo_ptr = undorecptr;

	zsbt_replace_item(rel, attno, scan.lastbuf,
					  (ZSBtreeItem *) item, (ZSBtreeItem *) newitem,
					  NULL, NIL);
	scan.lastbuf_is_locked = false;	/* zsbt_replace_item released */
	zsbt_end_scan(&scan);

	pfree(newitem);

	return TM_Ok;
}

/*
 * Mark item with given TID as dead.
 *
 * This is used during VACUUM.
 */
void
zsbt_mark_item_dead(Relation rel, AttrNumber attno, zstid tid, ZSUndoRecPtr undoptr)
{
	ZSBtreeScan scan;
	ZSUncompressedBtreeItem *item;
	ZSUncompressedBtreeItem deaditem;

	zsbt_begin_scan(rel, attno, tid, NULL, &scan);
	scan.for_update = true;

	/* Find the item to delete. (It could be compressed) */
	item = zsbt_scan_next_internal(&scan);
	if (item->t_tid != tid)
	{
		zsbt_end_scan(&scan);
		elog(WARNING, "could not find tuple to remove with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), attno);
		return;
	}

	/* Replace the ZSBreeItem with a DEAD item. (Unless it's already dead) */
	if ((item->t_flags & ZSBT_DEAD) != 0)
	{
		zsbt_end_scan(&scan);
		return;
	}

	memset(&deaditem, 0, offsetof(ZSUncompressedBtreeItem, t_payload));
	deaditem.t_tid = tid;
	deaditem.t_size = sizeof(ZSUncompressedBtreeItem);
	deaditem.t_flags = ZSBT_DEAD;
	deaditem.t_undo_ptr = undoptr;

	zsbt_replace_item(rel, attno, scan.lastbuf,
					  (ZSBtreeItem *) item, (ZSBtreeItem *) &deaditem,
					  NULL, NIL);
	scan.lastbuf_is_locked = false;	/* zsbt_replace_item released */
	zsbt_end_scan(&scan);
}

/* ----------------------------------------------------------------
 *						 Internal routines
 * ----------------------------------------------------------------
 */

/*
 * Find the leaf page containing the given key TID.
 */
static Buffer
zsbt_descend(Relation rel, BlockNumber rootblk, zstid key)
{
	BlockNumber next;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	int			nextlevel = -1;

	next = rootblk;
	for (;;)
	{
		buf = ReadBuffer(rel, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);		/* TODO: shared */
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (nextlevel == -1)
			nextlevel = opaque->zs_level;
		else if (opaque->zs_level != nextlevel)
			elog(ERROR, "unexpected level encountered when descending tree");

		if (opaque->zs_level == 0)
			return buf;

		/*
		 * Do we need to walk right? This could happen if the page was concurrently split.
		 */
		if (key >= opaque->zs_hikey)
		{
			/* follow the right-link */
			next = opaque->zs_next;
			if (next == InvalidBlockNumber)
				elog(ERROR, "fell off the end of btree");
		}
		else
		{
			/* follow the downlink */
			items = ZSBtreeInternalPageGetItems(page);
			nitems = ZSBtreeInternalPageGetNumItems(page);

			itemno = zsbt_binsrch_internal(key, items, nitems);
			if (itemno < 0)
				elog(ERROR, "could not descend tree for tid (%u, %u)",
					 ZSTidGetBlockNumber(key), ZSTidGetOffsetNumber(key));
			next = items[itemno].childblk;
			nextlevel--;
		}
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Re-find the parent page containing downlink for given block.
 * The returned page is exclusive-locked, and *itemno_p is set to the
 * position of the downlink in the parent.
 *
 * If 'childblk' is the root, returns InvalidBuffer.
 */
static Buffer
zsbt_find_downlink(Relation rel, AttrNumber attno,
				   zstid key, BlockNumber childblk, int level,
				   int *itemno_p)
{
	BlockNumber rootblk;
	int16		attlen;
	bool		attbyval;
	BlockNumber next;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	int			nextlevel = -1;

	/* start from root */
	rootblk = zsmeta_get_root_for_attribute(rel, attno, true, &attlen, &attbyval);
	if (rootblk == childblk)
		return InvalidBuffer;

	/* XXX: this is mostly the same as zsbt_descend, but we stop at an internal
	 * page instead of descending all the way down to leaf */
	next = rootblk;
	for (;;)
	{
		buf = ReadBuffer(rel, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (nextlevel == -1)
			nextlevel = opaque->zs_level;
		else if (nextlevel != opaque->zs_level)
			elog(ERROR, "unexpected level encountered when descending tree");

		if (opaque->zs_level <= level)
			elog(ERROR, "unexpected page level encountered");

		/*
		 * Do we need to walk right? This could happen if the page was concurrently split.
		 */
		if (key >= opaque->zs_hikey)
		{
			next = opaque->zs_next;
			if (next == InvalidBlockNumber)
				elog(ERROR, "fell off the end of btree");
		}
		else
		{
			items = ZSBtreeInternalPageGetItems(page);
			nitems = ZSBtreeInternalPageGetNumItems(page);

			itemno = zsbt_binsrch_internal(key, items, nitems);
			if (itemno < 0)
				elog(ERROR, "could not descend tree for tid (%u, %u)",
					 ZSTidGetBlockNumber(key), ZSTidGetOffsetNumber(key));

			if (opaque->zs_level == level + 1)
			{
				if (items[itemno].childblk != childblk)
					elog(ERROR, "could not re-find downlink for block %u", childblk);
				*itemno_p = itemno;
				return buf;
			}

			next = items[itemno].childblk;
			nextlevel--;
		}
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Create a new btree root page, containing two downlinks.
 *
 * NOTE: the very first root page of a btree, which is also the leaf, is created
 * in zsmeta_get_root_for_attribute(), not here.
 */
static void
zsbt_newroot(Relation rel, AttrNumber attno, int level,
			 zstid key1, BlockNumber blk1,
			 zstid key2, BlockNumber blk2,
			 Buffer leftchildbuf)
{
	ZSBtreePageOpaque *opaque;
	ZSBtreePageOpaque *leftchildopaque;
	Buffer		buf;
	Page		page;
	ZSBtreeInternalPageItem *items;
	Buffer		metabuf;

	metabuf = ReadBuffer(rel, ZS_META_BLK);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	Assert(key1 < key2);

	buf = zs_getnewbuf(rel);
	page = BufferGetPage(buf);
	PageInit(page, BLCKSZ, sizeof(ZSBtreePageOpaque));
	opaque = ZSBtreePageGetOpaque(page);
	opaque->zs_attno = attno;
	opaque->zs_next = InvalidBlockNumber;
	opaque->zs_lokey = MinZSTid;
	opaque->zs_hikey = MaxPlusOneZSTid;
	opaque->zs_level = level;
	opaque->zs_flags = 0;
	opaque->zs_page_id = ZS_BTREE_PAGE_ID;

	items = ZSBtreeInternalPageGetItems(page);
	items[0].tid = key1;
	items[0].childblk =  blk1;
	items[1].tid = key2;
	items[1].childblk = blk2;
	((PageHeader) page)->pd_lower += 2 * sizeof(ZSBtreeInternalPageItem);
	Assert(ZSBtreeInternalPageGetNumItems(page) == 2);

	/* clear the follow-right flag on left child */
	leftchildopaque = ZSBtreePageGetOpaque(BufferGetPage(leftchildbuf));
	leftchildopaque->zs_flags &= ~ZS_FOLLOW_RIGHT;

	/* TODO: wal-log all, including metapage */

	MarkBufferDirty(buf);
	MarkBufferDirty(leftchildbuf);

	/* Before exiting, update the metapage */
	zsmeta_update_root_for_attribute(rel, attno, metabuf, BufferGetBlockNumber(buf));

	UnlockReleaseBuffer(leftchildbuf);
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * After page split, insert the downlink of 'rightblkno' to the parent.
 *
 * On entry, 'leftbuf' must be pinned exclusive-locked. It is released on exit.
 */
static void
zsbt_insert_downlink(Relation rel, AttrNumber attno, Buffer leftbuf,
					 zstid rightlokey, BlockNumber rightblkno)
{
	BlockNumber	leftblkno = BufferGetBlockNumber(leftbuf);
	Page		leftpage = BufferGetPage(leftbuf);
	ZSBtreePageOpaque *leftopaque = ZSBtreePageGetOpaque(leftpage);
	zstid		leftlokey = leftopaque->zs_lokey;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	Buffer		parentbuf;
	Page		parentpage;

	/*
	 * re-find parent
	 *
	 * TODO: this is a bit inefficient. Usually, we have just descended the
	 * tree, and if we just remembered the path we descended, we could just
	 * walk back up.
	 */
	parentbuf = zsbt_find_downlink(rel, attno, leftlokey, leftblkno, leftopaque->zs_level, &itemno);
	if (parentbuf == InvalidBuffer)
	{
		zsbt_newroot(rel, attno, leftopaque->zs_level + 1,
					 leftlokey, BufferGetBlockNumber(leftbuf),
					 rightlokey, rightblkno, leftbuf);
		return;
	}
	parentpage = BufferGetPage(parentbuf);

	/* Find the position in the parent for the downlink */
	items = ZSBtreeInternalPageGetItems(parentpage);
	nitems = ZSBtreeInternalPageGetNumItems(parentpage);
	itemno = zsbt_binsrch_internal(rightlokey, items, nitems);

	/* sanity checks */
	if (itemno < 0 || items[itemno].tid != leftlokey ||
		items[itemno].childblk != leftblkno)
	{
		elog(ERROR, "could not find downlink for block %u TID (%u, %u)",
			 leftblkno, ZSTidGetBlockNumber(leftlokey),
			 ZSTidGetOffsetNumber(leftlokey));
	}
	itemno++;

	if (ZSBtreeInternalPageIsFull(parentpage))
	{
		/* split internal page */
		zsbt_split_internal_page(rel, attno, parentbuf, leftbuf, itemno, rightlokey, rightblkno);
	}
	else
	{
		/* insert the new downlink for the right page. */
		memmove(&items[itemno + 1],
				&items[itemno],
				(nitems - itemno) * sizeof(ZSBtreeInternalPageItem));
		items[itemno].tid = rightlokey;
		items[itemno].childblk = rightblkno;
		((PageHeader) parentpage)->pd_lower += sizeof(ZSBtreeInternalPageItem);

		leftopaque->zs_flags &= ~ZS_FOLLOW_RIGHT;

		/* TODO: WAL-log */

		MarkBufferDirty(leftbuf);
		MarkBufferDirty(parentbuf);
		UnlockReleaseBuffer(leftbuf);
		UnlockReleaseBuffer(parentbuf);
	}
}

/*
 * Split an internal page.
 *
 * The new downlink specified by 'newkey' and 'childblk' is inserted to
 * position 'newoff', on 'leftbuf'. The page is split.
 */
static void
zsbt_split_internal_page(Relation rel, AttrNumber attno, Buffer leftbuf, Buffer childbuf,
						 OffsetNumber newoff, zstid newkey, BlockNumber childblk)
{
	Buffer		rightbuf;
	Page		origpage = BufferGetPage(leftbuf);
	Page		leftpage;
	Page		rightpage;
	BlockNumber rightblkno;
	ZSBtreePageOpaque *leftopaque;
	ZSBtreePageOpaque *rightopaque;
	ZSBtreeInternalPageItem *origitems;
	ZSBtreeInternalPageItem *leftitems;
	ZSBtreeInternalPageItem *rightitems;
	int			orignitems;
	int			leftnitems;
	int			rightnitems;
	int			splitpoint;
	zstid		splittid;
	bool		newitemonleft;
	int			i;
	ZSBtreeInternalPageItem newitem;

	leftpage = PageGetTempPageCopySpecial(origpage);
	leftopaque = ZSBtreePageGetOpaque(leftpage);
	Assert(leftopaque->zs_level > 0);
	/* any previous incomplete split must be finished first */
	Assert((leftopaque->zs_flags & ZS_FOLLOW_RIGHT) == 0);

	rightbuf = zs_getnewbuf(rel);
	rightpage = BufferGetPage(rightbuf);
	rightblkno = BufferGetBlockNumber(rightbuf);
	PageInit(rightpage, BLCKSZ, sizeof(ZSBtreePageOpaque));
	rightopaque = ZSBtreePageGetOpaque(rightpage);

	/*
	 * Figure out the split point.
	 *
	 * TODO: currently, always do 90/10 split.
	 */
	origitems = ZSBtreeInternalPageGetItems(origpage);
	orignitems = ZSBtreeInternalPageGetNumItems(origpage);
	splitpoint = orignitems * 0.9;
	splittid = origitems[splitpoint].tid;
	newitemonleft = (newkey < splittid);

	/* Set up the page headers */
	rightopaque->zs_attno = attno;
	rightopaque->zs_next = leftopaque->zs_next;
	rightopaque->zs_lokey = splittid;
	rightopaque->zs_hikey = leftopaque->zs_hikey;
	rightopaque->zs_level = leftopaque->zs_level;
	rightopaque->zs_flags = 0;
	rightopaque->zs_page_id = ZS_BTREE_PAGE_ID;

	leftopaque->zs_next = rightblkno;
	leftopaque->zs_hikey = splittid;
	leftopaque->zs_flags |= ZS_FOLLOW_RIGHT;

	/* copy the items */
	leftitems = ZSBtreeInternalPageGetItems(leftpage);
	leftnitems = 0;
	rightitems = ZSBtreeInternalPageGetItems(rightpage);
	rightnitems = 0;

	newitem.tid = newkey;
	newitem.childblk = childblk;

	for (i = 0; i < orignitems; i++)
	{
		if (i == newoff)
		{
			if (newitemonleft)
				leftitems[leftnitems++] = newitem;
			else
				rightitems[rightnitems++] = newitem;
		}

		if (i < splitpoint)
			leftitems[leftnitems++] = origitems[i];
		else
			rightitems[rightnitems++] = origitems[i];
	}
	/* cope with possibility that newitem goes at the end */
	if (i <= newoff)
	{
		Assert(!newitemonleft);
		rightitems[rightnitems++] = newitem;
	}
	((PageHeader) leftpage)->pd_lower += leftnitems * sizeof(ZSBtreeInternalPageItem);
	((PageHeader) rightpage)->pd_lower += rightnitems * sizeof(ZSBtreeInternalPageItem);

	Assert(leftnitems + rightnitems == orignitems + 1);

	PageRestoreTempPage(leftpage, origpage);

	/* TODO: WAL-logging */
	MarkBufferDirty(leftbuf);
	MarkBufferDirty(rightbuf);

	MarkBufferDirty(childbuf);
	ZSBtreePageGetOpaque(BufferGetPage(childbuf))->zs_flags &= ~ZS_FOLLOW_RIGHT;
	UnlockReleaseBuffer(childbuf);

	UnlockReleaseBuffer(rightbuf);

	/* recurse to insert downlink. (this releases 'leftbuf') */
	zsbt_insert_downlink(rel, attno, leftbuf, splittid, rightblkno);
}

/*
 * Returns the next item in the scan. This doesn't pay attention to visibility.
 *
 * The returned pointer might point directly to a btree-buffer, or it might be
 * palloc'd copy. If it points to a buffer, scan->lastbuf_is_locked is true,
 * otherwise false.
 */
static ZSUncompressedBtreeItem *
zsbt_scan_next_internal(ZSBtreeScan *scan)
{
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber off;
	OffsetNumber maxoff;
	BlockNumber	next;

	if (!scan->active)
		return NULL;

	for (;;)
	{
		while (scan->has_decompressed)
		{
			ZSUncompressedBtreeItem *item = zs_decompress_read_item(&scan->decompressor);

			if (item == NULL)
			{
				scan->has_decompressed = false;
				break;
			}
			if (item->t_tid >= scan->nexttid)
			{
				scan->nexttid = item->t_tid;
				scan->nexttid = ZSTidIncrement(scan->nexttid);
				return item;
			}
		}

		buf = scan->lastbuf;
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (!scan->lastbuf_is_locked)
			LockBuffer(buf, scan->for_update ? BUFFER_LOCK_EXCLUSIVE : BUFFER_LOCK_SHARE);
		scan->lastbuf_is_locked = true;

		/* TODO: check that the page is a valid zs btree page */

		/* TODO: check the last offset first, as an optimization */
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(page, iid);

			if ((item->t_flags & ZSBT_COMPRESSED) != 0)
			{
				ZSCompressedBtreeItem *citem = (ZSCompressedBtreeItem *) item;

				if (citem->t_lasttid >= scan->nexttid)
				{
					MemoryContext oldcxt = MemoryContextSwitchTo(scan->context);

					zs_decompress_chunk(&scan->decompressor, citem);
					MemoryContextSwitchTo(oldcxt);
					scan->has_decompressed = true;
					if (!scan->for_update)
					{
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						scan->lastbuf_is_locked = false;
					}
					break;
				}
			}
			else
			{
				ZSUncompressedBtreeItem *uitem = (ZSUncompressedBtreeItem *) item;

				if (uitem->t_tid >= scan->nexttid)
				{
					scan->nexttid = uitem->t_tid;
					scan->nexttid = ZSTidIncrement(scan->nexttid);
					return uitem;
				}
			}
		}

		if (scan->has_decompressed)
			continue;

		/* No more items on this page. Walk right, if possible */
		next = opaque->zs_next;
		if (next == BufferGetBlockNumber(buf))
			elog(ERROR, "btree page %u next-pointer points to itself", next);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		scan->lastbuf_is_locked = false;

		if (next == InvalidBlockNumber)
		{
			scan->active = false;
			ReleaseBuffer(scan->lastbuf);
			scan->lastbuf = InvalidBuffer;
			return NULL;
		}

		scan->lastbuf = ReleaseAndReadBuffer(scan->lastbuf, scan->rel, next);
	}
}

/*
 * This helper function is used to implement INSERT, UPDATE and DELETE.
 *
 * If 'olditem' is not NULL, then 'olditem' on the page is replaced with
 * 'replacementitem'. 'replacementitem' can be NULL, to remove an old item.
 *
 * If 'newitem' is not NULL, it is added to the page, to the correct position.
 *
 * This function handles decompressing and recompressing items, and splitting
 * the page if needed.
 */
static void
zsbt_replace_item(Relation rel, AttrNumber attno, Buffer buf,
				  ZSBtreeItem *olditem,
				  ZSBtreeItem *replacementitem,
				  ZSBtreeItem *newitem,
				  List       *newitems)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber off;
	OffsetNumber maxoff;
	List	   *items;
	bool		found_old_item = false;
	/* We might need to decompress up to two previously compressed items */
	ZSDecompressContext decompressors[2];
	int			numdecompressors = 0;

	/*
	 * Helper routine, to append the given old item 'x' to the list.
	 * If the 'x' matches the old item, then append 'replacementitem' instead.
	 * And if thew 'newitem' shoudl go before 'x', then append that first.
	 *
	 * TODO: We could also leave out any old, deleted, items that are no longer
	 * visible to anyone.
	 */
#define PROCESS_ITEM(x) \
	do { \
		if (newitem && (x)->t_tid >= newitem->t_tid) \
		{ \
			Assert((x)->t_tid != newitem->t_tid); \
			items = lappend(items, newitem); \
			newitem = NULL; \
		} \
		if (olditem && (x)->t_tid == olditem->t_tid) \
		{ \
			Assert(!found_old_item); \
			found_old_item = true; \
			if (replacementitem) \
				items = lappend(items, replacementitem); \
		} \
		else \
			items = lappend(items, x); \
	} while(0)

	/* Loop through all old items on the page */
	items = NIL;
	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(page, iid);

		if ((item->t_flags & ZSBT_COMPRESSED) != 0)
		{
			ZSCompressedBtreeItem *citem = (ZSCompressedBtreeItem *) item;

			if ((olditem && citem->t_tid <= olditem->t_tid && olditem->t_tid <= citem->t_lasttid) ||
				(newitem && citem->t_tid <= newitem->t_tid && newitem->t_tid <= citem->t_lasttid))
			{
				/* Found it, this compressed item covers the target or the new TID. */
				/* We have to decompress it, and recompress */
				ZSDecompressContext *decompressor = &decompressors[numdecompressors++];
				ZSUncompressedBtreeItem *uitem;

				Assert(numdecompressors <= 2);

				zs_decompress_init(decompressor);
				zs_decompress_chunk(decompressor, citem);

				while ((uitem = zs_decompress_read_item(decompressor)) != NULL)
					PROCESS_ITEM(uitem);
			}
			else
			{
				/* this item does not cover the target, nor the newitem. Add as it is. */
				items = lappend(items, item);
				continue;
			}
		}
		else
			PROCESS_ITEM(item);
	}

	if (olditem && !found_old_item)
		elog(ERROR, "could not find old item to replace");

	/* if the new item was not added in the loop, it goes to the end */
	if (newitem)
		items = lappend(items, newitem);

	if (newitems)
		items = list_concat(items, newitems);

	/* Now pass the list to the recompressor. */
	IncrBufferRefCount(buf);
	zsbt_recompress_replace(rel, attno, buf, items);

	/*
	 * We can now free the decompression contexts. The pointers in the 'items' list
	 * point to decompression buffers, so we cannot free them until after writing out
	 * the pages.
	 */
	for (int i = 0; i < numdecompressors; i++)
		zs_decompress_free(&decompressors[i]);
	list_free(items);
}

/*
 * Recompressor routines
 */
typedef struct
{
	Page		currpage;
	ZSCompressContext compressor;
	int			compressed_items;
	List	   *pages;		/* first page writes over the old buffer,
							 * subsequent pages get newly-allocated buffers */

	int			total_items;
	int			total_compressed_items;
	int			total_already_compressed_items;

	AttrNumber	attno;
	zstid		hikey;
} zsbt_recompress_context;

static void
zsbt_recompress_newpage(zsbt_recompress_context *cxt, zstid nexttid)
{
	Page		newpage;
	ZSBtreePageOpaque *newopaque;

	if (cxt->currpage)
	{
		/* set the last tid on previous page */
		ZSBtreePageOpaque *oldopaque = ZSBtreePageGetOpaque(cxt->currpage);

		oldopaque->zs_hikey = nexttid;
	}

	newpage = (Page) palloc(BLCKSZ);
	PageInit(newpage, BLCKSZ, sizeof(ZSBtreePageOpaque));
	cxt->pages = lappend(cxt->pages, newpage);
	cxt->currpage = newpage;

	newopaque = ZSBtreePageGetOpaque(newpage);
	newopaque->zs_attno = cxt->attno;
	newopaque->zs_next = InvalidBlockNumber; /* filled in later */
	newopaque->zs_lokey = nexttid;
	newopaque->zs_hikey = cxt->hikey;		/* overwritten later, if this is not last page */
	newopaque->zs_level = 0;
	newopaque->zs_flags = 0;
	newopaque->zs_page_id = ZS_BTREE_PAGE_ID;
}

static void
zsbt_recompress_add_to_page(zsbt_recompress_context *cxt, ZSBtreeItem *item)
{
	if (PageGetFreeSpace(cxt->currpage) < MAXALIGN(item->t_size))
		zsbt_recompress_newpage(cxt, item->t_tid);

	if (PageAddItemExtended(cxt->currpage,
							(Item) item, item->t_size,
							PageGetMaxOffsetNumber(cxt->currpage) + 1,
							PAI_OVERWRITE) == InvalidOffsetNumber)
		elog(ERROR, "could not add item to page while recompressing");

	cxt->total_items++;
}

static bool
zsbt_recompress_add_to_compressor(zsbt_recompress_context *cxt, ZSUncompressedBtreeItem *item)
{
	bool		result;

	if (cxt->compressed_items == 0)
		zs_compress_begin(&cxt->compressor, PageGetFreeSpace(cxt->currpage));

	result = zs_compress_add(&cxt->compressor, item);
	if (result)
	{
		cxt->compressed_items++;

		cxt->total_compressed_items++;
	}

	return result;
}

static void
zsbt_recompress_flush(zsbt_recompress_context *cxt)
{
	ZSCompressedBtreeItem *citem;

	if (cxt->compressed_items == 0)
		return;

	citem = zs_compress_finish(&cxt->compressor);

	zsbt_recompress_add_to_page(cxt, (ZSBtreeItem *) citem);
	cxt->compressed_items = 0;
}

/*
 * Rewrite a leaf page, with given 'items' as the new content.
 *
 * If there are any uncompressed items in the list, we try to compress them.
 * Any already-compressed items are added as is.
 *
 * If the items no longer fit on the page, then the page is split. It is
 * entirely possible that they don't fit even on two pages; we split the page
 * into as many pages as needed. Hopefully not more than a few pages, though,
 * because otherwise you might hit limits on the number of buffer pins (with
 * tiny shared_buffers).
 *
 * On entry, 'oldbuf' must be pinned and exclusive-locked. On exit, the lock
 * is released, but it's still pinned.
 */
static void
zsbt_recompress_replace(Relation rel, AttrNumber attno, Buffer oldbuf, List *items)
{
	ListCell   *lc;
	ListCell   *lc2;
	zsbt_recompress_context cxt;
	ZSBtreePageOpaque *oldopaque = ZSBtreePageGetOpaque(BufferGetPage(oldbuf));
	ZSUndoRecPtr recent_oldest_undo = { 0 };
	List	   *bufs;
	int			i;
	BlockNumber orignextblk;

	cxt.currpage = NULL;
	zs_compress_init(&cxt.compressor);
	cxt.compressed_items = 0;
	cxt.pages = NIL;
	cxt.attno = attno;
	cxt.hikey = oldopaque->zs_hikey;

	cxt.total_items = 0;
	cxt.total_compressed_items = 0;
	cxt.total_already_compressed_items = 0;

	zsbt_recompress_newpage(&cxt, oldopaque->zs_lokey);

	foreach(lc, items)
	{
		ZSBtreeItem *item = (ZSBtreeItem *) lfirst(lc);

		/* We can leave out any old-enough DEAD items */
		if ((item->t_flags & ZSBT_DEAD) != 0)
		{
			ZSUncompressedBtreeItem *uitem = (ZSUncompressedBtreeItem *) item;

			if (recent_oldest_undo.counter == 0)
				recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);

			if (uitem->t_undo_ptr.counter < recent_oldest_undo.counter)
				continue;
		}

		if ((item->t_flags & ZSBT_COMPRESSED) != 0)
		{
			/* already compressed, add as it is. */
			zsbt_recompress_flush(&cxt);
			cxt.total_already_compressed_items++;
			zsbt_recompress_add_to_page(&cxt, item);
		}
		else
		{
			/* try to add this item to the compressor */
			ZSUncompressedBtreeItem *uitem = (ZSUncompressedBtreeItem *) item;

			if (!zsbt_recompress_add_to_compressor(&cxt, uitem))
			{
				if (cxt.compressed_items > 0)
				{
					/* flush, and retry */
					zsbt_recompress_flush(&cxt);

					if (!zsbt_recompress_add_to_compressor(&cxt, uitem))
					{
						/* could not compress, even on its own. Store it uncompressed, then */
						zsbt_recompress_add_to_page(&cxt, item);
					}
				}
				else
				{
					/* could not compress, even on its own. Store it uncompressed, then */
					zsbt_recompress_add_to_page(&cxt, item);
				}
			}
		}
	}

	/* flush the last one, if any */
	zsbt_recompress_flush(&cxt);

	zs_compress_free(&cxt.compressor);

	/*
	 * Ok, we now have a list of pages, to replace the original page, as private
	 * in-memory copies. Allocate buffers for them, and write them out
	 *
	 * allocate all the pages before entering critical section, so that
	 * out-of-disk-space doesn't lead to PANIC
	 */
	bufs = list_make1_int(oldbuf);
	for (i = 0; i < list_length(cxt.pages) - 1; i++)
	{
		Buffer		newbuf = zs_getnewbuf(rel);

		bufs = lappend_int(bufs, newbuf);
	}

	START_CRIT_SECTION();

	orignextblk = oldopaque->zs_next;
	forboth(lc, cxt.pages, lc2, bufs)
	{
		Page		page_copy = (Page) lfirst(lc);
		Buffer		buf = (Buffer) lfirst_int(lc2);
		Page		page = BufferGetPage(buf);
		ZSBtreePageOpaque *opaque;

		PageRestoreTempPage(page_copy, page);
		opaque = ZSBtreePageGetOpaque(page);

		/* TODO: WAL-log */
		if (lnext(lc2))
		{
			Buffer		nextbuf = (Buffer) lfirst_int(lnext(lc2));

			opaque->zs_next = BufferGetBlockNumber(nextbuf);
			opaque->zs_flags |= ZS_FOLLOW_RIGHT;
		}
		else
		{
			/* last one in the chain. */
			opaque->zs_next = orignextblk;
		}

		MarkBufferDirty(buf);
	}
	list_free(cxt.pages);

	END_CRIT_SECTION();

	/* If we had to split, insert downlinks for the new pages. */
	while (list_length(bufs) > 1)
	{
		Buffer		leftbuf = (Buffer) linitial_int(bufs);
		Buffer		rightbuf = (Buffer) lsecond_int(bufs);

		zsbt_insert_downlink(rel, attno, leftbuf,
							 ZSBtreePageGetOpaque(BufferGetPage(leftbuf))->zs_hikey,
							 BufferGetBlockNumber(rightbuf));
		/* zsbt_insert_downlink() released leftbuf */
		bufs = list_delete_first(bufs);
	}
	/* release the last page */
	UnlockReleaseBuffer((Buffer) linitial_int(bufs));
	list_free(bufs);
}

static int
zsbt_binsrch_internal(zstid key, ZSBtreeInternalPageItem *arr, int arr_elems)
{
	int			low,
		high,
		mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (key >= arr[mid].tid)
			low = mid + 1;
		else
			high = mid;
	}
	return low - 1;
}
