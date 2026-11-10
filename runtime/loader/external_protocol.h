// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#define POCKET_EXTERNAL_OPEN_MAGIC 0x324f5850u
#define POCKET_EXTERNAL_ATTACH_MAGIC 0x32415850u
#define POCKET_EXTERNAL_LOCATOR_MAGIC 0x324c5850u
struct PocketExternalOpen {
    uint32_t magic,version,flags,tid,path_size,mode,reserved[2];
};
// Version2 retains the v1 prefix. No UID, native inode or provider handle is
// accepted from an application. Descriptor operations carry one SCM_RIGHTS FD.
// STATFS v1 uses a kernel proxy inode; v2 proves a directory/O_PATH descriptor
// and returns the same PocketExternalStatfsReply, without accepting an inode.
enum PocketExternalOperation {
    POCKET_EXT_OPEN=0,POCKET_EXT_STATFS=1,POCKET_EXT_STAT=2,POCKET_EXT_FSTAT=3,
    POCKET_EXT_MKDIR=4,POCKET_EXT_REMOVE=5,POCKET_EXT_RENAME=6,
    POCKET_EXT_TRUNCATE=7,POCKET_EXT_FTRUNCATE=8,POCKET_EXT_ACCESS=9,
    POCKET_EXT_DIR_READ=10,POCKET_EXT_DIR_SEEK=11,POCKET_EXT_DIR_FLAGS=12,
    POCKET_EXT_DIR_SYNC=13,POCKET_EXT_FD_LINK=14,POCKET_EXT_DIR_SEARCH=15,
    POCKET_EXT_FD_REOPEN=16
};
#define POCKET_EXTERNAL_DIRECTORY_TAG "pocket.dir32"
#define POCKET_EXTERNAL_DIRECTORY_TAG_SIZE 12u
#define POCKET_EXTERNAL_PATH_TAG "pocket.path32"
#define POCKET_EXTERNAL_PATH_TAG_SIZE 13u
#ifndef SO_COOKIE
#define SO_COOKIE 57
#endif
#define POCKET_EXTERNAL_OPEN_DIRECTORY 1u
#define POCKET_EXTERNAL_OPEN_PATH 2u
// Version2 namespace paths may be relative to authenticated directory FDs.
// Rights are ordered first-path then second-path, with no native inode/cap IDs.
#define POCKET_EXTERNAL_AT_FIRST 1u
#define POCKET_EXTERNAL_AT_SECOND 2u
struct PocketExternalDirectoryReply {int32_t error;uint32_t size;uint64_t value;};
struct PocketExternalRequest2 {
    struct PocketExternalOpen header;
    uint32_t permissions,umask;
    uint64_t size;
};
struct PocketExternalAttributes {
    uint64_t device,inode,size,blocks,atime,mtime,ctime;
    uint32_t mode,nlink,uid,gid,blksize,atimensec,mtimensec,ctimensec;
};
struct PocketExternalNamespaceReply {
    int32_t error;uint32_t reserved;
    struct PocketExternalAttributes attributes;
};
struct PocketExternalAttach {
    uint32_t magic,version,slot,flags;
    uint64_t cookie;
    uint32_t mode,reserved;
};
struct PocketExternalReply {int32_t error;uint32_t reserved;};
struct PocketExternalAttachReply {int32_t error;uint32_t reserved;uint64_t device,node;};
struct PocketExternalStatfsReply {
    int32_t error;uint32_t reserved;
    uint64_t blocks,bfree,bavail,files,ffree,bsize,namelen,frsize;
};
struct PocketExternalLocator {uint32_t magic,version,pid,reserved;uint64_t start;};
