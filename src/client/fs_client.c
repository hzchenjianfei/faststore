#include <stdlib.h>
#include "idempotency/client_channel.h"
#include "client_global.h"
#include "fs_client.h"

int fs_unlink_file(FSClientContext *client_ctx, const int64_t oid,
        const int64_t file_size)
{
    FSBlockKey bkey;
    int64_t remain;
    int result;
    int dec_alloc;

    remain = file_size;
    fs_set_block_key(&bkey, oid, 0);
    while (1) {
        logInfo("block {oid: %"PRId64", offset: %"PRId64"}",
                bkey.oid, bkey.offset);

        result = fs_client_block_delete(client_ctx, &bkey, &dec_alloc);
        if (result == ENOENT) {
            result = 0;
        } else if (result != 0) {
            break;
        }

        remain -= FS_FILE_BLOCK_SIZE;
        if (remain <= 0) {
            break;
        }

        fs_next_block_key(&bkey);
    }

    return result;
}

static int stat_data_group_by_addresses(FSClientContext *client_ctx,
        const int data_group_id, FCAddressPtrArray *addr_ptr_array,
        FSClientClusterStatEntry *stats, const int size, int *count)
{
    FCAddressInfo **addr;
    FCAddressInfo **end;
    int result;

    result = ENOENT;
    end = addr_ptr_array->addrs + addr_ptr_array->count;
    for (addr=addr_ptr_array->addrs; addr<end; addr++) {
        if ((result=fs_client_proto_cluster_stat(client_ctx, &(*addr)->conn,
                        data_group_id, stats, size, count)) == 0)
        {
            break;
        }
    }

    return result;
}

static int stat_data_group(FSClientContext *client_ctx,
        const int data_group_id, const int only_this_group,
        FSClientClusterStatEntry *stats, const int size, int *count)
{
    FSServerGroup *server_group;
    FCServerInfo **server;
    int new_group_id;
    int index;
    int i;
    int result;

    if ((server_group=fs_cluster_cfg_get_server_group(client_ctx->cluster_cfg.ptr,
                    data_group_id - 1)) == NULL)
    {
        return ENOENT;
    }

    new_group_id = only_this_group ? data_group_id : 0;
    result = ENOENT;
    for (i=0; i<server_group->server_array.count; i++) {
        if (i == 0) {
            index = 0;
        } else {
            index = (int)(((int64_t)server_group->server_array.count *
                        (int64_t)rand()) / (int64_t)RAND_MAX);
        }
        server = server_group->server_array.servers + index;
        if ((result=stat_data_group_by_addresses(client_ctx, new_group_id,
                        &FS_CFG_SERVICE_ADDRESS_ARRAY(client_ctx, *server),
                        stats, size, count)) == 0)
        {
            logInfo("stat by server id: %d", (*server)->id);
            break;
        }
    }

    return result;
}

int fs_cluster_stat(FSClientContext *client_ctx, const int data_group_id,
        FSClientClusterStatEntry *stats, const int size, int *count)
{
#define FIXED_DATA_GROUP_SIZE  1024
    int data_group_count;
    int fixed_ids[FIXED_DATA_GROUP_SIZE];
    int *ids;
    FSClientClusterStatEntry *stat;
    FSClientClusterStatEntry *end;
    int i;
    int n;
    int bytes;
    int result;

    if (data_group_id > 0) {
        return stat_data_group(client_ctx, data_group_id, true,
                stats, size, count);
    }

    data_group_count = FS_DATA_GROUP_COUNT(*client_ctx->cluster_cfg.ptr);
    if (data_group_count <= FIXED_DATA_GROUP_SIZE) {
        ids = fixed_ids;
    } else {
        bytes = sizeof(int) * data_group_count;
        ids = (int *)fc_malloc(bytes);
        if (ids == NULL) {
            return ENOMEM;
        }
    }
    for (i=0; i<data_group_count; i++) {
        ids[i] = i + 1;
    }

    result = 0;
    *count = 0;
    for (i=0; i<data_group_count; i++) {
        if (ids[i] == 0) {
            continue;
        }

        stat = stats + *count;
        if ((result=stat_data_group(client_ctx, i + 1, false,
                        stat, size - *count, &n)) != 0)
        {
            break;
        }

        end = stat + n;
        while (stat < end) {
            ids[stat->data_group_id - 1] = 0;
            stat++;
        }

        *count += n;
    }

    if (ids != fixed_ids) {
        free(ids);
    }

    return result;
}

int fs_client_slice_write(FSClientContext *client_ctx,
        const FSBlockSliceKeyInfo *bs_key, const char *data,
        int *write_bytes, int *inc_alloc)
{
    const FSConnectionParameters *connection_params;
    ConnectionInfo *conn;
    IdempotencyClientChannel *old_channel;
    FSBlockSliceKeyInfo new_key;
    int result;
    int remain;
    int bytes;
    int current_alloc;
    int i;
    uint64_t req_id;
    SFNetRetryIntervalContext net_retry_ctx;

    if ((conn=client_ctx->conn_manager.get_master_connection(client_ctx,
                    FS_CLIENT_DATA_GROUP_INDEX(client_ctx,
                        bs_key->block.hash_code), &result)) == NULL)
    {
        return result;
    }

    connection_params = client_ctx->conn_manager.get_connection_params(
            client_ctx, conn);

    sf_init_net_retry_interval_context(&net_retry_ctx,
            &client_ctx->net_retry_cfg.interval_mm,
            &client_ctx->net_retry_cfg.network);

    *inc_alloc = *write_bytes = 0;
    new_key = *bs_key;
    remain = bs_key->slice.length;
    while (remain > 0) {
        if (remain <= connection_params->buffer_size) {
            bytes = remain;
        } else {
            bytes = connection_params->buffer_size;
        }
        new_key.slice.offset += *write_bytes;
        new_key.slice.length = bytes;

        if (client_ctx->idempotency_enabled) {
            req_id = idempotency_client_channel_next_seq_id(
                    connection_params->channel);
        } else {
            req_id = 0;
        }

        logInfo("slice offset: %d, slice length: %d, current offset: %d, "
                "current length: %d", bs_key->slice.offset,
                bs_key->slice.length, new_key.slice.offset,
                new_key.slice.length);

        old_channel = connection_params->channel;
        i = 0;
        while (1) {
            if ((result=fs_client_proto_slice_write(client_ctx, conn,
                            req_id, &new_key, data + *write_bytes,
                            &current_alloc)) == 0)
            {
                break;
            }

            if (result == SF_RETRIABLE_ERROR_CHANNEL_INVALID &&
                    client_ctx->idempotency_enabled)
            {
                idempotency_client_channel_check_reconnect(
                        connection_params->channel);
            }

            SF_NET_RETRY_CHECK_AND_SLEEP(net_retry_ctx, client_ctx->
                    net_retry_cfg.network.times, ++i, result);

            fs_client_release_connection(client_ctx, conn, result);
            if ((conn=client_ctx->conn_manager.get_master_connection(
                            client_ctx, FS_CLIENT_DATA_GROUP_INDEX(
                                client_ctx, bs_key->block.hash_code),
                            &result)) == NULL)
            {
                return result;
            }

            connection_params = client_ctx->conn_manager.
                get_connection_params(client_ctx, conn);
            if (connection_params->channel != old_channel) {
                break;
            }
        }

        if (connection_params->channel != old_channel) { //master changed
            sf_reset_net_retry_interval(&net_retry_ctx);
            continue;
        }

        if (client_ctx->idempotency_enabled) {
            idempotency_client_channel_push(
                    connection_params->channel, req_id);
        }

        if (result != 0) {
            break;
        }

        *inc_alloc += current_alloc;
        *write_bytes += bytes;
        remain -= bytes;

        if (remain == 0) {
            break;
        }
        sf_reset_net_retry_interval(&net_retry_ctx);
    }

    fs_client_release_connection(client_ctx, conn, result);
    return result;
}

int fs_client_slice_read(FSClientContext *client_ctx,
        const FSBlockSliceKeyInfo *bs_key, char *buff, int *read_bytes)
{
    ConnectionInfo *conn;
    FSBlockSliceKeyInfo new_key;
    int result;
    int remain;
    int bytes;
    int i;
    SFNetRetryIntervalContext net_retry_ctx;

    if ((conn=client_ctx->conn_manager.get_readable_connection(client_ctx,
                    FS_CLIENT_DATA_GROUP_INDEX(client_ctx, bs_key->block.
                        hash_code), &result)) == NULL)
    {
        return result;
    }

    sf_init_net_retry_interval_context(&net_retry_ctx,
            &client_ctx->net_retry_cfg.interval_mm,
            &client_ctx->net_retry_cfg.network);

    *read_bytes = 0;
    new_key = *bs_key;
    remain = bs_key->slice.length;
    i = 0;
    while (remain > 0) {
        new_key.slice.offset += *read_bytes;
        new_key.slice.length = remain;
        if ((result=fs_client_proto_slice_read(client_ctx, conn,
                        &new_key, buff + *read_bytes, &bytes)) == 0)
        {
            break;
        }

        SF_NET_RETRY_CHECK_AND_SLEEP(net_retry_ctx, client_ctx->
                net_retry_cfg.network.times, ++i, result);

        fs_client_release_connection(client_ctx, conn, result);
        if ((conn=client_ctx->conn_manager.get_readable_connection(client_ctx,
                        FS_CLIENT_DATA_GROUP_INDEX(client_ctx, bs_key->block.
                            hash_code), &result)) == NULL)
        {
            break;
        }

        *read_bytes += bytes;
        remain -= bytes;
        if (remain == 0) {
            break;
        }

        sf_reset_net_retry_interval(&net_retry_ctx);
    }

    if (conn != NULL) {
        fs_client_release_connection(client_ctx, conn, result);
    }
    if (*read_bytes > 0) {
        return 0;
        //return result == ENODATA ? 0 : result;
    } else {
        return result == 0 ? ENODATA : result;
    }
}

int fs_client_bs_operate(FSClientContext *client_ctx,
        const void *key, const uint32_t hash_code,
        const int req_cmd, const int resp_cmd, int *inc_alloc)
{
    const FSConnectionParameters *connection_params;
    ConnectionInfo *conn;
    IdempotencyClientChannel *old_channel;
    int result;
    int i;
    uint64_t req_id;
    SFNetRetryIntervalContext net_retry_ctx;

    if ((conn=client_ctx->conn_manager.get_master_connection(client_ctx,
                    FS_CLIENT_DATA_GROUP_INDEX(client_ctx, hash_code),
                    &result)) == NULL)
    {
        return result;
    }
    connection_params = client_ctx->conn_manager.get_connection_params(
            client_ctx, conn);

    sf_init_net_retry_interval_context(&net_retry_ctx,
            &client_ctx->net_retry_cfg.interval_mm,
            &client_ctx->net_retry_cfg.network);

    while (1) {
        if (client_ctx->idempotency_enabled) {
            req_id = idempotency_client_channel_next_seq_id(
                    connection_params->channel);
        } else {
            req_id = 0;
        }

        old_channel = connection_params->channel;
        i = 0;
        while (1) {
            if ((result=fs_client_proto_bs_operate(client_ctx, conn,
                            req_id, key, req_cmd, resp_cmd, inc_alloc)) == 0)
            {
                break;
            }

            if (result == SF_RETRIABLE_ERROR_CHANNEL_INVALID &&
                    client_ctx->idempotency_enabled)
            {
                idempotency_client_channel_check_reconnect(
                        connection_params->channel);
            }

            SF_NET_RETRY_CHECK_AND_SLEEP(net_retry_ctx, client_ctx->
                    net_retry_cfg.network.times, ++i, result);

            fs_client_release_connection(client_ctx, conn, result);
            if ((conn=client_ctx->conn_manager.get_master_connection(
                            client_ctx, FS_CLIENT_DATA_GROUP_INDEX(
                                client_ctx, hash_code), &result)) == NULL)
            {
                return result;
            }

            connection_params = client_ctx->conn_manager.
                get_connection_params(client_ctx, conn);
            if (connection_params->channel != old_channel) {
                break;
            }
        }

        if (connection_params->channel != old_channel) { //master changed
            sf_reset_net_retry_interval(&net_retry_ctx);
            continue;
        }

        if (client_ctx->idempotency_enabled) {
            idempotency_client_channel_push(
                    connection_params->channel, req_id);
        }

        break;
    }

    fs_client_release_connection(client_ctx, conn, result);
    return result;
}
