// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fuse_transport.h"
#include <linux/fuse.h>
#include <sys/stat.h>
#include <string>

namespace pocket::fuse32 {
// All names and attributes come from the live provider. There is no dentry or
// attribute cache here: a mutation must be visible to the next path operation.
// Only trusted broker code may supply Identity (never an app-provided UID).
class Namespace final {
    Channel& channel_;
    static int components(const std::string& path,std::vector<std::string>* names) {
        if(path.empty() || path[0]!='/' || path.size()>4095 || path.find('\0')!=std::string::npos)return -EINVAL;
        for(size_t at=1;at<path.size();) {
            size_t end=path.find('/',at);if(end==std::string::npos)end=path.size();
            auto name=path.substr(at,end-at);at=end+1;
            if(name.empty())continue;
            if(name==".." || name.size()>255)return -EINVAL;
            names->push_back(std::move(name));
        }
        return 0;
    }
public:
    // LOOKUP/MKDIR/CREATE return one lookup reference, unlike GETATTR. Keeping
    // the complete ancestor chain alive also prevents provider inode reuse
    // while a handle or a two-parent RENAME is in flight.
    struct Path final {
        Channel& channel;
        Identity caller;
        uint64_t node=FUSE_ROOT_ID;
        fuse_attr attr{};
        std::vector<uint64_t> references;
        Path(Channel& c,Identity i):channel(c),caller(i){}
        Path(const Path&)=delete;
        ~Path(){for(auto n:references)channel.forget(n,1,caller);}
    };
    explicit Namespace(Channel& channel):channel_(channel){}
    int lookup(Path& path,const std::string& name,bool allowSymlink=false) {
        if(name=="."){int rc=attributes(path.node,0,false,path.caller,&path.attr);return rc<0?rc:S_ISDIR(path.attr.mode)?0:-ENOTDIR;}
        if(name.empty() || name==".." || name.size()>255 || name.find('/')!=std::string::npos || name.find('\0')!=std::string::npos)return -EINVAL;
        std::vector<char> out;int rc=channel_.call(FUSE_LOOKUP,path.node,path.caller,name.c_str(),name.size()+1,&out);
        if(rc<0)return rc;
        if(out.size()!=sizeof(fuse_entry_out))return -EPROTO;
        fuse_entry_out entry{};memcpy(&entry,out.data(),sizeof(entry));
        if(!entry.nodeid)return -ENOENT; // A valid negative lookup reply.
        path.references.push_back(entry.nodeid);path.node=entry.nodeid;path.attr=entry.attr;
        return S_ISLNK(entry.attr.mode) && !allowSymlink?-ELOOP:0;
    }
    int resolve(const std::string& name,Path& path) {
        std::vector<std::string> names;int rc=components(name,&names);if(rc<0)return rc;
        if(name.size()>1 && name.back()=='/')names.push_back(".");
        if(names.empty())return attributes(path.node,0,false,path.caller,&path.attr);
        for(size_t i=0;i<names.size();i++) {
            rc=lookup(path,names[i]);if(rc<0)return rc;
            if(i+1<names.size() && !S_ISDIR(path.attr.mode))return -ENOTDIR;
        }
        return 0;
    }
    int parent(const std::string& name,Path& path,std::string* leaf,bool trailing=false) {
        std::vector<std::string> names;int rc=components(name,&names);if(rc<0)return rc;
        if(names.empty())return -EBUSY;
        // Read-only resolution supports trailing slashes and dot components.
        // Mutations need additional atomic final-type semantics; never strip
        // a slash and accidentally unlink/rename a regular file instead.
        if((!trailing && name.back()=='/') || names.back()==".")return -EOPNOTSUPP;
        *leaf=names.back();names.pop_back();
        for(const auto& part:names) {
            rc=lookup(path,part);if(rc<0)return rc;
            if(!S_ISDIR(path.attr.mode))return -ENOTDIR;
        }
        return 0;
    }
    int attributes(uint64_t node,uint64_t handle,bool byHandle,Identity caller,fuse_attr* result) {
        if(!result)return -EINVAL;
        fuse_getattr_in input{};input.getattr_flags=byHandle?FUSE_GETATTR_FH:0;input.fh=handle;
        std::vector<char> out;int rc=channel_.call(FUSE_GETATTR,node,caller,&input,sizeof(input),&out);if(rc<0)return rc;
        if(out.size()!=sizeof(fuse_attr_out))return -EPROTO;
        fuse_attr_out value{};memcpy(&value,out.data(),sizeof(value));*result=value.attr;return 0;
    }
    int stat(const std::string& name,Identity caller,fuse_attr* result,uint64_t base=FUSE_ROOT_ID) {
        if(!result)return -EINVAL;
        Path path(channel_,caller);path.node=base;int rc=resolve(name,path);if(rc<0)return rc;
        return attributes(path.node,0,false,caller,result);
    }
    int access(const std::string& name,uint32_t mask,Identity caller,uint64_t base=FUSE_ROOT_ID) {
        if(mask&~7U)return -EINVAL;
        Path path(channel_,caller);path.node=base;int rc=resolve(name,path);if(rc<0)return rc;
        return accessNode(path.node,mask,caller);
    }
    int accessNode(uint64_t node,uint32_t mask,Identity caller) {
        if(!node || mask&~7U)return -EINVAL;
        fuse_access_in input{};input.mask=mask;std::vector<char> out;
        int rc=channel_.call(FUSE_ACCESS,node,caller,&input,sizeof(input),&out);
        return rc<0?rc:out.empty()?0:-EPROTO;
    }
    int mkdir(const std::string& name,uint32_t mode,uint32_t mask,Identity caller,uint64_t base=FUSE_ROOT_ID) {
        if(mode&~07777U || mask&~0777U)return -EINVAL;
        Path path(channel_,caller);path.node=base;std::string leaf;int rc=parent(name,path,&leaf,true);if(rc<0)return rc;
        fuse_mkdir_in input{(mode&~mask)|S_IFDIR,mask};std::vector<char> body(sizeof(input)+leaf.size()+1),out;
        memcpy(body.data(),&input,sizeof(input));memcpy(body.data()+sizeof(input),leaf.c_str(),leaf.size()+1);
        rc=channel_.call(FUSE_MKDIR,path.node,caller,body.data(),body.size(),&out);if(rc<0)return rc;
        if(out.size()!=sizeof(fuse_entry_out))return -EPROTO;
        fuse_entry_out entry{};memcpy(&entry,out.data(),sizeof(entry));if(!entry.nodeid)return -EPROTO;
        path.references.push_back(entry.nodeid);return S_ISDIR(entry.attr.mode)?0:-EPROTO;
    }
    int remove(const std::string& name,bool directory,Identity caller,uint64_t base=FUSE_ROOT_ID) {
        Path path(channel_,caller);path.node=base;std::string leaf;int rc=parent(name,path,&leaf);if(rc<0)return rc;
        // Kernel VFS normally enforces these types before dispatching FUSE.
        // MediaProvider UNLINK assumes a file and may return ENOENT for a
        // directory, which prevents Java File.delete from trying RMDIR.
        // UNLINK operates on the final symlink itself; it must not follow it.
        // Intermediate resolution keeps the normal no-symlink policy.
        Path target(channel_,caller);target.node=path.node;rc=lookup(target,leaf,true);if(rc<0)return rc;
        if(S_ISDIR(target.attr.mode)!=directory)return directory?-ENOTDIR:-EISDIR;
        std::vector<char> out;rc=channel_.call(directory?FUSE_RMDIR:FUSE_UNLINK,path.node,caller,leaf.c_str(),leaf.size()+1,&out);
        return rc<0?rc:out.empty()?0:-EPROTO;
    }
    int rename(const std::string& oldName,const std::string& newName,Identity caller,uint32_t flags=0,uint64_t fromBase=FUSE_ROOT_ID,uint64_t toBase=FUSE_ROOT_ID) {
        // The pinned MediaProvider rejects nonzero renameat2 flags. Preserve
        // that answer from the provider instead of approximating no-replace.
        if(flags&~3U || flags==3)return -EINVAL;
        Path from(channel_,caller),to(channel_,caller);from.node=fromBase;to.node=toBase;std::string oldLeaf,newLeaf;
        int rc=parent(oldName,from,&oldLeaf);if(rc<0)return rc;
        rc=parent(newName,to,&newLeaf);if(rc<0)return rc;
        fuse_rename2_in input{};input.newdir=to.node;input.flags=flags;
        // Legacy RENAME has the same initial uint64_t, without flags/padding.
        size_t fixed=flags?sizeof(input):sizeof(fuse_rename_in);
        std::vector<char> body(fixed+oldLeaf.size()+newLeaf.size()+2),out;
        memcpy(body.data(),&input,fixed);memcpy(body.data()+fixed,oldLeaf.c_str(),oldLeaf.size()+1);
        memcpy(body.data()+fixed+oldLeaf.size()+1,newLeaf.c_str(),newLeaf.size()+1);
        rc=channel_.call(flags?FUSE_RENAME2:FUSE_RENAME,from.node,caller,body.data(),body.size(),&out);
        return rc<0?rc:out.empty()?0:-EPROTO;
    }
};
static_assert(sizeof(fuse_mkdir_in)==8 && sizeof(fuse_rename_in)==8 && sizeof(fuse_rename2_in)==16);
}
