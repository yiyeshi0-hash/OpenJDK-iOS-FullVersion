// Syntax harness for the Air2 (JIT26) RX toggle block of os_bsd.cpp.
//
// usage: python3 slice_block.py <openjdk>/hotspot/src/os/bsd/vm/os_bsd.cpp rx_block.inc
//        clang++ -std=gnu++98 -fsyntax-only -Wall -Wextra -Wno-unused-parameter rx_harness.cpp
//        g++     -std=gnu++98 -fsyntax-only -Wall rx_harness.cpp
//
// (the "unused function 'a2_rx_init_window'" warning is expected here: in the real
//  file it is called from get_debug_jit_mapping(), which is not part of the slice)
//
// The block between the BEGIN/END markers in rx_block.inc is a verbatim copy of
// the code added to hotspot/src/os/bsd/vm/os_bsd.cpp.  The stubs below mimic the
// declarations hotspot actually provides (address is `unsigned char*`, Bsd is a
// nested class of os, MirrorMappedCodeCache/A2InlineRxWrites are product globals,
// CodeCache::low_bound()/high_bound() return addresses), so this TU type checks
// the real code with a real C++98 compiler.
#include <sys/types.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

typedef unsigned char* address;

class CodeCache {
 public:
  static address low_bound();
  static address high_bound();
};

extern bool MirrorMappedCodeCache;
extern bool A2InlineRxWrites;

class os {
 public:
  class Bsd {
   public:
    static address debug_jit_mapping_mirrored;
    static address debug_jit_rx_base;
    static size_t  debug_jit_rx_size;

    static address ucontext_get_pc(ucontext_t* uc);

    static address mirrored_swap_wx(address addr);
    static address mirrored_find_rw(address addr);
    static address mirrored_find_rx(address addr);

    static bool    a2_rx_contains(address addr);
    static bool    a2_rx_make_writable(address addr, size_t len);
    static void    a2_rx_restore(address addr, size_t len);
    static void    a2_rx_restore_all();
    static bool    a2_rx_handle_fault(int sig, siginfo_t* info, void* ucVoid);
  };
};

address os::Bsd::debug_jit_mapping_mirrored = NULL;

#include "rx_block.inc"
