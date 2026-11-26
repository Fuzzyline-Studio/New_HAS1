"""Exercise actual firmware wire headers against Python and durable SQLite ACK.

Standalone collector distributions may omit repository C++ sources/compiler;
the repository Linux CI runs this contract test with its system C++ compiler.
"""
from pathlib import Path
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT.parents[1] / "libraries" / "HAS1BlackBox" / "src"
COMPILER = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
sys.path.insert(0, str(ROOT))
from protocol import parse_batch
from storage import Store
from decode_flash import decode

SOURCE = r'''
#include "BlackBoxUpload.h"
#include "BlackBoxRing.h"
#include <algorithm>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
using namespace has1blackbox;
struct MemoryFlash : Flash {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(Ring::Size, 255);
  bool read(size_t offset, uint8_t *out, size_t count) override {
    if (offset > bytes.size() || count > bytes.size()-offset) return false;
    std::copy_n(bytes.data()+offset,count,out); return true;
  }
  bool write(size_t offset, const uint8_t *data, size_t count) override {
    if (offset > bytes.size() || count > bytes.size()-offset) return false;
    for(size_t i=0;i<count;++i) {
      if ((bytes[offset+i] & data[i]) != data[i]) return false;
      bytes[offset+i] &= data[i];
    }
    return true;
  }
  bool erase(size_t offset,size_t count) override {
    if (offset > bytes.size() || count > bytes.size()-offset) return false;
    std::fill_n(bytes.data()+offset,count,255); return true;
  }
  size_t size() const override { return bytes.size(); }
};
int main(int argc, char **argv) {
#ifdef _WIN32
  // Preserve wire LF and arbitrary raw-flash bytes through Windows CRT pipes.
  _setmode(_fileno(stdout),_O_BINARY);
  _setmode(_fileno(stdin),_O_BINARY);
#endif
  if (argc==2 && std::string(argv[1])=="flash") {
    MemoryFlash flash;
    Ring ring(flash);
    if (!ring.begin(0xfedcba9876543210ULL)) return 5;
    // More than four full metadata-journal cycles, without recycling data.
    for (unsigned i=1;i<=600;++i) {
      auto record=Record::make(EventType::Health,42,i,INT32_MIN,1,2,3,"journal.contract");
      if (ring.append(record)!=AppendResult::Persisted || record.seq()!=i ||
          !ring.acknowledge(i) || !ring.checkpointAck()) return 6;
    }
    Ring reboot(flash);
    if (!reboot.begin(0) || reboot.acked()!=600) return 7;
    std::cout.write(reinterpret_cast<const char*>(flash.bytes.data()),flash.bytes.size());
    return 0;
  }
  Batch b;
  b.ring=0xfedcba9876543210ULL; b.token=0x89abcdef; b.count=16;
  for (unsigned i=0;i<16;++i) {
    b.records[i]=Record::make(EventType::Health,0xffffffff,0xfffffffe,
       INT32_MIN,INT32_MAX,-1,int32_t(i),"cross\nlanguage");
    b.records[i].sequence(0xffffffe0u+i);
  }
  if(argc==2 && std::string(argv[1])=="ack") {
    std::string ack{std::istreambuf_iterator<char>(std::cin),{}};
    return exactAck(ack.data(),ack.size(),b)?0:2;
  }
  if(argc!=2) return 3;
  char payload[PayloadCapacity];
  size_t n=encodeBatch(b,argv[1],"AABBCCDDEEFF",payload,sizeof(payload));
  if(!n) return 4;
  std::cout.write(payload,n);
}
'''


@unittest.skipUnless(COMPILER and (HEADERS / "BlackBoxUpload.h").is_file(),
                     "repository C++ headers and compiler required")
class CppContractTests(unittest.TestCase):
    def compile_fixture(self, base):
        source, executable = base / "contract.cpp", base / "contract"
        source.write_text(SOURCE, encoding="utf-8")
        subprocess.run([COMPILER, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-I", str(HEADERS), str(source), "-o", str(executable)],
                       check=True, capture_output=True)
        return executable

    def test_firmware_batch_sqlite_commit_and_firmware_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            executable = self.compile_fixture(base)
            for device in ("iotglove", "HAS1_generator", "HAS1_altar", "HAS3_tagmachine"):
                with self.subTest(device=device):
                    wire = subprocess.check_output([str(executable), device])
                    batch = parse_batch("has1/blackbox/v1/AABBCCDDEEFF/batch", wire)
                    self.assertEqual(len(batch.records), 16)
                    self.assertEqual(batch.records[-1].seq, 0xffffffef)
                    self.assertEqual(batch.records[0].values, (-2147483648, 2147483647, -1, 0))
                    self.assertEqual(batch.records[0].text, "cross?language")
                    store = Store(base / f"{device}.sqlite3")
                    try:
                        store.save(batch)
                        store.save(batch)
                        self.assertEqual(store.db.execute("SELECT count(*) FROM records").fetchone()[0], 16)
                        self.assertEqual(store.db.execute("SELECT raw FROM records WHERE seq=?",
                                                        (batch.records[-1].seq,)).fetchone()[0],
                                         batch.records[-1].raw)
                        subprocess.run([str(executable), "ack"], input=batch.ack_payload, check=True)
                        altered = batch.ack_payload.replace(b"89abcdef", b"89abcdee")
                        rejected = subprocess.run([str(executable), "ack"], input=altered)
                        self.assertEqual(rejected.returncode, 2)
                    finally:
                        store.close()

    def test_firmware_metadata_journal_recycling_and_python_offline_decoder(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            executable = self.compile_fixture(base)
            partition = base / "partition.bin"
            partition.write_bytes(subprocess.check_output([str(executable), "flash"]))
            output = io.StringIO()
            decode(partition, output)
            rows = [json.loads(line) for line in output.getvalue().splitlines()]
            meta = rows[0]
            self.assertEqual(meta["ring"], "fedcba9876543210")
            self.assertEqual(meta["acked"], 600)
            self.assertEqual(meta["reserved_through_exclusive"], 641)
            self.assertEqual(meta["generation"], 611)
            records = [row for row in rows if row["kind"] == "record"]
            self.assertEqual(len(records), 600)
            self.assertEqual([row["seq"] for row in records], list(range(1,601)))
            self.assertTrue(all(row["acked"] for row in records))
            self.assertEqual(rows[-1]["damaged"], 0)
            self.assertEqual(rows[-1]["erased"], 1320)


if __name__ == "__main__":
    unittest.main()
