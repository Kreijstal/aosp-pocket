// SPDX-License-Identifier: GPL-3.0-only
import android.os.*;
import android.content.pm.PackageInfo;
import java.lang.reflect.*;
import org.json.*;

/** Owner-side WebView provider diagnostics, using the real framework service. */
public final class WebViewOwner {
    public static void main(String[] args) {
        try {
            android.system.Os.setgid(1000);android.system.Os.setuid(1000);
            Looper.prepareMainLooper();
            Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
            if(args.length>0 && args[0].equals("--configure")) {
                Method configure=Class.forName("dev.kreijstal.aosppocket.guest.GuestControl").getDeclaredMethod("configureWebView");
                configure.setAccessible(true);configure.invoke(null);
            }
            IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"webviewupdate");
            Class<?> type=Class.forName("android.webkit.IWebViewUpdateService");
            Object service=Class.forName("android.webkit.IWebViewUpdateService$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
            JSONObject report=new JSONObject();JSONArray providers=new JSONArray();
            for(Object item:(Object[])type.getMethod("getAllWebViewPackages").invoke(service)) {
                JSONObject provider=new JSONObject();
                for(String field:new String[]{"packageName","description","availableByDefault","isFallback"})provider.put(field,item.getClass().getField(field).get(item));
                providers.put(provider);
            }
            report.put("providers",providers);
            if(args.length>0 && args[0].equals("--multiprocess"))
                type.getMethod("enableMultiProcess",boolean.class).invoke(service,Boolean.parseBoolean(args[1]));
            if(args.length>0 && args[0].equals("--select"))report.put("selected",type.getMethod("changeProviderAndSetting",String.class).invoke(service,args[1]));
            PackageInfo current=(PackageInfo)type.getMethod("getCurrentWebViewPackage").invoke(service);
            report.put("current",current==null?JSONObject.NULL:current.packageName)
                .put("version",current==null?JSONObject.NULL:current.versionName)
                .put("multiprocess",type.getMethod("isMultiProcessEnabled").invoke(service));
            System.out.println("WEBVIEW_OWNER_RESULT "+report);System.exit(0);
        } catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
