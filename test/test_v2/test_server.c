 /*
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Triad National Security, LLC
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/util/error.h"

#include "test_server.h"
#include "test_common.h"
#include "cli_stages.h"
#include "server_callbacks.h"

int my_server_id = 0;

server_info_t *my_server_info = NULL;
pmix_list_t *server_list = NULL;
pmix_list_t *server_nspace = NULL;

/* server destructor */
static void sdes(server_info_t *s)
{
    close(s->rd_fd);
    close(s->wr_fd);
    if (s->evread) {
        pmix_event_del(s->evread);
    }
    s->evread = NULL;
    if (NULL != s->hostname) {
        free(s->hostname);
    }
}

/* server constructor */
static void scon(server_info_t *s)
{
    s->hostname = NULL;
    s->idx = 0;
    s->pid = 0;
    s->rd_fd = -1;
    s->wr_fd = -1;
    s->evread = NULL;
    s->modex_cbfunc = NULL;
    s->cbdata = NULL;
}

PMIX_CLASS_INSTANCE(server_info_t,
                    pmix_list_item_t,
                    scon, sdes);

/* namespace destructor */
static void nsdes(server_nspace_t *ns)
{
    if (ns->task_map) {
        free(ns->task_map);
    }
}

/* namespace constructor */
static void nscon(server_nspace_t *ns)
{
    memset(ns->name, 0, PMIX_MAX_NSLEN);
    ns->ntasks = 0;
    ns->task_map = NULL;
}

PMIX_CLASS_INSTANCE(server_nspace_t,
                    pmix_list_item_t,
                    nscon, nsdes);

static int server_send_procs(void);
static void server_read_cb(int fd, short event, void *arg);
static int srv_wait_all(double timeout);
static int server_fwd_msg(msg_hdr_t *msg_hdr, char *buf, size_t size);
static int server_send_msg(msg_hdr_t *msg_hdr, char *data, size_t size);
static void remove_server_item(server_info_t *server);
static void server_unpack_dmdx(char *buf, int *sender, pmix_proc_t *proc);
static int server_pack_dmdx(int sender_id, const char *nspace, int rank,
                            char **buf);
static void _dmdx_cb(int status, char *data, size_t sz, void *cbdata);
static void fill_global_validation_params(pmix_proc_t proc, int univ_size, validation_params *v_params);

static void release_cb(pmix_status_t status, void *cbdata)
{
    int *ptr = (int*)cbdata;
    *ptr = 0;
}

void set_client_argv(test_params *params, char ***argv)
{
    pmix_argv_append_nosize(argv, params->binary);

    pmix_argv_append_nosize(argv, "-n");
    if (NULL == params->np) {
        pmix_argv_append_nosize(argv, "1");
    } else {
        pmix_argv_append_nosize(argv, params->np);
    }

    if( params->verbose ){
        pmix_argv_append_nosize(argv, "-v");
    }
    if (NULL != params->prefix) {
        pmix_argv_append_nosize(argv, "-o");
        pmix_argv_append_nosize(argv, params->prefix);
    }
    if (params->nonblocking) {
        pmix_argv_append_nosize(argv, "-nb");
    }
    /*
    if (params->collect) {
        pmix_argv_append_nosize(argv, "-c");
    }
    if (params->collect_bad) {
        pmix_argv_append_nosize(argv, "--collect-corrupt");
    }
    */
}

static void fill_seq_ranks_array(uint32_t nprocs, char **ranks)
{
    uint32_t i;
    int len = 0, max_ranks_len;
    if (0 >= nprocs) {
        return;
    }
    max_ranks_len = nprocs * (MAX_DIGIT_LEN+1);
    *ranks = (char*) malloc(max_ranks_len);
    // i is equivalent to local rank
    for (i = 0; i < nprocs; i++) {
        len += snprintf(*ranks + len, max_ranks_len-len-1, "%d", nodes[my_server_id].pmix_rank[i]);
        if (i != nprocs-1) {
            len += snprintf(*ranks + len, max_ranks_len-len-1, "%c", ',');
        }
    }
    if (len >= max_ranks_len-1) {
        free(*ranks);
        *ranks = NULL;
        TEST_ERROR_EXIT(("ERROR: Server id: %d Not enough allocated space for global ranks array.", my_server_id));
    }
}

static int server_find_id(const char *nspace, int rank)
{
    server_nspace_t *tmp;

    PMIX_LIST_FOREACH(tmp, server_nspace, server_nspace_t) {
        if (0 == strcmp(tmp->name, nspace)) {
            return tmp->task_map[rank];
        }
    }
    return -1;
}

static void set_namespace(validation_params *v_params)
{
    size_t ninfo;
    pmix_info_t *info;
    ninfo = 8;
    char *regex, *ppn, *tmp;
    char *ranks = NULL, **node_string = NULL;
    char **rks=NULL;
    int i, j;
    int rc;

    PMIX_INFO_CREATE(info, ninfo);
    pmix_strncpy(info[0].key, PMIX_UNIV_SIZE, PMIX_MAX_KEYLEN);
    info[0].value.type = PMIX_UINT32;
    info[0].value.data.uint32 = v_params->pmix_univ_size;

    pmix_strncpy(info[1].key, PMIX_SPAWNED, PMIX_MAX_KEYLEN);
    info[1].value.type = PMIX_UINT32;
    info[1].value.data.uint32 = 0;

    pmix_strncpy(info[2].key, PMIX_LOCAL_SIZE, PMIX_MAX_KEYLEN);
    info[2].value.type = PMIX_UINT32;
    info[2].value.data.uint32 = v_params->pmix_local_size;

    /* generate the array of local peers */
    TEST_VERBOSE(("Server id: %d local_size: %d", my_server_id, v_params->pmix_local_size));
    fill_seq_ranks_array(v_params->pmix_local_size, &ranks);
    if (NULL == ranks) {
        return;
    }
    strncpy(v_params->pmix_local_peers, ranks, PMIX_MAX_KEYLEN);
    TEST_VERBOSE(("Server id: %d Local peers array: %s", my_server_id, ranks));
    pmix_strncpy(info[3].key, PMIX_LOCAL_PEERS, PMIX_MAX_KEYLEN);
    info[3].value.type = PMIX_STRING;
    info[3].value.data.string = strdup(ranks);

    /* assemble the node and proc map info */
    for (i = 0; i < v_params->pmix_num_nodes; i++) {
        pmix_argv_append_nosize(&node_string, nodes[i].pmix_hostname);
    }

    if (NULL != node_string) {
        tmp = pmix_argv_join(node_string, ',');
        pmix_argv_free(node_string);
        node_string = NULL;
        if (PMIX_SUCCESS != (rc = PMIx_generate_regex(tmp, &regex) )) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        free(tmp);
        PMIX_INFO_LOAD(&info[4], PMIX_NODE_MAP, regex, PMIX_REGEX);
    }

    /* generate the global proc map for multiserver case  */
    if (2 <= v_params->pmix_num_nodes) {
        for (j = 0; j < v_params->pmix_num_nodes; j++){
            for (i = 0; i < nodes[j].pmix_local_size; i++) {
                asprintf(&ppn, "%d", nodes[j].pmix_rank[i]);
                pmix_argv_append_nosize(&node_string, ppn);
                TEST_VERBOSE(("multiserver, server id: %d, ppn: %s, node_string: %s",
                my_server_id, ppn, node_string[i]));
                free(ppn);
            }
            ppn = pmix_argv_join(node_string, ',');
            pmix_argv_append_nosize(&rks, ppn);
            TEST_VERBOSE(("my_server id: %d, remote server: %d, remote's local ranks: %s",
                my_server_id, j, ppn));
            free(ppn);
            pmix_argv_free(node_string);
            node_string = NULL;
        }
        ranks = pmix_argv_join(rks, ';');
    }
    TEST_VERBOSE(("server ID: %d ranks array: %s", my_server_id, ranks));
    PMIx_generate_ppn(ranks, &ppn);
    free(ranks);
    PMIX_INFO_LOAD(&info[5], PMIX_PROC_MAP, ppn, PMIX_REGEX);
    free(ppn);

    pmix_strncpy(info[6].key, PMIX_JOB_SIZE, PMIX_MAX_KEYLEN);
    info[6].value.type = PMIX_UINT32;
    info[6].value.data.uint32 = v_params->pmix_univ_size;

    pmix_strncpy(info[7].key, PMIX_APPNUM, PMIX_MAX_KEYLEN);
    info[7].value.type = PMIX_UINT32;
    info[7].value.data.uint32 = getpid ();

    int in_progress = 1;
    if (PMIX_SUCCESS == (rc = PMIx_server_register_nspace(v_params->pmix_nspace, v_params->pmix_local_size,
                                    info, ninfo, release_cb, &in_progress))) {
        PMIX_WAIT_FOR_COMPLETION(in_progress);
    }
    PMIX_INFO_FREE(info, ninfo);
}

static void server_unpack_procs(char *buf, size_t size)
{
    char *ptr = buf;
    size_t i;
    size_t ns_count;
    char *nspace;

    while ((size_t)(ptr - buf) < size) {
        memcpy (&ns_count, ptr, sizeof(size_t));
        ptr += sizeof(size_t);

        for (i = 0; i < ns_count; i++) {
            server_nspace_t *tmp, *ns_item = NULL;
            size_t ltasks, ntasks;
            int server_id;

            memcpy (&server_id, ptr, sizeof(int));
            ptr += sizeof(int);

            nspace = ptr;
            ptr += PMIX_MAX_NSLEN+1;

            memcpy (&ntasks, ptr, sizeof(size_t));
            ptr += sizeof(size_t);

            memcpy (&ltasks, ptr, sizeof(size_t));
            ptr += sizeof(size_t);

            PMIX_LIST_FOREACH(tmp, server_nspace, server_nspace_t) {
                if (0 == strcmp(nspace, tmp->name)) {
                    ns_item = tmp;
                    break;
                }
            }
            if (NULL == ns_item) {
                ns_item = PMIX_NEW(server_nspace_t);
                memcpy(ns_item->name, nspace, PMIX_MAX_NSLEN);
                pmix_list_append(server_nspace, &ns_item->super);
                ns_item->ltasks = ltasks;
                ns_item->ntasks = ntasks;
                ns_item->task_map = (int*)malloc(sizeof(int) * ntasks);
                memset(ns_item->task_map, -1, sizeof(int) * ntasks);
            } else {
                assert(ns_item->ntasks == ntasks);
            }
            size_t i;
            for (i = 0; i < ltasks; i++) {
                int rank;
                memcpy (&rank, ptr, sizeof(int));
                ptr += sizeof(int);
                if (ns_item->task_map[rank] >= 0) {
                    continue;
                }
                ns_item->task_map[rank] = server_id;
            }
        }
    }
}

static size_t server_pack_procs(int server_id, char **buf, size_t size)
{
    size_t ns_count = pmix_list_get_size(server_nspace);
    size_t buf_size = sizeof(size_t) + (PMIX_MAX_NSLEN+1)*ns_count;
    server_nspace_t *tmp;
    char *ptr;

    if (0 == ns_count) {
        return 0;
    }

    buf_size += size;
    /* compute size: server_id + total + local procs count + ranks */
    PMIX_LIST_FOREACH(tmp, server_nspace, server_nspace_t) {
        buf_size += sizeof(int) + sizeof(size_t) + sizeof(size_t) +
                sizeof(int) * tmp->ltasks;
    }
    *buf = (char*)realloc(*buf, buf_size);
    memset(*buf + size, 0, buf_size);
    ptr = *buf + size;
    /* pack ns count */
    memcpy(ptr, &ns_count, sizeof(size_t));
    ptr += sizeof(size_t);

    assert(server_nspace->pmix_list_length);

    PMIX_LIST_FOREACH(tmp, server_nspace, server_nspace_t) {
        size_t i;
        /* pack server_id */
        memcpy(ptr, &server_id, sizeof(int));
        ptr += sizeof(int);
        /* pack ns name */
        memcpy(ptr, tmp->name, PMIX_MAX_NSLEN+1);
        ptr += PMIX_MAX_NSLEN+1;
        /* pack ns total size */
        memcpy(ptr, &tmp->ntasks, sizeof(size_t));
        ptr += sizeof(size_t);
        /* pack ns local size */
        memcpy(ptr, &tmp->ltasks, sizeof(size_t));
        ptr += sizeof(size_t);
        /* pack ns ranks */
        for(i = 0; i < tmp->ntasks; i++) {
            if (tmp->task_map[i] == server_id) {
                int rank = (int)i;
                memcpy(ptr, &rank, sizeof(int));
                ptr += sizeof(int);
            }
        }
    }
    assert((size_t)(ptr - *buf) == buf_size);
    return buf_size;
}

static void remove_server_item(server_info_t *server)
{
    pmix_list_remove_item(server_list, &server->super);
    PMIX_DESTRUCT_LOCK(&server->lock);
    PMIX_RELEASE(server);
}

static int srv_wait_all(double timeout)
{
    server_info_t *server, *next;
    pid_t pid;
    int status;
    struct timeval tv;
    double start_time, cur_time;
    int ret = 0;

    gettimeofday(&tv, NULL);
    start_time = tv.tv_sec + 1E-6*tv.tv_usec;
    cur_time = start_time;

    /* Remove this server from the list */
    PMIX_LIST_FOREACH_SAFE(server, next, server_list, server_info_t) {
        if (server->pid == getpid()) {
            /* remove himself */
            remove_server_item(server);
            break;
        }
    }

    while (!pmix_list_is_empty(server_list) &&
                                (timeout >= (cur_time - start_time))) {
        TEST_VERBOSE(("Server list is not empty"));
        pid = waitpid(-1, &status, 0);
        if (pid >= 0) {
            PMIX_LIST_FOREACH_SAFE(server, next, server_list, server_info_t) {
                if (server->pid == pid) {
                    TEST_VERBOSE(("server %d finalize PID:%d with status %d", server->idx,
                                server->pid, WEXITSTATUS(status)));
                    ret += WEXITSTATUS(status);
                    remove_server_item(server);
                }
            }
        }
        // calculate current timestamp
        gettimeofday(&tv, NULL);
        cur_time = tv.tv_sec + 1E-6*tv.tv_usec;
    }
    TEST_VERBOSE(("Inside serv_wait_all, ret = %d", ret));
    return ret;
}

static int server_fwd_msg(msg_hdr_t *msg_hdr, char *buf, size_t size)
{
    server_info_t *tmp_server, *server = NULL;
    int rc = PMIX_SUCCESS;

    PMIX_LIST_FOREACH(tmp_server, server_list, server_info_t) {
        if (tmp_server->idx == msg_hdr->dst_id) {
            server = tmp_server;
            break;
        }
    }
    if (NULL == server) {
        return PMIX_ERROR;
    }
    rc = write(server->wr_fd, msg_hdr, sizeof(msg_hdr_t));
    if (rc != sizeof(msg_hdr_t)) {
        return PMIX_ERROR;
    }
    rc = write(server->wr_fd, buf, size);
    if (rc != (ssize_t)size) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

static int server_send_msg(msg_hdr_t *msg_hdr, char *data, size_t size)
{
    size_t ret = 0;
    server_info_t *server = NULL, *server_tmp;
    if (0 == my_server_id) {
        PMIX_LIST_FOREACH(server_tmp, server_list, server_info_t) {
            if (server_tmp->idx == msg_hdr->dst_id) {
                server = server_tmp;
                break;
            }
        }
        if (NULL == server) {
            abort();
        }
    } else {
        server = (server_info_t *)pmix_list_get_first(server_list);
    }

    ret += write(server->wr_fd, msg_hdr, sizeof(msg_hdr_t));
    ret += write(server->wr_fd, data, size);
    if (ret != (sizeof(*msg_hdr) + size)) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

static void _send_procs_cb(pmix_status_t status, const char *data,
                           size_t ndata, void *cbdata,
                           pmix_release_cbfunc_t relfn, void *relcbd)
{
    server_info_t *server = (server_info_t*)cbdata;

    server_unpack_procs((char*)data, ndata);
    free((char*)data);
    PMIX_WAKEUP_THREAD(&server->lock);
}

static int server_send_procs(void)
{
    server_info_t *server;
    msg_hdr_t msg_hdr;
    int rc = PMIX_SUCCESS;
    char *buf = NULL;

    if (0 == my_server_id) {
        server = my_server_info;
    } else {
        server = (server_info_t *)pmix_list_get_first(server_list);
    }

    msg_hdr.cmd = CMD_FENCE_CONTRIB;
    msg_hdr.dst_id = 0;
    msg_hdr.src_id = my_server_id;
    msg_hdr.size = server_pack_procs(my_server_id, &buf, 0);
    server->modex_cbfunc = _send_procs_cb;
    server->cbdata = (void*)server;

    server->lock.active = true;

    if (PMIX_SUCCESS != (rc = server_send_msg(&msg_hdr, buf, msg_hdr.size))) {
        if (buf) {
            free(buf);
        }
        return PMIX_ERROR;
    }
    if (buf) {
        free(buf);
    }

    PMIX_WAIT_THREAD(&server->lock);
    return PMIX_SUCCESS;
}

int server_barrier(void)
{
    server_info_t *server;
    msg_hdr_t msg_hdr;
    int rc = PMIX_SUCCESS;

    if (0 == my_server_id) {
        server = my_server_info;
    } else {
        server = (server_info_t *)pmix_list_get_first(server_list);
    }

    msg_hdr.cmd = CMD_BARRIER_REQUEST;
    msg_hdr.dst_id = 0;
    msg_hdr.src_id = my_server_id;
    msg_hdr.size = 0;

    server->lock.active = true;

    if (PMIX_SUCCESS != (rc = server_send_msg(&msg_hdr, NULL, 0))) {
        return PMIX_ERROR;
    }
    PMIX_WAIT_THREAD(&server->lock);

    return PMIX_SUCCESS;
}

static void _libpmix_cb(void *cbdata)
{
    char *ptr = (char*)cbdata;
    if (ptr) {
        free(ptr);
    }
}

static void server_read_cb(int fd, short event, void *arg)
{
    server_info_t *server = (server_info_t*)arg;
    msg_hdr_t msg_hdr;
    char *msg_buf = NULL;
    static char *fence_buf = NULL;
    int rc;
    static size_t barrier_cnt = 0;
    static size_t contrib_cnt = 0;
    static size_t fence_buf_offset = 0;

    rc = read(server->rd_fd, &msg_hdr, sizeof(msg_hdr_t));
    if (rc <= 0) {
        return;
    }
    if (msg_hdr.size) {
        msg_buf = (char*) malloc(sizeof(char) * msg_hdr.size);
        rc += read(server->rd_fd, msg_buf, msg_hdr.size);
    }
    if (rc != (int)(sizeof(msg_hdr_t) + msg_hdr.size)) {
        TEST_ERROR(("error read from %d", server->idx));
    }

    if (my_server_id != msg_hdr.dst_id) {
        server_fwd_msg(&msg_hdr, msg_buf, msg_hdr.size);
        free(msg_buf);
        return;
    }

    switch(msg_hdr.cmd) {
        case CMD_BARRIER_REQUEST:
            barrier_cnt++;
            TEST_VERBOSE(("CMD_BARRIER_REQ req from %d cnt %lu", msg_hdr.src_id,
                          (unsigned long)barrier_cnt));
            if (pmix_list_get_size(server_list) == barrier_cnt) {
                barrier_cnt = 0; /* reset barrier counter */
                server_info_t *tmp_server;
                PMIX_LIST_FOREACH(tmp_server, server_list, server_info_t) {
                    msg_hdr_t resp_hdr;
                    resp_hdr.dst_id = tmp_server->idx;
                    resp_hdr.src_id = my_server_id;
                    resp_hdr.cmd = CMD_BARRIER_RESPONSE;
                    resp_hdr.size = 0;
                    server_send_msg(&resp_hdr, NULL, 0);
                }
            }
            break;
        case CMD_BARRIER_RESPONSE:
            TEST_VERBOSE(("%d: CMD_BARRIER_RESP", my_server_id));
            PMIX_WAKEUP_THREAD(&server->lock);
            break;
        case CMD_FENCE_CONTRIB:
            contrib_cnt++;
            if (msg_hdr.size > 0) {
                fence_buf = (char*)realloc((void*)fence_buf,
                                           fence_buf_offset + msg_hdr.size);
                memcpy(fence_buf + fence_buf_offset, msg_buf, msg_hdr.size);
                fence_buf_offset += msg_hdr.size;
                free(msg_buf);
                msg_buf = NULL;
            }

            TEST_VERBOSE(("CMD_FENCE_CONTRIB req from %d cnt %lu size %d",
                        msg_hdr.src_id, (unsigned long)contrib_cnt, msg_hdr.size));
            if (pmix_list_get_size(server_list) == contrib_cnt) {
                server_info_t *tmp_server;
                PMIX_LIST_FOREACH(tmp_server, server_list, server_info_t) {
                    msg_hdr_t resp_hdr;
                    resp_hdr.dst_id = tmp_server->idx;
                    resp_hdr.src_id = my_server_id;
                    resp_hdr.cmd = CMD_FENCE_COMPLETE;
                    resp_hdr.size = fence_buf_offset;
                    server_send_msg(&resp_hdr, fence_buf, fence_buf_offset);
                }
                TEST_VERBOSE(("CMD_FENCE_CONTRIB complete, size %lu",
                              (unsigned long)fence_buf_offset));
                if (fence_buf) {
                    free(fence_buf);
                    fence_buf = NULL;
                    fence_buf_offset = 0;
                }
                contrib_cnt = 0;
            }
            break;
        case CMD_FENCE_COMPLETE:
            TEST_VERBOSE(("%d: CMD_FENCE_COMPLETE size %d", my_server_id,
                        msg_hdr.size));
            server->modex_cbfunc(PMIX_SUCCESS, msg_buf, msg_hdr.size,
                                 server->cbdata, _libpmix_cb, msg_buf);
            msg_buf = NULL;
            break;
        case CMD_DMDX_REQUEST: {
            int *sender_id;
            pmix_proc_t proc;
            if (NULL == msg_buf) {
                abort();
            }
            sender_id = (int*)malloc(sizeof(int));
            server_unpack_dmdx(msg_buf, sender_id, &proc);
            TEST_VERBOSE(("%d: CMD_DMDX_REQUEST from %d: %s:%d", my_server_id,
                        *sender_id, proc.nspace, proc.rank));
            rc = PMIx_server_dmodex_request(&proc, _dmdx_cb, (void*)sender_id);
            break;
        }
        case CMD_DMDX_RESPONSE:
            TEST_VERBOSE(("%d: CMD_DMDX_RESPONSE", my_server_id));
            server->modex_cbfunc(PMIX_SUCCESS, msg_buf, msg_hdr.size,
                                 server->cbdata, _libpmix_cb, msg_buf);
            msg_buf = NULL;
            break;
    }
    if (NULL != msg_buf) {
        free(msg_buf);
    }
}

int server_fence_contrib(char *data, size_t ndata,
                         pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    server_info_t *server;
    msg_hdr_t msg_hdr;
    int rc = PMIX_SUCCESS;

    if (0 == my_server_id) {
        server = my_server_info;
    } else {
        server = (server_info_t *)pmix_list_get_first(server_list);
    }
    msg_hdr.cmd = CMD_FENCE_CONTRIB;
    msg_hdr.dst_id = 0;
    msg_hdr.src_id = my_server_id;
    msg_hdr.size = ndata;
    server->modex_cbfunc = cbfunc;
    server->cbdata = cbdata;

    if (PMIX_SUCCESS != (rc = server_send_msg(&msg_hdr, data, ndata))) {
        return PMIX_ERROR;
    }
    return rc;
}

static int server_pack_dmdx(int sender_id, const char *nspace, int rank,
                            char **buf)
{
    size_t buf_size = sizeof(int) + PMIX_MAX_NSLEN +1 + sizeof(int);
    char *ptr;

    *buf = (char*)malloc(buf_size);
    ptr = *buf;

    memcpy(ptr, &sender_id, sizeof(int));
    ptr += sizeof(int);

    memcpy(ptr, nspace, PMIX_MAX_NSLEN+1);
    ptr += PMIX_MAX_NSLEN +1;

    memcpy(ptr, &rank, sizeof(int));
    ptr += sizeof(int);

    return buf_size;
}

static void server_unpack_dmdx(char *buf, int *sender, pmix_proc_t *proc)
{
    char *ptr = buf;

    *sender = *(int *)ptr;
    ptr += sizeof(int);

    memcpy(proc->nspace, ptr, PMIX_MAX_NSLEN +1);
    ptr += PMIX_MAX_NSLEN +1;

    proc->rank = *(int *)ptr;
    ptr += sizeof(int);
}


static void _dmdx_cb(int status, char *data, size_t sz, void *cbdata)
{
    msg_hdr_t msg_hdr;
    int *sender_id = (int*)cbdata;

    msg_hdr.cmd = CMD_DMDX_RESPONSE;
    msg_hdr.src_id = my_server_id;
    msg_hdr.size = sz;
    msg_hdr.dst_id = *sender_id;
    TEST_VERBOSE(("srv #%d: DMDX RESPONSE: receiver=%d, size=%lu,",
                  my_server_id, *sender_id, (unsigned long)sz));
    free(sender_id);

    server_send_msg(&msg_hdr, data, sz);
}

int server_dmdx_get(const char *nspace, int rank,
                    pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    server_info_t *server = NULL, *tmp;
    msg_hdr_t msg_hdr;
    pmix_status_t rc = PMIX_SUCCESS;
    char *buf = NULL;


    if (0 > (msg_hdr.dst_id = server_find_id(nspace, rank))) {
        TEST_ERROR(("%d: server not found for %s:%d", my_server_id, nspace, rank));
        goto error;
    }

    if (0 == my_server_id) {
        PMIX_LIST_FOREACH(tmp, server_list, server_info_t) {
            if (tmp->idx == msg_hdr.dst_id) {
                server = tmp;
                break;
            }
        }
    } else {
        server = (server_info_t *)pmix_list_get_first(server_list);
    }

    if (server == NULL) {
        goto error;
    }

    msg_hdr.cmd = CMD_DMDX_REQUEST;
    msg_hdr.src_id = my_server_id;
    msg_hdr.size = server_pack_dmdx(my_server_id, nspace, rank, &buf);
    server->modex_cbfunc = cbfunc;
    server->cbdata = cbdata;

    if (PMIX_SUCCESS != (rc = server_send_msg(&msg_hdr, buf, msg_hdr.size))) {
        rc = PMIX_ERROR;
    }
    free(buf);
    return rc;

error:
    cbfunc(PMIX_ERROR, NULL, 0, cbdata, NULL, 0);
    return PMIX_ERROR;
}

static void set_handler_default(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(sig, &act, (struct sigaction *)0);
}

static pmix_event_t handler;
static void wait_signal_callback(int fd, short event, void *arg)
{
    pmix_event_t *sig = (pmix_event_t*) arg;
    int status;
    pid_t pid;
    int i;

    if (SIGCHLD != pmix_event_get_signal(sig)) {
        return;
    }

    /* we can have multiple children leave but only get one
     * sigchild callback, so reap all the waitpids until we
     * don't get anything valid back */
    while (1) {
        pid = waitpid(-1, &status, WNOHANG);
        if (-1 == pid && EINTR == errno) {
            /* try it again */
            continue;
        }
        /* if we got garbage, then nothing we can do */
        if (pid <= 0) {
            goto done;
        }
        /* we are already in an event, so it is safe to access the list */
        for(i=0; i < cli_info_cnt; i++){
            if( cli_info[i].pid == pid ){
                /* found it! */
                if (WIFEXITED(status)) {
                    cli_info[i].exit_code = WEXITSTATUS(status);
                    TEST_VERBOSE(("WIFEXITED, pid = %d, exit_code = %d", pid, cli_info[i].exit_code));
                } else {
                    if (WIFSIGNALED(status)) {
                        cli_info[i].exit_code = WTERMSIG(status) + 128;
                        TEST_VERBOSE(("WIFSIGNALED, pid = %d, exit_code = %d", pid, cli_info[i].exit_code));
                    }
                }
                cli_cleanup(&cli_info[i]);
                cli_info[i].alive = false;
                break;
            }
        }
    }
  done:
    for(i=0; i < cli_info_cnt; i++){
        if (cli_info[i].alive) {
            /* someone is still alive */
            return;
        }
    }
    /* get here if nobody is still alive */
    test_complete = true;
}

int server_init(test_params *params, validation_params *v_params)
{
    pmix_info_t info[2];
    uint32_t local_size;
    int rc = PMIX_SUCCESS;

    /* fork/init servers procs */
    if (v_params->pmix_num_nodes >= 1) {
        int i;
        server_info_t *server_info = NULL;
        server_list = PMIX_NEW(pmix_list_t);

        TEST_VERBOSE(("pmix server %d started PID:%d", my_server_id, getpid()));
        for (i = v_params->pmix_num_nodes - 1; i >= 0; i--) {
            pid_t pid;
            server_info = PMIX_NEW(server_info_t);

            int fd1[2];
            int fd2[2];

            pipe(fd1);
            pipe(fd2);

            // copy hostname from nodes array
            server_info->hostname = strdup(nodes[i].pmix_hostname);
            strncpy(v_params->pmix_hostname, server_info->hostname, PMIX_MAX_KEYLEN-1);
            if (0 != i) {
                pid = fork();
                if (pid < 0) {
                    TEST_ERROR(("Fork failed"));
                    return pid;
                }
                if (pid == 0) {
                    server_list = PMIX_NEW(pmix_list_t);
                    my_server_info = server_info;
                    my_server_id = i;
                    server_info->idx = 0;
                    TEST_VERBOSE(("my_server_id after fork: %d, server_info->idx = %d",
                                  my_server_id, server_info->idx));
                    server_info->pid = getppid();
                    server_info->rd_fd = fd1[0];
                    server_info->wr_fd = fd2[1];
                    close(fd1[1]);
                    close(fd2[0]);
                    PMIX_CONSTRUCT_LOCK(&server_info->lock);
                    pmix_list_append(server_list, &server_info->super);
                    break;
                }

                server_info->idx = i; // idx is id of server we are talking to (descriptor of remote peer)
                server_info->pid = pid;
                server_info->wr_fd = fd1[1];
                server_info->rd_fd = fd2[0];
                PMIX_CONSTRUCT_LOCK(&server_info->lock);
                close(fd1[0]);
                close(fd2[1]);
            }
            else {
                my_server_info = server_info;
                server_info->pid = getpid();
                server_info->idx  = 0;
                server_info->rd_fd = fd1[0];
                server_info->wr_fd = fd1[1];
                PMIX_CONSTRUCT_LOCK(&server_info->lock);
                close(fd2[0]);
                close(fd2[1]);
            }
            pmix_list_append(server_list, &server_info->super);
        }
    }
    // set validation params for server-specific (i.e., node-specific) info.
    v_params->pmix_nodeid = my_server_id;
    TEST_VERBOSE(("my_server_id: %d, pmix_node_id: %d, pmix_hostname: %s",
                   my_server_id, v_params->pmix_nodeid, v_params->pmix_hostname));
    /* set local proc size */
    v_params->pmix_local_size = nodes[my_server_id].pmix_local_size;
    TEST_VERBOSE(("my_server_id: %d local_size: %d, job_size: %d",
        my_server_id, v_params->pmix_local_size, v_params->pmix_job_size));

    /* setup the server library */
    uint32_t u32 = 0666;
    PMIX_INFO_LOAD(&info[0], PMIX_SOCKET_MODE, &u32, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_HOSTNAME, my_server_info->hostname, PMIX_STRING);

    server_nspace = PMIX_NEW(pmix_list_t);

    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, info, 2))) {
        TEST_ERROR(("Init failed with error %d", rc));
        goto error;
    }

    /* register test server read thread */
    if (v_params->pmix_num_nodes && pmix_list_get_size(server_list)) {
        server_info_t *server;
        PMIX_LIST_FOREACH(server, server_list, server_info_t) {
            server->evread = pmix_event_new(pmix_globals.evbase, server->rd_fd,
                                            EV_READ|EV_PERSIST, server_read_cb, server);
            pmix_event_add(server->evread, NULL);
        }
    }

    /* register the errhandler */
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                errhandler, errhandler_reg_callbk, NULL);

    /* setup to see sigchld on the forked tests */
    pmix_event_assign(&handler, pmix_globals.evbase, SIGCHLD,
                      EV_SIGNAL|EV_PERSIST, wait_signal_callback, &handler);
    pmix_event_add(&handler, NULL);


    if (0 != (rc = server_barrier())) {
        goto error;
    }

    return PMIX_SUCCESS;

error:
    PMIX_DESTRUCT(server_nspace);
    return rc;
}

int server_finalize(validation_params *v_params, int local_fail)
{
    int rc = PMIX_SUCCESS;
    int total_ret = local_fail;

    if (0 != (rc = server_barrier())) {
        total_ret++;
        goto exit;
    }

    if (0 != my_server_id) {
        server_info_t *server = (server_info_t*)pmix_list_get_first(server_list);
        remove_server_item(server);
    }

    if (v_params->pmix_num_nodes && 0 == my_server_id) {
        /* wait for all servers to finish */
        total_ret += srv_wait_all(10.0);
        PMIX_LIST_RELEASE(server_list);
        TEST_VERBOSE(("SERVER %d FINALIZE PID:%d with status %d",
                        my_server_id, getpid(), total_ret));
        if (0 == total_ret) {
            TEST_OUTPUT(("Test finished OK!"));
        } else {
            rc = PMIX_ERROR;
        }
    }
    PMIX_LIST_RELEASE(server_nspace);

    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        TEST_ERROR(("Finalize failed with error %d", rc));
        total_ret += rc;
        goto exit;
    }

exit:
    return total_ret;
}

int server_launch_clients(test_params *params, validation_params *v_params,
                          char *** client_env, char ***base_argv)
{
    int n;
    uid_t myuid;
    gid_t mygid;
    char *ranks = NULL;
    char digit[MAX_DIGIT_LEN];
    int rc;
    static int cli_counter = 0;
    static int num_ns = 0;
    pmix_proc_t proc;
    int custom_rank_val, rank_counter = 0;
    uint32_t local_size, univ_size;
    validation_params local_v_params;
    char *vptr;
    server_nspace_t *nspace_item = PMIX_NEW(server_nspace_t);

    TEST_VERBOSE(("Server ID: %d: pmix_local_size: %d, pmix_univ_size: %d, num_nodes %d",
                  my_server_id, v_params->pmix_local_size, v_params->pmix_univ_size,
                  v_params->pmix_num_nodes));

    (void)snprintf(proc.nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, num_ns);
    strncpy(v_params->pmix_nspace, proc.nspace, PMIX_MAX_NSLEN);

    set_namespace(v_params);
    if (NULL != ranks) {
        free(ranks);
    }

    local_size = v_params->pmix_local_size;
    univ_size = v_params->pmix_univ_size;
    /* add namespace entry */
    nspace_item->ntasks = univ_size;
    nspace_item->ltasks = local_size;
    nspace_item->task_map = (int*)malloc(sizeof(int) * univ_size);
    memset(nspace_item->task_map, -1, sizeof(int)*univ_size);
    strcpy(nspace_item->name, proc.nspace);
    pmix_list_append(server_nspace, &nspace_item->super);

    /* turn on validation */
    v_params->validate_params = true;

    for (n = 0; n < local_size; n++) {
       custom_rank_val = nodes[my_server_id].pmix_rank[n];
       nspace_item->task_map[custom_rank_val] = my_server_id;
    }

    server_send_procs(); // note: calls server_pack_procs

    myuid = getuid();
    mygid = getgid();

    /* fork/exec the test */
    for (n = 0; n < local_size; n++) {
        // set proc.rank from the appropriate valie in nodes array
        proc.rank = nodes[my_server_id].pmix_rank[n];

        rc = PMIx_server_register_client(&proc, myuid, mygid, NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            TEST_ERROR(("Server register client failed with error %d", rc));
            PMIx_server_finalize();
            cli_kill_all();
            return 0;
        }
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(&proc, client_env))) {
            TEST_ERROR(("Server fork setup failed with error %d", rc));
            PMIx_server_finalize();
            cli_kill_all();
            return rc;
        }
        TEST_VERBOSE(("run namespace: %s rank:%d", proc.nspace, proc.rank));
        // fork the client
        cli_info[cli_counter].pid = fork();
        //sleep for debugging purposes (to attach to clients)
        //sleep(120);
        if (cli_info[cli_counter].pid < 0) {
            TEST_ERROR(("Fork failed"));
            PMIx_server_finalize();
            cli_kill_all();
            return 0;
        }

        cli_info[cli_counter].rank = proc.rank;
        cli_info[cli_counter].ns = strdup(proc.nspace);

        char **client_argv = pmix_argv_copy(*base_argv);

        if (v_params->validate_params) {
            // bring in previously set globally applicable validation params
            local_v_params = *v_params; // is copying into a local truly necessary? we run the
                                        // risk of messing up any pointers that might be in
                                        // the struct later because this is a 'shallow' copy

            /* client-specific params set here */
            local_v_params.pmix_rank = proc.rank;
            local_v_params.pmix_local_rank = n;
            local_v_params.pmix_node_rank = n;
            /* end client-specific */

            vptr = (char *) &local_v_params;
            char *v_params_ascii = pmixt_encode(vptr, sizeof(local_v_params));
            /* provide the validation data to the client */
            pmix_argv_append_nosize(&client_argv, "--validate-params");
            pmix_argv_append_nosize(&client_argv, v_params_ascii);
            free(v_params_ascii);
        }

        sprintf(digit, "%d", univ_size);
        pmix_argv_append_nosize(&client_argv, "--ns-size");
        pmix_argv_append_nosize(&client_argv, digit);

        sprintf(digit, "%d", num_ns);
        pmix_argv_append_nosize(&client_argv, "--ns-id");
        pmix_argv_append_nosize(&client_argv, digit);

        /*
        sprintf(digit, "%d", 0);
        pmix_argv_append_nosize(&client_argv, "--base-rank");
        pmix_argv_append_nosize(&client_argv, digit);
        */

        // child case
        if (cli_info[cli_counter].pid == 0) {
            sigset_t sigs;
            set_handler_default(SIGTERM);
            set_handler_default(SIGINT);
            set_handler_default(SIGHUP);
            set_handler_default(SIGPIPE);
            set_handler_default(SIGCHLD);
            sigprocmask(0, 0, &sigs);
            sigprocmask(SIG_UNBLOCK, &sigs, 0);

            if( !TEST_VERBOSE_GET() ){
                // Hide clients stdout
                if (NULL == freopen("/dev/null","w", stdout)) {
                    return 0;
                }
            }
            execve(params->binary, client_argv, *client_env);
            /* Does not return */
            TEST_ERROR(("execve() failed"));
            return 0;
        }
        cli_info[cli_counter].alive = true;
        cli_info[cli_counter].state = CLI_FORKED;

        pmix_argv_free(client_argv);

        cli_counter++;
        rank_counter++;
    }
    num_ns++;
    return rank_counter;
}
