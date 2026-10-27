// SPDX-License-Identifier: GPL-3.0-only
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import org.json.JSONObject;

/** Verify real keycode-to-character conversion, not just injection acceptance. */
public final class VirtualKeyMapProbe {
    public static void main(String[] args)throws Exception {
        KeyCharacterMap map=KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
        if(map.get(KeyEvent.KEYCODE_1,0)!='1' || map.get(KeyEvent.KEYCODE_0,0)!='0'
                || map.get(KeyEvent.KEYCODE_A,0)!='a' || map.get(KeyEvent.KEYCODE_A,KeyEvent.META_SHIFT_ON)!='A')
            throw new AssertionError("Virtual key character conversion is missing or incorrect");
        KeyEvent[] events=map.getEvents("1aA".toCharArray());
        if(events==null)throw new AssertionError("Virtual key event generation failed");
        StringBuilder text=new StringBuilder();
        for(KeyEvent event:events)if(event.getAction()==KeyEvent.ACTION_DOWN && event.getUnicodeChar()!=0)
            text.appendCodePoint(event.getUnicodeChar());
        if(!text.toString().equals("1aA"))throw new AssertionError("Generated events do not round trip: "+text);
        System.out.println("VIRTUAL_KEYMAP_RESULT "+new JSONObject().put("validation","PASS")
                .put("key1Unicode",map.get(KeyEvent.KEYCODE_1,0)).put("eventRoundTrip",text));
        System.exit(0);
    }
}
