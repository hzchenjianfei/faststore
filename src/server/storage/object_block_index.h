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


#ifndef _OBJECT_BLOCK_INDEX_H
#define _OBJECT_BLOCK_INDEX_H

#include "../server_types.h"

#ifdef __cplusplus
extern "C" {
#endif

    extern OBHashtable g_ob_hashtable;

#define ob_index_add_slice(slice, sn, inc_alloc) \
    ob_index_add_slice_ex(&g_ob_hashtable, slice, sn, inc_alloc)

#define ob_index_delete_slices(bs_key, sn, dec_alloc) \
    ob_index_delete_slices_ex(&g_ob_hashtable, bs_key, sn, dec_alloc)

#define ob_index_delete_block(bkey, sn, dec_alloc) \
    ob_index_delete_block_ex(&g_ob_hashtable, bkey, sn, dec_alloc)

#define ob_index_get_slices(bs_key, sarray) \
    ob_index_get_slices_ex(&g_ob_hashtable, bs_key, sarray)

#define ob_index_alloc_slice(bkey) \
    ob_index_alloc_slice_ex(&g_ob_hashtable, bkey, 1)

#define ob_index_init_htable(ht) \
    ob_index_init_htable_ex(ht, STORAGE_CFG.object_block.hashtable_capacity, \
            false, false)

    int ob_index_init();
    void ob_index_destroy();

    int ob_index_init_htable_ex(OBHashtable *htable, const int64_t capacity,
        const bool need_lock, const bool modify_sallocator);
    void ob_index_destroy_htable(OBHashtable *htable);

    int ob_index_add_slice_ex(OBHashtable *htable, OBSliceEntry *slice,
            uint64_t *sn, int *inc_alloc);

    int ob_index_delete_slices_ex(OBHashtable *htable,
            const FSBlockSliceKeyInfo *bs_key,
            uint64_t *sn, int *dec_alloc);

    int ob_index_delete_block_ex(OBHashtable *htable,
            const FSBlockKey *bkey,
            uint64_t *sn, int *dec_alloc);

    OBEntry *ob_index_get_ob_entry(OBHashtable *htable,
            const FSBlockKey *bkey);

    OBSliceEntry *ob_index_alloc_slice_ex(OBHashtable *htable,
            const FSBlockKey *bkey, const int init_refer);

    void ob_index_free_slice(OBSliceEntry *slice);

    int ob_index_get_slices_ex(OBHashtable *htable,
            const FSBlockSliceKeyInfo *bs_key,
            OBSlicePtrArray *sarray);

    static inline void ob_index_init_slice_ptr_array(OBSlicePtrArray *sarray)
    {
        sarray->slices = NULL;
        sarray->alloc = sarray->count = 0;
    }

    static inline void ob_index_free_slice_ptr_array(OBSlicePtrArray *sarray)
    {
        if (sarray->slices != NULL) {
            free(sarray->slices);
            sarray->slices = NULL;
            sarray->alloc = sarray->count = 0;
        }
    }

    int ob_index_add_slice_by_binlog(OBSliceEntry *slice);

    static inline int ob_index_delete_slices_by_binlog(
            const FSBlockSliceKeyInfo *bs_key)
    {
        int dec_alloc;
        return ob_index_delete_slices(bs_key, NULL, &dec_alloc);
    }

    static inline int ob_index_delete_block_by_binlog(
            const FSBlockKey *bkey)
    {
        int dec_alloc;
        return ob_index_delete_block(bkey, NULL, &dec_alloc);
    }

    static inline void ob_index_enable_modify_used_space()
    {
        g_ob_hashtable.modify_used_space = true;
    }

#ifdef __cplusplus
}
#endif

#endif
