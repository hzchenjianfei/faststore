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


#ifndef _FS_FUSE_WRAPPER_H
#define _FS_FUSE_WRAPPER_H

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 35
#endif

#include "fastcommon/logger.h"
#include "fsapi/fs_api.h"
#include "fsapi/fs_api_util.h"
#include "fuse3/fuse_lowlevel.h"

#define FS_FUSE_SET_OMP(omp, m, euid, egid) \
    do {  \
        omp.mode = m;  \
        if (g_fuse_global_vars.owner.type == owner_type_fixed) { \
            omp.uid = g_fuse_global_vars.owner.uid; \
            omp.gid = g_fuse_global_vars.owner.gid; \
        } else {  \
            omp.uid = euid; \
            omp.gid = egid; \
        }  \
    } while (0)

#define FS_FUSE_SET_OMP_BY_REQ(omp, m, req) \
    do { \
        const struct fuse_ctx *fctx;  \
        fctx = fuse_req_ctx(req);     \
        FS_FUSE_SET_OMP(omp, m, fctx->uid, fctx->gid); \
    } while (0)


#ifdef __cplusplus
extern "C" {
#endif

	int fs_fuse_wrapper_init(struct fuse_lowlevel_ops *ops);

#ifdef __cplusplus
}
#endif

#endif
