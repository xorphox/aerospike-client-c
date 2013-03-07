/*
 * UDF Scan interface
 *
 *
 * Citrusleaf, 2012
 * All rights reserved
 */

#include "citrusleaf.h"
#include "citrusleaf-internal.h"
#include "cl_cluster.h"
#include "cl_uscan.h"
#include "cl_udf.h"

#include <citrusleaf/cf_atomic.h>
#include <citrusleaf/cf_socket.h>
#include <citrusleaf/cf_vector.h>
#include <citrusleaf/cf_random.h>
#include <citrusleaf/proto.h>

#include <sys/types.h>
#include <stdio.h>
#include <errno.h> //errno
#include <stdlib.h> //fprintf
#include <unistd.h> // close
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <assert.h>
#include <asm/byteorder.h> // 64-bit swap macro
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/proto.h"
#include "citrusleaf/cf_socket.h"
#include "citrusleaf/cf_vector.h"
#include "as_msgpack.h"
#include "as_serializer.h"
#include "as_list.h"
#include "as_string.h"


#include <as_aerospike.h>
#include <as_module.h>
#include <mod_lua.h>
#include <mod_lua_config.h>

extern as_val * citrusleaf_udf_bin_to_val(as_serializer *ser, cl_bin *);

/******************************************************************************
 * MACROS
 *****************************************************************************/

/*
 * Provide a safe number for your system linux tends to have 8M 
 * stacks these days
 */ 
#define STACK_BUF_SZ        (1024 * 16) 
#define STACK_BINS           100
#define N_MAX_SCAN_THREADS  5

static void __log(const char * file, const int line, const char * fmt, ...) {
    char msg[256] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, 256, fmt, ap);
    va_end(ap);
    printf("[%s:%d] %s\n",file,line,msg);
}

#define LOG(__fmt, args...) \
    __log(__FILE__,__LINE__,__fmt, ## args)


/******************************************************************************
 * TYPES
 *****************************************************************************/

/*
 * Work item which gets queued up to each node
 */
typedef struct {
    cl_cluster *            asc; 
    const char *            ns;
    char                    node_name[NODE_NAME_SIZE];    
    const uint8_t *         scan_buf;
    size_t                  scan_sz;
    cf_queue *              node_complete_q;     // Asyncwork item queue
    void *                  udata;
    int                     (* callback)(as_val *, void *);
} as_scan_task;

/******************************************************************************
 * VARIABLES
 *****************************************************************************/

cf_atomic32     scan_initialized  = 0;
cf_queue *      g_scan_q          = 0;
pthread_t       g_scan_th[N_MAX_SCAN_THREADS];
static as_scan_task    g_null_task;
static bool            gasq_abort         = false;

/******************************************************************************
 * STATIC FUNCTIONS
 *****************************************************************************/

static int scan_compile(const as_scan * scan, uint8_t **buf_r, size_t *buf_sz_r);

static cl_rv as_scan_params_init(as_scan_params * oparams, as_scan_params *iparams);

static cl_rv as_scan_udf_destroy(as_scan_udf * udf);

cl_rv as_scan_execute(cl_cluster * cluster, const as_scan * scan, void * udata, int (* callback)(as_val *, void *));

// Creates a message, internally calling cl_compile to pass to the server
static int scan_compile(const as_scan * scan, uint8_t ** buf_r, size_t * buf_sz_r) {

	if (!scan) return CITRUSLEAF_FAIL_CLIENT;

	//  Prepare udf call to send to the server
	as_call call;
	as_serializer ser;
	as_buffer argbuffer;
	as_buffer_init(&argbuffer);

	if ( scan->udf.type != AS_SCAN_UDF_NONE) {
		as_string file;
		as_string_init(&file, (char *) scan->udf.filename, true /*ismalloc*/);

		as_string func;
		as_string_init(&func, (char *) scan->udf.function, true /*ismalloc*/);

		if (scan->udf.arglist != NULL) {
			/**
			 * If the query has a udf w/ arglist,
			 * then serialize it.
			 */
			as_msgpack_init(&ser);
			as_serializer_serialize(&ser, (as_val *) scan->udf.arglist, &argbuffer);
		}
		call.file = &file;
		call.func = &func;
		call.args = &argbuffer;
	}

	// Prepare to send scan parameters
	cl_scan_param_field     scan_param_field;
	as_scan_params params = scan->params;
	scan_param_field.scan_pct = params.pct > 100 ? 100 : params.pct;
	scan_param_field.byte1 = (params.priority << 4)  | (params.fail_on_cluster_change << 3);
	
	// Prepare the msg type to be sent
	uint info;
	if (params.nobindata == true) {
		info = (CL_MSG_INFO1_READ | CL_MSG_INFO1_NOBINDATA);
	} else {
		info = CL_MSG_INFO1_READ;
	}

	// Pass on to the cl_compile to create the msg
	cl_compile(info /*info1*/, 0, 0, scan->ns /*namespace*/, scan->setname /*setname*/, 0 /*key*/, 0/*digest*/, NULL /*bins*/, 0/*op*/, 0/*operations*/, 0/*n_values*/, buf_r, buf_sz_r, 0 /*w_p*/, NULL /*d_ret*/, scan->job_id, &scan_param_field, &call /*udf call*/);

	if (scan->udf.arglist) {
		as_serializer_destroy(&ser);
	}
	as_buffer_destroy(&argbuffer);
	return CITRUSLEAF_OK;
}

/**
 * Get a value for a bin of with the given key.
 */
static as_val * scan_response_get(const as_rec * rec, const char * name)  {
    as_val * v = NULL;
    as_serializer ser;
    as_msgpack_init(&ser);
    as_scan_response_rec * r = as_rec_source(rec);
    for (int i = 0; i < r->n_bins; i++) {
        if (!strcmp(r->bins[i].bin_name, name)) {
            v = citrusleaf_udf_bin_to_val(&ser, &r->bins[i]);
            break;
        }
    }
    as_serializer_destroy(&ser);
    return v;
}

static uint32_t scan_response_ttl(const as_rec * rec) {
    as_scan_response_rec * r = as_rec_source(rec);
    return r->record_ttl;
}

static uint16_t scan_response_gen(const as_rec * rec) {
    as_scan_response_rec * r = as_rec_source(rec);
    if (!r) return 0;
    return r->generation;
}

int scan_response_destroy(as_rec *rec) {
    as_scan_response_rec * r = as_rec_source(rec);
    if (!r) return 0;
    citrusleaf_bins_free(r->bins, r->n_bins);
    if (r->bins) free(r->bins);
    if (r->ns)   free(r->ns);
    if (r->set)  free(r->set);
    if (r->ismalloc) free(r);
    rec->source = NULL;
    return 0;
}

const as_rec_hooks scan_response_hooks = {
    .get        = scan_response_get,
    .set        = NULL,
    .remove     = NULL,
    .ttl        = scan_response_ttl,
    .gen        = scan_response_gen,
    .destroy    = scan_response_destroy
};

/* 
 * this is an actual instance of the scan, running on a scan thread
 * It reads on the node fd till it finds the last msg, in the meantime calling
 * task->callback on the returned data. The returned data is a bin of name SUCCESS/FAILURE
 * and the value of the bin is the return value from the udf.
 */
static int as_scan_worker_do(cl_cluster_node * node, as_scan_task * task) {

    uint8_t     rd_stack_buf[STACK_BUF_SZ] = {0};    
    uint8_t *   rd_buf = rd_stack_buf;
    size_t      rd_buf_sz = 0;
    
    int fd = cl_cluster_node_fd_get(node, false, task->asc->nbconnect);
    if ( fd == -1 ) { 
        fprintf(stderr,"do query monte: cannot get fd for node %s ",node->name);
        return CITRUSLEAF_FAIL_CLIENT; 
    }

    // send it to the cluster - non blocking socket, but we're blocking
    if (0 != cf_socket_write_forever(fd, (uint8_t *) task->scan_buf, (size_t) task->scan_sz)) {
        return CITRUSLEAF_FAIL_CLIENT;
    }

    cl_proto  proto;
    int       rc   = CITRUSLEAF_OK;
    bool      done = false;

    do {
        // multiple CL proto per response
        // Now turn around and read a fine cl_proto - that's the first 8 bytes 
        // that has types and lengths
        if ( (rc = cf_socket_read_forever(fd, (uint8_t *) &proto, sizeof(cl_proto) ) ) ) {
            fprintf(stderr, "network error: errno %d fd %d\n", rc, fd);
            return CITRUSLEAF_FAIL_CLIENT;
        }
        cl_proto_swap(&proto);

        if ( proto.version != CL_PROTO_VERSION) {
            fprintf(stderr, "network error: received protocol message of wrong version %d\n",proto.version);
            return CITRUSLEAF_FAIL_CLIENT;
        }

        if ( proto.type != CL_PROTO_TYPE_CL_MSG && proto.type != CL_PROTO_TYPE_CL_MSG_COMPRESSED ) {
            fprintf(stderr, "network error: received incorrect message version %d\n",proto.type);
            return CITRUSLEAF_FAIL_CLIENT;
        }

        // second read for the remainder of the message - expect this to cover 
        // lots of data, many lines if there's no error
        rd_buf_sz =  proto.sz;
        if (rd_buf_sz > 0) {

            if (rd_buf_sz > sizeof(rd_stack_buf)){
                rd_buf = malloc(rd_buf_sz);
            }
            else {
                rd_buf = rd_stack_buf;
            }

            if (rd_buf == NULL) return CITRUSLEAF_FAIL_CLIENT;

            if ( (rc = cf_socket_read_forever(fd, rd_buf, rd_buf_sz)) ) {
                fprintf(stderr, "network error: errno %d fd %d\n", rc, fd);
                if ( rd_buf != rd_stack_buf ) free(rd_buf);
                return CITRUSLEAF_FAIL_CLIENT;
            }
        }

        // process all the cl_msg in this proto
        uint8_t *   buf = rd_buf;
        uint        pos = 0;
        cl_bin      stack_bins[STACK_BINS];
        cl_bin *    bins;

        while (pos < rd_buf_sz) {

            uint8_t *   buf_start = buf;
            cl_msg *    msg = (cl_msg *) buf;

            cl_msg_swap_header(msg);
            buf += sizeof(cl_msg);

            if ( msg->header_sz != sizeof(cl_msg) ) {
                fprintf(stderr, "received cl msg of unexpected size: expecting %zd found %d, internal error\n",
                        sizeof(cl_msg),msg->header_sz);
                return CITRUSLEAF_FAIL_CLIENT;
            }

            // parse through the fields
            cf_digest       keyd;
            char            ns_ret[33]  = {0};
            char *          set_ret     = NULL;
            cl_msg_field *  mf          = (cl_msg_field *)buf;

            for (int i=0; i < msg->n_fields; i++) {
                cl_msg_swap_field(mf);
                if (mf->type == CL_MSG_FIELD_TYPE_KEY) {
                    fprintf(stderr, "read: found a key - unexpected\n");
                }
                else if (mf->type == CL_MSG_FIELD_TYPE_DIGEST_RIPE) {
                    memcpy(&keyd, mf->data, sizeof(cf_digest));
                }
                else if (mf->type == CL_MSG_FIELD_TYPE_NAMESPACE) {
                    memcpy(ns_ret, mf->data, cl_msg_field_get_value_sz(mf));
                    ns_ret[ cl_msg_field_get_value_sz(mf) ] = 0;
                }
                else if (mf->type == CL_MSG_FIELD_TYPE_SET) {
                    uint32_t set_name_len = cl_msg_field_get_value_sz(mf);
                    set_ret = (char *)malloc(set_name_len + 1);
                    memcpy(set_ret, mf->data, set_name_len);
                    set_ret[ set_name_len ] = '\0';
                }
                mf = cl_msg_field_get_next(mf);
            }

            buf = (uint8_t *) mf;
            if (msg->n_ops > STACK_BINS) {
                bins = malloc(sizeof(cl_bin) * msg->n_ops);
            }
            else {
                bins = stack_bins;
            }
            
            if (bins == NULL) {
                if (set_ret) {
                    free(set_ret);
                }
                return CITRUSLEAF_FAIL_CLIENT;
            }

            // parse through the bins/ops
            cl_msg_op * op = (cl_msg_op *) buf;
            for (int i=0;i<msg->n_ops;i++) {
                cl_msg_swap_op(op);

#ifdef DEBUG_VERBOSE
                fprintf(stderr, "op receive: %p size %d op %d ptype %d pversion %d namesz %d \n",
                        op,op->op_sz, op->op, op->particle_type, op->version, op->name_sz);
#endif            

#ifdef DEBUG_VERBOSE
                dump_buf("individual op (host order)", (uint8_t *) op, op->op_sz + sizeof(uint32_t));
#endif    

                cl_set_value_particular(op, &bins[i]);
                op = cl_msg_op_get_next(op);
            }
            buf = (uint8_t *) op;

            if (msg->result_code != CL_RESULT_OK) {

                rc = (int) msg->result_code;
                done = true;
            }
            else if (msg->info3 & CL_MSG_INFO3_LAST)    {

#ifdef DEBUG                
                fprintf(stderr, "received final message\n");
#endif                
                done = true;
            }
            else if ((msg->n_ops || (msg->info1 & CL_MSG_INFO1_NOBINDATA))) {

                as_scan_response_rec rec;
                as_scan_response_rec *recp = &rec;

                recp->ns         = strdup(ns_ret);
                recp->keyd       = keyd;
                recp->set        = set_ret;
                recp->generation = msg->generation;
                recp->record_ttl = msg->record_ttl;
                recp->bins       = bins;
                recp->n_bins     = msg->n_ops;
                recp->ismalloc   = false;
        
                as_rec r;
                as_rec *rp = &r;
                rp = as_rec_init(rp, recp, &scan_response_hooks);
            
                as_val * v = as_rec_get(rp, "SUCCESS");
                if ( v  != NULL ) {
                    // Got a non null value for the resposne bin,
                    // call callback on it and destroy the record
                    task->callback(v, task->udata);
                    
                    as_rec_destroy(rp);

                }
                
                rc = CITRUSLEAF_OK;
            }

            // if done free it 
            if (done) {
		citrusleaf_bins_free(bins, msg->n_ops);
                if (bins != stack_bins) {
                    free(bins);
                    bins = 0;
                }

                if (set_ret) {
                    free(set_ret);
                    set_ret = NULL;
                }
            }

            // don't have to free object internals. They point into the read buffer, where
            // a pointer is required
            pos += buf - buf_start;
            if (gasq_abort) {
                break;
            }

        }

        if (rd_buf && (rd_buf != rd_stack_buf))    {
            free(rd_buf);
            rd_buf = 0;
        }

        // abort requested by the user
        if (gasq_abort) {
            close(fd);
            goto Final;
        }
    } while ( done == false );

    cl_cluster_node_fd_put(node, fd, false);

    goto Final;

Final:    

#ifdef DEBUG_VERBOSE    
    fprintf(stderr, "exited loop: rc %d\n", rc );
#endif    

    return rc;
}

void * as_scan_worker(void * dummy) {
    while (1) {
        
        as_scan_task task;

        if ( 0 != cf_queue_pop(g_scan_q, &task, CF_QUEUE_FOREVER) ) {
            fprintf(stderr, "queue pop failed\n");
        }

        if ( cf_debug_enabled() ) {
            fprintf(stderr, "as_query_worker: getting one task item\n");
        }
        
        // a NULL structure is the condition that we should exit. See shutdown()
        if( 0 == memcmp(&task, &g_null_task, sizeof(as_scan_task)) ) { 
            pthread_exit(NULL); 
        }

        // query if the node is still around
        int rc = CITRUSLEAF_FAIL_UNAVAILABLE;

        cl_cluster_node * node = cl_cluster_node_get_byname(task.asc, task.node_name);
        if ( node ) {
            rc = as_scan_worker_do(node, &task);
        }

        cf_queue_push(task.node_complete_q, (void *)&rc);
    }
}

cl_rv as_scan_params_init(as_scan_params * oparams, as_scan_params *iparams) {

	// If there is an input structure use the values from that else use the default ones
	oparams->fail_on_cluster_change = iparams ? iparams->fail_on_cluster_change : false;
	oparams->priority = iparams ? iparams->priority : AS_SCAN_PRIORITY_AUTO;
	oparams->concurrent_nodes = iparams ? iparams->concurrent_nodes : true;
	oparams->threads_per_node = iparams ? iparams->threads_per_node : 1;
	oparams->nobindata = iparams ? iparams->nobindata : false;
	oparams->pct = iparams ? iparams->pct : 100;
	oparams->get_key = iparams ? iparams->get_key : false;
	return CITRUSLEAF_OK;	
}

cl_rv as_scan_udf_init(as_scan_udf * udf, as_scan_udf_type type, const char * filename, const char * function, as_list * arglist) {
    udf->type        = type;
    udf->filename    = filename == NULL ? NULL : strdup(filename);
    udf->function    = function == NULL ? NULL : strdup(function);
    udf->arglist     = arglist;
    return CITRUSLEAF_OK;
}

cl_rv as_scan_udf_destroy(as_scan_udf * udf) {

    udf->type = AS_SCAN_UDF_NONE;

    if ( udf->filename ) {
        free(udf->filename);
        udf->filename = NULL;
    }

    if ( udf->function ) {
        free(udf->function);
        udf->function = NULL;
    }

    if ( udf->arglist ) {
        as_list_destroy(udf->arglist);
        udf->arglist = NULL;
    }

    return CITRUSLEAF_OK;
}

cl_rv as_scan_execute(cl_cluster * cluster, const as_scan * scan, void * udata, int (* callback)(as_val *, void *)) {
    
    cl_rv       rc                          = CITRUSLEAF_OK;
    uint8_t     wr_stack_buf[STACK_BUF_SZ]  = { 0 };
    uint8_t *   wr_buf                      = wr_stack_buf;
    size_t      wr_buf_sz                   = sizeof(wr_stack_buf);
    
    rc = scan_compile(scan, &wr_buf, &wr_buf_sz);
    
    if ( rc != CITRUSLEAF_OK ) {
        // TODO: use proper logging function
        fprintf(stderr, "do query monte: query compile failed: \n");
        return rc;
    }

    // Setup worker
    as_scan_task task = {
        .asc                = cluster,
        .ns                 = scan->ns,
        .scan_buf          = wr_buf,
        .scan_sz           = wr_buf_sz,
        .node_complete_q    = cf_queue_create(sizeof(int),true),
        .udata              = udata,
        .callback           = callback,
    };
    
    char *node_names    = NULL;    
    int   node_count    = 0;

    // Get a list of the node names, so we can can send work to each node
    cl_cluster_get_node_names(cluster, &node_count, &node_names);
    if ( node_count == 0 ) {
        // TODO: use proper loggin function
        fprintf(stderr, "citrusleaf query nodes: don't have any nodes?\n");
        cf_queue_destroy(task.node_complete_q);
        if ( wr_buf && (wr_buf != wr_stack_buf) ) {
            free(wr_buf); 
            wr_buf = 0;
        }
        return CITRUSLEAF_FAIL_CLIENT;
    }

    // Dispatch work to the worker queue to allow the transactions in parallel
    // NOTE: if a new node is introduced in the middle, it is NOT taken care of
    char * node_name = node_names;
    for ( int i=0; i < node_count; i++ ) {
        // fill in per-request specifics
        strcpy(task.node_name, node_name);
        cf_queue_push(g_scan_q, &task);
        node_name += NODE_NAME_SIZE;                    
    }
    free(node_names);
    node_names = NULL;
    
    // wait for the work to complete from all the nodes.
    rc = CITRUSLEAF_OK;
    for ( int i=0; i < node_count; i++ ) {
        int node_rc;
        cf_queue_pop(task.node_complete_q, &node_rc, CF_QUEUE_FOREVER);
        if ( node_rc != 0 ) {
            // Got failure from one node. Trigger abort for all 
            // the ongoing request
            gasq_abort = true;
            rc = node_rc;
        }
    }
    gasq_abort = false;

    if ( wr_buf && (wr_buf != wr_stack_buf) ) { 
        free(wr_buf); 
        wr_buf = 0;
    }

    callback(NULL, udata);
    
    cf_queue_destroy(task.node_complete_q);

    return rc;
}

/**
 * Allocates and initializes a new as_scan.
 */
as_scan * as_scan_new(const char * ns, const char * setname) {
    as_scan * scan = (as_scan*) malloc(sizeof(as_scan));
    memset(scan, 0, sizeof(as_scan));
    return as_scan_init(scan, ns, setname);
}

/**
 * Initializes an as_scan
 */
as_scan * as_scan_init(as_scan * scan, const char * ns, const char * setname) {
    if ( scan == NULL ) return scan;
    
    cf_queue * result_queue = cf_queue_create(sizeof(void *), true);
    if ( !result_queue ) {
        scan->res_streamq = NULL;
        return scan;
    }

    scan->res_streamq = result_queue;
    scan->job_id = cf_get_rand64();
    scan->setname = setname == NULL ? NULL : strdup(setname);
    scan->ns = ns == NULL ? NULL : strdup(ns);
    as_scan_params_init(&scan->params, NULL);
    as_scan_udf_init(&scan->udf, AS_SCAN_UDF_NONE, NULL, NULL, NULL);

    return scan;
}

void as_scan_destroy(as_scan *scan) {

    if ( scan == NULL ) return;

    as_scan_udf_destroy(&scan->udf);
    if (scan->ns)      free(scan->ns);
    if (scan->setname) free(scan->setname);

    if ( scan->res_streamq ) {
        as_val *val = NULL;
        while (CF_QUEUE_OK == cf_queue_pop (scan->res_streamq, 
                                        &val, CF_QUEUE_NOWAIT)) {
            as_val_destroy(val);
            val = NULL;
        }

        cf_queue_destroy(scan->res_streamq);
        scan->res_streamq = NULL;
    }

    free(scan);
    scan = NULL;
}

cl_rv as_scan_foreach(as_scan * scan, const char * filename, const char * function, as_list * arglist) {
    return as_scan_udf_init(&scan->udf, AS_SCAN_UDF_CLIENT_RECORD, filename, function, arglist);
}

cl_rv as_scan_limit(as_scan *scan, uint64_t limit) {
    return CITRUSLEAF_OK;    
}

int citrusleaf_scan_init() {
    if (1 == cf_atomic32_incr(&scan_initialized)) {

        if (cf_debug_enabled()) {
            fprintf(stderr, "scan_init: creating %d threads\n",N_MAX_SCAN_THREADS);
        }

        memset(&g_null_task,0,sizeof(as_scan_task));

        // create dispatch queue
        g_scan_q = cf_queue_create(sizeof(as_scan_task), true);

        // create thread pool
        for (int i = 0; i < N_MAX_SCAN_THREADS; i++) {
            pthread_create(&g_scan_th[i], 0, as_scan_worker, 0);
        }
    }
    return(0);    
}

void citrusleaf_scan_shutdown() {

    for( int i=0; i<N_MAX_SCAN_THREADS; i++) {
        cf_queue_push(g_scan_q, &g_null_task);
    }

    for( int i=0; i<N_MAX_SCAN_THREADS; i++) {
        pthread_join(g_scan_th[i],NULL);
    }
    cf_queue_destroy(g_scan_q);
}
