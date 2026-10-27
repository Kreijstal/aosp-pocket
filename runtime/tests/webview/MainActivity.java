// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketwebviewtest;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.*;
import android.content.pm.PackageInfo;
import java.io.*;
import org.json.JSONObject;

/** An ordinary guest app must create a real WebView and execute JavaScript. */
public final class MainActivity extends Activity {
    private boolean finished;
    private void result(String status,String detail) {
        if(finished)return;finished=true;
        try {
            PackageInfo provider=WebView.getCurrentWebViewPackage();
            JSONObject report=new JSONObject().put("validation",status).put("detail",detail)
                .put("uid",android.os.Process.myUid()).put("provider",provider==null?JSONObject.NULL:provider.packageName)
                .put("providerVersion",provider==null?JSONObject.NULL:provider.versionName);
            try(FileOutputStream out=openFileOutput("report.json",0)){out.write(report.toString().getBytes("UTF-8"));}
            android.util.Log.i("PocketWebViewTest",report.toString());
        } catch(Exception error){throw new RuntimeException(error);}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            WebView browser=new WebView(this);browser.getSettings().setJavaScriptEnabled(true);
            browser.setWebViewClient(new WebViewClient(){
                @Override public void onPageFinished(WebView view,String url) {
                    view.evaluateJavascript("document.getElementById('proof').textContent+'|'+(6*7)",
                        value->result(value.equals("\"Pocket WebView|42\"")?"PASS":"FAIL",value));
                }
                @Override public void onReceivedError(WebView view,WebResourceRequest request,WebResourceError error) {
                    if(request.isForMainFrame())result("FAIL",error.getDescription().toString());
                }
            });
            setContentView(browser);
            browser.loadDataWithBaseURL("https://pocket.invalid/","<html><body style='background:#d4f5df'><h1 id='proof'>Pocket WebView</h1><p>Real Chromium rendering and JavaScript.</p></body></html>","text/html","UTF-8",null);
        } catch(Throwable error){result("FAIL",error.toString());}
    }
}
