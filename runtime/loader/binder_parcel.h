// SPDX-License-Identifier: GPL-3.0-only
// Add the existing guest credential header to standard native Binder parcels.
// The caller snapshots the original data at storage+8 and owns the offset table.
#pragma once
#include <errno.h>
#include <linux/android/binder.h>
#include "../api28/binder_token.h"

#define POCKET_BINDER_IDENTITY_BYTES sizeof(uint64_t)
#define POCKET_BINDER_TOKEN_BYTES 2080u

static inline size_t pocket_binder_object_size(uint32_t type) {
 switch(type) {
 case BINDER_TYPE_BINDER:case BINDER_TYPE_WEAK_BINDER:
 case BINDER_TYPE_HANDLE:case BINDER_TYPE_WEAK_HANDLE:return sizeof(struct flat_binder_object);
 case BINDER_TYPE_FD:return sizeof(struct binder_fd_object);
 case BINDER_TYPE_PTR:return sizeof(struct binder_buffer_object);
 case BINDER_TYPE_FDA:return sizeof(struct binder_fd_array_object);
 default:return 0;
 }
}
static inline int pocket_binder_add_identity(unsigned char*storage,size_t capacity,size_t data_size,
                                             binder_size_t*offsets,size_t offsets_size,uint64_t identity) {
 if(!storage)return -EFAULT;
 if(data_size>SIZE_MAX-POCKET_BINDER_IDENTITY_BYTES || capacity<data_size+POCKET_BINDER_IDENTITY_BYTES)return -ENOSPC;
 struct PocketBinderToken token;
 unsigned char*data=storage+POCKET_BINDER_IDENTITY_BYTES;
 if(!pocket_binder_token(data,data_size,&token) || token.format>=3)return 0;
 if(!(uint32_t)identity || (identity>>32)>INT32_MAX)return -EINVAL;
 if(offsets_size%sizeof(binder_size_t))return -EINVAL;
 if(offsets_size && !offsets)return -EFAULT;
 // HIDL and other object-first protocols can accidentally resemble a short
 // UTF-16 token. An object at byte zero cannot be a standard interface header.
 if(offsets_size && offsets[0]==0 && data_size>=sizeof(struct binder_object_header)) {
  struct binder_object_header first;memcpy(&first,data,sizeof(first));
  if(pocket_binder_object_size(first.type))return 0;
 }
 // Validate the complete table before changing any byte. Parent indices and
 // parent_offset inside SG objects refer to child buffers, not Parcel bytes.
 size_t end=0;
 for(size_t i=0;i<offsets_size/sizeof(binder_size_t);i++) {
  binder_size_t offset=offsets[i];
  if(offset%4 || offset<end || offset>data_size || sizeof(struct binder_object_header)>data_size-offset)return -EINVAL;
  struct binder_object_header header;memcpy(&header,data+offset,sizeof(header));
  size_t size=pocket_binder_object_size(header.type);
  if(!size || size>data_size-offset)return -EINVAL;
  end=(size_t)offset+size;
 }
 memcpy(storage,&identity,sizeof(identity));
 if(token.format<2) {
  // Android 9/10 distinguish their prefixed header using this policy bit.
  uint32_t policy=token.policy|0x00400000u;memcpy(data,&policy,sizeof(policy));
 }
 for(size_t i=0;i<offsets_size/sizeof(binder_size_t);i++)offsets[i]+=POCKET_BINDER_IDENTITY_BYTES;
 return 1;
}
