import importlib.util
import struct
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('inventory_tc3',Path(__file__).resolve().parents[1]/'tools/inventory_tc3.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def elf():
    b=bytearray(132)
    b[:7]=b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH',b,16,2,8,1,0x100000,52,0,0,52,32,1,40,0,0)
    struct.pack_into('<8I',b,52,1,128,0x100000,0x100000,4,8,5,16)
    return b


class ELFParsing(unittest.TestCase):
    def test_load_bounds_and_embedded_offsets(self):
        b=elf();r=m.parse_elf(b)
        self.assertEqual(r['entry'],0x100000);self.assertEqual(r['segments'][0]['memory_size'],8)
        self.assertEqual(m.parse_elf(b'prefix'+b,6)['segments'],r['segments'])

    def test_every_truncation_rejected(self):
        b=elf()
        for n in range(len(b)):
            with self.subTest(n=n),self.assertRaises(m.FormatError):m.parse_elf(b[:n])

    def test_class_endian_version_machine(self):
        for offset,value in [(4,2),(5,2),(6,0),(18,62),(20,0)]:
            b=elf();b[offset]=value
            with self.subTest(offset=offset),self.assertRaises(m.FormatError):m.parse_elf(b)

    def test_malformed_tables(self):
        for off,fmt,value in [(28,'I',0xfffffff0),(40,'H',51),(42,'H',31),(44,'H',65535),(32,'I',128),(46,'H',39),(48,'H',1),(50,'H',65535)]:
            b=elf();struct.pack_into('<'+fmt,b,off,value)
            if off==46:struct.pack_into('<H',b,48,1)
            with self.subTest(offset=off),self.assertRaises(m.FormatError):m.parse_elf(b)

    def test_segment_wrap_filesz_and_alignment(self):
        for index,value in [(1,0xfffffff0),(2,0xfffffffc),(4,9),(7,3),(7,256)]:
            b=elf();struct.pack_into('<I',b,52+index*4,value)
            with self.subTest(index=index,value=value),self.assertRaises(m.FormatError):m.parse_elf(b)

    def test_irx_type_from_header_not_filename(self):
        b=elf();struct.pack_into('<H',b,16,0xff80)
        self.assertEqual(m.parse_elf(b)['classification'],'mips_irx')

    def test_entry_outside_load_is_warning(self):
        b=elf();struct.pack_into('<I',b,24,0x200000)
        self.assertIn('Executable entry outside executable PT_LOAD',m.parse_elf(b)['warnings'])

    def test_nobits_has_no_file_payload(self):
        b=elf()+bytearray(40);struct.pack_into('<I',b,32,132);struct.pack_into('<H',b,48,1)
        struct.pack_into('<10I',b,132,0,8,6,0x200000,0xffffffff,4096,0,0,16,0)
        self.assertEqual(m.parse_elf(b)['executable_sections'][0]['size'],4096)
        struct.pack_into('<I',b,136,1)
        with self.assertRaises(m.FormatError):m.parse_elf(b)

    def test_negative_base(self):
        with self.assertRaises(m.FormatError):m.parse_elf(elf(),-1)

    def test_span_arbitrary_large_integers(self):
        for offset,length in [(-1,1),(0,-1),(2**64,1),(1,2**64)]:
            with self.assertRaises(m.FormatError):m.checked_span(132,offset,length,'test')

if __name__=='__main__':unittest.main()
