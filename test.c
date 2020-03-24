#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>

struct async_getaddrinfo_state
{
    struct gaicb gai_request; // TODO: call gai_cancel(&gai_request) to cancel.
    struct gaicb *gai_requests;
    struct sigevent sigevent;

    void (*callback)(void*, int, struct addrinfo*);
    void *context;
};

static void async_getaddrinfo_complete(union sigval context)
{
    struct async_getaddrinfo_state *state = (struct async_getaddrinfo_state *)context.sival_ptr;
    
    atomic_thread_fence(memory_order_acquire);
    
    int res = gai_error(&state->gai_request);

    void (*callback)(void*, int, struct addrinfo*) = state->callback;
    void *callback_context = state->context;
    struct addrinfo *info = res == 0 ? state->gai_request.ar_result : NULL;

    free(state);

    callback(callback_context, res, info);
}

static int async_getaddrinfo(const char *name, const char *service, const struct addrinfo *hints, void (*callback)(void*, int, struct addrinfo*), void *context)
{
    struct async_getaddrinfo_state *state = malloc(sizeof(struct async_getaddrinfo_state));

    state->gai_request.ar_name = name;
    state->gai_request.ar_service = service;
    state->gai_request.ar_request = hints;
    state->gai_request.ar_result = NULL;
    state->gai_requests = &state->gai_request;

    state->sigevent.sigev_notify = SIGEV_THREAD;
    state->sigevent.sigev_value.sival_ptr = state;
    state->sigevent.sigev_notify_function = async_getaddrinfo_complete;

    state->callback = callback;
    state->context = context;

    atomic_thread_fence(memory_order_release);

    int res = getaddrinfo_a(GAI_NOWAIT, &state->gai_requests, 1, &state->sigevent);

    if(res != 0)
    {
        free(state);
    }

    return res;
}

struct test_all_state
{
    sem_t semaphore;
    unsigned completed;
};

struct test_one_state
{
    struct test_all_state *all_state;
    const char *host;
};

static void test_onresolve(void *context, int result, struct addrinfo *info)
{
    int thread_id = pthread_self();

    struct test_one_state *state = (struct test_one_state*)context;

    if(result == 0)
    {
        for(struct addrinfo *iter = info; iter; iter = iter->ai_next)
        {
            void *sin_addr;

            switch(iter->ai_family)
            {
                case AF_INET:
                    sin_addr = &((struct sockaddr_in*)iter->ai_addr)->sin_addr;
                    break;
                case AF_INET6:
                    sin_addr = &((struct sockaddr_in6*)iter->ai_addr)->sin6_addr;
                    break;
                default:
                    sin_addr = NULL;
            }

            if(sin_addr)
            {
                char buffer[256];
                inet_ntop(iter->ai_family, &((struct sockaddr_in*)iter->ai_addr)->sin_addr, buffer, sizeof(buffer));
                printf("(%d) [%s] %s\n", thread_id, state->host, buffer);
            }
        }

        freeaddrinfo(info);
    }
    
    if(atomic_fetch_add(&state->all_state->completed, 1) == 1)
    {
        sem_post(&state->all_state->semaphore);
    }
}

int main(void)
{
    struct test_all_state state;
    sem_init(&state.semaphore, 0, 0);
    state.completed = 0;

    struct addrinfo hints =
    {
        .ai_family = AF_UNSPEC,
        .ai_protocol = IPPROTO_TCP,
        .ai_socktype = SOCK_STREAM
    };

    struct test_one_state stateA = { &state, "contoso.com" };
    int resA = async_getaddrinfo(stateA.host, NULL, &hints, test_onresolve, &stateA);

    struct test_one_state stateB = { &state, "microsoft.com" };
    int resB = async_getaddrinfo(stateB.host, NULL, &hints, test_onresolve, &stateB);
    
    printf("resA: %d, resB: %d\n", resA, resB);
    
    if(resA != 0 || resB != 0)
    {
        return 0;
    }

    sem_wait(&state.semaphore);
    sem_destroy(&state.semaphore);

    return 0;
}
