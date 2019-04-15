/*-------------------------------------------------------------------------
 *
 * zedstoream_handler.c
 *	  ZedStore table access method code
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstoream_handler.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"

#include "access/heapam.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/zedstore_internal.h"
#include "access/zedstore_undo.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "optimizer/plancat.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/rel.h"


typedef enum
{
	ZSSCAN_STATE_UNSTARTED,
	ZSSCAN_STATE_SCANNING,
	ZSSCAN_STATE_FINISHED_RANGE,
	ZSSCAN_STATE_FINISHED
} zs_scan_state;

typedef struct ZedStoreDescData
{
	/* scan parameters */
	TableScanDescData rs_scan;  /* */
	int		   *proj_atts;
	ZSBtreeScan *btree_scans;
	int			num_proj_atts;
	bool        *project_columns;

	zs_scan_state state;
	zstid		cur_range_start;
	zstid		cur_range_end;
	bool		finished;

	MemoryContext context;

	/* These fields are used for bitmap scans, to hold a "block's" worth of data */
#define	MAX_ITEMS_PER_LOGICAL_BLOCK		MaxHeapTuplesPerPage
	int			bmscan_ntuples;
	zstid	   *bmscan_tids;
	Datum	  **bmscan_datums;
	bool	  **bmscan_isnulls;
	int			bmscan_nexttuple;

} ZedStoreDescData;

typedef struct ZedStoreDescData *ZedStoreDesc;

typedef struct ZedStoreIndexFetchData
{
	IndexFetchTableData idx_fetch_data;
	int		   *proj_atts;
	int			num_proj_atts;
} ZedStoreIndexFetchData;

typedef struct ZedStoreIndexFetchData *ZedStoreIndexFetch;

typedef struct ParallelZSScanDescData *ParallelZSScanDesc;

static bool zedstoream_fetch_row(Relation rel,
							 ItemPointer tid_p,
							 Snapshot snapshot,
							 TupleTableSlot *slot,
							 int num_proj_atts,
							 int *proj_atts);

static Size zs_parallelscan_estimate(Relation rel);
static Size zs_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
static void zs_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
static bool zs_parallelscan_nextrange(Relation rel, ParallelZSScanDesc pzscan,
									  zstid *start, zstid *end);


/* ----------------------------------------------------------------
 *				storage AM support routines for zedstoream
 * ----------------------------------------------------------------
 */

static bool
zedstoream_fetch_row_version(Relation rel,
							 ItemPointer tid_p,
							 Snapshot snapshot,
							 TupleTableSlot *slot)
{
	return zedstoream_fetch_row(rel, tid_p, snapshot, slot, 0, NULL);
}

static void
zedstoream_get_latest_tid(Relation relation,
							 Snapshot snapshot,
							 ItemPointer tid)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s not implemented yet", __func__)));
}

static void
zedstoream_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
				  int options, struct BulkInsertStateData *bistate)
{
	AttrNumber	attno;
	Datum	   *d;
	bool	   *isnulls;
	zstid		tid;
	ZSUndoRecPtr undorecptr;
	TransactionId xid = GetCurrentTransactionId();

	if (slot->tts_tupleDescriptor->natts == 0)
		elog(ERROR, "zero-column tables not supported in zedstore yet");
	if (slot->tts_tupleDescriptor->natts != relation->rd_att->natts)
		elog(ERROR, "slot's attribute count doesn't match relcache entry");

	slot_getallattrs(slot);
	d = slot->tts_values;
	isnulls = slot->tts_isnull;

	tid = InvalidZSTid;
	ZSUndoRecPtrInitialize(&undorecptr);
	for (attno = 1; attno <= relation->rd_att->natts; attno++)
	{
		Form_pg_attribute attr = &slot->tts_tupleDescriptor->attrs[attno - 1];
		Datum		datum = d[attno - 1];
		bool		isnull = isnulls[attno - 1];
		Datum		toastptr = (Datum) 0;

		/* If this datum is too large, toast it */
		if (!isnull && attr->attlen < 0 &&
			VARSIZE_ANY_EXHDR(datum) > MaxZedStoreDatumSize)
		{
			toastptr = datum = zedstore_toast_datum(relation, attno, datum);
		}

		zsbt_multi_insert(relation, attno,
						  &datum, &isnull, &tid, 1,
						  xid, cid, &undorecptr);

		if (toastptr != (Datum) 0)
			zedstore_toast_finish(relation, attno, toastptr, tid);
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = ItemPointerFromZSTid(tid);
}

static void
zedstoream_insert_speculative(Relation relation, TupleTableSlot *slot, CommandId cid,
							  int options, BulkInsertState bistate, uint32 specToken)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s not implemented yet", __func__)));
}

static void
zedstoream_complete_speculative(Relation relation, TupleTableSlot *slot, uint32 spekToken,
								bool succeeded)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s not implemented yet", __func__)));
}

static void
zedstoream_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
						CommandId cid, int options, BulkInsertState bistate)
{
	AttrNumber	attno;
	ZSUndoRecPtr undorecptr;
	int			i;
	bool		slotgetandset = true;
	TransactionId xid = GetCurrentTransactionId();
	int		   *tupletoasted;
	Datum	   *datums;
	bool	   *isnulls;
	zstid	   *tids;

	if (relation->rd_att->natts == 0)
		elog(ERROR, "zero-column tables not supported in zedstore yet");

	tupletoasted = palloc(ntuples * sizeof(int));
	datums = palloc(ntuples * sizeof(Datum));
	isnulls = palloc(ntuples * sizeof(bool));
	tids = palloc(ntuples * sizeof(zstid));

	ZSUndoRecPtrInitialize(&undorecptr);

	for (attno = 1; attno <= relation->rd_att->natts; attno++)
	{
		Form_pg_attribute attr = &(slots[0])->tts_tupleDescriptor->attrs[attno - 1];
		int			ntupletoasted = 0;

		for (i = 0; i < ntuples; i++)
		{
			Datum		datum = slots[i]->tts_values[attno - 1];
			bool		isnull = slots[i]->tts_isnull[attno - 1];

			if (slotgetandset)
			{
				slot_getallattrs(slots[i]);
				tids[i] = InvalidZSTid;
			}

			/* If this datum is too large, toast it */
			if (!isnull && attr->attlen < 0 &&
				VARSIZE_ANY_EXHDR(datum) > MaxZedStoreDatumSize)
			{
				datum = zedstore_toast_datum(relation, attno, datum);
				tupletoasted[ntupletoasted++] = i;
			}
			datums[i] = datum;
			isnulls[i] = isnull;
		}

		zsbt_multi_insert(relation, attno,
						  datums, isnulls, tids, ntuples,
						  xid, cid, &undorecptr);

		for (i = 0; i < ntupletoasted; i++)
		{
			int		idx = tupletoasted[i];

			zedstore_toast_finish(relation, attno, datums[idx], tids[idx]);
		}

		slotgetandset = false;
	}

	for (i = 0; i < ntuples; i++)
	{
		slots[i]->tts_tableOid = RelationGetRelid(relation);
		slots[i]->tts_tid = ItemPointerFromZSTid(tids[i]);
	}

	pfree(tids);
	pfree(tupletoasted);
	pfree(datums);
	pfree(isnulls);
}

static TM_Result
zedstoream_delete(Relation relation, ItemPointer tid_p, CommandId cid,
				  Snapshot snapshot, Snapshot crosscheck, bool wait,
				  TM_FailureData *hufd, bool changingPart)
{
	zstid		tid = ZSTidFromItemPointer(*tid_p);
	TransactionId xid = GetCurrentTransactionId();
	AttrNumber	attno;
	TM_Result result = TM_Ok;

retry:
	for (attno = 1; attno <= relation->rd_att->natts; attno++)
	{
		result = zsbt_delete(relation, attno, tid, xid, cid,
							 snapshot, crosscheck, wait, hufd, changingPart);
		if (result != TM_Ok)
			break;
	}

	if (result != TM_Ok)
	{
		if (attno != 1)
		{
			/* failed to delete this attribute, but we might already have
			 * deleted other attributes. */
			elog(ERROR, "could not delete all columns of row");
		}

		if (result == TM_Invisible)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("attempted to delete invisible tuple")));
		else if (result == TM_BeingModified && wait)
		{
			TransactionId	xwait = hufd->xmax;

			/* TODO: use something like heap_acquire_tuplock() for priority */
			if (!TransactionIdIsCurrentTransactionId(xwait))
			{
				XactLockTableWait(xwait, relation, tid_p, XLTW_Delete);
				goto retry;
			}
		}
	}

	return result;
}


static TM_Result
zedstoream_lock_tuple(Relation relation, ItemPointer tid_p, Snapshot snapshot,
					  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					  LockWaitPolicy wait_policy, uint8 flags,
					  TM_FailureData *hufd)
{
	zstid		tid = ZSTidFromItemPointer(*tid_p);
	TransactionId xid = GetCurrentTransactionId();
	TM_Result result;

	/*
	 * For now, we lock just the first attribute. As long as everyone
	 * does that, that's enough.
	 */
	result = zsbt_lock_item(relation, 1 /* attno */, tid, xid, cid,
							snapshot, mode, wait_policy, hufd);

	if (result != TM_Ok)
	{
		if (result == TM_Invisible)
		{
			/*
			 * This is possible, but only when locking a tuple for ON CONFLICT
			 * UPDATE.  We return this value here rather than throwing an error in
			 * order to give that case the opportunity to throw a more specific
			 * error.
			 */
		}
		else if (result == TM_BeingModified ||
				 result == TM_Updated ||
				 result == TM_Deleted)
		{
			elog(ERROR, "tuple-lock conflict handling not implemented yet");
		}

		/* TODO: do we need to fill in the slot if we fail to lock? */
		return result;
	}

	/* Fetch the tuple, too. */
	if (!zedstoream_fetch_row(relation, tid_p, snapshot, slot, 0, NULL))
		elog(ERROR, "could not fetch locked tuple");

	return TM_Ok;
}


static TM_Result
zedstoream_update(Relation relation, ItemPointer otid_p, TupleTableSlot *slot,
				  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
				  bool wait, TM_FailureData *hufd,
				  LockTupleMode *lockmode, bool *update_indexes)
{
	zstid		otid = ZSTidFromItemPointer(*otid_p);
	TransactionId xid = GetCurrentTransactionId();
	AttrNumber	attno;
	Datum	   *d;
	bool	   *isnulls;
	TM_Result	result;
	zstid		newtid;

	slot_getallattrs(slot);
	d = slot->tts_values;
	isnulls = slot->tts_isnull;

	/*
	 * TODO: Since we have visibility information on each column, we could skip
	 * updating columns whose value didn't change.
	 */
retry:
	result = TM_Ok;
	newtid = InvalidZSTid;
	for (attno = 1; attno <= relation->rd_att->natts; attno++)
	{
		Form_pg_attribute attr = &relation->rd_att->attrs[attno - 1];
		Datum		newdatum = d[attno - 1];
		bool		newisnull = isnulls[attno - 1];
		Datum		toastptr = (Datum) 0;

		/* If this datum is too large, toast it */
		if (!newisnull && attr->attlen < 0 &&
			VARSIZE_ANY_EXHDR(newdatum) > MaxZedStoreDatumSize)
		{
			toastptr = newdatum = zedstore_toast_datum(relation, attno, newdatum);
		}

		result = zsbt_update(relation, attno, otid, newdatum, newisnull,
							 xid, cid, snapshot, crosscheck,
							 wait, hufd, &newtid);

		if (result != TM_Ok)
			break;

		if (toastptr != (Datum) 0)
			zedstore_toast_finish(relation, attno, toastptr, newtid);
	}

	if (result != TM_Ok)
	{
		if (attno != 1)
		{
			/* failed to delete this attribute, but we might already have
			 * deleted other attributes. */
			elog(ERROR, "could not delete all columns of row");
		}

		if (result == TM_Invisible)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("attempted to update invisible tuple")));
		else if (result == TM_BeingModified && wait)
		{
			TransactionId	xwait = hufd->xmax;

			/* TODO: use something like heap_acquire_tuplock() for priority */
			if (!TransactionIdIsCurrentTransactionId(xwait))
			{
				XactLockTableWait(xwait, relation, otid_p, XLTW_Delete);
				goto retry;
			}
		}
	}
	else
		slot->tts_tid = ItemPointerFromZSTid(newtid);

	/* TODO: could we do HOT udates? */
	/* TODO: What should we set lockmode to? */

	return result;
}

static const TupleTableSlotOps *
zedstoream_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

static void
zs_initialize_proj_attributes(ZedStoreDesc scan, int natts)
{
	if (scan->num_proj_atts == 0)
	{
		Relation rel = scan->rs_scan.rs_rd;
		bool validcolumnstoscan = false;

		/*
		 * convert booleans array into an array of the attribute numbers of the
		 * required columns.
		 */
		for (int i = 0; i < natts; i++)
		{
			/* project_columns empty also conveys need all the columns */
			if (scan->project_columns == NULL || scan->project_columns[i])
			{
				scan->proj_atts[scan->num_proj_atts++] = i;

				if (!rel->rd_att->attrs[i].attisdropped &&
					!rel->rd_att->attrs[i].atthasmissing)
					validcolumnstoscan = true;
			}
		}

		/*
		 * Just based on dropped columns or columns with missing values, it's
		 * impossible to know how many tuples are present in the table. Hence,
		 * need at least a valid column (not dropped and does not contain
		 * missing values) to know what all tuples (means TIDs) are present in
		 * the table for which datums must be returned. Below logic hence
		 * tries to add at least one valid column to project list for scanning
		 * if not present already.
		 *
		 * Note: Ideally seems this part is better handled in planner. It can
		 * decide which column to add to project list and also which column to
		 * scan based on cost to scan the column. AM layer having this
		 * intelligence seems little odd.
		 */
		if (!validcolumnstoscan)
		{
			for (int i = 0; i < rel->rd_att->natts; i++)
			{
				if (!rel->rd_att->attrs[i].attisdropped &&
					!rel->rd_att->attrs[i].atthasmissing)
				{
					scan->proj_atts[scan->num_proj_atts++] = i;
					validcolumnstoscan = true;
					break;
				}
			}

			if (!validcolumnstoscan)
				elog(ERROR,
					 "zedstore does not support scanning tables composed entirely of dropped and or missing values");
		}

		/* Extra setup for bitmap and sample scans */
		if (scan->rs_scan.rs_bitmapscan || scan->rs_scan.rs_samplescan)
		{
			scan->bmscan_ntuples = 0;
			scan->bmscan_tids = palloc(MAX_ITEMS_PER_LOGICAL_BLOCK * sizeof(zstid));

			scan->bmscan_datums = palloc(scan->num_proj_atts * sizeof(Datum *));
			scan->bmscan_isnulls = palloc(scan->num_proj_atts * sizeof(bool *));
			for (int i = 0; i < scan->num_proj_atts; i++)
			{
				scan->bmscan_datums[i] = palloc(MAX_ITEMS_PER_LOGICAL_BLOCK * sizeof(Datum));
				scan->bmscan_isnulls[i] = palloc(MAX_ITEMS_PER_LOGICAL_BLOCK * sizeof(bool));
			}
		}
	}
}

static TableScanDesc
zedstoream_beginscan_with_column_projection(Relation relation, Snapshot snapshot,
											int nkeys, ScanKey key,
											ParallelTableScanDesc parallel_scan,
											bool *project_columns,
											bool allow_strat,
											bool allow_sync,
											bool allow_pagemode,
											bool is_bitmapscan,
											bool is_samplescan,
											bool temp_snap)
{
	ZedStoreDesc scan;

	/* Sample scans have no snapshot, but we need one */
	if (!snapshot)
	{
		Assert(is_samplescan);
		snapshot = SnapshotAny;
	}

	/*
	 * allocate and initialize scan descriptor
	 */
	scan = (ZedStoreDesc) palloc(sizeof(ZedStoreDescData));

	scan->rs_scan.rs_rd = relation;
	scan->rs_scan.rs_snapshot = snapshot;
	scan->rs_scan.rs_nkeys = nkeys;
	scan->rs_scan.rs_bitmapscan = is_bitmapscan;
	scan->rs_scan.rs_samplescan = is_samplescan;
	scan->rs_scan.rs_allow_strat = allow_strat;
	scan->rs_scan.rs_allow_sync = allow_sync;
	scan->rs_scan.rs_temp_snap = temp_snap;
	scan->rs_scan.rs_parallel = parallel_scan;

	scan->context = CurrentMemoryContext;

	/*
	 * we can use page-at-a-time mode if it's an MVCC-safe snapshot
	 */
	scan->rs_scan.rs_pageatatime = allow_pagemode && snapshot && IsMVCCSnapshot(snapshot);

	scan->state = ZSSCAN_STATE_UNSTARTED;

	/*
	 * we do this here instead of in initscan() because heap_rescan also calls
	 * initscan() and we don't want to allocate memory again
	 */
	if (nkeys > 0)
		scan->rs_scan.rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->rs_scan.rs_key = NULL;

	scan->proj_atts = palloc(relation->rd_att->natts * sizeof(int));
	scan->project_columns = project_columns;

	scan->btree_scans = palloc0(relation->rd_att->natts * sizeof(ZSBtreeScan));
	scan->num_proj_atts = 0;

	return (TableScanDesc) scan;
}

static TableScanDesc
zedstoream_beginscan(Relation relation, Snapshot snapshot,
					 int nkeys, ScanKey key,
					 ParallelTableScanDesc parallel_scan,
					 bool allow_strat,
					 bool allow_sync,
					 bool allow_pagemode,
					 bool is_bitmapscan,
					 bool is_samplescan,
					 bool temp_snap)
{
	return zedstoream_beginscan_with_column_projection(relation, snapshot, nkeys, key, parallel_scan,
													   NULL, allow_strat, allow_sync, allow_pagemode,
													   is_bitmapscan, is_samplescan, temp_snap);
}

static void
zedstoream_endscan(TableScanDesc sscan)
{
	ZedStoreDesc scan = (ZedStoreDesc) sscan;

	if (scan->proj_atts)
		pfree(scan->proj_atts);

	for (int i = 0; i < sscan->rs_rd->rd_att->natts; i++)
		zsbt_end_scan(&scan->btree_scans[i]);

	if (scan->rs_scan.rs_temp_snap)
		UnregisterSnapshot(scan->rs_scan.rs_snapshot);

	pfree(scan->btree_scans);
	pfree(scan);
}

static void
zedstoream_rescan(TableScanDesc sscan, struct ScanKeyData *key,
				  bool set_params, bool allow_strat,
				  bool allow_sync, bool allow_pagemode)
{
	ZedStoreDesc scan = (ZedStoreDesc) sscan;

	/* these params don't do much in zedstore yet, but whatever */
	if (set_params)
	{
		scan->rs_scan.rs_allow_strat = allow_strat;
		scan->rs_scan.rs_allow_sync = allow_sync;
		scan->rs_scan.rs_pageatatime =
			allow_pagemode && IsMVCCSnapshot(scan->rs_scan.rs_snapshot);
	}

	for (int i = 0; i < scan->num_proj_atts; i++)
	{
		zsbt_end_scan(&scan->btree_scans[i]);
	}
	scan->state = ZSSCAN_STATE_UNSTARTED;
}

static bool
zedstoream_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ZedStoreDesc scan = (ZedStoreDesc) sscan;
	int			i;

	if (slot->tts_tupleDescriptor->natts == 0)
		elog(ERROR, "zero-column tables not supported in zedstore yet");

	zs_initialize_proj_attributes(scan, slot->tts_tupleDescriptor->natts);

	if (scan->num_proj_atts > slot->tts_tupleDescriptor->natts)
	{
		/*
		 * FIXME: This actually happens sometimes, during DROP COLUMN. When no
		 * column list was given, zedstore_beginscan creates it from the
		 * relation's descriptor, which is out of sync with the slot.
		 */
		elog(ERROR, "scan has more projected attributes than slot");
	}

	/*
	 * Initialize the slot.
	 *
	 * We initialize all columns to NULL. The values for columns that are projected
	 * will be set to the actual values below, but it's important that non-projected
	 * columns are NULL.
	 */
	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	for (i = 0; i < slot->tts_tupleDescriptor->natts; i++)
		slot->tts_isnull[i] = true;

	while (scan->state != ZSSCAN_STATE_FINISHED)
	{
		zstid		this_tid;

		if (scan->state == ZSSCAN_STATE_UNSTARTED ||
			scan->state == ZSSCAN_STATE_FINISHED_RANGE)
		{
			if (scan->rs_scan.rs_parallel)
			{
				/* Allocate next range of TIDs to scan */
				if (!zs_parallelscan_nextrange(scan->rs_scan.rs_rd,
											   (ParallelZSScanDesc) scan->rs_scan.rs_parallel,
											   &scan->cur_range_start, &scan->cur_range_end))
				{
					scan->state = ZSSCAN_STATE_FINISHED;
					break;
				}
			}
			else
			{
				if (scan->state == ZSSCAN_STATE_FINISHED_RANGE)
				{
					scan->state = ZSSCAN_STATE_FINISHED;
					break;
				}
				scan->cur_range_start = MinZSTid;
				scan->cur_range_end = MaxPlusOneZSTid;
			}

			MemoryContextSwitchTo(scan->context);
			for (int i = 0; i < scan->num_proj_atts; i++)
			{
				int			natt = scan->proj_atts[i];

				zsbt_begin_scan(scan->rs_scan.rs_rd, natt + 1,
								scan->cur_range_start,
								scan->rs_scan.rs_snapshot,
								&scan->btree_scans[i]);
			}
			MemoryContextSwitchTo(oldcontext);
			scan->state = ZSSCAN_STATE_SCANNING;
		}

		/* We now have a range to scan */
		Assert(scan->state == ZSSCAN_STATE_SCANNING);
		this_tid = InvalidZSTid;
		for (int i = 0; i < scan->num_proj_atts; i++)
		{
			ZSBtreeScan	*btscan = &scan->btree_scans[i];
			int			natt = scan->proj_atts[i];
			Datum		datum;
			bool        isnull;
			bool        isvaluemissing;
			zstid		tid;

			if (!zsbt_scan_next(btscan, &datum, &isnull, &tid, &isvaluemissing))
			{
				scan->state = ZSSCAN_STATE_FINISHED_RANGE;
				break;
			}

			if (isvaluemissing)
			{
				slot->tts_values[natt] = datum;
				slot->tts_isnull[natt] = isnull;
				continue;
			}

			if (tid >= scan->cur_range_end)
			{
				scan->state = ZSSCAN_STATE_FINISHED_RANGE;
				break;
			}

			if (this_tid == InvalidZSTid)
				this_tid = tid;
			else if (this_tid != tid)
			{
				elog(ERROR, "scans on different attributes out of sync");
			}

			/*
			 * flatten any ZS-TOASTed values, becaue the rest of the system
			 * doesn't know how to deal with them.
			 */
			if (!isnull && btscan->attlen == -1 &&
				VARATT_IS_EXTERNAL(datum) && VARTAG_EXTERNAL(datum) == VARTAG_ZEDSTORE)
			{
				datum = zedstore_toast_flatten(scan->rs_scan.rs_rd, natt + 1, tid, datum);
			}

			slot->tts_values[natt] = datum;
			slot->tts_isnull[natt] = isnull;
		}

		if (scan->state == ZSSCAN_STATE_FINISHED_RANGE)
		{
			for (int i = 0; i < scan->num_proj_atts; i++)
			{
				int			natt = scan->proj_atts[i];

				zsbt_end_scan(&scan->btree_scans[natt]);
			}
		}
		else
		{
			Assert(scan->state == ZSSCAN_STATE_SCANNING);
			slot->tts_tid = ItemPointerFromZSTid(this_tid);
			slot->tts_nvalid = slot->tts_tupleDescriptor->natts;
			slot->tts_flags &= ~TTS_FLAG_EMPTY;
			return true;
		}
	}

	ExecClearTuple(slot);
	return false;
}

static bool
zedstoream_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
									Snapshot snapshot)
{
	/*
	 * TODO: we didn't keep any visibility information about the tuple in the
	 * slot, so we have to fetch it again. A custom slot type might be a
	 * good idea..
	 */
	zstid		tid = ZSTidFromItemPointer(slot->tts_tid);
	zstid		ftid;
	ZSBtreeScan btree_scan;
	bool		found;
	Datum		datum;
	bool		isnull;
	bool        isvaluemissing;

	/* Use the first column for the visibility information. */
	zsbt_begin_scan(rel, 1, tid, snapshot, &btree_scan);

	found = zsbt_scan_next(&btree_scan, &datum, &isnull, &ftid, &isvaluemissing);
	Assert(!isvaluemissing);

	if (found && tid != ftid)
		found = false;

	zsbt_end_scan(&btree_scan);

	return found;
}

static TransactionId
zedstoream_compute_xid_horizon_for_tuples(Relation rel,
										  ItemPointerData *items,
										  int nitems)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s not implemented yet", __func__)));

}

static IndexFetchTableData *
zedstoream_begin_index_fetch(Relation rel)
{
	ZedStoreIndexFetch zscan = palloc0(sizeof(ZedStoreIndexFetchData));

	zscan->idx_fetch_data.rel = rel;

	zscan->proj_atts = NULL;
	zscan->num_proj_atts = 0;

	return (IndexFetchTableData *) zscan;
}

static void
zedstoream_fetch_set_column_projection(struct IndexFetchTableData *scan,
									   bool *project_column)
{
	ZedStoreIndexFetch zscan = (ZedStoreIndexFetch)scan;
	Relation	rel = zscan->idx_fetch_data.rel;

	zscan->proj_atts = palloc(rel->rd_att->natts * sizeof(int));
	zscan->num_proj_atts = 0;

	/*
	 * convert booleans array into an array of the attribute numbers of the
	 * required columns.
	 */
	for (int i = 0; i < rel->rd_att->natts; i++)
	{
		/* if project_columns is empty means need all the columns */
		if (project_column == NULL || project_column[i])
		{
			zscan->proj_atts[zscan->num_proj_atts++] = i;
		}
	}
}

static void
zedstoream_reset_index_fetch(IndexFetchTableData *scan)
{
}

static void
zedstoream_end_index_fetch(IndexFetchTableData *scan)
{
	ZedStoreIndexFetch zscan = (ZedStoreIndexFetch) scan;

	if (zscan->proj_atts)
		pfree(zscan->proj_atts);
	pfree(zscan);
}

static bool
zedstoream_index_fetch_tuple(struct IndexFetchTableData *scan,
							 ItemPointer tid_p,
							 Snapshot snapshot,
							 TupleTableSlot *slot,
							 bool *call_again, bool *all_dead)
{
	ZedStoreIndexFetch zscan = (ZedStoreIndexFetch) scan;

	/*
	 * we don't do in-place updates, so this is essentially the same as
	 * fetch_row_version.
	 */
	if (call_again)
		*call_again = false;
	if (all_dead)
		*all_dead = false;
	return zedstoream_fetch_row(scan->rel, tid_p, snapshot, slot,
								zscan->num_proj_atts, zscan->proj_atts);
}

/*
 * Shared implementation of fetch_row_version and index_fetch_tuple callbacks.
 */
static bool
zedstoream_fetch_row(Relation rel,
					 ItemPointer tid_p,
					 Snapshot snapshot,
					 TupleTableSlot *slot,
					 int num_proj_atts,
					 int *proj_atts)
{
	zstid		tid = ZSTidFromItemPointer(*tid_p);
	bool		found = true;

	/*
	 * Initialize the slot.
	 *
	 * If we're not fetching all columns, initialize the unfetched values
	 * in the slot to NULL. (Actually, this initializes all to NULL, and the
	 * code below will overwrite them for the columns that are projected)
	 */
	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	if (proj_atts)
	{
		for (int i = 0; i < slot->tts_tupleDescriptor->natts; i++)
			slot->tts_isnull[i] = true;
	}
	else
		num_proj_atts = slot->tts_tupleDescriptor->natts;

	for (int i = 0; i < num_proj_atts && found; i++)
	{
		int         natt = proj_atts ? proj_atts[i] : i;
		Form_pg_attribute att = &rel->rd_att->attrs[natt];
		ZSBtreeScan btree_scan;
		Datum		datum;
		bool        isnull;
		bool        isvaluemissing;
		zstid		this_tid;

		if (att->attisdropped)
		{
			slot->tts_values[natt] = (Datum) 0;
			slot->tts_isnull[natt] = true;
			continue;
		}

		zsbt_begin_scan(rel, natt + 1, tid, snapshot, &btree_scan);

		if (zsbt_scan_next(&btree_scan, &datum, &isnull, &this_tid, &isvaluemissing))
		{
			if (!isvaluemissing && this_tid != tid)
				found = false;
			else
			{
				/*
				 * flatten any ZS-TOASTed values, becaue the rest of the system
				 * doesn't know how to deal with them.
				 */
				if (!isnull && btree_scan.attlen == -1 &&
					VARATT_IS_EXTERNAL(datum) && VARTAG_EXTERNAL(datum) == VARTAG_ZEDSTORE)
				{
					datum = zedstore_toast_flatten(rel, natt + 1, tid, datum);
				}
				slot->tts_values[natt] = datum;
				slot->tts_isnull[natt] = isnull;
			}
		}
		else
			found = false;

		zsbt_end_scan(&btree_scan);
	}

	if (found)
	{
		slot->tts_tid = ItemPointerFromZSTid(tid);
		slot->tts_nvalid = slot->tts_tupleDescriptor->natts;
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		return true;
	}
	else
	{
		/*
		 * not found
		 *
		 * TODO: as a sanity check, it would be good to check if we
		 * get *any* of the columns. Currently, if any of the columns
		 * is missing, we treat the tuple as non-existent
		 */
		ExecClearTuple(slot);
		return false;
	}
}

static void
zedstoream_index_validate_scan(Relation heapRelation,
							   Relation indexRelation,
							   IndexInfo *indexInfo,
							   Snapshot snapshot,
							   ValidateIndexState *state)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s not implemented yet", __func__)));
}

static double
zedstoream_index_build_range_scan(Relation baseRelation,
								  Relation indexRelation,
								  IndexInfo *indexInfo,
								  bool allow_sync,
								  bool anyvisible,
								  bool progress,
								  BlockNumber start_blockno,
								  BlockNumber numblocks,
								  IndexBuildCallback callback,
								  void *callback_state,
								  TableScanDesc scan)
{
	bool		checking_uniqueness;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	bool		need_unregister_snapshot = false;
	TransactionId OldestXmin;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));

	/* See whether we're verifying uniqueness/exclusion properties */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/*
	 * "Any visible" mode is not compatible with uniqueness checks; make sure
	 * only one of those is requested.
	 */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(baseRelation, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples). In a
	 * concurrent build, or during bootstrap, we take a regular MVCC snapshot
	 * and index whatever's live according to that.
	 */
	OldestXmin = InvalidTransactionId;

	/* okay to ignore lazy VACUUMs here */
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
		OldestXmin = GetOldestXmin(baseRelation, PROCARRAY_FLAGS_VACUUM);

	/*
	 * TODO: It would be very good to fetch only the columns we need.
	 */
	if (!scan)
	{
		bool	   *proj;
		int			attno;

		/*
		 * Serial index build.
		 *
		 * Must begin our own zedstore scan in this case.  We may also need to
		 * register a snapshot whose lifetime is under our direct control.
		 */
		if (!TransactionIdIsValid(OldestXmin))
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;

		proj = palloc0(baseRelation->rd_att->natts * sizeof(bool));
		for (attno = 0; attno < indexInfo->ii_NumIndexKeyAttrs; attno++)
		{
			Assert(indexInfo->ii_IndexAttrNumbers[attno] <= baseRelation->rd_att->natts);
			/* skip expressions */
			if (indexInfo->ii_IndexAttrNumbers[attno] > 0)
				proj[indexInfo->ii_IndexAttrNumbers[attno] - 1] = true;
		}

		GetNeededColumnsForNode((Node *)indexInfo->ii_Expressions, proj,
								baseRelation->rd_att->natts);

		scan = table_beginscan_with_column_projection(baseRelation,	/* relation */
													  snapshot,	/* snapshot */
													  0, /* number of keys */
													  NULL,	/* scan key */
													  proj);

		if (start_blockno != 0 || numblocks != InvalidBlockNumber)
		{
			ZedStoreDesc zscan = (ZedStoreDesc) scan;

			zscan->cur_range_start = ZSTidFromBlkOff(start_blockno, 1);
			zscan->cur_range_end = ZSTidFromBlkOff(numblocks, 1);

			for (int i = 0; i < zscan->num_proj_atts; i++)
			{
				int			natt = zscan->proj_atts[i];

				zsbt_begin_scan(zscan->rs_scan.rs_rd, natt + 1,
								zscan->cur_range_start,
								zscan->rs_scan.rs_snapshot,
								&zscan->btree_scans[i]);
			}
			zscan->state = ZSSCAN_STATE_SCANNING;
		}
	}
	else
	{
		/*
		 * Parallel index build.
		 *
		 * Parallel case never registers/unregisters own snapshot.  Snapshot
		 * is taken from parallel zedstore scan, and is SnapshotAny or an MVCC
		 * snapshot, based on same criteria as serial case.
		 */
		Assert(!IsBootstrapProcessingMode());
		Assert(allow_sync);
		Assert(start_blockno == 0);
		Assert(numblocks == InvalidBlockNumber);
		snapshot = scan->rs_snapshot;
	}

	/*
	 * Must call GetOldestXmin() with SnapshotAny.  Should never call
	 * GetOldestXmin() with MVCC snapshot. (It's especially worth checking
	 * this for parallel builds, since ambuild routines that support parallel
	 * builds must work these details out for themselves.)
	 */
	Assert(snapshot == SnapshotAny || IsMVCCSnapshot(snapshot));
	Assert(snapshot == SnapshotAny ? TransactionIdIsValid(OldestXmin) :
		   !TransactionIdIsValid(OldestXmin));
	Assert(snapshot == SnapshotAny || !anyvisible);

	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		tupleIsAlive;
		HeapTuple	heapTuple;

		if (numblocks != InvalidBlockNumber &&
			ItemPointerGetBlockNumber(&slot->tts_tid) >= numblocks)
			break;

		CHECK_FOR_INTERRUPTS();

		/* table_scan_getnextslot did the visibility check */
		tupleIsAlive = true;
		reltuples += 1;

		/*
		 * TODO: Once we have in-place updates, like HOT, this will need
		 * to work harder, to figure out which tuple version to index.
		 */

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * For the current heap tuple, extract all the attributes we use in
		 * this index, and note which are null.  This also performs evaluation
		 * of any expressions needed.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/* Call the AM's callback routine to process the tuple */
		heapTuple = ExecCopySlotHeapTuple(slot);
		heapTuple->t_self = slot->tts_tid;
		callback(indexRelation, heapTuple, values, isnull, tupleIsAlive,
				 callback_state);
		pfree(heapTuple);
	}

	table_endscan(scan);

	/* we can now forget our snapshot, if set and registered by us */
	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

static void
zedstoream_finish_bulk_insert(Relation relation, int options)
{
	/*
	 * If we skipped writing WAL, then we need to sync the zedstore (but not
	 * indexes since those use WAL anyway / don't go through tableam)
	 */
	if (options & HEAP_INSERT_SKIP_WAL)
		heap_sync(relation);
}

/* ------------------------------------------------------------------------
 * DDL related callbacks for zedstore AM.
 * ------------------------------------------------------------------------
 */

static void
zedstoream_relation_set_new_filenode(Relation rel, char persistence,
									 TransactionId *freezeXid,
									 MultiXactId *minmulti)
{
	/*
	 * Initialize to the minimum XID that could put tuples in the table. We
	 * know that no xacts older than RecentXmin are still running, so that
	 * will do.
	 */
	*freezeXid = RecentXmin;

	/*
	 * Similarly, initialize the minimum Multixact to the first value that
	 * could possibly be stored in tuples in the table.  Running transactions
	 * could reuse values from their local cache, so we are careful to
	 * consider all currently running multis.
	 *
	 * XXX this could be refined further, but is it worth the hassle?
	 */
	*minmulti = GetOldestMultiXactId();

	RelationCreateStorage(rel->rd_node, persistence);

	/*
	 * If required, set up an init fork for an unlogged table so that it can
	 * be correctly reinitialized on restart.  An immediate sync is required
	 * even if the page has been logged, because the write did not go through
	 * shared_buffers and therefore a concurrent checkpoint may have moved the
	 * redo pointer past our xlog record.  Recovery may as well remove it
	 * while replaying, for example, XLOG_DBASE_CREATE or XLOG_TBLSPC_CREATE
	 * record. Therefore, logging is necessary even if wal_level=minimal.
	 */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		RelationOpenSmgr(rel);
		smgrcreate(rel->rd_smgr, INIT_FORKNUM, false);
		log_smgrcreate(&rel->rd_smgr->smgr_rnode.node, INIT_FORKNUM);
		smgrimmedsync(rel->rd_smgr, INIT_FORKNUM);
	}
}

static void
zedstoream_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
}

static void
zedstoream_relation_copy_data(Relation rel, RelFileNode newrnode)
{
	SMgrRelation dstrel;

	dstrel = smgropen(newrnode, rel->rd_backend);
	RelationOpenSmgr(rel);

	/*
	 * Create and copy all the relation, and schedule unlinking of the
	 * old physical file.
	 *
	 * NOTE: any conflict in relfilenode value will be caught in
	 * RelationCreateStorage().
	 *
	 * NOTE: There is only the main fork in zedstore. Otherwise
	 * this would need to copy other forkst, too.
	 */
	RelationCreateStorage(newrnode, rel->rd_rel->relpersistence);

	/* copy main fork */
	RelationCopyStorage(rel->rd_smgr, dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* drop old relation, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
zedstoream_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
									 Relation OldIndex, bool use_sort,
									 TransactionId OldestXmin,
									 TransactionId FreezeXid,
									 MultiXactId MultiXactCutoff,
									 double *num_tuples,
									 double *tups_vacuumed,
									 double *tups_recently_dead)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("function %s not implemented yet", __func__)));
}

/*
 * FIXME: The ANALYZE API is problematic for us. acquire_sample_rows() calls
 * RelationGetNumberOfBlocks() directly on the relation, and chooses the
 * block numbers to sample based on that. But the logical block numbers
 * have little to do with physical ones in zedstore.
 */
static bool
zedstoream_scan_analyze_next_block(TableScanDesc sscan, BlockNumber blockno,
								   BufferAccessStrategy bstrategy)
{
	ZedStoreDesc scan = (ZedStoreDesc) sscan;
	int			ntuples;
	int			first_ntuples = 0;
	bool		firstcol;

	/*
	 * Our strategy for a bitmap scan is to scan the tree of each attribute,
	 * starting at the given logical block number, and store all the datums
	 * in the scan struct. zedstoream_scan_bitmap_next_tuple() then just
	 * needs to store the datums of the next TID in the slot.
	 *
	 * An alternative would be to keep the scans of each attribute open,
	 * like in a sequential scan. I'm not sure which is better.
	 */
	firstcol = true;
	for (int i = 0; i < scan->num_proj_atts; i++)
	{
		int			natt = scan->proj_atts[i];
		ZSBtreeScan	btree_scan;
		Datum		datum;
		bool        isnull;
		bool        isvaluemissing;
		zstid		tid;
		Datum	   *datums = scan->bmscan_datums[natt];
		bool	   *isnulls = scan->bmscan_isnulls[natt];

		zsbt_begin_scan(scan->rs_scan.rs_rd, natt + 1,
						ZSTidFromBlkOff(blockno, 1),
						scan->rs_scan.rs_snapshot,
						&btree_scan);

		/*
		 * TODO: it would be good to pass the next expected TID down to zsbt_scan_next,
		 * so that it could skip over to it more efficiently.
		 */
		ntuples = 0;
		while (zsbt_scan_next(&btree_scan, &datum, &isnull, &tid, &isvaluemissing))
		{
			if (!isvaluemissing && ZSTidGetBlockNumber(tid) != blockno)
			{
				Assert(ZSTidGetBlockNumber(tid) > blockno);
				break;
			}

			datums[ntuples] = datum;
			isnulls[ntuples] = isnull;
			if (firstcol)
				scan->bmscan_tids[ntuples] = tid;
			else if (!isvaluemissing && tid != scan->bmscan_tids[ntuples])
				elog(ERROR, "scans on different attributes out of sync");

			ntuples++;

			/*
			 * need a termination condition for a missing value because it
			 * doesn't know how many tuples it has
			 */
			if (isvaluemissing && (ntuples == first_ntuples))
				break;
		}
		if (firstcol)
			first_ntuples = ntuples;
		else if (ntuples != first_ntuples)
			elog(ERROR, "scans on different attributes out of sync");

		zsbt_end_scan(&btree_scan);

		firstcol = false;
	}

	scan->bmscan_nexttuple = 0;
	scan->bmscan_ntuples = first_ntuples;

	return true;
}

static bool
zedstoream_scan_analyze_next_tuple(TableScanDesc sscan, TransactionId OldestXmin,
								   double *liverows, double *deadrows,
								   TupleTableSlot *slot)
{
	ZedStoreDesc scan = (ZedStoreDesc) sscan;
	zstid		tid;

	if (scan->bmscan_nexttuple >= scan->bmscan_ntuples)
		return false;

	tid = scan->bmscan_tids[scan->bmscan_nexttuple];
	for (int i = 0; i < scan->num_proj_atts; i++)
	{
		Form_pg_attribute att = &scan->rs_scan.rs_rd->rd_att->attrs[i];
		int			natt = scan->proj_atts[i];
		Datum		datum;
		bool        isnull;

		datum = (scan->bmscan_datums[i])[scan->bmscan_nexttuple];
		isnull = (scan->bmscan_isnulls[i])[scan->bmscan_nexttuple];

		/*
		 * flatten any ZS-TOASTed values, becaue the rest of the system
		 * doesn't know how to deal with them.
		 */
		if (!isnull && att->attlen == -1 &&
			VARATT_IS_EXTERNAL(datum) && VARTAG_EXTERNAL(datum) == VARTAG_ZEDSTORE)
		{
			datum = zedstore_toast_flatten(scan->rs_scan.rs_rd, natt + 1, tid, datum);
		}

		slot->tts_values[natt] = datum;
		slot->tts_isnull[natt] = isnull;
	}
	slot->tts_tid = ItemPointerFromZSTid(tid);
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;
	slot->tts_flags &= ~TTS_FLAG_EMPTY;

	scan->bmscan_nexttuple++;
	(*liverows)++;

	return true;
}

/* ------------------------------------------------------------------------
 * Planner related callbacks for the zedstore AM
 * ------------------------------------------------------------------------
 */

/*
 * currently this is exact duplicate of heapam_estimate_rel_size().
 * TODO fix to tune it based on zedstore storage.
 */
static void
zedstoream_relation_estimate_size(Relation rel, int32 *attr_widths,
								  BlockNumber *pages, double *tuples,
								  double *allvisfrac)
{
	BlockNumber curpages;
	BlockNumber relpages;
	double		reltuples;
	BlockNumber relallvisible;
	double		density;

	/* it has storage, ok to call the smgr */
	curpages = RelationGetNumberOfBlocks(rel);

	/* coerce values in pg_class to more desirable types */
	relpages = (BlockNumber) rel->rd_rel->relpages;
	reltuples = (double) rel->rd_rel->reltuples;
	relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

	/*
	 * HACK: if the relation has never yet been vacuumed, use a minimum size
	 * estimate of 10 pages.  The idea here is to avoid assuming a
	 * newly-created table is really small, even if it currently is, because
	 * that may not be true once some data gets loaded into it.  Once a vacuum
	 * or analyze cycle has been done on it, it's more reasonable to believe
	 * the size is somewhat stable.
	 *
	 * (Note that this is only an issue if the plan gets cached and used again
	 * after the table has been filled.  What we're trying to avoid is using a
	 * nestloop-type plan on a table that has grown substantially since the
	 * plan was made.  Normally, autovacuum/autoanalyze will occur once enough
	 * inserts have happened and cause cached-plan invalidation; but that
	 * doesn't happen instantaneously, and it won't happen at all for cases
	 * such as temporary tables.)
	 *
	 * We approximate "never vacuumed" by "has relpages = 0", which means this
	 * will also fire on genuinely empty relations.  Not great, but
	 * fortunately that's a seldom-seen case in the real world, and it
	 * shouldn't degrade the quality of the plan too much anyway to err in
	 * this direction.
	 *
	 * If the table has inheritance children, we don't apply this heuristic.
	 * Totally empty parent tables are quite common, so we should be willing
	 * to believe that they are empty.
	 */
	if (curpages < 10 &&
		relpages == 0 &&
		!rel->rd_rel->relhassubclass)
		curpages = 10;

	/* report estimated # pages */
	*pages = curpages;
	/* quick exit if rel is clearly empty */
	if (curpages == 0)
	{
		*tuples = 0;
		*allvisfrac = 0;
		return;
	}

	/* estimate number of tuples from previous tuple density */
	if (relpages > 0)
		density = reltuples / (double) relpages;
	else
	{
		/*
		 * When we have no data because the relation was truncated, estimate
		 * tuple width from attribute datatypes.  We assume here that the
		 * pages are completely full, which is OK for tables (since they've
		 * presumably not been VACUUMed yet) but is probably an overestimate
		 * for indexes.  Fortunately get_relation_info() can clamp the
		 * overestimate to the parent table's size.
		 *
		 * Note: this code intentionally disregards alignment considerations,
		 * because (a) that would be gilding the lily considering how crude
		 * the estimate is, and (b) it creates platform dependencies in the
		 * default plans which are kind of a headache for regression testing.
		 */
		int32		tuple_width;

		tuple_width = get_rel_data_width(rel, attr_widths);
		tuple_width += MAXALIGN(SizeofHeapTupleHeader);
		tuple_width += sizeof(ItemIdData);
		/* note: integer division is intentional here */
		density = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
	}
	*tuples = rint(density * (double) curpages);

	/*
	 * We use relallvisible as-is, rather than scaling it up like we do for
	 * the pages and tuples counts, on the theory that any pages added since
	 * the last VACUUM are most likely not marked all-visible.  But costsize.c
	 * wants it converted to a fraction.
	 */
	if (relallvisible == 0 || curpages <= 0)
		*allvisfrac = 0;
	else if ((double) relallvisible >= curpages)
		*allvisfrac = 1;
	else
		*allvisfrac = (double) relallvisible / curpages;
}

/* ------------------------------------------------------------------------
 * Executor related callbacks for the zedstore AM
 * ------------------------------------------------------------------------
 */

static bool
zedstoream_scan_bitmap_next_block(TableScanDesc sscan,
								  TBMIterateResult *tbmres)
{
	ZedStoreDesc scan = (ZedStoreDesc) sscan;
	BlockNumber tid_blkno = tbmres->blockno;
	int			ntuples;
	int			first_ntuples = 0;
	bool		firstcol;

	zs_initialize_proj_attributes(scan, scan->rs_scan.rs_rd->rd_att->natts);

	/*
	 * Our strategy for a bitmap scan is to scan the tree of each attribute,
	 * starting at the given logical block number, and store all the datums
	 * in the scan struct. zedstoream_scan_analyze_next_tuple() then just
	 * needs to store the datums of the next TID in the slot.
	 *
	 * An alternative would be to keep the scans of each attribute open,
	 * like in a sequential scan. I'm not sure which is better.
	 */
	firstcol = true;
	for (int i = 0; i < scan->num_proj_atts; i++)
	{
		int			natt = scan->proj_atts[i];
		ZSBtreeScan	btree_scan;
		Datum		datum;
		bool        isnull;
		bool        isvaluemissing;
		zstid		tid;
		Datum	   *datums = scan->bmscan_datums[natt];
		bool	   *isnulls = scan->bmscan_isnulls[natt];
		int			noff = 0;

		zsbt_begin_scan(scan->rs_scan.rs_rd, natt + 1,
						ZSTidFromBlkOff(tid_blkno, 1),
						scan->rs_scan.rs_snapshot,
						&btree_scan);

		/*
		 * TODO: it would be good to pass the next expected TID down to zsbt_scan_next,
		 * so that it could skip over to it more efficiently.
		 */
		ntuples = 0;
		while (zsbt_scan_next(&btree_scan, &datum, &isnull, &tid, &isvaluemissing))
		{
			if (!isvaluemissing)
			{
				if (ZSTidGetBlockNumber(tid) != tid_blkno)
				{
					Assert(ZSTidGetBlockNumber(tid) > tid_blkno);
					break;
				}

				if (tbmres->ntuples != -1)
				{
					while (ZSTidGetOffsetNumber(tid) > tbmres->offsets[noff] && noff < tbmres->ntuples)
						noff++;

					if (noff == tbmres->ntuples)
						break;

					if (ZSTidGetOffsetNumber(tid) < tbmres->offsets[noff])
						continue;
				}
			}

			datums[ntuples] = datum;
			isnulls[ntuples] = isnull;
			if (firstcol)
				scan->bmscan_tids[ntuples] = tid;
			else if (!isvaluemissing && tid != scan->bmscan_tids[ntuples])
				elog(ERROR, "scans on different attributes out of sync");

			ntuples++;
			if (isvaluemissing && ntuples == first_ntuples)
				break;
		}
		if (firstcol)
			first_ntuples = ntuples;
		else if (ntuples != first_ntuples)
			elog(ERROR, "scans on different attributes out of sync");

		zsbt_end_scan(&btree_scan);

		firstcol = false;
	}

	scan->bmscan_nexttuple = 0;
	scan->bmscan_ntuples = first_ntuples;

	return first_ntuples > 0;
}

static bool
zedstoream_scan_bitmap_next_tuple(TableScanDesc sscan,
								  TBMIterateResult *tbmres,
								  TupleTableSlot *slot)
{
	ZedStoreDesc scan = (ZedStoreDesc) sscan;
	zstid		tid;

	if (scan->bmscan_nexttuple >= scan->bmscan_ntuples)
		return false;

	tid = scan->bmscan_tids[scan->bmscan_nexttuple];
	for (int i = 0; i < scan->num_proj_atts; i++)
	{
		Form_pg_attribute att = &scan->rs_scan.rs_rd->rd_att->attrs[i];
		int			natt = scan->proj_atts[i];
		Datum		datum;
		bool        isnull;

		datum = (scan->bmscan_datums[i])[scan->bmscan_nexttuple];
		isnull = (scan->bmscan_isnulls[i])[scan->bmscan_nexttuple];

		/*
		 * flatten any ZS-TOASTed values, becaue the rest of the system
		 * doesn't know how to deal with them.
		 */
		if (!isnull && att->attlen == -1 &&
			VARATT_IS_EXTERNAL(datum) && VARTAG_EXTERNAL(datum) == VARTAG_ZEDSTORE)
		{
			datum = zedstore_toast_flatten(scan->rs_scan.rs_rd, natt + 1, tid, datum);
		}

		slot->tts_values[natt] = datum;
		slot->tts_isnull[natt] = isnull;
	}
	slot->tts_tid = ItemPointerFromZSTid(tid);
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;
	slot->tts_flags &= ~TTS_FLAG_EMPTY;

	scan->bmscan_nexttuple++;

	return true;
}

static bool
zedstoream_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("function %s not implemented yet", __func__)));
	return false;
}

static bool
zedstoream_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
								  TupleTableSlot *slot)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("function %s not implemented yet", __func__)));
	return false;
}

static void
zedstoream_vacuum_rel(Relation onerel, VacuumParams *params,
					  BufferAccessStrategy bstrategy)
{
	zsundo_vacuum(onerel, params, bstrategy,
				  GetOldestXmin(onerel, PROCARRAY_FLAGS_VACUUM));
}

static const TableAmRoutine zedstoream_methods = {
	.type = T_TableAmRoutine,
	.scans_leverage_column_projection = true,

	.slot_callbacks = zedstoream_slot_callbacks,

	.scan_begin = zedstoream_beginscan,
	.scan_begin_with_column_projection = zedstoream_beginscan_with_column_projection,
	.scan_end = zedstoream_endscan,
	.scan_rescan = zedstoream_rescan,
	.scan_getnextslot = zedstoream_getnextslot,

	.parallelscan_estimate = zs_parallelscan_estimate,
	.parallelscan_initialize = zs_parallelscan_initialize,
	.parallelscan_reinitialize = zs_parallelscan_reinitialize,

	.index_fetch_begin = zedstoream_begin_index_fetch,
	.index_fetch_reset = zedstoream_reset_index_fetch,
	.index_fetch_end = zedstoream_end_index_fetch,
	.index_fetch_set_column_projection = zedstoream_fetch_set_column_projection,
	.index_fetch_tuple = zedstoream_index_fetch_tuple,

	.tuple_insert = zedstoream_insert,
	.tuple_insert_speculative = zedstoream_insert_speculative,
	.tuple_complete_speculative = zedstoream_complete_speculative,
	.multi_insert = zedstoream_multi_insert,
	.tuple_delete = zedstoream_delete,
	.tuple_update = zedstoream_update,
	.tuple_lock = zedstoream_lock_tuple,
	.finish_bulk_insert = zedstoream_finish_bulk_insert,

	.tuple_fetch_row_version = zedstoream_fetch_row_version,
	.tuple_get_latest_tid = zedstoream_get_latest_tid,
	.tuple_satisfies_snapshot = zedstoream_tuple_satisfies_snapshot,
	.compute_xid_horizon_for_tuples = zedstoream_compute_xid_horizon_for_tuples,

	.relation_set_new_filenode = zedstoream_relation_set_new_filenode,
	.relation_nontransactional_truncate = zedstoream_relation_nontransactional_truncate,
	.relation_copy_data = zedstoream_relation_copy_data,
	.relation_copy_for_cluster = zedstoream_relation_copy_for_cluster,
	.relation_vacuum = zedstoream_vacuum_rel,
	.scan_analyze_next_block = zedstoream_scan_analyze_next_block,
	.scan_analyze_next_tuple = zedstoream_scan_analyze_next_tuple,

	.index_build_range_scan = zedstoream_index_build_range_scan,
	.index_validate_scan = zedstoream_index_validate_scan,

	.relation_estimate_size = zedstoream_relation_estimate_size,

	.scan_bitmap_next_block = zedstoream_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = zedstoream_scan_bitmap_next_tuple,
	.scan_sample_next_block = zedstoream_scan_sample_next_block,
	.scan_sample_next_tuple = zedstoream_scan_sample_next_tuple
};

Datum
zedstore_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&zedstoream_methods);
}


/*
 * Routines for dividing up the TID range for parallel seq scans
 */

typedef struct ParallelZSScanDescData
{
	ParallelTableScanDescData base;

	zstid		pzs_endtid;		/* last tid + 1 in relation at start of scan */
	pg_atomic_uint64 pzs_allocatedtid_blk;	/* TID space allocated to workers so far. (in  65536 increments) */
} ParallelZSScanDescData;
typedef struct ParallelZSScanDescData *ParallelZSScanDesc;

static Size
zs_parallelscan_estimate(Relation rel)
{
	return sizeof(ParallelZSScanDescData);
}

static Size
zs_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelZSScanDesc zpscan = (ParallelZSScanDesc) pscan;

	zpscan->base.phs_relid = RelationGetRelid(rel);
	/* FIXME: if attribute 1 is dropped, should use another attribute */
	zpscan->pzs_endtid = zsbt_get_last_tid(rel, 1);
	pg_atomic_init_u64(&zpscan->pzs_allocatedtid_blk, 0);

	return sizeof(ParallelZSScanDescData);
}

static void
zs_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelZSScanDesc bpscan = (ParallelZSScanDesc) pscan;

	pg_atomic_write_u64(&bpscan->pzs_allocatedtid_blk, 0);
}

/*
 * get the next TID range to scan
 *
 * Returns true if there is more to scan, false otherwise.
 *
 * Get the next TID range to scan.  Even if there are no TIDs left to scan,
 * another backend could have grabbed a range to scan and not yet finished
 * looking at it, so it doesn't follow that the scan is done when the first
 * backend gets 'false' return.
 */
static bool
zs_parallelscan_nextrange(Relation rel, ParallelZSScanDesc pzscan,
						  zstid *start, zstid *end)
{
	uint64		allocatedtid_blk;

	/*
	 * zhs_allocatedtid tracks how much has been allocated to workers
	 * already.  When phs_allocatedtid >= rs_lasttid, all TIDs have been
	 * allocated.
	 *
	 * Because we use an atomic fetch-and-add to fetch the current value, the
	 * phs_allocatedtid counter will exceed rs_lasttid, because workers will
	 * still increment the value, when they try to allocate the next block but
	 * all blocks have been allocated already. The counter must be 64 bits
	 * wide because of that, to avoid wrapping around when rs_lasttid is close
	 * to 2^32.  That's also one reason we do this at granularity of 2^16 TIDs,
	 * even though zedstore isn't block-oriented.
	 *
	 * TODO: we divide the TID space into chunks of 2^16 TIDs each. That's
	 * pretty inefficient, there's a fair amount of overhead in re-starting
	 * the B-tree scans between each range. We probably should use much larger
	 * ranges. But this is good for testing.
	 */
	allocatedtid_blk = pg_atomic_fetch_add_u64(&pzscan->pzs_allocatedtid_blk, 1);
	*start = ZSTidFromBlkOff(allocatedtid_blk, 1);
	*end = ZSTidFromBlkOff(allocatedtid_blk + 1, 1);

	return *start < pzscan->pzs_endtid;
}
