#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the actual interpreter's managed-policy path on local sockets."""
import ctypes as C
import errno
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
from target import ROOT


def main():
    out=ROOT/'out/api29';out.mkdir(parents=True,exist_ok=True)
    libpath=out/'network-policy-test.so'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',
        str(ROOT/'runtime/tests/firewall_runtime_host.c'),'-o',str(libpath)],check=True)
    lib=C.CDLL(str(libpath));lib.fw_setup.argtypes=[C.c_char_p,C.c_uint]
    lib.fw_call.argtypes=[C.c_long]*7;lib.fw_call.restype=C.c_long
    lib.fw_nr.argtypes=[C.c_int];lib.fw_nr.restype=C.c_long
    # UDP connect selects the local interface without transmitting a packet.
    route=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    route.connect(('192.0.2.1',9));local=route.getsockname()[0];route.close()
    assert not local.startswith('127.'),'A local non-loopback address is required'
    checks=0
    with tempfile.TemporaryDirectory(prefix='pocket-api29-policy-') as tmp:
        root=Path(tmp);(root/'data/misc').mkdir(parents=True)
        policy=root/'data/misc/pocket-network-policy.rules'
        sender=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        receiver=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);receiver.bind((local,0));receiver.settimeout(.05)
        loop=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);loop.bind(('127.0.0.1',0));loop.settimeout(.05)
        payload=C.create_string_buffer(b'policy')
        def configure(mask,members,version=2):
            data=bytearray(struct.pack('=IIII',0x504e5031,version,mask,0)+bytes((5 if version==3 else 4)*12500))
            for chain,ids in members.items():
                for uid in ids:data[16+chain*12500+uid//8]|=1<<(uid%8)
            policy.write_bytes(data)
        def send(uid,allowed,target=receiver,root_path=root):
            nonlocal checks
            lib.fw_setup(str(root_path).encode(),uid)
            address=target.getsockname()
            packed=C.create_string_buffer(struct.pack('=H',socket.AF_INET)+struct.pack('!H',address[1])+socket.inet_aton(address[0])+bytes(8))
            result=lib.fw_call(lib.fw_nr(1),sender.fileno(),C.addressof(payload),6,0,C.addressof(packed),16)
            assert result==(6 if allowed else -errno.EACCES),(uid,result,allowed)
            checks+=1
            if allowed:assert target.recv(100)==b'policy';checks+=1
            else:
                try:target.recv(100);raise AssertionError('Blocked packet reached receiver')
                except socket.timeout:checks+=1
        send(10017,True)
        configure(2,{1:[10017]});send(10017,False);send(10018,True)
        configure(1,{0:[10017]});send(10017,True);send(10018,False);send(9999,True)
        send(10018,True,loop)
        configure(5,{0:[10017]});send(10017,False)
        configure(5,{0:[10017],2:[10017]});send(10017,True)
        configure(7,{0:[10017],1:[10017],2:[10017]});send(10017,False)
        send(100000,False)
        # Malformed/truncated managed state fails closed, even with no manual policy.
        policy.write_bytes(b'broken');send(10017,False)
        configure(16,{});send(10017,False)
        configure(8,{3:[10017]});send(10017,False);send(10017,False,loop);send(10018,True)
        configure(8,{});send(10017,True)
        # API 32's new restricted allowlist uses version 3. Check real UDP
        # delivery through the same interpreter path, including loopback scope.
        configure(16,{4:[10017]},3);send(10017,True);send(10018,False);send(9999,True)
        send(10018,True,loop)
        configure(17,{4:[10017]},3);send(10017,False)
        configure(17,{0:[10017],4:[10017]},3);send(10017,True)
        configure(24,{3:[10017],4:[10017]},3);send(10017,False);send(10017,False,loop)
        def create(uid,family,allowed):
            nonlocal checks
            lib.fw_setup(str(root).encode(),uid)
            result=lib.fw_call(lib.fw_nr(8),family,socket.SOCK_DGRAM,0,0,0,0)
            assert (result>=0 if allowed else result==-errno.EACCES),(uid,family,result,allowed)
            if result>=0:socket.socket(fileno=result).close()
            checks+=1
        create(10017,socket.AF_INET,False);create(10017,socket.AF_INET6,False)
        configure(16,{},3);create(10017,socket.AF_INET,True)
        configure(8,{},3);create(10017,socket.AF_INET,True)
        configure(8,{3:[10017]});create(10017,socket.AF_INET,True)
        configure(32,{},3);send(10017,False)
        configure(0,{},3);policy.write_bytes(policy.read_bytes()[:-1]);send(10017,False)
        configure(0,{});policy.write_bytes(policy.read_bytes()+bytes(12500));send(10017,False)
        # The Manager's canonical data alias must select the same policy.
        (root/'system-data/misc').mkdir(parents=True);configure(2,{1:[10017]})
        (root/'system-data/misc/pocket-network-policy.rules').write_bytes(policy.read_bytes())
        policy.unlink();lib.fw_data_alias(1);send(10017,False);lib.fw_data_alias(0)
        sender.close();receiver.close();loop.close()
    print(f'API29_NETWORK_POLICY_HOST_PASS assertions={checks} real_udp_delivery=checked')


if __name__=='__main__':main()
