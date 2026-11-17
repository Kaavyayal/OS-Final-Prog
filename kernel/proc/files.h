#pragma once
#include "fs/ext2.h"
#include "sched/async.h"
#include "sched/mutex.h"

struct Pipe {
  SpinLock lock;
  char bytes[4096];
  unsigned head = 0, size = 0;
  Atomic<unsigned> readers{1}, writers{1};
};
struct OpenFile {
  StrongRef<Node> node;
  int kind; // 0: file, 1: input, 2: output, 3: debug
  int flags;
  StrongRef<Pipe> pipe;
  ~OpenFile() {
    if (pipe != nullptr) {
      if (kind == 4)
        pipe->readers.fetch_sub(1);
      if (kind == 5)
        pipe->writers.fetch_sub(1);
    }
  }
  Mutex<uint64_t> offset{0};
  OpenFile(StrongRef<Node> node, int kind, int flags)
      : node(node), kind(kind), flags(flags) {}
};

struct FileTable {
  static constexpr int LIMIT = 256;
  StrongRef<OpenFile> files[LIMIT];
  bool cloexec[LIMIT]{};
  FileTable() {
    files[0] = StrongRef<OpenFile>::make(StrongRef<Node>{}, 1, 0);
    files[1] = StrongRef<OpenFile>::make(StrongRef<Node>{}, 2, 1);
    files[2] = StrongRef<OpenFile>::make(StrongRef<Node>{}, 3, 1);
  }
};

struct ChildProcess {
  int pid;
  async<int> exited;
  ChildProcess *next;
};
