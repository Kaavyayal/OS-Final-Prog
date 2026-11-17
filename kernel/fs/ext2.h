#pragma once

#include "io/block_io.h"
#include "lib/debug.h"
#include "lib/shared.h"
#include "mm/physmem.h"
#include "sched/mutex.h"

class Node;
// Owns one reference per cached frame; mappings retain their own references.
class CachedPages {
  struct Page { uint64_t index; PPN frame; Page *next; };
  Mutex<Page *> pages{nullptr};
public:
  ~CachedPages();
  sync<uint64_t> get(uint64_t index, Node *file = nullptr);
};

struct SuperBlock {
  uint32_t inodes_count;
  uint32_t blocks_count;
  uint32_t reserved_blocks_count;
  uint32_t free_blocks_count;
  uint32_t free_inodes_count;
  uint32_t first_data_block;
  uint32_t log_block_size;
  uint32_t log_frag_size;
  uint32_t blocks_per_group;
  uint32_t frags_per_group;
  uint32_t inodes_per_group;
  uint32_t mtime;
  uint32_t wtime;
  uint16_t mnt_count;
  uint16_t max_mnt_count;
  uint16_t magic;
  uint16_t state;
  uint16_t errors;
  uint16_t minor_rev_level;
  uint32_t lastcheck;
  uint32_t checkinterval;
  uint32_t creator_os;
  uint32_t rev_level;
  uint16_t def_resuid;
  uint16_t def_resgid;
  uint32_t first_inode;
  uint16_t inode_size;
  uint16_t block_group_nr;
  uint32_t feature_compat;
  uint32_t feature_incompat;
  uint32_t feature_ro_compat;
  char uuid[16];
  char volume_name[16];
  uint32_t algo_bitmap;
};

struct BlockGroup {
  uint32_t block_bitmap;
  uint32_t inode_bitmap;
  uint32_t inode_table;
  uint16_t free_blocks_count;
  uint16_t free_inodes_count;
  uint16_t used_dirs_count;
  uint16_t pad;
  char reserved[12];
};

// An i-node's on-disk bytes.
//   Q: How does mode encode the node's type?
struct NodeData {
  uint16_t mode;
  uint16_t uid;
  uint32_t size_low;
  uint32_t atime;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t dtime;
  uint16_t gid;
  uint16_t n_links;
  uint32_t n_sectors;
  uint32_t flags;
  uint32_t os1;
  uint32_t direct0;
  uint32_t direct1;
  uint32_t direct2;
  uint32_t direct3;
  uint32_t direct4;
  uint32_t direct5;
  uint32_t direct6;
  uint32_t direct7;
  uint32_t direct8;
  uint32_t direct9;
  uint32_t direct10;
  uint32_t direct11;
  uint32_t indirect_1;
  uint32_t indirect_2;
  uint32_t indirect_3;
  uint32_t gen;
  uint32_t reserved1;
  uint32_t reserved2;
  uint32_t fragment;
  char os2[12];

  sync<uint16_t> get_type() { co_return mode >> 12; }

  sync<bool> is_dir() { co_return co_await get_type() == 4; }

  sync<bool> is_file() { co_return co_await get_type() == 8; }

  sync<bool> is_symlink() { co_return co_await get_type() == 0xa; }

  void show(const char *);
};

// An i-node, readable as a BlockIO.
//   Q: Why is a file's block size the file system's, not the device's?
class Node : public BlockIO { // we implement BlockIO because we
                              // represent data

public:
  // Its i-number.
  const uint32_t number;
  NodeData data;
  Mutex<bool> write_lock{false};
  StrongRef<CachedPages> cache = StrongRef<CachedPages>::make();
  sync<int64_t> read_all(uint32_t offset, uint32_t n, char *buffer) override;
  sync<int64_t> write_cached(uint64_t offset, uint64_t n, const char *buffer);
  StrongRef<BlockIO> device;

  Node(uint32_t number, uint32_t block_size, StrongRef<BlockIO> device)
      : BlockIO(block_size), number(number), device(device) {}

  virtual ~Node() {}

  // The bytes it represents.
  //   Q: What do they mean for a file, a directory, and a symbolic link?
  sync<uint32_t> size_in_bytes() override { co_return data.size_low; }

  // Reads block number of this node into buffer.
  //   Q: How does it find the disk block: direct, indirect, double?
  //   Q: What does a hole read as?
  sync<void> read_block(uint32_t number, char *buffer) override;

  // Not yet: writing into a hole needs block allocation.
  sync<void> write_block(uint32_t number, const char *buffer) override;

  // Returns its type (from mode).
  sync<uint16_t> get_type() { co_return co_await data.get_type(); }

  // true if this node is a directory
  sync<bool> is_dir() { co_return co_await data.is_dir(); }

  // true if this node is a file
  sync<bool> is_file() { co_return co_await data.is_file(); }

  // true if this node is a symbolic link
  sync<bool> is_symlink() { co_return co_await data.is_symlink(); }

  // Copies a symbolic link's target into buffer.
  //   Q: Where is a short target stored, and where is a long one?
  //   Q: How big must buffer be?
  sync<void> get_symbol(char *buffer);

  // Returns the number of hard links to it.
  sync<uint32_t> n_links() { co_return data.n_links; }

  void show(const char *msg) { data.show(msg); }

  // Calls work on each entry of this directory.
  //   Q: How does it step from one entry to the next?
  //   Q: What does it do with deleted entries?
  template <typename Work> sync<void> entries(Work work) {
    ASSERT(co_await is_dir());
    uint32_t offset = 0;

    while (offset < data.size_low) {
      uint32_t inode;
      co_await read(offset, inode);

      uint16_t total_size;
      co_await read(offset + 4, total_size);

      if (inode != 0) {

        uint8_t name_length;
        co_await read(offset + 6, name_length);

        auto name = new char[name_length + 1];
        name[name_length] = 0;

        auto cnt = co_await read_all(offset + 8, name_length, name);
        ASSERT(cnt == name_length);

        work(inode, name);

        delete[] name;
      }
      offset += total_size;
    }
  }

  sync<uint32_t> find(const char *name);

  // Returns the number of entries in this directory.
  sync<uint32_t> entry_count();
};

// An ext2 file system on a BlockIO.
//   Q: Why does it put a BlockCache in front of the device?
class Ext2 {
  // The device it's on (through a cache).
  StrongRef<BlockIO> device;

public:
  // Its root directory (i-node 2).
  StrongRef<Node> root;

private:
  struct CachedNode { StrongRef<Node> node; CachedNode *next; };
  Mutex<CachedNode *> nodes{nullptr};
  uint32_t blockSize;
  uint32_t numberOfNodes;
  uint32_t numberOfBlocks;
  uint32_t iNodeSize;
  uint32_t nGroups;
  uint32_t *iNodeTables;
  uint32_t iNodesPerGroup;
  BlockGroup *groupTable;

  Ext2(StrongRef<BlockIO> device);

public:
  // Mounts the file system on device; panics if it's invalid.
  //   Q: What does it read from the superblock?
  static sync<StrongRef<Ext2>> mount(StrongRef<BlockIO> device);

  ~Ext2();

  // Its block size (not necessarily the device's).
  sync<uint32_t> get_block_size() { co_return blockSize; }

  // The size of an i-node on disk (at least 128 bytes).
  //   Q: Why can it be bigger than NodeData?
  sync<uint32_t> get_inode_size() { co_return iNodeSize; }

  // Returns the node with i-number number.
  //   Q: How does it find the block group and offset of an i-node?
  sync<StrongRef<Node>> get_node(uint32_t number);

  // Looks up path from dir, a component at a time.
  //   Q: How are absolute paths, empty components, '.' and '..' handled?
  //   Q: What does it return for a missing component?
  sync<StrongRef<Node>> find(StrongRef<Node> dir, const char *path);
};
