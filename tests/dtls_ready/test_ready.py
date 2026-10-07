#!/usr/bin/env python3
"""Standalone in-memory DTLS experiment against the installed shared library.
Compiles and exercises the production READY helper. No key material is logged.
"""
import ctypes as C
import ctypes.util
import os
import pathlib
import shlex
import unittest
import json
import subprocess
import sys
import tempfile
import time

libdir=os.environ.get('OPENSSL_LIB_DIR', '')
libext='dylib' if sys.platform=='darwin' else 'so'
ssl=C.CDLL(str(pathlib.Path(libdir)/('libssl.'+libext)) if libdir else ctypes.util.find_library('ssl'))
crypto=C.CDLL(str(pathlib.Path(libdir)/('libcrypto.'+libext)) if libdir else ctypes.util.find_library('crypto'))
P=C.c_void_p; I=C.c_int; L=C.c_long; Z=C.c_size_t; U=C.c_uint; Q=C.c_uint64
def bind(lib,name,restype,args):
    f=getattr(lib,name); f.restype=restype; f.argtypes=args; return f
for name,ret,args in [
    ('DTLS_method',P,[]),('SSL_CTX_new',P,[P]),('SSL_CTX_free',None,[P]),
    ('SSL_CTX_use_certificate_file',I,[P,C.c_char_p,I]),('SSL_CTX_use_PrivateKey_file',I,[P,C.c_char_p,I]),
    ('SSL_CTX_set_cipher_list',I,[P,C.c_char_p]),('SSL_CTX_set_tlsext_use_srtp',I,[P,C.c_char_p]),
    ('SSL_new',P,[P]),('SSL_free',None,[P]),('SSL_set_bio',None,[P,P,P]),
    ('SSL_set_accept_state',None,[P]),('SSL_set_connect_state',None,[P]),
    ('SSL_set_options',Q,[P,Q]),('SSL_ctrl',L,[P,I,L,P]),
    ('SSL_do_handshake',I,[P]),('SSL_get_error',I,[P,I]),('SSL_is_init_finished',I,[P]),
    ('SSL_read',I,[P,P,I]),('SSL_write',I,[P,P,I]),
    ('SSL_export_keying_material',I,[P,P,Z,C.c_char_p,Z,P,Z,I]),
    ('SSL_get_state',I,[P]),('SSL_state_string_long',C.c_char_p,[P]),
    ('SSL_renegotiate',I,[P]),
    ('SSL_set_shutdown',None,[P,I]),
]: bind(ssl,name,ret,args)
for name,ret,args in [('BIO_s_mem',P,[]),('BIO_new',P,[P]),('BIO_ctrl',L,[P,I,L,P]),
                      ('BIO_write',I,[P,P,I]),('BIO_read',I,[P,P,I]),
                      ('ERR_clear_error',None,[]),('ERR_get_error',C.c_ulong,[]),
                      ('ERR_error_string',C.c_char_p,[C.c_ulong,P]),('OpenSSL_version',C.c_char_p,[I])]:
    bind(crypto,name,ret,args)
TIMER=C.CFUNCTYPE(U,P,U)
bind(ssl,'DTLS_set_timer_cb',None,[P,TIMER])
MSG=C.CFUNCTYPE(None,I,I,I,P,Z,P,P)
bind(ssl,'SSL_set_msg_callback',None,[P,MSG])
def emit(event,**kw): print(json.dumps(dict(event=event,**kw)),flush=True)

def records(data):
    out=[]; i=0
    while i+13<=len(data):
        n=int.from_bytes(data[i+11:i+13],'big')
        assert i+13+n<=len(data)
        out.append(dict(type=data[i],epoch=int.from_bytes(data[i+3:i+5],'big'),seq=int.from_bytes(data[i+5:i+11],'big'),length=n))
        i+=13+n
    assert i==len(data)
    return out

class Peer:
    def __init__(self,server,no_ticket=False):
        self.ctx=ssl.SSL_CTX_new(ssl.DTLS_method()); assert self.ctx
        if server:
            assert ssl.SSL_CTX_use_certificate_file(self.ctx,(CERT_DIR+'/cert.pem').encode(),1)==1
            assert ssl.SSL_CTX_use_PrivateKey_file(self.ctx,(CERT_DIR+'/key.pem').encode(),1)==1
        assert ssl.SSL_CTX_set_cipher_list(self.ctx,b'ECDHE-RSA-AES256-GCM-SHA384')==1
        assert ssl.SSL_CTX_set_tlsext_use_srtp(self.ctx,b'SRTP_AES128_CM_SHA1_80')==0
        self.s=ssl.SSL_new(self.ctx); assert self.s
        self.r=crypto.BIO_new(crypto.BIO_s_mem()); self.w=crypto.BIO_new(crypto.BIO_s_mem())
        for b in (self.r,self.w): crypto.BIO_ctrl(b,130,-1,None) # BIO_C_SET_BUF_MEM_EOF_RETURN
        ssl.SSL_set_bio(self.s,self.r,self.w)
        ssl.SSL_set_options(self.s,(1<<12)|((1<<14) if no_ticket else 0))
        assert ssl.SSL_ctrl(self.s,17,1200,None)==1200
        ssl.SSL_ctrl(self.s,33,4,None) # AUTO_RETRY as in FreeSWITCH
        ssl.SSL_ctrl(self.s,41,1,None) # read-ahead as in FreeSWITCH
        ssl.SSL_ctrl(self.s,123,0xfefd,None); ssl.SSL_ctrl(self.s,124,0xfefd,None)
        self.timer=TIMER(lambda s,old: 60000000 if server else 100000)
        ssl.DTLS_set_timer_cb(self.s,self.timer)
        self.sent=[]
        def message(write,version,content,buf,length,s,arg):
            if write and content in (20,22): self.sent.append((content,C.string_at(buf,length)))
        self.message=MSG(message); ssl.SSL_set_msg_callback(self.s,self.message)
        (ssl.SSL_set_accept_state if server else ssl.SSL_set_connect_state)(self.s)
    def feed(self,data):
        if data: assert crypto.BIO_write(self.r,data,len(data))==len(data)
    def pending(self): return crypto.BIO_ctrl(self.r,10,0,None)
    def drain(self):
        n=crypto.BIO_ctrl(self.w,10,0,None)
        if not n:return b''
        b=C.create_string_buffer(n); assert crypto.BIO_read(self.w,b,n)==n
        return b.raw
    def step(self,read=False):
        crypto.ERR_clear_error(); b=C.create_string_buffer(16384)
        ret=ssl.SSL_read(self.s,b,len(b)) if read else ssl.SSL_do_handshake(self.s)
        err=ssl.SSL_get_error(self.s,ret)
        return dict(ret=ret,error=err,finished=bool(ssl.SSL_is_init_finished(self.s)),pending=self.pending())
    def keys(self):
        b=C.create_string_buffer(60); label=b'EXTRACTOR-dtls_srtp'
        assert ssl.SSL_export_keying_material(self.s,b,len(b),label,len(label),None,0,0)==1
        return b.raw
    def close(self): ssl.SSL_free(self.s); ssl.SSL_CTX_free(self.ctx)

class Timeval(C.Structure):
    _fields_=[('seconds',L),('microseconds',L)]

class ReadyTests(unittest.TestCase):
    def setUp(self):
        self.server=Peer(True, 'no_ticket' in self._testMethodName)
        self.client=Peer(False, 'no_ticket' in self._testMethodName)
        self.state=shim.ready_new()
        self.assertTrue(self.state)
        self.now=1000000
        for _ in range(20):
            self.client.step(); self.server.feed(self.client.drain())
            result=self.server.step(); data=self.server.drain()
            if result['finished']:
                self.final=data
                break
            self.client.feed(data)
        else: self.fail('handshake failed to reach server completion')
        self.assertTrue(self.final)
        self.assertFalse(ssl.SSL_is_init_finished(self.client.s))
        self.keys=self.server.keys()
        self.finished=[b for t,b in self.server.sent if t==22 and b[0]==20][-1]

    def tearDown(self):
        shim.ready_free(self.state)
        self.client.close(); self.server.close()

    def retry(self):
        time.sleep(.13)
        self.assertGreater(ssl.SSL_ctrl(self.client.s,74,0,None),0)
        data=self.client.drain()
        self.assertTrue(data)
        return data

    def drive(self,data,advance=True):
        if advance: self.now+=100000
        ret=shim.ready_receive(self.state,self.server.s,self.server.r,self.server.w,data,len(data),self.now)
        self.assertEqual(self.server.pending(),0)
        return ret,self.server.drain()

    def assert_recovered(self,data):
        self.assertTrue(data)
        self.client.feed(data)
        self.assertTrue(self.client.step()['finished'])
        self.assertEqual(self.keys,self.server.keys())
        self.assertEqual(self.keys,self.client.keys())
        self.assertEqual(self.finished,[b for t,b in self.server.sent if t==22 and b[0]==20][-1])
        self.assertEqual(ssl.SSL_ctrl(self.server.s,12,0,None),0)
        self.assertFalse(shim.ready_disabled(self.state))

    def test_control_no_loss(self):
        self.assert_recovered(self.final)
        self.assertEqual(shim.ready_responses(self.state),0)

    def test_dropped_final_flight(self):
        ret,data=self.drive(self.retry())
        self.assertEqual(ret,1)
        self.assertEqual(shim.ready_responses(self.state),1)
        self.assert_recovered(data)

    def test_no_ticket_dropped_final_flight(self):
        ret,data=self.drive(self.retry())
        self.assertEqual(ret,1)
        self.assertEqual([x['type'] for x in records(data)],[20,22])
        self.assert_recovered(data)

    def test_split_flight_and_exact_replay(self):
        flight=self.retry(); pos=0; response=b''
        for r in records(flight):
            n=13+r['length']
            ret,data=self.drive(flight[pos:pos+n],advance=False)
            if r['epoch']==0: self.assertEqual((ret,data),(0,b''))
            response+=data; pos+=n
        self.assert_recovered(response)
        ret,data=self.drive(flight)
        self.assertEqual((ret,data),(1,b''))
        self.assertEqual(shim.ready_responses(self.state),1)

    def test_bad_auth_does_not_consume_valid_retry(self):
        retry=self.retry(); bad=bytearray(retry); bad[-1]^=1
        self.assertEqual(self.drive(bytes(bad)),(1,b''))
        self.assertEqual(shim.ready_responses(self.state),0)
        ret,data=self.drive(retry)
        self.assertEqual(ret,1)
        self.assert_recovered(data)

    def test_rate_limit_does_not_queue_input(self):
        retry=self.retry(); bad=bytearray(retry); bad[-1]^=1
        self.assertEqual(self.drive(bytes(bad)),(1,b''))
        for _ in range(1000): self.assertEqual(self.drive(retry,False),(0,b''))
        self.now+=99999
        self.assertEqual(self.drive(retry,False),(0,b''))
        self.now+=1
        ret,data=self.drive(retry,False)
        self.assertEqual(ret,1)
        self.assert_recovered(data)

    def test_response_budget_preserves_association(self):
        for i in range(12):
            retry=self.retry()
            ret,data=self.drive(retry)
            self.assertEqual(ret,1); self.assertTrue(data)
            self.assertEqual(shim.ready_responses(self.state),i+1)
        for _ in range(100): self.assertEqual(self.drive(retry),(0,b''))
        self.assertTrue(ssl.SSL_is_init_finished(self.server.s))
        self.assertEqual(self.keys,self.server.keys())
        self.assertFalse(shim.ready_disabled(self.state))

    def test_malformed_and_multiple_records(self):
        retry=self.retry(); offset=Z()
        n=shim.ready_record(retry,len(retry),C.byref(offset))
        self.assertGreater(n,0)
        finished=retry[offset.value:offset.value+n]
        app=bytearray(finished); app[0]=23
        next_epoch=bytearray(finished); next_epoch[4]=2
        bad_version=bytearray(finished); bad_version[1]=3
        cases=[b'',b'x',retry[:-1],finished+finished,bytes(app),bytes(next_epoch),bytes(bad_version),retry+b'x',b'x'*4097]
        # Nine well-framed records exceed the packet processing bound.
        ccs=bytes.fromhex('14fefd0000000000000000000101')
        cases.append(ccs*8+finished)
        for packet in cases:
            with self.subTest(length=len(packet)):
                self.assertEqual(self.drive(packet),(0,b''))
                self.assertEqual(shim.ready_responses(self.state),0)
        ret,data=self.drive(retry)
        self.assertEqual(ret,1); self.assert_recovered(data)

    def test_setup_queued_retry_does_not_disable_recovery(self):
        retry=self.retry(); self.server.feed(retry)
        ret,data=self.drive(retry)
        self.assertEqual(ret,1)
        self.assert_recovered(data)

    def test_terminal_input_disables_once_without_reset(self):
        # A closed SSL association must not be driven repeatedly or recreated.
        retry=self.retry(); ssl.SSL_set_shutdown(self.server.s,2)
        ret=shim.ready_receive(self.state,self.server.s,self.server.r,self.server.w,retry,len(retry),self.now)
        self.assertEqual(ret,-1)
        self.assertEqual(self.server.pending(),0)
        self.assertTrue(shim.ready_disabled(self.state))
        self.assertEqual(shim.ready_receive(self.state,self.server.s,self.server.r,self.server.w,retry,len(retry),self.now+100000),0)
        self.assertTrue(ssl.SSL_is_init_finished(self.server.s))
        self.assertEqual(self.server.keys(),self.keys)

    def test_bidirectional_srtp_srtcp_after_recovery(self):
        if not hasattr(shim,'ready_srtp_new'): self.skipTest('set SRTP_LIBS to enable real SRTP/SRTCP checks')
        server_srtp=shim.ready_srtp_new(self.keys,1)
        self.assertTrue(server_srtp)
        client_srtp=None
        try:
            ret,data=self.drive(self.retry())
            self.assertEqual(ret,1); self.assert_recovered(data)
            client_srtp=shim.ready_srtp_new(self.client.keys(),0)
            self.assertTrue(client_srtp)
            for rtcp in (0,1):
                self.assertEqual(shim.ready_srtp_exchange(server_srtp,client_srtp,rtcp),1)
                self.assertEqual(shim.ready_srtp_exchange(client_srtp,server_srtp,rtcp),1)
        finally:
            shim.ready_srtp_free(server_srtp)
            if client_srtp: shim.ready_srtp_free(client_srtp)

    def test_authenticated_renegotiation_is_not_started(self):
        self.assert_recovered(self.final)
        self.assertEqual(ssl.SSL_renegotiate(self.client.s),1)
        self.client.step()
        request=self.client.drain()
        self.assertTrue(request)
        self.drive(request)
        self.assertTrue(ssl.SSL_is_init_finished(self.server.s))
        self.assertEqual(ssl.SSL_ctrl(self.server.s,12,0,None),0)
        self.assertEqual(self.server.keys(),self.keys)

if __name__=='__main__':
    here=pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix='dtls-ready-test-') as temp:
        CERT_DIR=temp
        subprocess.run([os.environ.get('OPENSSL','openssl'),'req','-x509','-newkey','rsa:2048','-nodes','-keyout',temp+'/key.pem','-out',temp+'/cert.pem','-days','1','-subj','/CN=DTLS-ready-test'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
        target=temp+'/ready.'+libext
        flags=shlex.split(os.environ.get('OPENSSL_CFLAGS',''))
        libs=shlex.split(os.environ.get('OPENSSL_LIBS','-lssl -lcrypto'))
        if os.environ.get('SRTP_LIBS'):
            flags+=['-DTEST_SRTP','-I'+str(here.parents[1]/'libs/srtp/include')]
            libs+=shlex.split(os.environ['SRTP_LIBS'])
        cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu89','-Wall','-Wextra','-Werror','-Werror=declaration-after-statement','-fPIC','-shared','-I'+str(here.parents[1]/'src/include')]+flags+[str(here/'ready_shim.c'),'-o',target]+libs
        subprocess.run(cmd,check=True)
        shim=C.CDLL(target)
        for name,ret,args in [('ready_new',P,[]),('ready_free',None,[P]),('ready_receive',I,[P,P,P,P,P,Z,Q]),('ready_responses',U,[P]),('ready_disabled',I,[P]),('ready_record',Z,[P,Z,P])]:
            bind(shim,name,ret,args)
        if os.environ.get('SRTP_LIBS'):
            for name,ret,args in [('ready_srtp_new',P,[P,I]),('ready_srtp_free',None,[P]),('ready_srtp_exchange',I,[P,P,I])]: bind(shim,name,ret,args)
        print(crypto.OpenSSL_version(0).decode(),flush=True)
        unittest.main(verbosity=2)
