#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "sf/sf_global.h"
#include "sf/sf_service.h"
#include "../../common/fs_proto.h"
#include "../server_global.h"
#include "../server_group_info.h"
#include "../cluster_relationship.h"
#include "../server_binlog.h"
#include "../server_replication.h"
#include "binlog_fetch.h"
#include "binlog_dedup.h"
#include "data_recovery.h"

#define DATA_RECOVERY_SYS_DATA_FILENAME       "data_recovery.dat"
#define DATA_RECOVERY_SYS_DATA_SECTION_FETCH  "fetch"
#define DATA_RECOVERY_SYS_DATA_ITEM_STAGE     "stage"
#define DATA_RECOVERY_SYS_DATA_ITEM_LAST_DV   "last_data_version"
#define DATA_RECOVERY_SYS_DATA_ITEM_LAST_BKEY "last_bkey"

#define DATA_RECOVERY_STAGE_FETCH   'F'
#define DATA_RECOVERY_STAGE_DEDUP   'D'
#define DATA_RECOVERY_STAGE_REPLAY  'R'

static int init_recovery_sub_path(DataRecoveryContext *ctx, const char *subdir)
{
    char filepath[PATH_MAX];
    const char *subdir_names[3];
    char data_group_id[16];
    int result;
    int gid_len;
    int path_len;
    int i;
    bool create;

    gid_len = sprintf(data_group_id, "%d", ctx->data_group_id);
    subdir_names[0] = FS_RECOVERY_BINLOG_SUBDIR_NAME;
    subdir_names[1] = data_group_id;
    subdir_names[2] = subdir;

    path_len = snprintf(filepath, sizeof(filepath), "%s", DATA_PATH_STR);
    if (PATH_MAX - path_len < gid_len + strlen(FS_RECOVERY_BINLOG_SUBDIR_NAME)
            + strlen(subdir) + 3)
    {
        logError("file: "__FILE__", line: %d, "
                "the length of data path is too long, exceeds %d",
                __LINE__, PATH_MAX);
        return EOVERFLOW;
    }

    for (i=0; i<3; i++) {
        path_len += sprintf(filepath + path_len, "/%s", subdir_names[i]);

        logInfo("%d. filepath: %s", i + 1, filepath);
        if ((result=fc_check_mkdir_ex(filepath, 0775, &create)) != 0) {
            return result;
        }
        if (create) {
            SF_CHOWN_RETURN_ON_ERROR(filepath, geteuid(), getegid());
        }
    }

    return 0;
}

FSClusterDataServerInfo *data_recovery_get_master(
        DataRecoveryContext *ctx, int *err_no)
{
    FSClusterDataGroupInfo *group;
    FSClusterDataServerInfo *master;

    if ((group=fs_get_data_group(ctx->data_group_id)) == NULL) {
        *err_no = ENOENT;
        return NULL;
    }
    master = (FSClusterDataServerInfo *)
        __sync_fetch_and_add(&group->master, 0);
    if (master == NULL) {
        logError("file: "__FILE__", line: %d, "
                "data group id: %d, no master",
                __LINE__, ctx->data_group_id);
        *err_no = ENOENT;
        return NULL;
    }

    if (group->myself == NULL) {
        logError("file: "__FILE__", line: %d, "
                "data group id: %d NOT belongs to me",
                __LINE__, ctx->data_group_id);
        *err_no = ENOENT;
        return NULL;
    }

    if (group->myself == master) {
        logError("file: "__FILE__", line: %d, "
                "data group id: %d, i am already master, "
                "do NOT need recovery!", __LINE__, ctx->data_group_id);
        *err_no = EBUSY;
        return NULL;
    }

    *err_no = 0;
    return master;
}

static void data_recovery_get_sys_data_filename(DataRecoveryContext *ctx,
        char *filename, const int size)
{
    snprintf(filename, size, "%s/%s/%d/%s", DATA_PATH_STR,
            FS_RECOVERY_BINLOG_SUBDIR_NAME, ctx->data_group_id,
            DATA_RECOVERY_SYS_DATA_FILENAME);
}

static int data_recovery_save_sys_data(DataRecoveryContext *ctx)
{
    char filename[PATH_MAX];
    char buff[256];
    int len;

    data_recovery_get_sys_data_filename(ctx, filename, sizeof(filename));
    len = sprintf(buff, "%s=%c\n"
            "[%s]\n"
            "%s=%"PRId64"\n"
            "%s=%"PRId64",%"PRId64"\n",
            DATA_RECOVERY_SYS_DATA_ITEM_STAGE, ctx->stage,
            DATA_RECOVERY_SYS_DATA_SECTION_FETCH,
            DATA_RECOVERY_SYS_DATA_ITEM_LAST_DV, ctx->fetch.last_data_version,
            DATA_RECOVERY_SYS_DATA_ITEM_LAST_BKEY, ctx->fetch.last_bkey.oid,
            ctx->fetch.last_bkey.offset);

    return safeWriteToFile(filename, buff, len);
}

static int data_recovery_unlink_sys_data(DataRecoveryContext *ctx)
{
    char filename[PATH_MAX];

    data_recovery_get_sys_data_filename(ctx, filename, sizeof(filename));
    return fc_delete_file(filename);
}

static int data_recovery_load_sys_data(DataRecoveryContext *ctx)
{
    IniContext ini_context;
    char filename[PATH_MAX];
    char *stage;
    char *last_bkey;
    int result;

    data_recovery_get_sys_data_filename(ctx, filename, sizeof(filename));
    if (access(filename, F_OK) != 0) {
        result = errno != 0 ? errno : EPERM;
        if (result != ENOENT) {
            logError("file: "__FILE__", line: %d, "
                    "access file: %s fail, errno: %d, error info: %s",
                    __LINE__, filename, result, STRERROR(result));
            return result;
        }

        ctx->stage = DATA_RECOVERY_STAGE_FETCH;
        return data_recovery_save_sys_data(ctx);
    }

    if ((result=iniLoadFromFile(filename, &ini_context)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "load conf file \"%s\" fail, ret code: %d",
                __LINE__, filename, result);
        return result;
    }

    stage = iniGetStrValue(NULL, DATA_RECOVERY_SYS_DATA_ITEM_STAGE,
            &ini_context);
    if (stage == NULL || *stage == '\0') {
        ctx->stage = DATA_RECOVERY_STAGE_FETCH;
    } else {
        ctx->stage = stage[0];
    }

    ctx->fetch.last_data_version = iniGetInt64Value(
            DATA_RECOVERY_SYS_DATA_SECTION_FETCH,
            DATA_RECOVERY_SYS_DATA_ITEM_LAST_DV,
            &ini_context, 0);
    last_bkey = iniGetStrValue(DATA_RECOVERY_SYS_DATA_SECTION_FETCH,
            DATA_RECOVERY_SYS_DATA_ITEM_LAST_BKEY, &ini_context);
    if (last_bkey != NULL && *last_bkey != '\0') {
        char *cols[2];
        int count;

        count = splitEx(last_bkey, ',', cols, 2);
        ctx->fetch.last_bkey.oid = strtoll(cols[0], NULL, 10);
        ctx->fetch.last_bkey.offset = strtoll(cols[1], NULL, 10);
    }

    iniFreeContext(&ini_context);
    return 0;
}

static int data_recovery_init(DataRecoveryContext *ctx, const int data_group_id)
{
    int result;
    struct nio_thread_data *thread_data;

    ctx->data_group_id = data_group_id;
    ctx->start_time = get_current_time_ms();

    if ((result=init_recovery_sub_path(ctx,
                    RECOVERY_BINLOG_SUBDIR_NAME_FETCH)) != 0)
    {
        return result;
    }
    if ((result=init_recovery_sub_path(ctx,
                    RECOVERY_BINLOG_SUBDIR_NAME_REPLAY)) != 0)
    {
        return result;
    }

    thread_data = sf_get_random_thread_data_ex(&REPLICA_SF_CTX);
    ctx->server_ctx = (FSServerContext *)thread_data->arg;
    return data_recovery_load_sys_data(ctx);
}

static void data_recovery_destroy(DataRecoveryContext *ctx)
{
}

static int replica_binlog_log_padding(DataRecoveryContext *ctx)
{
    int result;
    uint64_t current_version;

    current_version = __sync_fetch_and_add(&ctx->master->dg->
            myself->replica.data_version, 0);
    if (ctx->fetch.last_data_version > current_version) {
        replica_binlog_set_data_version(ctx->master->dg->myself,
                ctx->fetch.last_data_version - 1);
        result = replica_binlog_log_no_op(ctx->master->dg->id,
                ctx->fetch.last_data_version, &ctx->fetch.last_bkey);
    } else {
        result = 0;
    }

    return result;
}

static int proto_active_confirm(ConnectionInfo *conn,
        DataRecoveryContext *ctx)
{
    int result;
    FSResponseInfo response;
    FSProtoReplicaActiveConfirmReq *req;
    char out_buff[sizeof(FSProtoHeader) + sizeof(
            FSProtoReplicaActiveConfirmReq)];

    req = (FSProtoReplicaActiveConfirmReq *)
        (out_buff + sizeof(FSProtoHeader));
    int2buff(ctx->data_group_id, req->data_group_id);
    int2buff(CLUSTER_MYSELF_PTR->server->id, req->server_id);
    FS_PROTO_SET_HEADER((FSProtoHeader *)out_buff,
            FS_REPLICA_PROTO_ACTIVE_CONFIRM_REQ,
            sizeof(FSProtoReplicaActiveConfirmReq));

    response.error.length = 0;
    if ((result=fs_send_and_recv_none_body_response(conn, out_buff,
                    sizeof(out_buff), &response, SF_G_NETWORK_TIMEOUT,
                    FS_REPLICA_PROTO_ACTIVE_CONFIRM_RESP)) != 0)
    {
        fs_log_network_error(&response, conn, result);
    }

    return result;
}

static int active_confirm(DataRecoveryContext *ctx)
{
    int result;
    ConnectionInfo conn;

    if ((result=fc_server_make_connection_ex(&REPLICA_GROUP_ADDRESS_ARRAY(
                        ctx->master->cs->server), &conn,
                    SF_G_CONNECT_TIMEOUT, NULL, true)) != 0)
    {
        return result;
    }

    result = proto_active_confirm(&conn, ctx);
    conn_pool_disconnect_server(&conn);
    return result;
}

static int active_me(DataRecoveryContext *ctx)
{
    int result;

    if ((result=active_confirm(ctx)) != 0) {
        return result;
    }

    if (cluster_relationship_swap_report_ds_status(ctx->master->dg->myself,
                FS_SERVER_STATUS_ONLINE, FS_SERVER_STATUS_ACTIVE))
    {
        return 0;
    } else {
        int status;
        status = __sync_add_and_fetch(&ctx->master->dg->myself->status, 0);
        logError("file: "__FILE__", line: %d, "
                "data group id: %d, change my status to ACTIVE fail, "
                "current status is %d (%s)", __LINE__, ctx->data_group_id,
                status, fs_get_server_status_caption(status));
        return EBUSY;
    }
}

static int do_data_recovery(DataRecoveryContext *ctx)
{
    int result;
    int64_t binlog_count;
    int64_t binlog_size;
    int64_t start_time;

    start_time = 0;
    binlog_count = 0;
    result = 0;
    switch (ctx->stage) {
        case DATA_RECOVERY_STAGE_FETCH:
            start_time = get_current_time_ms();
            if ((result=data_recovery_fetch_binlog(ctx, &binlog_size)) != 0) {
                break;
            }

            logInfo("file: "__FILE__", line: %d, func: %s, "
                    "binlog_size: %"PRId64, __LINE__, __FUNCTION__,
                    binlog_size);
            if (binlog_size == 0) {
                break;
            }

            ctx->stage = DATA_RECOVERY_STAGE_DEDUP;
            if ((result=data_recovery_save_sys_data(ctx)) != 0) {
                break;
            }
        case DATA_RECOVERY_STAGE_DEDUP:
            if ((result=data_recovery_dedup_binlog(ctx, &binlog_count)) != 0) {
                break;
            }

            if (binlog_count == 0) {  //no binlog to replay
                break;
            }

            ctx->stage = DATA_RECOVERY_STAGE_REPLAY;
            if ((result=data_recovery_save_sys_data(ctx)) != 0) {
                break;
            }
        case DATA_RECOVERY_STAGE_REPLAY:
            //TODO replay
            result = replica_binlog_log_padding(ctx);
            break;
        default:
            logError("file: "__FILE__", line: %d, "
                    "invalid stage value: 0x%02x",
                    __LINE__, ctx->stage);
            result = EINVAL;
            break;
    }

    if (result != 0) {
        return result;
    }

    if (ctx->is_online) {
        if ((result=active_me(ctx)) != 0) {
            return result;
        }
    } else {
        if (ctx->catch_up == DATA_RECOVERY_CATCH_UP_DOING) {
            if (get_current_time_ms() - start_time < 1000) {
                ctx->catch_up = DATA_RECOVERY_CATCH_UP_LAST_BATCH;
            }
        }
    }

    return 0;
}

int data_recovery_start(const int data_group_id)
{
    DataRecoveryContext ctx;
    int result;

    memset(&ctx, 0, sizeof(ctx));
    if ((result=data_recovery_init(&ctx, data_group_id)) != 0) {
        return result;
    }

    ctx.catch_up = DATA_RECOVERY_CATCH_UP_DOING;
    do {
        if ((ctx.master=data_recovery_get_master(&ctx, &result)) == NULL) {
            break;
        }

        if ((result=do_data_recovery(&ctx)) != 0) {
            break;
        }

        logInfo("======= stage: %d, catch_up: %d, is_online: %d",
                ctx.stage, ctx.catch_up, ctx.is_online);

        ctx.stage = DATA_RECOVERY_STAGE_FETCH;
    } while (result == 0 && !ctx.is_online);

    if (result == 0) {
        result = data_recovery_unlink_sys_data(&ctx);
    }
    data_recovery_destroy(&ctx);
    return result;
}
