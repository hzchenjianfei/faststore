/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef _TRUNK_ALLOCATOR_H
#define _TRUNK_ALLOCATOR_H

#include "fastcommon/uniq_skiplist.h"
#include "fastcommon/fc_list.h"
#include "../../common/fs_types.h"
#include "storage_config.h"
#include "object_block_index.h"

#define FS_TRUNK_SKIPLIST_INIT_LEVEL_COUNT      10
#define FS_TRUNK_SKIPLIST_MAX_LEVEL_COUNT       16
#define FS_TRUNK_SKIPLIST_DELAY_FREE_SECONDS   600

#define FS_TRUNK_STATUS_NONE        0
#define FS_TRUNK_STATUS_ALLOCING    1
#define FS_TRUNK_STATUS_RECLAIMING  2

#define FS_TRUNK_AVAIL_SPACE(trunk) ((trunk)->size - (trunk)->free_start)

typedef struct {
    FSTrunkIdInfo id_info;
    int status;
    struct {
        int count;  //slice count
        int64_t bytes;
        struct fc_list_head slice_head; //OBSliceEntry double link
    } used;
    int64_t size;        //file size
    int64_t free_start;  //free space offset
} FSTrunkFileInfo;

typedef struct fs_trunk_free_node {
    FSTrunkFileInfo *trunk_info;
    struct fs_trunk_free_node *next;
} FSTrunkFreeNode;

typedef struct {
    int count;
    int prealloc_trunks;
    FSTrunkFreeNode *head;  //allocate from head
    FSTrunkFreeNode *tail;  //push to tail
} FSTrunkFreelist;

typedef struct {
    FSTrunkFreelist normal;   //general purpose
    FSTrunkFreelist reclaim;  //special purpose for reclaiming
} FSTrunkFreelistPair;

typedef struct {
    int alloc;
    int count;
    FSTrunkFileInfo **trunks;
} FSTrunkInfoPtrArray;

typedef struct {
    FSStoragePathInfo *path_info;
    UniqSkiplist *sl_trunks;   //all trunks order by id
    FSTrunkFreelistPair *freelists; //current allocator map to disk write threads
    FSTrunkInfoPtrArray priority_array;  //for trunk reclaim
    pthread_lock_cond_pair_t lcp;
} FSTrunkAllocator;

#ifdef __cplusplus
extern "C" {
#endif

    int trunk_allocator_init(FSTrunkAllocator *allocator,
            FSStoragePathInfo *path_info);

    int trunk_allocator_add(FSTrunkAllocator *allocator,
            const FSTrunkIdInfo *id_info, const int64_t size,
            FSTrunkFileInfo **pp_trunk);

    int trunk_allocator_delete(FSTrunkAllocator *allocator, const int64_t id);

    int trunk_allocator_normal_alloc(FSTrunkAllocator *allocator,
            const uint32_t blk_hc, const int size,
            FSTrunkSpaceInfo *spaces, int *count);

    int trunk_allocator_reclaim_alloc(FSTrunkAllocator *allocator,
            const uint32_t blk_hc, const int size,
            FSTrunkSpaceInfo *spaces, int *count);

    int trunk_allocator_free(FSTrunkAllocator *allocator,
            const int id, const int size);

    int trunk_allocator_add_slice(FSTrunkAllocator *allocator,
            OBSliceEntry *slice);

    int trunk_allocator_delete_slice(FSTrunkAllocator *allocator,
            OBSliceEntry *slice);

    void trunk_allocator_add_to_freelist(FSTrunkAllocator *allocator,
            FSTrunkFreelist *freelist, FSTrunkFileInfo *trunk_info);

    void trunk_allocator_array_to_freelists(FSTrunkAllocator *allocator,
            const FSTrunkInfoPtrArray *trunk_ptr_array);

    void trunk_allocator_prealloc_trunks(FSTrunkAllocator *allocator);

    //to find freelist when startup 
    const FSTrunkInfoPtrArray *trunk_allocator_free_size_top_n(
            FSTrunkAllocator *allocator, const int count);

    //to reclaim trunk space
    const FSTrunkInfoPtrArray *trunk_allocator_avail_space_top_n(
            FSTrunkAllocator *allocator, const int count);

    void trunk_allocator_trunk_stat(FSTrunkAllocator *allocator,
            FSTrunkSpaceStat *stat);

    void trunk_allocator_log_trunk_info(FSTrunkFileInfo *trunk_info);

#ifdef __cplusplus
}
#endif

#endif
