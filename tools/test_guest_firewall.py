#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the actual loader firewall code against real localhost sockets."""
import ctypes as C
import errno
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
from target import ROOT

def main():
    out=ROOT/'out/guest-firewall';out.mkdir(parents=True,exist_ok=True)
    def run(*args):subprocess.run(list(map(str,args)),cwd=ROOT,check=True)
    run('cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',ROOT/'runtime/tests/firewall_runtime_host.c','-o',out/'firewall-test.so')
    run('cc','-O2','-Wall','-Wextra','-Werror','-DPOCKET_FIREWALL_TEST',ROOT/'runtime/loader/iptables.c','-o',out/'iptables-test')
    lib=C.CDLL(str(out/'firewall-test.so'));lib.fw_setup.argtypes=[C.c_char_p,C.c_uint]
    lib.fw_call.argtypes=[C.c_long]*7;lib.fw_call.restype=C.c_long
    lib.fw_nr.argtypes=[C.c_int];lib.fw_nr.restype=C.c_long
    checks=0
    def call(which,*args):return lib.fw_call(lib.fw_nr(which),*args,*([0]*(6-len(args))))
    def expect(value,want):
        nonlocal checks
        assert value==want,(value,want);checks+=1
    with tempfile.TemporaryDirectory(prefix='pocket-firewall-') as tmp:
        root=Path(tmp);(root/'data/misc').mkdir(parents=True);policy=root/'data/misc/pocket-firewall.rules'
        env=dict(os.environ,POCKET_FIREWALL_FILE=str(policy));lib.fw_setup(str(root).encode(),10017)
        def cli(*args,ok=True):
            p=subprocess.run([str(out/'iptables-test'),*args],env=env,capture_output=True,text=True)
            assert (p.returncode==0)==ok,(args,p.stdout,p.stderr);return p.stdout
        udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',0));udp.settimeout(.1)
        sender=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);port=udp.getsockname()[1]
        address=C.create_string_buffer(struct.pack('=H',socket.AF_INET)+struct.pack('!H',port)+socket.inet_aton('127.0.0.1')+bytes(8))
        payload=C.create_string_buffer(b'policy-test');size=len(b'policy-test')
        def send():return call(1,sender.fileno(),C.addressof(payload),size,0,C.addressof(address),16)
        expect(send(),size);expect(udp.recv(100),b'policy-test')
        cli('-A','OUTPUT','-m','owner','--uid-owner','10017','-j','DROP')
        expect(send(),-errno.EACCES)
        try:udp.recv(100);raise AssertionError('denied datagram reached server')
        except socket.timeout:checks+=1
        lib.fw_setup(str(root).encode(),10018);expect(send(),size);udp.recv(100)
        lib.fw_setup(str(root).encode(),10017)
        cli('-I','OUTPUT','-p','udp','-d','127.0.0.0/8','--dport',str(port),'-j','ACCEPT')
        expect(send(),size);udp.recv(100)
        cli('-D','OUTPUT','1');expect(send(),-errno.EACCES)
        cli('-F','OUTPUT');sender.connect(udp.getsockname())
        expect(call(2,sender.fileno(),C.addressof(payload),size),size);udp.recv(100)
        cli('-P','OUTPUT','DROP')
        expect(call(2,sender.fileno(),C.addressof(payload),size),-errno.EACCES)
        class Iov(C.Structure):_fields_=[('base',C.c_void_p),('length',C.c_size_t)]
        iov=Iov(C.addressof(payload),size)
        expect(call(3,sender.fileno(),C.addressof(iov),1),-errno.EACCES)
        class Msg(C.Structure):
            _fields_=[('name',C.c_void_p),('namelen',C.c_uint),('iov',C.POINTER(Iov)),('iovlen',C.c_size_t),('control',C.c_void_p),('controllen',C.c_size_t),('flags',C.c_int)]
        msg=Msg(C.addressof(address),16,C.pointer(iov),1,None,0,0)
        expect(call(4,sender.fileno(),C.addressof(msg),0),-errno.EACCES)
        class Mmsg(C.Structure):_fields_=[('hdr',Msg),('length',C.c_uint)]
        mmsg=Mmsg(msg,0);expect(call(5,sender.fileno(),C.addressof(mmsg),1,0),-errno.EACCES)
        with tempfile.TemporaryFile() as file:
            file.write(b'policy-test');file.seek(0)
            expect(call(6,sender.fileno(),file.fileno(),0,size),-errno.EACCES)
        readfd,writefd=os.pipe();os.write(writefd,b'policy-test')
        expect(call(7,readfd,0,sender.fileno(),0,size,0),-errno.EACCES);os.close(readfd);os.close(writefd)
        # Guest IPC and regular file writes survive default-deny Internet policy.
        left,right=socket.socketpair();expect(call(2,left.fileno(),C.addressof(payload),size),size);expect(right.recv(100),b'policy-test')
        left.close();right.close()
        with tempfile.TemporaryFile() as file:expect(call(2,file.fileno(),C.addressof(payload),size),size)
        tcp=socket.socket();tcp.bind(('127.0.0.1',0));tcp.listen(1)
        tcp_address=C.create_string_buffer(struct.pack('=H',socket.AF_INET)+struct.pack('!H',tcp.getsockname()[1])+socket.inet_aton('127.0.0.1')+bytes(8))
        client=socket.socket();expect(call(0,client.fileno(),C.addressof(tcp_address),16),-errno.EACCES);client.close()
        cli('-P','OUTPUT','ACCEPT');client=socket.socket();expect(call(0,client.fileno(),C.addressof(tcp_address),16),0)
        connection,_=tcp.accept();cli('-A','OUTPUT','-p','tcp','-j','DROP')
        expect(call(2,client.fileno(),C.addressof(payload),size),-errno.EACCES)
        client.close();connection.close();tcp.close()
        # IPv6 and IPv4-mapped IPv6 must not escape the IPv4 rule store.
        ipv6=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM)
        mapped=C.create_string_buffer(struct.pack('=H',socket.AF_INET6)+struct.pack('!H',port)+bytes(4)+socket.inet_pton(socket.AF_INET6,'::ffff:127.0.0.1')+bytes(4))
        cli('-A','OUTPUT','-p','udp','-j','DROP');expect(call(1,ipv6.fileno(),C.addressof(payload),size,0,C.addressof(mapped),28),-errno.EACCES)
        native6=C.create_string_buffer(struct.pack('=H',socket.AF_INET6)+struct.pack('!H',port)+bytes(4)+socket.inet_pton(socket.AF_INET6,'::1')+bytes(4))
        cli('-6','-A','OUTPUT','-d','::1/128','-j','DROP');expect(call(1,ipv6.fileno(),C.addressof(payload),size,0,C.addressof(native6),28),-errno.EACCES)
        assert '-d ::1/128' in cli('-6','-S');checks+=1
        # Unsupported syntax cannot silently claim success or change the file.
        original=policy.read_bytes()
        for args in [('-A','INPUT','-j','DROP'),('-t','nat','-F'),('-A','OUTPUT','--sport','80','-j','DROP'),('-A','OUTPUT','-p','tcp','--dport','0','-j','DROP'),('-P','OUTPUT','ACCEPT','-j','DROP')]:cli(*args,ok=False)
        expect(policy.read_bytes(),original)
        # Corruption must fail closed, not reset to allow-all.
        policy.write_bytes(b'corrupt');expect(send(),-errno.EACCES);cli('-F',ok=False)
        policy.unlink();expect(send(),size);udp.recv(100)
        # The actual Manager stores guest /data under the system-data alias.
        (root/'system-data/misc').mkdir(parents=True)
        env['POCKET_FIREWALL_FILE']=str(root/'system-data/misc/pocket-firewall.rules')
        lib.fw_data_alias(1);cli('-P','OUTPUT','DROP');expect(send(),-errno.EACCES)
        cli('-P','OUTPUT','ACCEPT');expect(send(),size);udp.recv(100)
        udp.close();sender.close();ipv6.close()
    print(f'PASS: {checks} real-socket policy assertions, including existing connections, IPv6, direct syscalls and corrupt policy')

if __name__=='__main__':main()
