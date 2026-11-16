import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'archive-deploy-symbols.py'
spec = importlib.util.spec_from_file_location('archive_deploy_symbols', SCRIPT)
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)


def elf(text=b'code', symbols=True):
    names = b'\0.text\0.shstrtab\0.symtab\0'
    content = bytearray(64)
    content[:6] = b'\x7fELF\x01\x02'
    text_offset = len(content); content.extend(text)
    name_offset = len(content); content.extend(names)
    sym_offset = len(content); content.extend(b'\0' * 16)
    shoff = len(content)
    headers = [(0,)*10, (1,1,2,0x400000,text_offset,len(text),0,0,4,0),
               (7,3,0,0,name_offset,len(names),0,0,1,0)]
    if symbols:
        headers.append((17,2,0,0,sym_offset,16,0,0,4,16))
    for header in headers: content.extend(struct.pack('>10I', *header))
    struct.pack_into('>I', content, 32, shoff)
    struct.pack_into('>HHH', content, 46, 40, len(headers), 2)
    return bytes(content)


class ArchiveTest(unittest.TestCase):
    def test_archive_and_refuse_mismatch_or_missing_symbols(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); binary = root / 'microhad'; symbols = root / 'microhad.elf'
            binary.write_bytes(elf(symbols=False)); symbols.write_bytes(elf())
            command = [sys.executable, str(SCRIPT), 'microhad', str(binary), str(root / 'archive')]
            subprocess.run(command, check=True, capture_output=True)
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            self.assertEqual((root / 'archive' / 'microhad' / digest / 'unstripped.elf').read_bytes(), symbols.read_bytes())
            symbols.write_bytes(elf(b'bad!'))
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            symbols.write_bytes(elf(symbols=False))
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)


if __name__ == '__main__': unittest.main()
