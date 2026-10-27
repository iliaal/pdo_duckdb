#include <php.h>
#include <php_main.h>
#include <php_embed.h>
#include <zend_exceptions.h>
#include <zend_signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#ifndef ZTS
# error This test requires a ZTS PHP embed library.
#endif

#define WORKERS 2

static pthread_mutex_t barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cond = PTHREAD_COND_INITIALIZER;
static unsigned barrier_generation;
static unsigned barrier_arrived;
static bool barrier_failed;

static void abort_workers(void)
{
	pthread_mutex_lock(&barrier_mutex);
	barrier_failed = true;
	pthread_cond_broadcast(&barrier_cond);
	pthread_mutex_unlock(&barrier_mutex);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zts_instance_barrier, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zts_instance_barrier)
{
	unsigned generation;
	struct timespec deadline;
	bool failed;
	int status = 0;

	ZEND_PARSE_PARAMETERS_NONE();
	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
		zend_throw_exception(NULL, "Cannot read barrier clock", 0);
		RETURN_THROWS();
	}
	deadline.tv_sec += 30;
	pthread_mutex_lock(&barrier_mutex);
	generation = barrier_generation;
	if (++barrier_arrived == WORKERS) {
		barrier_arrived = 0;
		barrier_generation++;
		pthread_cond_broadcast(&barrier_cond);
	} else {
		while (generation == barrier_generation && !barrier_failed && status == 0) {
			status = pthread_cond_timedwait(&barrier_cond, &barrier_mutex, &deadline);
		}
		if (status != 0) {
			barrier_failed = true;
			pthread_cond_broadcast(&barrier_cond);
		}
	}
	failed = barrier_failed;
	pthread_mutex_unlock(&barrier_mutex);
	if (failed) {
		zend_throw_exception(NULL, status == ETIMEDOUT
			? "Concurrent PDO barrier timed out" : "Another concurrent PDO worker failed", 0);
		RETURN_THROWS();
	}
}

static const zend_function_entry functions[] = {
	PHP_FE(zts_instance_barrier, arginfo_zts_instance_barrier)
	PHP_FE_END
};

typedef struct {
	int id;
	int failed;
} worker_context;

static void *run_worker(void *arg)
{
	worker_context *worker = arg;
	int request;
	char code[160];

	ts_resource(0);
	ZEND_TSRMLS_CACHE_UPDATE();
	for (request = 0; request < 2 && !worker->failed; request++) {
		if (php_request_startup() == FAILURE) {
			worker->failed = 1;
			abort_workers();
			break;
		}
		SG(headers_sent) = 1;
		SG(request_info).no_headers = 1;
		snprintf(code, sizeof(code),
			"$worker=%d; $request=%d; require getenv('PDO_DUCKDB_ZTS_SCRIPT');",
			worker->id, request);
		zend_first_try {
			if (zend_eval_string(code, NULL, "concurrent PDO test") == FAILURE || EG(exception)) {
				if (EG(exception)) {
					zend_exception_error(EG(exception), E_WARNING);
				}
				worker->failed = 1;
			}
		} zend_catch {
			worker->failed = 1;
		} zend_end_try();
		if (worker->failed) {
			abort_workers();
		}
		php_request_shutdown(NULL);
	}
	/* Retiring a worker must close its persistent resources before TSRM drops
	 * the thread. Module globals such as PCRE also require this thread's TLS. */
	zend_hash_graceful_reverse_destroy(&EG(persistent_list));
	ts_free_thread();
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t threads[WORKERS];
	worker_context workers[WORKERS] = {{0, 0}, {1, 0}};
	char *ini;
	size_t ini_size;
	int started = 0, failed = 0, i;

	if (argc != 2 || argv[1][0] != '/' || strchr(argv[1], '\n') || strchr(argv[1], '"')) {
		fprintf(stderr, "usage: %s /absolute/path/pdo_duckdb.so\n", argv[0]);
		return 2;
	}
	ini_size = strlen(argv[1]) + 256;
	ini = malloc(ini_size);
	if (!ini) {
		return 2;
	}
	snprintf(ini, ini_size,
		"extension=\"%s\"\nhtml_errors=0\ndisplay_errors=stderr\nlog_errors=0\n"
		"max_execution_time=0\nmemory_limit=-1\n", argv[1]);
	if (!php_tsrm_startup()) {
		free(ini);
		return 2;
	}
	ZEND_TSRMLS_CACHE_UPDATE();
	zend_signal_startup();
	sapi_startup(&php_embed_module);
	php_embed_module.php_ini_ignore = 1;
	php_embed_module.php_ini_ignore_cwd = 1;
	php_embed_module.ini_entries = ini;
	php_embed_module.additional_functions = functions;
	if (php_embed_module.startup(&php_embed_module) == FAILURE) {
		sapi_shutdown();
		tsrm_shutdown();
		free(ini);
		return 2;
	}
	for (i = 0; i < WORKERS; i++) {
		int status = pthread_create(&threads[i], NULL, run_worker, &workers[i]);
		if (status != 0) {
			fprintf(stderr, "pthread_create: %s\n", strerror(status));
			failed = 1;
			abort_workers();
			break;
		}
		started++;
	}
	for (i = 0; i < started; i++) {
		if (pthread_join(threads[i], NULL) != 0 || workers[i].failed) {
			failed = 1;
		}
	}
	php_module_shutdown();
	sapi_shutdown();
	tsrm_shutdown();
	free(ini);
	pthread_cond_destroy(&barrier_cond);
	pthread_mutex_destroy(&barrier_mutex);
	if (!failed) {
		puts("PASS: concurrent PDO requests and shutdown");
	}
	return failed;
}
