
#include "../test.h"
#include <citrusleaf/as_stream.h>
#include <citrusleaf/as_types.h>
#include <citrusleaf/as_module.h>
#include <citrusleaf/mod_lua.h>
#include <citrusleaf/mod_lua_config.h>
#include <citrusleaf/cl_query.h>
#include <limits.h>
#include <stdlib.h>

/******************************************************************************
 * FUNCTIONS
 *****************************************************************************/

as_aerospike as;

as_stream_status print_stream_write(const as_stream * s, const as_val * v) {
    if ( v != NULL ) info("%s",as_val_tostring(v));
    return AS_STREAM_OK;
}

const as_stream_hooks print_stream_hooks = {
    .read       = NULL,
    .write      = print_stream_write
};

as_stream * print_stream_new() {
    return as_stream_new(NULL, &print_stream_hooks);
}

static int test_log(const as_aerospike * as, const char * file, const int line, const int level, const char * msg) {
    char l[10] = {'\0'};
    switch(level) {
        case 1:
            strncpy(l,"WARN",10);
            break;
        case 2:
            strncpy(l,"INFO",10);
            break;
        case 3:
            strncpy(l,"DEBUG",10);
            break;
        default:
            strncpy(l,"TRACE",10);
            break;
    }
    atf_log_line(stderr, l, ATF_LOG_PREFIX, file, line, msg);
    return 0;
}

static const as_aerospike_hooks test_aerospike_hooks = {
    .destroy = NULL,
    .rec_create = NULL,
    .rec_update = NULL,
    .rec_remove = NULL,
    .rec_exists = NULL,
    .log = test_log,
};

/******************************************************************************
 * TEST CASES
 *****************************************************************************/
 
TEST( aggr_simple_1, "get numeric bin without aggregation" ) {
    
    extern cl_cluster * cluster;

    as_stream * pstream = print_stream_new();

    as_query * q = as_query_new("test","test");
    as_query_select(q, "b");
    as_query_where(q, "a", string_equals("abc"));
    
    citrusleaf_query_stream(cluster, q, pstream);

	as_query_destroy(q);
    as_stream_destroy(pstream);
}

TEST( aggr_simple_2, "sum of numeric bins" ) {
    
    extern cl_cluster * cluster;

    as_stream * pstream = print_stream_new();

    as_query * q = as_query_new("test","test");
    as_query_select(q, "b");
    as_query_where(q, "a", string_equals("abc"));
    as_query_aggregate(q, "aggr", "sum", NULL);
    
    citrusleaf_query_stream(cluster, q, pstream);

	as_query_destroy(q);
    as_stream_destroy(pstream);
}

/******************************************************************************
 * TEST SUITE
 *****************************************************************************/

static bool before(atf_suite * suite) {

    citrusleaf_query_init();

    as_aerospike_init(&as, NULL, &test_aerospike_hooks);

    // chris: disabling Lua cache, because it takes too long to prime.
    mod_lua_config_op conf_op = {
        .optype     = MOD_LUA_CONFIG_OP_INIT,
        .arg        = NULL,
        .config     = mod_lua_config_client(false, "modules/mod-lua/src/lua", "modules/mod-lua/src/test/lua")
    }; 

	// Chris (Todo) it is leaking here, Who cleans it
	// up ??
    // chris:   we don't have a destroy for modules (yet)
    //          the reason is we didn't need it in asd.
    as_module_init(&mod_lua);
    as_module_configure(&mod_lua, &conf_op);
 
    return true;
}

static bool after(atf_suite * suite) {
    citrusleaf_query_shutdown();
    return true;
}

SUITE( aggr_simple, "aggregate simple" ) {
    suite_before( before );
    suite_after( after );
    
    suite_add( aggr_simple_1 );
    suite_add( aggr_simple_2 );
}
