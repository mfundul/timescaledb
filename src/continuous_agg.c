/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */

/* This file handles commands on continuous aggs that should be allowed in
 * apache only mode. Right now this consists mostly of drop commands
 */

#include <postgres.h>
#include <fmgr.h>
#include <access/htup_details.h>
#include <catalog/dependency.h>
#include <catalog/namespace.h>
#include <storage/lmgr.h>
#include <catalog/pg_trigger.h>
#include <commands/trigger.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>

#include "compat.h"

#include "bgw/job.h"
#include "continuous_agg.h"
#include "hypertable.h"
#include "scan_iterator.h"

#if !PG96
#include <utils/fmgrprotos.h>
#endif

static void
init_scan_by_mat_hypertable_id(ScanIterator *iterator, const int32 mat_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(), CONTINUOUS_AGG, CONTINUOUS_AGG_PKEY);

	ts_scan_iterator_scan_key_init(iterator,
								   Anum_continuous_agg_pkey_mat_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(mat_hypertable_id));
}

static void
init_completed_threshold_scan_by_mat_id(ScanIterator *iterator, const int32 mat_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(),
											CONTINUOUS_AGGS_COMPLETED_THRESHOLD,
											CONTINUOUS_AGGS_COMPLETED_THRESHOLD_PKEY);

	ts_scan_iterator_scan_key_init(iterator,
								   Anum_continuous_aggs_completed_threshold_pkey_materialization_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(mat_hypertable_id));
}

static void
init_invalidation_threshold_scan_by_hypertable_id(ScanIterator *iterator,
												  const int32 raw_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(),
											CONTINUOUS_AGGS_INVALIDATION_THRESHOLD,
											CONTINUOUS_AGGS_INVALIDATION_THRESHOLD_PKEY);

	ts_scan_iterator_scan_key_init(iterator,
								   Anum_continuous_aggs_invalidation_threshold_pkey_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(raw_hypertable_id));
}

static void
init_hypertable_invalidation_log_scan_by_hypertable_id(ScanIterator *iterator,
													   const int32 raw_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(),
											CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG,
											CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG_IDX);

	ts_scan_iterator_scan_key_init(
		iterator,
		Anum_continuous_aggs_hypertable_invalidation_log_idx_hypertable_id,
		BTEqualStrategyNumber,
		F_INT4EQ,
		Int32GetDatum(raw_hypertable_id));
}

static int32
number_of_continuous_aggs_attached(int32 raw_hypertable_id)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	int32 count = 0;

	ts_scanner_foreach(&iterator)
	{
		FormData_continuous_agg *data =
			(FormData_continuous_agg *) GETSTRUCT(ts_scan_iterator_tuple(&iterator));
		if (data->raw_hypertable_id == raw_hypertable_id)
			count++;
	}
	return count;
}

static void
completed_threshold_delete(int32 materialization_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CONTINUOUS_AGGS_COMPLETED_THRESHOLD,
													RowExclusiveLock,
													CurrentMemoryContext);

	init_completed_threshold_scan_by_mat_id(&iterator, materialization_id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete(ti->scanrel, ti->tuple);
	}
}

static void
invalidation_threshold_delete(int32 raw_hypertable_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CONTINUOUS_AGGS_INVALIDATION_THRESHOLD,
													RowExclusiveLock,
													CurrentMemoryContext);

	init_invalidation_threshold_scan_by_hypertable_id(&iterator, raw_hypertable_id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete(ti->scanrel, ti->tuple);
	}
}

static void
hypertable_invalidation_log_delete(int32 raw_hypertable_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG,
													RowExclusiveLock,
													CurrentMemoryContext);

	init_hypertable_invalidation_log_scan_by_hypertable_id(&iterator, raw_hypertable_id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete(ti->scanrel, ti->tuple);
	}
}

static void
continuous_agg_init(ContinuousAgg *cagg, FormData_continuous_agg *fd)
{
	memcpy(&cagg->data, fd, sizeof(cagg->data));
}

ContinuousAgg *
ts_continuous_agg_find_by_view_name(const char *schema, const char *name)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	ContinuousAgg *ca = NULL;
	int count = 0;

	ts_scanner_foreach(&iterator)
	{
		FormData_continuous_agg *data =
			(FormData_continuous_agg *) GETSTRUCT(ts_scan_iterator_tuple(&iterator));
		if (ts_continuous_agg_is_user_view(data, schema, name) ||
			ts_continuous_agg_is_partial_view(data, schema, name))
		{
			ca = palloc0(sizeof(*ca));
			continuous_agg_init(ca, data);
			count++;
		}
	}
	Assert(count <= 1);
	return ca;
}

/*
 * Drops continuous aggs and all related objects.
 *
 * These objects are: the user view itself, the catalog entry in
 * continuous-agg , the partial view,
 * the materialization hypertable and
 * trigger on the raw hypertable (hypertable specified in the user view ).
 * NOTE: The order in which the objects are dropped should be EXACTLy same as in materialize.c"
 *
 * drop_user_view indicates whether to drop the user view.
 *                (should be false if called as part of the drop-user-view callback)
 */
static void
drop_continuous_agg(ContinuousAgg *agg, bool drop_user_view)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);
	Catalog *catalog = ts_catalog_get();
	ObjectAddress user_view = { .objectId = InvalidOid }, partial_view = { .objectId = InvalidOid },
				  rawht_trig = { .objectId = InvalidOid };
	Hypertable *mat_hypertable, *raw_hypertable;
	int32 count = 0;
	bool raw_hypertable_has_other_caggs = true;
	bool raw_hypertable_exists;

	/* NOTE: the lock order matters, see tsl/src/materialization.c. Perform all locking upfront */

	if (drop_user_view)
	{
		user_view = (ObjectAddress){
			.classId = RelationRelationId,
			.objectId =
				get_relname_relid(NameStr(agg->data.user_view_name),
								  get_namespace_oid(NameStr(agg->data.user_view_schema), false)),
		};
		LockRelationOid(user_view.objectId, AccessExclusiveLock);
	}

	raw_hypertable = ts_hypertable_get_by_id(agg->data.raw_hypertable_id);
	/* The raw hypertable might be already dropped if this is a cascade from that drop */
	raw_hypertable_exists =
		(raw_hypertable != NULL && OidIsValid(raw_hypertable->main_table_relid));
	if (raw_hypertable_exists)
		/* AccessExclusiveLock is needed to drop triggers.
		 * Also prevent concurrent DML commands */
		LockRelationOid(raw_hypertable->main_table_relid, AccessExclusiveLock);
	mat_hypertable = ts_hypertable_get_by_id(agg->data.mat_hypertable_id);
	/* AccessExclusiveLock is needed to drop this table. */
	LockRelationOid(mat_hypertable->main_table_relid, AccessExclusiveLock);

	/* lock catalogs */
	LockRelationOid(catalog_get_table_id(catalog, BGW_JOB), RowExclusiveLock);
	LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGG), RowExclusiveLock);
	raw_hypertable_has_other_caggs = number_of_continuous_aggs_attached(raw_hypertable->fd.id) > 1;
	if (!raw_hypertable_has_other_caggs)
		LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG),
						RowExclusiveLock);
	LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_COMPLETED_THRESHOLD),
					RowExclusiveLock);
	if (!raw_hypertable_has_other_caggs)
		LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_INVALIDATION_THRESHOLD),
						RowExclusiveLock);

	/* The trigger will be dropped if the hypertable still exists and no other caggs attached */
	if (!raw_hypertable_has_other_caggs && raw_hypertable_exists)
	{
		Oid rawht_trigoid =
			get_trigger_oid(raw_hypertable->main_table_relid, CAGGINVAL_TRIGGER_NAME, false);
		rawht_trig = (ObjectAddress){ .classId = TriggerRelationId,
									  .objectId = rawht_trigoid,
									  .objectSubId = 0 };
		/* raw hypertable is locked above */
		LockRelationOid(rawht_trigoid, AccessExclusiveLock);
	}

	partial_view = (ObjectAddress){
		.classId = RelationRelationId,
		.objectId =
			get_relname_relid(NameStr(agg->data.partial_view_name),
							  get_namespace_oid(NameStr(agg->data.partial_view_schema), false)),
	};
	/* The partial view may already be dropped by PG's dependency system (e.g. the raw table was
	 * dropped) */
	if (OidIsValid(partial_view.objectId))
		LockRelationOid(partial_view.objectId, AccessExclusiveLock);

	/*  END OF LOCKING. Perform actual deletions now. */

	if (OidIsValid(user_view.objectId))
		performDeletion(&user_view, DROP_RESTRICT, 0);

	/* Delete catalog entry. */
	init_scan_by_mat_hypertable_id(&iterator, agg->data.mat_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		HeapTuple tuple = ti->tuple;
		Form_continuous_agg form = (Form_continuous_agg) GETSTRUCT(tuple);
		/* delete the job */
		ts_bgw_job_delete_by_id(form->job_id);
		ts_catalog_delete(ti->scanrel, ti->tuple);

		/* delete all related rows */
		if (!raw_hypertable_has_other_caggs)
			hypertable_invalidation_log_delete(form->raw_hypertable_id);

		completed_threshold_delete(form->mat_hypertable_id);

		if (!raw_hypertable_has_other_caggs)
			invalidation_threshold_delete(form->raw_hypertable_id);
		count++;
	}
	Assert(count == 1);

	if (OidIsValid(rawht_trig.objectId))
		performDeletion(&rawht_trig, DROP_RESTRICT, 0);

	/* delete the materialization table */
	ts_hypertable_drop(mat_hypertable);

	if (OidIsValid(partial_view.objectId))
		performDeletion(&partial_view, DROP_RESTRICT, 0);
}

/*
 * This is a called when a hypertable gets dropped.
 *
 * If the hypertable is a raw hypertable for a continuous agg,
 * drop the continuous agg.
 *
 * If the hypertable is a materialization hypertable, error out
 * and force the user to drop the continuous agg instead.
 */
void
ts_continuous_agg_drop_hypertable_callback(int32 hypertable_id)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	ContinuousAgg ca;

	ts_scanner_foreach(&iterator)
	{
		FormData_continuous_agg *data =
			(FormData_continuous_agg *) GETSTRUCT(ts_scan_iterator_tuple(&iterator));
		if (data->raw_hypertable_id == hypertable_id)
		{
			continuous_agg_init(&ca, data);
			drop_continuous_agg(&ca, true);
		}
		if (data->mat_hypertable_id == hypertable_id)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop the materialized table because it is required by a "
							"continuous aggregate")));
	}
}

/* Block dropping the partial view if the continuous aggregate still exists */
static void
drop_partial_view(ContinuousAgg *agg)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	int count = 0;
	init_scan_by_mat_hypertable_id(&iterator, agg->data.mat_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete(ti->scanrel, ti->tuple);
		count++;
	}
	if (count > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop the partial view because it is required by a continuous "
						"aggregate")));
}

/* This gets called when a view gets dropped. */
void
ts_continuous_agg_drop_view_callback(ContinuousAgg *ca, const char *schema, const char *name)
{
	if (ts_continuous_agg_is_user_view(&ca->data, schema, name))
		drop_continuous_agg(ca, false /* The user view has already been dropped */);
	else if (ts_continuous_agg_is_partial_view(&ca->data, schema, name))
		drop_partial_view(ca);
	else
		elog(ERROR, "unknown continuous aggregate view type");
}

bool
ts_continuous_agg_is_user_view(FormData_continuous_agg *data, const char *schema, const char *name)
{
	return (namestrcmp(&data->user_view_schema, schema) == 0) &&
		   (namestrcmp(&data->user_view_name, name) == 0);
}

bool
ts_continuous_agg_is_partial_view(FormData_continuous_agg *data, const char *schema,
								  const char *name)
{
	return (namestrcmp(&data->partial_view_schema, schema) == 0) &&
		   (namestrcmp(&data->partial_view_name, name) == 0);
}
