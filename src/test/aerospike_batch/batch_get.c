/*
 * Copyright 2008-2021 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#include <aerospike/aerospike.h>
#include <aerospike/aerospike_batch.h>
#include <aerospike/aerospike_key.h>
#include <aerospike/as_arraylist.h>
#include <aerospike/as_atomic.h>
#include <aerospike/as_batch.h>
#include <aerospike/as_error.h>
#include <aerospike/as_hashmap.h>
#include <aerospike/as_integer.h>
#include <aerospike/as_list.h>
#include <aerospike/as_map.h>
#include <aerospike/as_record.h>
#include <aerospike/as_sleep.h>
#include <aerospike/as_status.h>
#include <aerospike/as_string.h>
#include <aerospike/as_tls.h>
#include <aerospike/as_val.h>
#include <pthread.h>

#include "../test.h"

/******************************************************************************
 * GLOBAL VARS
 *****************************************************************************/

extern aerospike* as;

uint32_t num_threads = 0;
pthread_rwlock_t rwlock;

/******************************************************************************
 * MACROS
 *****************************************************************************/
#define NAMESPACE "test"
#define SET "test_batch"
#define LIST_BIN "listbin"
#define N_KEYS 200

/******************************************************************************
 * TYPES
 *****************************************************************************/

typedef struct batch_read_data_s {
	uint32_t thread_id;
	uint32_t total;
	uint32_t found;
	uint32_t errors;
	uint32_t last_error;
} batch_read_data;

/******************************************************************************
 * STATIC FUNCTIONS
 *****************************************************************************/

static as_status
insert_record(as_error* err, int i)
{
	// Do not write some records to test not found logic too.
	if (i % 20 == 0) {
		return AEROSPIKE_OK;
	}
		
	as_key key;
	as_key_init_int64(&key, NAMESPACE, SET, i);

	as_arraylist list;
	as_arraylist_inita(&list, i);

	for (int j = 0; j < i; j++) {
		as_arraylist_append_int64(&list, j * i);
	}

	as_record rec;
	
	// Some records should be missing bins to test bin filters.
	if (i % 25 == 0) {
		as_record_inita(&rec, 2);
		as_record_set_int64(&rec, "val", i);
		as_record_set_list(&rec, LIST_BIN, (as_list*)&list);
	}
	else {
		as_record_inita(&rec, 3);
		as_record_set_int64(&rec, "val", i);
		as_record_set_int64(&rec, "val2", i);
		as_record_set_list(&rec, LIST_BIN, (as_list*)&list);
	}

	as_status status = aerospike_key_put(as, err, NULL, &key, &rec);
	as_record_destroy(&rec);
	return status;
}

static bool
before(atf_suite* suite)
{
	as_error err;

	for (int i = 0; i < N_KEYS; i++) {
		as_status status = insert_record(&err, i);

		if (status != AEROSPIKE_OK) {
			info("error(%d): %s", err.code, err.message);
			return false;
		}
	}
	return true;
}

static bool
after(atf_suite* suite)
{
	as_error err;
	
	for (int i = 1; i < N_KEYS; i++) {
		as_key key;
		as_key_init_int64(&key, NAMESPACE, SET, i);
		
		as_status status = aerospike_key_remove(as, &err, NULL, &key);
		
		if (status != AEROSPIKE_OK && status != AEROSPIKE_ERR_RECORD_NOT_FOUND) {
			info("error(%d): %s", err.code, err.message);
			return false;
		}
	}
	return true;
}

static bool
batch_get_1_callback(const as_batch_read* results, uint32_t n, void* udata)
{
	batch_read_data* data = (batch_read_data *) udata;
	
	data->total = n;

	for (uint32_t i = 0; i < n; i++) {

		if (results[i].result == AEROSPIKE_OK) {
			data->found++;

			int64_t key = as_integer_getorelse((as_integer *) results[i].key->valuep, -1);
			int64_t val = as_record_get_int64(&results[i].record, "val", -1);
			if (key != val) {
				warn("key(%d) != val(%d)",key,val);
				data->errors++;
				data->last_error = -2;
			}
		}
		else if (results[i].result != AEROSPIKE_ERR_RECORD_NOT_FOUND) {
			data->errors++;
			data->last_error = results[i].result;
			warn("batch callback thread(%d) error(%d)", data->thread_id, data->last_error);
		}
	}

	info("total: %d, found: %d, errors: %d", data->total, data->found, data->errors);

	return true;
}

static bool
batch_sequence_callback(as_key* key, as_record* record, void* udata)
{
	batch_read_data* data = (batch_read_data*)udata;

	data->total++;
	data->found++;

	int64_t k = as_integer_getorelse((as_integer *)key->valuep, -1);
	int64_t v = as_record_get_int64(record, "val", -1);
	if (k != v) {
		warn("key(%d) != val(%d)", k, v);
		data->errors++;
		data->last_error = -2;
	}
	return true;
}

/******************************************************************************
 * TEST CASES
 *****************************************************************************/

TEST(batch_get_1, "Simple")
{
	as_error err;

	as_batch batch;
	as_batch_inita(&batch, N_KEYS);

	for (uint32_t i = 0; i < N_KEYS; i++) {
		as_key_init_int64(as_batch_keyat(&batch,i), NAMESPACE, SET, i+1);
	}

	batch_read_data data = {0};

	aerospike_batch_get(as, &err, NULL, &batch, batch_get_1_callback, &data);
	if (err.code != AEROSPIKE_OK) {
		info("error(%d): %s", err.code, err.message);
	}
	assert_int_eq(err.code, AEROSPIKE_OK);

	assert_int_eq(data.found, N_KEYS - N_KEYS/20);
	assert_int_eq(data.errors, 0);
}

TEST(batch_get_sequence, "Batch get in sequence")
{
	as_error err;
	
	as_batch batch;
	as_batch_inita(&batch, N_KEYS);
	
	for (uint32_t i = 0; i < N_KEYS; i++) {
		as_key_init_int64(as_batch_keyat(&batch,i), NAMESPACE, SET, i+1);
	}
	
	batch_read_data data = {0};
	
	aerospike_batch_get_xdr(as, &err, NULL, &batch, batch_sequence_callback, &data);
	if (err.code != AEROSPIKE_OK) {
		info("error(%d): %s", err.code, err.message);
	}
	assert_int_eq(err.code, AEROSPIKE_OK);
	
	assert_int_eq(data.found, N_KEYS - N_KEYS/20);
	assert_int_eq(data.errors, 0);
}

static void*
batch_get_function(void* thread_id)
{
	int thread_num = *(int*)thread_id;
	as_error err;
	
	as_batch batch;
	as_batch_inita(&batch, 20);
	
	int start_index = thread_num * 20;
	int end_index = start_index + 20;
	int j = 0;

	for (int i = start_index; i < end_index; i++) {
		as_key_init_int64(as_batch_keyat(&batch,j++), NAMESPACE, SET, i+1);
	}
	
	batch_read_data data = {thread_num, 0, 0, 0, 0};
	
	as_incr_uint32(&num_threads);
	pthread_rwlock_rdlock(&rwlock);
	
	aerospike_batch_get(as, &err, NULL, &batch, batch_get_1_callback, &data);
	
	pthread_rwlock_unlock(&rwlock);

	if (err.code != AEROSPIKE_OK) {
		info("multi-thread error(%d): %s", err.code, err.message);
	}

	as_tls_thread_cleanup();
	return(0);
}

TEST(multithreaded_batch_get, "Batch Get - with multiple threads ")
{
	uint32_t threads = 10;
	
	pthread_t* batch_thread = alloca(sizeof(pthread_t) * threads);
	int* ids = alloca(sizeof(int) * threads);
	pthread_rwlock_init(&rwlock, NULL);
	pthread_rwlock_wrlock(&rwlock);
	for (uint32_t i = 0; i < threads; i++) {
		ids[i] = i;
		pthread_create(&batch_thread[i], 0, batch_get_function, &ids[i]);
	}
	
	while (as_load_uint32(&num_threads) < threads) {
		as_sleep(1 * 1000);
	}
	pthread_rwlock_unlock(&rwlock);
	
	for (uint32_t i = 0; i < threads; i++) {
		pthread_join(batch_thread[i], NULL);
	}
	pthread_rwlock_destroy(&rwlock);
}

static bool
batch_get_bins_callback(const as_batch_read* results, uint32_t n, void* udata)
{
	batch_read_data* data = udata;

	data->total = n;
	
	for (uint32_t i = 0; i < n; i++) {
		
		if (results[i].result == AEROSPIKE_OK) {
			data->found++;
			
			int64_t val = as_record_get_int64(&results[i].record, "val", -1);
			if (val != -1) {
				warn("val(%d) should not have been returned!", val);
				data->errors++;
				data->last_error = -2;
			}
			
			int64_t key = as_integer_getorelse((as_integer *) results[i].key->valuep, -1);
			int64_t val2 = as_record_get_int64(&results[i].record, "val2", -1);

			if (i % 25 == 0) {
				if (val2 != -1) {
					data->errors++;
					warn("val2(%d) exists when it shouldn't exist!", i, val2);
				}
			}
			else {
				if (val2 == -1 || key != val2) {
					warn("key(%d) != val2(%d)", key, val2);
					data->errors++;
					data->last_error = -2;
				}
			}
		}
		else if (results[i].result != AEROSPIKE_ERR_RECORD_NOT_FOUND) {
			data->errors++;
			data->last_error = results[i].result;
			warn("batch callback thread(%d) error(%d)", data->thread_id, data->last_error);
		}
	}
	
	info("total: %d, found: %d, errors: %d", data->total, data->found, data->errors);
	
	return true;
}

TEST(batch_get_bins, "Batch Get - with bin name filters")
{
	as_error err;
	
	as_batch batch;
	as_batch_inita(&batch, N_KEYS);
	
	for (uint32_t i = 0; i < N_KEYS; i++) {
		as_key_init_int64(as_batch_keyat(&batch,i), NAMESPACE, SET, i);
	}
	
	batch_read_data data = {0};
	const char* bins[] = {"val2"};
	
	aerospike_batch_get_bins(as, &err, NULL, &batch, bins, 1, batch_get_bins_callback, &data);
	if (err.code != AEROSPIKE_OK) {
		info("error(%d): %s", err.code, err.message);
	}
	assert_int_eq(err.code, AEROSPIKE_OK);
	assert_int_eq(data.found, N_KEYS - N_KEYS/20);
	assert_int_eq(data.errors, 0);
}

TEST(batch_read_complex, "Batch read complex")
{
	// Batch allows multiple namespaces in one call, but example test environment may only have one namespace.
	as_batch_read_records records;
	as_batch_read_inita(&records, 9);
	
	char* bins[] = {"val"};
	uint32_t n_bins = 1;
	
	// get specified bins
	as_batch_read_record* record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 1);
	record->bin_names = bins;
	record->n_bin_names = n_bins;
	
	// get all bins
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 2);
	record->read_all_bins = true;
	
	// get all bins
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 3);
	record->read_all_bins = true;

	// exists
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 4);
	
	// get all bins
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 5);
	record->read_all_bins = true;
	
	// get all bins
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 6);
	record->read_all_bins = true;

	// get specified bins
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 7);
	record->bin_names = bins;
	record->n_bin_names = n_bins;

	// This record should be found, but the requested bin will not be found.
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 8);
	char* bins2[] = {"binnotfound"};
	record->bin_names = bins2;
	record->n_bin_names = 1;
	
	// This record should not be found.
	record = as_batch_read_reserve(&records);
	as_key_init_int64(&record->key, NAMESPACE, SET, 20);
	record->bin_names = bins;
	record->n_bin_names = n_bins;

	as_error err;

	as_status status = aerospike_batch_read(as, &err, NULL, &records);
	
	uint32_t found = 0;
	uint32_t errors = 0;
	
	if (status == AEROSPIKE_OK) {
		as_vector* list = &records.list;
		for (uint32_t i = 0; i < list->size; i++) {
			as_batch_read_record* batch = as_vector_get(list, i);
			as_key* key = &batch->key;
			
			if (batch->result == AEROSPIKE_OK) {
				found++;
				
				if (batch->read_all_bins || batch->n_bin_names > 0) {
					int64_t val = as_record_get_int64(&batch->record, "val", -1);
					
					if (val != -1) {
						info("Record: ns=%s set=%s key=%d bin=%d",
							 key->ns, key->set, (int)key->valuep->integer.value, (int)val);
					}
					else {
						info("Record: ns=%s set=%s key=%d bin=null",
							 key->ns, key->set, (int)key->valuep->integer.value);
					}
				}
				else {
					info("Record: ns=%s set=%s key=%d exists=true",
						 key->ns, key->set, (int)key->valuep->integer.value);
				}
			}
			else if (batch->result == AEROSPIKE_ERR_RECORD_NOT_FOUND) {
				info("Record not found: ns=%s set=%s key=%d",
					 key->ns, key->set, (int)key->valuep->integer.value);
			}
			else {
				errors++;
				error("Unexpected error(%u): %s", i, as_error_string(batch->result));
			}
		}
	}
	as_batch_read_destroy(&records);
	
	if (status != AEROSPIKE_OK) {
		if (status == AEROSPIKE_ERR_UNSUPPORTED_FEATURE) {
			info("aerospike_batch_read() not supported by connected cluster");
			return;
		}
		error("error(%d): %s", err.code, err.message);
	}
	
	assert_int_eq(status, AEROSPIKE_OK);
	assert_int_eq(found, 8);
	assert_int_eq(errors, 0);
}

static bool
batch_list_operate_cb(const as_batch_read* results, uint32_t n, void* udata)
{
	batch_read_data* data = udata;
	data->total = n;

	for (uint32_t i = 0; i < n; i++) {
		const as_batch_read* r = &results[i];
		int k = (int)r->key->valuep->integer.value;

		if (r->result == AEROSPIKE_OK) {
			data->found++;

			as_bin* results = r->record.bins.entries;
			int v2 = (int)results[1].valuep->integer.value;
			int expected = k * (k - 1);

			if (v2 != expected) {
				data->errors++;
				warn("Result[%d]: v2(%d) != expected(%d)", k, v2, expected);
			}
		}
		else if (r->result != AEROSPIKE_ERR_RECORD_NOT_FOUND) {
			data->errors++;
			data->last_error = r->result;
			warn("Result[%d]: not found", k);
		}
	}
	return true;
}

TEST(batch_list_operate, "Batch list operate")
{
	as_batch batch;
	as_batch_inita(&batch, N_KEYS);
	
	for (uint32_t i = 0; i < N_KEYS; i++) {
		as_key_init_int64(as_batch_keyat(&batch,i), NAMESPACE, SET, i);
	}

	// Get size and last element of list bin for all records.
	as_operations ops;
	as_operations_inita(&ops, 2);
	as_operations_list_size(&ops, LIST_BIN, NULL);
	as_operations_list_get_by_index(&ops, LIST_BIN, NULL, -1, AS_LIST_RETURN_VALUE);

	batch_read_data data = {0};
	as_error err;
	as_status status = aerospike_batch_get_ops(as, &err, NULL, &batch, &ops, batch_list_operate_cb,
											   &data);

	as_operations_destroy(&ops);
	assert_int_eq(status, AEROSPIKE_OK);
	assert_int_eq(data.found, N_KEYS - N_KEYS/20);
	assert_int_eq(data.errors, 0);
}

/******************************************************************************
 * TEST SUITE
 *****************************************************************************/

SUITE(batch_get, "aerospike_batch_get tests")
{
	suite_before(before);
	suite_after(after);
	suite_add(batch_get_1);
	suite_add(batch_get_sequence);
	suite_add(multithreaded_batch_get);
	suite_add(batch_get_bins);
	suite_add(batch_read_complex);
	suite_add(batch_list_operate);
}
