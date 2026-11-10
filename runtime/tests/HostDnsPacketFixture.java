// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import java.io.IOException;
import java.util.Arrays;

/** Reject hostile DNS framing before forwarding it or delivering a response. */
public final class HostDnsPacketFixture {
    private static byte[] hex(String text) {
        byte[] bytes=new byte[text.length()/2];for(int i=0;i<bytes.length;i++)bytes[i]=(byte)Integer.parseInt(text.substring(i*2,i*2+2),16);return bytes;
    }
    private static void invalidQuery(byte[] bytes) {
        try{HostDnsPacket.validateQuery(bytes);throw new AssertionError("Accepted invalid query");}catch(IllegalArgumentException expected){}
    }
    private static void invalidAnswer(byte[] query,byte[] bytes) {
        try{HostDnsPacket.validateAnswer(query,bytes);throw new AssertionError("Accepted mismatched response");}catch(IOException expected){}
    }
    public static void main(String[] args)throws Exception {
        byte[] query=hex("4a3901000001000000000000076578616d706c6503636f6d0000010001");
        HostDnsPacket.validateQuery(query);
        for(int i=0;i<query.length;i++)invalidQuery(Arrays.copyOf(query,i));
        byte[] answer=hex("4a3981800001000100000000076578616d706c6503636f6d0000010001c00c000100010000003c00045db8d822");
        HostDnsPacket.validateAnswer(query,answer);
        byte[] upper=answer.clone();upper[13]='E';HostDnsPacket.validateAnswer(query,upper);
        byte[] bad=answer.clone();bad[0]++;invalidAnswer(query,bad);
        bad=answer.clone();bad[2]=1;invalidAnswer(query,bad);
        bad=answer.clone();bad[13]='z';invalidAnswer(query,bad);
        bad=answer.clone();bad[26]=28;invalidAnswer(query,bad);
        bad=answer.clone();bad[28]=3;invalidAnswer(query,bad);
        bad=answer.clone();bad[12]=(byte)0xc0;bad[13]=12;invalidAnswer(query,bad);
        for(int i=0;i<query.length;i++)invalidAnswer(query,Arrays.copyOf(answer,i));
        bad=query.clone();bad[12]=64;invalidQuery(bad);
        bad=query.clone();bad[12]=(byte)0xc0;invalidQuery(bad);
        bad=query.clone();bad[5]=2;invalidQuery(bad);
        bad=query.clone();bad[3]=3;invalidQuery(bad);
        invalidQuery(new byte[8193]);
        // A negative DNS answer is valid data, not an invented successful lookup.
        bad=answer.clone();bad[3]=(byte)0x83;bad[7]=0;HostDnsPacket.validateAnswer(query,Arrays.copyOf(bad,query.length));
        System.out.println("HOST_DNS_PACKET_FIXTURE_PASS truncated=all_prefixes id_name_type_class=checked pointer_loop=denied nxdomain=preserved");
    }
}
