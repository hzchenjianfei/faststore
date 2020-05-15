
#ifndef _FS_API_FILE_H
#define _FS_API_FILE_H

#include <fcntl.h>
#include "fs_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define fsapi_create(fi, path, mode) \
    fsapi_create_ex(&g_fs_api_ctx, fi, path, mode)

#define fsapi_create_by_inode(fi, parent_inode, name, mode) \
    fsapi_create_by_inode_ex(&g_fs_api_ctx, fi, parent_inode, name, mode)

#define fsapi_open(fi, path, flags, mode) \
    fsapi_open_ex(&g_fs_api_ctx, fi, path, flags, mode)

#define fsapi_open_by_inode(fi, inode, flags) \
    fsapi_open_by_inode_ex(&g_fs_api_ctx, fi, inode, flags)

#define fsapi_open_by_dentry(fi, dentry, flags) \
    fsapi_open_by_dentry_ex(&g_fs_api_ctx, fi, dentry, flags)

#define fsapi_truncate(path, new_size) \
    fsapi_truncate_ex(&g_fs_api_ctx, path, new_size)

#define fsapi_unlink(path)  \
    fsapi_unlink_ex(&g_fs_api_ctx, path)

#define fsapi_stat(path, buf)  \
    fsapi_stat_ex(&g_fs_api_ctx, path, buf)

    int fsapi_open_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const char *path, const int flags, const mode_t mode);

    int fsapi_open_by_inode_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const int64_t inode, const int flags);

    int fsapi_open_by_dentry_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const FDIRDEntryInfo *dentry, const int flags);

    static inline int fsapi_create_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const char *path, const mode_t mode)
    {
        const int flags = O_CREAT | O_TRUNC | O_WRONLY;
        return fsapi_open_ex(ctx, fi, path, flags, mode);
    }

    int fsapi_close(FSAPIFileInfo *fi);

    int fsapi_pwrite(FSAPIFileInfo *fi, const char *buff,
            const int size, const int64_t offset, int *written_bytes);

    int fsapi_write(FSAPIFileInfo *fi, const char *buff,
            const int size, int *written_bytes);

    int fsapi_pread(FSAPIFileInfo *fi, char *buff, const int size,
            const int64_t offset, int *read_bytes);

    int fsapi_read(FSAPIFileInfo *fi, char *buff,
            const int size, int *read_bytes);

    int fsapi_ftruncate(FSAPIFileInfo *fi, const int64_t new_size);

    int fsapi_truncate_ex(FSAPIContext *ctx, const char *path,
            const int64_t new_size);

    int fsapi_unlink_ex(FSAPIContext *ctx, const char *path);

    int fsapi_lseek(FSAPIFileInfo *fi, const int64_t offset, const int whence);

    int fsapi_fstat(FSAPIFileInfo *fi, struct stat *buf);

    int fsapi_stat_ex(FSAPIContext *ctx, const char *path, struct stat *buf);

    int fsapi_flock(FSAPIFileInfo *fi, const int operation);

    int fsapi_getlk(FSAPIFileInfo *fi, struct flock *lock, int64_t *owner_id);

    int fsapi_setlk(FSAPIFileInfo *fi, const struct flock *lock,
        const int64_t owner_id);

#ifdef __cplusplus
}
#endif

#endif
