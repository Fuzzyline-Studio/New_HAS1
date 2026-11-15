#!/usr/bin/env python3
"""Host policy tests, including retained history across simulated warm boots."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "generator_offline_recovery.h"
#include <cassert>
#include <cstdio>
#include <type_traits>
using namespace GeneratorOfflineRecovery;
static_assert(std::is_trivial<Retained>::value,"RTC NOINIT must not run an initializing constructor");
int main(){
 Retained h{};assert(!boot(h,false)&&valid(h));Policy p;
 assert(!p.sample(0,false,h)&&!p.due(179999,0)&&p.due(180000,0));
 assert(!p.due(184999,5000)&&p.due(185000,5000));
 p.sample(200000,true,h);p.sample(200001,false,h);
 assert(p.hadConnection&&!p.due(230000,0)&&p.due(230001,0));
 assert(!p.due(235000,5000)&&p.due(235001,5000));
 p.sample(240000,true,h);p.sample(240001,false,h);
 assert(!p.due(270000,0)&&p.due(270001,0));
 p=Policy{};p.sample(0,false,h);assert(!p.hadConnection&&!p.due(30000,0)&&p.due(180000,0));
 puts("PASS never-associated boot waits 180 seconds; observed online then down waits 30 seconds; flap resets outage, new boot resets observation");
 requested(h,185000);assert(h.attempts==1&&h.pending==1);
 assert(boot(h,true)&&h.attempts==1&&h.outageMs==185000&&h.pending==0);
 assert(delayMs(0,false)==180000&&delayMs(0,true)==30000&&delayMs(5000,true)==35000&&delayMs(5001,true)==30000);
 for(unsigned i=0;i<12;++i){requested(h,30000);assert(boot(h,true));}
 assert(h.attempts==13);p=Policy{};p.sample(0,false,h);assert(!p.due(179999,0)&&p.due(180000,0));
 p.sample(200000,true,h);p.sample(200001,false,h);assert(!p.due(230000,0)&&p.due(230001,0));
 puts("PASS repeated recovery boots keep counting but never lengthen the 180/30 second limits or the five second jitter");
 Retained corrupt=h;corrupt.check^=1;assert(!boot(corrupt,true)&&corrupt.attempts==0);
 requested(h,123);assert(!boot(h,false)&&h.attempts==0);
 requested(h,123);assert(boot(h,true));assert(!boot(h,true)&&h.attempts==0);
 puts("PASS cold, corrupt and unrelated reset histories reset; consumed recovery intent is not replayed");
 requested(h,180000);assert(boot(h,true));p=Policy{};
 p.sample(0,true,h);p.sample(900000,true,h);assert(h.attempts==1);
 for(uint32_t t=900000;t<1500000;t+=1000){p.serverReply(t);assert(!p.sample(t,true,h));}
 p.serverReply(1500000);assert(p.sample(1500000,true,h)&&h.attempts==0);
 p.sample(1500001,false,h);assert(!p.due(1530000,0)&&p.due(1530001,0));
 puts("PASS healthy association alone cannot reset the restart count; ten minutes with fresh server replies does");
 requested(h,1);assert(boot(h,true));p=Policy{};
 p.serverReply(0);p.sample(0,true,h);p.sample(15000,true,h);
 p.serverReply(600000);assert(!p.sample(600000,true,h)&&h.attempts==1);
 p.sample(600001,false,h);p.serverReply(600002);assert(!p.sample(600002,true,h)&&h.attempts==1);
 puts("PASS server freshness gaps and link drops interrupt the stable recovery window");
 p=Policy{};p.sample(UINT32_MAX-100000,false,h);
 assert(!p.due(79998,0)&&p.due(79999,0));
 p.sample(200000,true,h);assert(!p.due(200001,0));
 h=Retained{};seal(h);p=Policy{};p.sample(UINT32_MAX-20000,true,h);p.sample(UINT32_MAX-10000,false,h);
 assert(!p.due(19998,0)&&p.due(19999,0));
 puts("PASS outage timer handles millis rollover and clears promptly on association");
 h=Retained{};seal(h);assert(faultRestartDue(h));requested(h,10000);assert(boot(h,true)&&!faultRestartDue(h));
 assert(!boot(h,false)&&faultRestartDue(h));
 puts("PASS scan driver fault restarts at once only when no own recovery restart preceded it");
 h.attempts=65535;seal(h);requested(h,7);assert(valid(h)&&h.attempts==65535);
 puts("PASS retained counter saturates without overflow");
}
'''
with tempfile.TemporaryDirectory(prefix='generator-offline-test-') as tmp:
 p=Path(tmp);src=p/'test.cpp';src.write_text(SOURCE);exe=p/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
