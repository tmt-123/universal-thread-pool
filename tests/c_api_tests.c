#include "universal_thread_pool/c_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s\n", #x); exit(1); } } while (0)

static void increment(void* context) { ++*(int*)context; }
static void error_callback(const char* message, void* context) {
    if (message && *message) ++*(int*)context;
}

int main(void) {
    utp_pool_options options = utp_default_options();
    options.threads = 1;
    utp_pool* pool = utp_create(&options);
    CHECK(pool != NULL);
    int count = 0;
    int errors = 0;
    CHECK(!utp_detach_checked(NULL, increment, &count, error_callback, &errors));
    CHECK(errors == 1);
    CHECK(utp_detach(pool, increment, &count));
    CHECK(utp_wait_for_idle_ms(pool, 2000));
    CHECK(count == 1);
    CHECK(utp_get_metrics(pool).completed_tasks_total == 1);
    size_t size = utp_metrics_json(pool, NULL, 0);
    CHECK(size > 1);
    char* json = (char*)malloc(size);
    CHECK(json != NULL);
    CHECK(utp_metrics_json(pool, json, size) == size);
    CHECK(json[size - 1] == '\0' && strstr(json, "c_api_pool") != NULL);
    free(json);
    utp_shutdown(pool, UTP_SHUTDOWN_DRAIN);
    CHECK(!utp_detach(pool, increment, &count));
    utp_destroy(pool);
    return 0;
}
