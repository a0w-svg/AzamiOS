#include <pthread.h>
#include "../../include/azami/net_buf.h"
static void *references(void *arg)
{
    for (unsigned i = 0; i < 100000; i++) net_buf_free(net_buf_ref(arg));
    return NULL;
}
int test_net_buf_references(net_buf_t *buf)
{
    pthread_t threads[4];
    unsigned count = 0;
    for (; count < 4; count++) {
        if (pthread_create(&threads[count], NULL, references, buf)) break;
    }
    for (unsigned i = 0; i < count; i++) pthread_join(threads[i], NULL);
    return count == 4 ? 0 : -1;
}
