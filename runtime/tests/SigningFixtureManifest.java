// SPDX-License-Identifier: GPL-3.0-only
import com.example.build.BinaryXmlEncoder;
import java.util.Map;
/** Add the real platform ID for the shared-UID signature-rejection fixture. */
public final class SigningFixtureManifest {
    @SuppressWarnings("unchecked")
    public static void main(String[] args)throws Exception {
        java.lang.reflect.Field field=BinaryXmlEncoder.class.getDeclaredField("ATTR_RESOURCE_IDS");field.setAccessible(true);
        Map<String,Integer> attributes=(Map<String,Integer>)field.get(null);
        attributes.put("sharedUserId",android.R.attr.sharedUserId);
        attributes.put("hasCode",android.R.attr.hasCode);
        PocketManifest.main(args);
    }
}
