// SPDX-License-Identifier: GPL-3.0-only
import com.example.build.BinaryXmlEncoder;
import java.lang.reflect.Field;
import java.util.Map;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;

/** Add platform attributes missing from the shared manifest builder.
 * This changes only the builder's map in this build process, not its checkout.
 */
public final class PocketManifest {
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        Field field = BinaryXmlEncoder.class.getDeclaredField("ATTR_RESOURCE_IDS");
        field.setAccessible(true);
        Map<String, Integer> attributes = (Map<String, Integer>) field.get(null);
        Integer previous = attributes.put("hardwareAccelerated", 0x010102d3);
        if (previous != null && previous != 0x010102d3)
            throw new IllegalStateException("Unexpected hardwareAccelerated resource ID");
        attributes.put("taskAffinity", 0x01010012);
        attributes.put("process", 0x01010011);
        attributes.put("resizeableActivity", 0x010104f6);
        attributes.put("supportsPictureInPicture", 0x010104f7);
        attributes.put("scheme", 0x01010027);
        attributes.put("authorities", 0x01010018);
        attributes.put("enableOnBackInvokedCallback", 0x0101066c);
        BinaryXmlEncoder.main(args);
        // The generic builder emits numeric values as integers. These two
        // attributes must instead contain references to public framework IDs.
        byte[] bytes = Files.readAllBytes(Paths.get(args[1]));
        ByteBuffer xml = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int[] resources = new int[0];
        for (int offset = 8; offset < bytes.length;) {
            int kind = xml.getShort(offset) & 0xffff;
            int header = xml.getShort(offset + 2) & 0xffff;
            int size = xml.getInt(offset + 4);
            if (size < header || header < 8 || offset + size > bytes.length)
                throw new IllegalStateException("Malformed binary manifest");
            if (kind == 0x180) {
                resources = new int[(size-header)/4];
                for (int i = 0; i < resources.length; i++) resources[i] = xml.getInt(offset+header+i*4);
            } else if (kind == 0x102) {
                int start = xml.getShort(offset+24) & 0xffff;
                int stride = xml.getShort(offset+26) & 0xffff;
                int count = xml.getShort(offset+28) & 0xffff;
                List<byte[]> sorted = new ArrayList<>();
                for (int i = 0; i < count; i++) {
                    int attribute = offset + 16 + start + i*stride;
                    int name = xml.getInt(attribute+4);
                    if (name < resources.length && (resources[name] == 0x01010000 || resources[name] == 0x01010002)
                            && bytes[attribute+15] == 0x10)
                        bytes[attribute+15] = 1; // TYPE_REFERENCE
                    byte[] entry = new byte[stride];
                    System.arraycopy(bytes, attribute, entry, 0, stride);
                    sorted.add(entry);
                }
                // Android's styled-attribute reader expects resource-ID order.
                // The shared encoder preserves source order, causing attributes
                // such as configChanges/theme/orientation to be silently missed.
                final int[] ids = resources;
                sorted.sort(Comparator.comparingInt(entry -> {
                    int name = ByteBuffer.wrap(entry).order(ByteOrder.LITTLE_ENDIAN).getInt(4);
                    return name < ids.length ? ids[name] : 0;
                }));
                for (int i = 0; i < count; i++)
                    System.arraycopy(sorted.get(i), 0, bytes, offset+16+start+i*stride, stride);
            }
            offset += size;
        }
        Files.write(Paths.get(args[1]), bytes);
    }
}
