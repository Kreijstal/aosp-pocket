// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;

import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** Real encrypted-store path resolution across the overlaid emulated mount. */
public final class CeExternalMediaViewTest {
    private static final int APP=12345;
    private static final int[] NONE={};
    private interface Action {void run()throws Exception;}
    private static void check(boolean value){if(!value)throw new AssertionError("CE external-view invariant");}
    private static void outside(String path,Action action)throws Exception {
        try{action.run();throw new AssertionError("Outer mount was swallowed");}
        catch(CeNamespace.Outside expected){check(expected.path.equals(path));}
    }
    private static void denied(int errno,Action action)throws Exception {
        try{action.run();throw new AssertionError("Expected errno "+errno);}
        catch(CeNamespace.FsException expected){check(expected.errno==errno);}
    }
    public static void main(String[] args)throws Exception {
        Path temporary=Files.createTempDirectory("pocket-ce-external-view-");byte[] secret=new byte[64];new java.security.SecureRandom().nextBytes(secret);
        try(EncryptedStore store=EncryptedStore.create(temporary.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,true,true);
            for(String root:fs.roots()) {
                CeNamespace.Node before=fs.resolve(root,0,0,NONE,false);
                int owner=before.uid,group=before.gid,mode=before.mode,links=before.links;
                denied(17,()->fs.create(root,CeNamespace.DIR|0700,0,0,NONE,null));
                check(fs.resolve(root,0,0,NONE,false)==before && before.uid==owner && before.gid==group && before.mode==mode && before.links==links);
            }
            denied(17,()->fs.create("/data/data",CeNamespace.DIR|0700,0,0,NONE,null));
            CeNamespace.Node directory=fs.create("/data/user/0/app",CeNamespace.DIR|0700,0,0,NONE,null);directory.uid=directory.gid=APP;
            CeNamespace.Node media=fs.resolve("/data/media/0",0,0,NONE,true);media.mode=CeNamespace.DIR|02770;media.uid=media.gid=1023;
            String root="/data/user/0/app/",target="/storage/emulated/0/Android/data/app/files/A";
            fs.create(root+"jump",CeNamespace.LNK|0777,APP,APP,NONE,target);
            fs.create(root+"lower",CeNamespace.LNK|0777,APP,APP,NONE,"/data/media/0");
            fs.create(root+"internal",CeNamespace.REG|0600,APP,APP,NONE,null);
            fs.create(root+"relative",CeNamespace.LNK|0777,APP,APP,NONE,"internal");
            fs.create(root+"loop",CeNamespace.LNK|0777,APP,APP,NONE,"loop");
            // The old/lower view retains the provider's real traversal boundary.
            denied(13,()->fs.resolve(root+"jump/../A/canary",APP,APP,NONE,true));
            check(fs.resolve("/storage/emulated/0",1023,1023,NONE,true)==media);
            try(CeNamespace.ExternalMediaView view=fs.externalMediaView()) {
                for(String encryptedRoot:fs.roots())denied(17,()->fs.create(encryptedRoot,CeNamespace.DIR|0700,0,0,NONE,null));
                outside(target+"/../A/canary",()->fs.resolve(root+"jump/../A/canary",APP,APP,NONE,true));
                check(fs.resolve(root+"jump",APP,APP,NONE,false).symlink());
                check(fs.resolve(root+"jump",APP,APP,NONE,false).target.equals(target));
                denied(13,()->fs.resolve(root+"jump",APP+1,APP+1,NONE,true));
                check(fs.resolve(root+"relative",APP,APP,NONE,true).regular());
                denied(40,()->fs.resolve(root+"loop",APP,APP,NONE,true));
                denied(2,()->fs.resolve(root+"missing/jump",APP,APP,NONE,true));
                denied(20,()->fs.resolve(root+"internal/jump",APP,APP,NONE,true));
                outside("/storage/emulated/0",()->fs.resolve(root+"lower",APP,APP,NONE,true));
                check(fs.resolve(root+"lower",0,0,NONE,true)==media);
                check(fs.resolve("/data/media/0",1000,1000,NONE,true)==media);
                denied(13,()->fs.resolve(root+"lower",1000,1000,NONE,true));
                // Mutation redirects must retain the final requested basename.
                outside(target+"/new-file",()->fs.create(root+"jump/new-file",CeNamespace.REG|0600,APP,APP,NONE,null));
                outside(target+"/new-dir",()->fs.create(root+"jump/new-dir",CeNamespace.DIR|0700,APP,APP,NONE,null));
                FutureTask<Boolean> provider=new FutureTask<>(()->fs.resolve("/storage/emulated/0",1023,1023,NONE,true)==media);
                new Thread(provider,"IndependentProviderBackingView").start();check(provider.get(10,TimeUnit.SECONDS));
                try(CeNamespace.ExternalMediaView nested=fs.externalMediaView()) {
                    outside(target,()->fs.resolve(root+"jump",APP,APP,NONE,true));
                }
                outside(target,()->fs.resolve(root+"jump",APP,APP,NONE,true));
                String[] aliases={"/storage/emulated/0","/mnt/runtime/default/emulated/0","/mnt/runtime/read/emulated/0",
                    "/mnt/runtime/write/emulated/0","/mnt/user/0/primary","/sdcard","/storage/self/primary","/data/media/0","/system-data/media/0"};
                for(String alias:aliases)outside("/storage/emulated/0/Android/data/peer/files/../canary",
                    ()->fs.resolve(alias+"/Android/data/peer/files/../canary",APP,APP,NONE,true));
                outside("/storage/emulated-extra/0",()->fs.resolve("/storage/emulated-extra/0",APP,APP,NONE,true));
            }
            // Normal provider/legacy requests on this same worker regain their view.
            check(fs.resolve("/storage/emulated/0",1023,1023,NONE,true)==media);
            denied(13,()->fs.resolve(root+"jump",APP,APP,NONE,true));
            // Held descriptors retain dentry names after unlink. CWD still
            // requires a live pathname, and removed ancestors can rename.
            CeNamespace.Node parent=fs.create(root+"held",CeNamespace.DIR|0700,APP,APP,NONE,null);
            CeNamespace.Node removed=fs.create(root+"held/child",CeNamespace.REG|0600,APP,APP,NONE,null);
            check(fs.descriptorPath(removed.id).equals(root+"held/child"));
            fs.remove(root+"held/child",false,APP,APP,NONE);
            denied(2,()->fs.path(removed.id));
            check(fs.descriptorPath(removed.id).equals(root+"held/child (deleted)"));
            fs.rename(root+"held",root+"renamed",false,APP,APP,NONE);
            check(fs.descriptorPath(parent.id).equals(root+"renamed"));
            check(fs.descriptorPath(removed.id).equals(root+"renamed/child (deleted)"));
            fs.remove(root+"renamed",true,APP,APP,NONE);
            denied(2,()->fs.path(parent.id));
            check(fs.descriptorPath(parent.id).equals(root+"renamed (deleted)"));
            check(fs.descriptorPath(removed.id).equals(root+"renamed/child (deleted)"));
            CeNamespace.Node replaced=fs.create(root+"replacement",CeNamespace.DIR|0700,APP,APP,NONE,null);
            CeNamespace.Node source=fs.create(root+"source",CeNamespace.DIR|0700,APP,APP,NONE,null);
            fs.rename(root+"source",root+"replacement",false,APP,APP,NONE);
            check(fs.descriptorPath(source.id).equals(root+"replacement"));
            denied(2,()->fs.path(replaced.id));
            check(fs.descriptorPath(replaced.id).equals(root+"replacement (deleted)"));
            CeNamespace.Node old=fs.create(root+"old",CeNamespace.REG|0600,APP,APP,NONE,null);
            fs.create(root+"new",CeNamespace.REG|0600,APP,APP,NONE,null);
            fs.rename(root+"new",root+"old",false,APP,APP,NONE);
            check(fs.descriptorPath(old.id).equals(root+"old (deleted)"));
            fs.sync();
        }finally{Arrays.fill(secret,(byte)0);}
        try(java.util.stream.Stream<Path> files=Files.walk(temporary)) {
            for(Path file:(Iterable<Path>)files.sorted(Comparator.reverseOrder())::iterator)Files.delete(file);
        }
        System.out.println("PASS_CE_EXTERNAL_MEDIA_VIEW existing_roots_eexist=5 root_metadata_preserved=1 root_alias_eexist=1 scoped_roots_eexist=5 symlink_before_dotdot=1 nofollow=1 parent_permissions=1 mutation_leaf=1 private_target_deferred=1 privileged_lower=1 provider_thread_isolation=1 nested_scope_restored=1 legacy_view_restored=1 aliases=9 deleted_descriptor_names=1 deleted_cwd_enoent=1 renamed_ancestor=1 replacement_targets=1");
    }
}
