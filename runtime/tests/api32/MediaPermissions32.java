// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.content.pm.*;
import android.os.*;
import android.system.Os;
import java.util.*;
import org.json.*;

/** Test-only grant for exactly the ordinary AV fixture, through stock PackageManager. */
public final class MediaPermissions32 {
    public static void main(String[] args) {
        try {
            if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=0 || args.length!=0)throw new IllegalStateException("Expected SDK32 owner entry");
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();Class<?> api=Class.forName("android.app.ActivityThread");Object thread=api.getMethod("systemMain").invoke(null);
            Context context=(Context)api.getMethod("getSystemContext").invoke(thread);PackageManager pm=context.getPackageManager();
            String name="dev.kreijstal.pocketav32test";PackageInfo app=pm.getPackageInfo(name,PackageManager.GET_PERMISSIONS);
            Set<String> expected=new HashSet<>(Arrays.asList(android.Manifest.permission.CAMERA,android.Manifest.permission.RECORD_AUDIO));
            if(app.applicationInfo.uid<10000 || app.applicationInfo.minSdkVersion!=32 || app.applicationInfo.targetSdkVersion!=32
                    || app.requestedPermissions==null || !new HashSet<>(Arrays.asList(app.requestedPermissions)).equals(expected))throw new IllegalStateException("Wrong AV fixture identity/permissions");
            java.lang.reflect.Method grant=PackageManager.class.getMethod("grantRuntimePermission",String.class,String.class,UserHandle.class);
            for(String permission:expected){grant.invoke(pm,name,permission,android.os.Process.myUserHandle());if(pm.checkPermission(permission,name)!=PackageManager.PERMISSION_GRANTED)throw new IllegalStateException("Permission grant failed");}
            System.out.println("POCKET_AV_PERMISSIONS32 "+new JSONObject().put("sdk",32).put("grantUid",Os.getuid()).put("appUid",app.applicationInfo.uid).put("package",name).put("via","PackageManager.grantRuntimePermission").put("permissions",new JSONArray(expected)).put("validation","PASS"));
            System.out.flush();System.exit(0);
        }catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
