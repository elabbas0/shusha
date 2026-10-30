#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    VFS_NODE_FILE = 1,
    VFS_NODE_DIR = 2
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t type;
    uint8_t readonly;
    /* append-only (Shusha): owner and permission bits */
    uint32_t uid;
    uint16_t mode;
} vfs_stat_t;

/* Permission bits for vfs_permitted. */
#define VFS_R 4
#define VFS_W 2
#define VFS_X 1

int vfs_init(void);
vfs_node_t *vfs_root(void);
int vfs_seed_readonly(const char *path, const char *data, uint64_t size);
void vfs_set_sync_hook(int (*hook)(void));
int vfs_sync(void);
vfs_node_t *vfs_resolve(vfs_node_t *cwd, const char *path);
int vfs_getcwd(vfs_node_t *node, char *buf, size_t size);
int vfs_mkdir(vfs_node_t *cwd, const char *path);
int vfs_create(vfs_node_t *cwd, const char *path);
int vfs_write(vfs_node_t *cwd, const char *path, const char *data, uint64_t size);
/* Offset write directly against a node (fd layer). Grows the file when
 * off+size exceeds it, zero-filling the gap. Returns 0 or -1. */
int vfs_node_write_at(vfs_node_t *node, uint64_t off, const char *data,
                      uint64_t size);
int vfs_import_node(const char *path, uint8_t type, uint8_t readonly, const char *data, uint64_t size,
                    uint64_t inode, uint64_t created, uint64_t modified);
const char *vfs_read(vfs_node_t *cwd, const char *path, uint64_t *size_out);
int vfs_stat(vfs_node_t *cwd, const char *path, vfs_stat_t *out);
vfs_node_t *vfs_child_at(vfs_node_t *dir, uint64_t index);
uint64_t vfs_child_count(vfs_node_t *dir);
const char *vfs_node_name(vfs_node_t *node);
uint8_t vfs_node_type(vfs_node_t *node);
uint8_t vfs_node_readonly(vfs_node_t *node);
uint64_t vfs_node_size(vfs_node_t *node);
uint64_t vfs_node_inode(vfs_node_t *node);
uint64_t vfs_node_created(vfs_node_t *node);
uint64_t vfs_node_modified(vfs_node_t *node);
const char *vfs_node_data(vfs_node_t *node);

/* Ownership and permissions. */
uint32_t vfs_current_uid(void);
vfs_node_t *vfs_resolve_parent(vfs_node_t *cwd, const char *path);
int vfs_permitted(vfs_node_t *node, uint32_t uid, int want);
void vfs_node_set_owner(vfs_node_t *node, uint32_t uid);
void vfs_node_set_mode(vfs_node_t *node, uint16_t mode);
uint32_t vfs_node_uid(vfs_node_t *node);
uint16_t vfs_node_mode(vfs_node_t *node);

#endif
