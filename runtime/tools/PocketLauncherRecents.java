// SPDX-License-Identifier: GPL-3.0-only
import java.io.File;
import java.nio.file.*;
import java.util.*;
import com.android.tools.smali.dexlib2.*;
import com.android.tools.smali.dexlib2.iface.*;
import com.android.tools.smali.dexlib2.iface.instruction.*;
import com.android.tools.smali.dexlib2.builder.*;
import com.android.tools.smali.dexlib2.builder.instruction.*;
import com.android.tools.smali.dexlib2.immutable.*;

/** Backport the upstream null-array guard without replacing task data or Binder contracts. */
public class PocketLauncherRecents {
    private static void require(boolean value,String message){if(!value)throw new IllegalArgumentException(message);}
    public static void main(String[] args)throws Exception {
        require(args.length==2,"input APK and fresh output directory required");
        Path output=Paths.get(args[1]);require(!Files.exists(output),"Use a fresh output");
        MultiDexContainer<?> container=DexFileFactory.loadDexContainer(new File(args[0]),Opcodes.forApi(32));
        int changed=0;
        for(String entry:container.getDexEntryNames()) {
            DexFile dex=container.getEntry(entry).getDexFile();ArrayList<ClassDef> classes=new ArrayList<>();boolean modified=false;
            for(ClassDef type:dex.getClasses()) {
                if(!type.getType().equals("Lcom/android/quickstep/SystemUiProxy;")){classes.add(type);continue;}
                ArrayList<Method> methods=new ArrayList<>();
                for(Method method:type.getMethods()) {
                    if(!method.toString().equals("Lcom/android/quickstep/SystemUiProxy;->getRecentTasks(II)Ljava/util/ArrayList;")){methods.add(method);continue;}
                    MutableMethodImplementation body=new MutableMethodImplementation(method.getImplementation());
                    List<BuilderInstruction> code=body.getInstructions();
                    require(body.getRegisterCount()==6 && code.size()==18,"Unexpected original method shape");
                    require(code.get(1).getOpcode()==Opcode.IF_EQZ && ((OneRegisterInstruction)code.get(1)).getRegisterA()==0,"Missing original disconnected-service guard");
                    Label empty=((BuilderOffsetInstruction)code.get(1)).getTarget();require(empty.getCodeAddress()==29,"Unexpected original empty-list target");
                    require(code.get(5).getOpcode()==Opcode.INVOKE_INTERFACE && ((ReferenceInstruction)code.get(5)).getReference().toString().equals(
                        "Lcom/android/wm/shell/recents/IRecentTasks;->getRecentTasks(III)[Lcom/android/wm/shell/util/GroupedRecentTaskInfo;"),"Wrong interface call");
                    require(code.get(6).getOpcode()==Opcode.MOVE_RESULT_OBJECT && ((OneRegisterInstruction)code.get(6)).getRegisterA()==1,"Wrong result register");
                    require(code.get(7).getOpcode()==Opcode.INVOKE_STATIC && ((ReferenceInstruction)code.get(7)).getReference().toString().equals(
                        "Ljava/util/Arrays;->asList([Ljava/lang/Object;)Ljava/util/List;"),"Wrong array consumer");
                    // Null follows the existing empty-list return. Non-null arrays,
                    // permissions, exceptions, task grouping and listeners are unchanged.
                    body.addInstruction(7,new BuilderInstruction21t(Opcode.IF_EQZ,1,empty));
                    methods.add(new ImmutableMethod(method.getDefiningClass(),method.getName(),method.getParameters(),method.getReturnType(),
                        method.getAccessFlags(),method.getAnnotations(),method.getHiddenApiRestrictions(),body));
                    changed++;modified=true;
                }
                classes.add(new ImmutableClassDef(type.getType(),type.getAccessFlags(),type.getSuperclass(),type.getInterfaces(),
                    type.getSourceFile(),type.getAnnotations(),type.getFields(),methods));
            }
            if(modified){Files.createDirectories(output);DexFileFactory.writeDexFile(output.resolve(entry).toString(),new ImmutableDexFile(dex.getOpcodes(),classes));System.out.println("PATCHED_RECENTS_DEX "+entry);}
        }
        require(changed==1,"Expected exactly one original getRecentTasks method");
        System.out.println("LAUNCHER_RECENTS_NULL_GUARD_BACKPORT_PASS methods=1 extra_branch=1 nonnull_tasks_preserved=1");
    }
}
