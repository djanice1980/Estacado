from pathlib import Path
import runpy
import struct
import tempfile
import unittest

a = runpy.run_path(str(Path(__file__).resolve().parents[2]/'scripts/analyze-owned-packet-writers.py'))


def pack(*words):
    return struct.pack(f'>{len(words)}I', *words)


def fixture():
    regs = a['REGISTERS']
    tuples = ','.join(f'{i+1}:00001000:{0x1004+4*i:08X}:{i:08X}:0' for i in range(36))
    text = ('REX_PC_DRAW_INPUT frame=10 index=0 raw=' + ','.join(f'{i:X}' for i in range(36)) + '\n'
            'REX_PC_DRAW_WRITERS frame=10 index=0 records='+tuples+
            ' scope=last_observed_register_write camera_identity=0 allocation_lifetime=0\n')
    index = {(0x1000,0x1004+4*i,reg,i): [dict(segment=0,copy=0)] for i,reg in enumerate(regs)}
    return text, index


class PacketTests(unittest.TestCase):
    def test_execution_publication_ownership(self):
        text,_ = fixture()
        text += ('REX_PC_DRAW_EXECUTIONS frame=10 index=0 records='+','.join(['90:80:100:81:2']*36)+
                 ' scope=actual_cp_execution cpu_publication_identity=0 camera_identity=0\n'
                 'REX_PC_DRAW_ROOT_PUBLICATIONS frame=10 index=0 records='+','.join(['7']*36)+
                 ' scope=root_ring_publication child_allocation_identity=0 camera_identity=0\n')
        result=a['verify_executions'](text)
        self.assertEqual((result['words'],result['packets'],result['root_publications']),(36,1,[7]))
        for old,new in [('90:80:100:81:2','90:80:100:91:2'),('records=7,7','records=0,7'),
                        ('records=7,7','records=8,7'),('cpu_publication_identity=0','cpu_publication_identity=1'),
                        ('frame=10 index=0 records=90','frame=11 index=0 records=90')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                a['verify_executions'](text.replace(old,new,1))

    def test_packet_types_and_big_endian(self):
        data = pack(0,0x80000000,0x40000000,7,8,0xC0000000,9,0x14000,0x12345678,0xABCDEF00)
        self.assertEqual(a['decode'](data,0x1000),
                         [(0x101C,0x1020,0x4000,0x12345678),(0x101C,0x1024,0x4001,0xABCDEF00)])

    def test_repeated_register(self):
        self.assertEqual([w[2] for w in a['decode'](pack(0x1C000,5,6),0)], [0x4000,0x4000])

    def test_truncated_or_bad_range(self):
        for data,physical in [(b'x',0),(pack(0x14000,1),0),(pack(0x40000000,1),0),
                              (pack(0xC0010000,1),0),(pack(0),1),(pack(0),0x20000000)]:
            with self.subTest(data=data), self.assertRaises(ValueError): a['decode'](data,physical)

    def test_unique_and_reused_addresses(self):
        text,index = fixture()
        result = a['join_writers'](text,index,[10])
        self.assertEqual(result['counts'],dict(unique_bytes=36))
        for candidates in index.values(): candidates.append(dict(segment=1,copy=1))
        result = a['join_writers'](text,index,[10])
        self.assertEqual(result['counts'],dict(ambiguous_bytes=36))
        self.assertEqual(len(result['matches'][0]['candidates']),2)
        self.assertEqual(a['join_writers'](text,{},[10])['counts'],dict(outside_cpu_window_or_unmatched=36))

    def test_corrupt_or_missing_writers(self):
        text,index = fixture()
        for old,new in [('camera_identity=0','camera_identity=1'),('allocation_lifetime=0','allocation_lifetime=1'),
                        ('1:00001000:00001004:00000000:0','1:00001000:00001004:00000001:0'),
                        ('2:00001000','1:00001000'),('frame=10 index=0 records','frame=11 index=0 records')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                a['join_writers'](text.replace(old,new),index,[10])
        with self.assertRaises(ValueError): a['join_writers'](text+text.splitlines()[1],index,[10])

    def test_empty_writer_is_not_proof(self):
        text,index = fixture()
        text = text.replace('1:00001000:00001004:00000000:0','0:FFFFFFFF:FFFFFFFF:00000000:0')
        self.assertEqual(a['join_writers'](text,index,[10])['counts']['unobserved'],1)

    def test_source_integrity_and_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'logs').mkdir()
            data = pack(0x4000,0x12345678)
            payload = root/'logs/pc_packet_source_0.bin'
            payload.write_bytes(data)
            text = ('PACKET_SOURCE_CAPTURE version=1 generation=1 max_segments=128 max_segment_bytes=262144 max_bytes=8388608 max_copies=4\n'
                    f'PACKET_SOURCE sequence=0 copy=0 generation=1 begin=A0001000 cursor=A0001004 physical=1000 hash={a["fnv"](data):X} bytes=8 valid=1 written=1 path=logs/pc_packet_source_0.bin scope=pre_publication_segment_not_camera_identity\n'
                    'PACKET_SOURCE_END generation=1 records=1 bytes=8 dropped=0 invalid=0 write_failures=0 boundary=next_qualified_viewport_copy\n')
            final = 'PC_PACKET_SOURCE_CAPTURE generation=1 records=1 dropped=0 invalid=0 write_failures=0 closed=1 path=logs/pc_packet_sources.log\n'
            path = root/'logs/pc_packet_sources.log'
            path.write_text(text,encoding='utf-8')
            self.assertEqual(len(a['load_sources'](root,final)[0]),1)
            for bad in [text.replace('dropped=0','dropped=1'),text.replace('bytes=8 dropped','bytes=12 dropped'),
                        text.replace('copy=0','copy=4'),text.replace('path=logs/pc_packet_source_0.bin','path=../escape.bin'),
                        '\n'.join(text.splitlines()[:-1])]:
                path.write_text(bad,encoding='utf-8')
                with self.assertRaises(ValueError): a['load_sources'](root,final)
            path.write_text(text,encoding='utf-8')
            payload.write_bytes(pack(0x4000,0xDEADBEEF))
            with self.assertRaises(ValueError): a['load_sources'](root,final)


if __name__ == '__main__': unittest.main()
