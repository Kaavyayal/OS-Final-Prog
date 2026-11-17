#include "fs/ext2.h"
#include "io/block_cache.h"
#include "lib/print.h"
#include "mm/vmm.h"

// A minimal, read-only ext2: enough to find and load programs.
//   Q: Which ext2 features does it leave out?
//   Q: Where does it find the superblock and the block group table?

Ext2::Ext2(StrongRef<BlockIO> device)
    : device(StrongRef<BlockIO>{new BlockCache(device, 31)}), root() {}

sync<StrongRef<Ext2>> Ext2::mount(StrongRef<BlockIO> device) {

  StrongRef<Ext2> fs{new Ext2(device)};

  SuperBlock sb;

  co_await device->read(1024, sb);
  fs->iNodeSize = sb.inode_size;
  fs->iNodesPerGroup = sb.inodes_per_group;
  fs->numberOfNodes = sb.inodes_count;
  fs->numberOfBlocks = sb.blocks_count;

  // Debug::printf("inodes_count %d\n",sb.inodes_count);
  // Debug::printf("blocks_count %d\n",sb.blocks_count);
  // Debug::printf("blocks_per_group %d\n",sb.blocks_per_group);
  // Debug::printf("inode_size %d\n",sb.inode_size);
  // Debug::printf("block_group_nr %d\n",sb.block_group_nr);
  // Debug::printf("first_inode %d\n",sb.first_inode);

  fs->blockSize = uint32_t(1) << (sb.log_block_size + 10);

  fs->nGroups =
      (sb.blocks_count + sb.blocks_per_group - 1) / sb.blocks_per_group;
  // Debug::printf("nGroups = %d\n",nGroups);
  ASSERT(fs->nGroups * sb.blocks_per_group >= sb.blocks_count);

  auto superBlockNumber = 1024 / fs->blockSize;
  // Debug::printf("super block number %d\n",superBlockNumber);

  auto groupTableNumber = superBlockNumber + 1;
  // Debug::printf("group table number %d\n",groupTableNumber);

  auto groupTableSize = sizeof(BlockGroup) * fs->nGroups;
  // Debug::printf("group table size %d\n",groupTableSize);

  fs->groupTable = new BlockGroup[fs->nGroups];
  auto cnt = co_await device->read_all(groupTableNumber * fs->blockSize,
                                       groupTableSize, (char *)fs->groupTable);
  ASSERT(uint64_t(cnt) == groupTableSize);

  fs->iNodeTables = new uint32_t[fs->nGroups];

  for (uint32_t i = 0; i < fs->nGroups; i++) {
    auto g = &fs->groupTable[i];

    fs->iNodeTables[i] = g->inode_table;

    // Debug::printf("group #%d\n",i);
    // Debug::printf("    block_bitmap %d\n",g->block_bitmap);
    // Debug::printf("    inode_bitmap %d\n",g->inode_bitmap);
    // Debug::printf("    inode_table %d\n",g->inode_table);
  }

  // Debug::printf("========\n");
  // Debug::printf("iNodeSize %d\n",iNodeSize);
  // Debug::printf("nGroups %d\n",nGroups);
  // Debug::printf("iNodesPerGroup %d\n",iNodesPerGroup);
  // Debug::printf("numberOfNodes %d\n",numberOfNodes);
  // Debug::printf("numberOfBlocks %d\n",numberOfBlocks);
  // Debug::printf("blockSize %d\n",blockSize);
  // for (unsigned i = 0; i < nGroups; i++) {
  //     Debug::printf("iNodeTable[%d] %d\n",i,iNodeTables[i]);
  // }

  // root = new Node(ide,2,blockSize);

  fs->root = co_await fs->get_node(2);

  // root->show("root");

  // root->entries([](uint32_t inode, char* name) {
  //     Debug::printf("%d %s\n",inode,name);
  // });

  // println(sb.uuid);
  // println(sb.volume_name);
  co_return fs;
}

Ext2::~Ext2() {
  auto n = nodes.unshared();
  while (n) { auto next = n->next; delete n; n = next; }
  delete[] iNodeTables;
  delete[] groupTable;
}

sync<StrongRef<Node>> Ext2::get_node(uint32_t number) {
  ASSERT(number > 0);
  ASSERT(number <= numberOfNodes);
  auto proof = co_await nodes;
  for (auto p = proof.data(); p; p = p->next)
    if (p->node->number == number) co_return p->node;
  auto index = number - 1;

  auto groupIndex = index / iNodesPerGroup;
  // Debug::printf("groupIndex %d\n",groupIndex);
  ASSERT(groupIndex < nGroups);
  auto indexInGroup = index % iNodesPerGroup;
  auto iTableBase = iNodeTables[groupIndex];
  ASSERT(iTableBase <= numberOfBlocks);
  // Debug::printf("iTableBase %d\n",iTableBase);
  auto nodeOffset = iTableBase * blockSize + indexInGroup * iNodeSize;
  // Debug::printf("nodeOffset %d\n",nodeOffset);

  auto out = StrongRef<Node>::make(number, blockSize, device);
  co_await device->read(nodeOffset, out->data);
  proof.data() = new CachedNode{out, proof.data()};
  co_return out;
}

////////////// NodeData //////////////

void NodeData::show(const char *what) {
  KPRINT("?\n", what);
  KPRINT("    mode 0x?\n", mode);
  KPRINT("    uid ?\n", Dec(uid));
  KPRINT("    gif ?\n", Dec(gid));
  KPRINT("    n_links ?\n", Dec(n_links));
  KPRINT("    n_sectors ?\n", Dec(n_sectors));
}

///////////// Node /////////////

sync<void> Node::get_symbol(char *buffer) {
  ASSERT(co_await is_symlink());
  auto sz = co_await size_in_bytes();
  if (sz < 60) {
    memcpy(buffer, &data.direct0, sz);
  } else {
    auto cnt = co_await read_all(0, sz, buffer);
    ASSERT(cnt == sz);
  }
}

sync<void> Node::read_block(uint32_t index, char *buffer) {
  // Check the size, not n_sectors: sparse files have holes.
  //   Q: How can a file have fewer blocks than its size implies?
  ASSERT(index < co_await size_in_blocks());

  auto refs_per_block = block_size / 4;

  // 0 means a hole (never written), at either level.
  uint32_t block_index = 0;

  if (index < 12) {
    uint32_t *direct = &data.direct0;
    block_index = direct[index];
  } else if (index < (12 + refs_per_block)) {
    if (data.indirect_1 != 0) {
      co_await device->read(data.indirect_1 * block_size + (index - 12) * 4,
                            block_index);
    }
  } else {
    uint64_t relative = uint64_t(index) - 12 - refs_per_block;
    uint64_t square = uint64_t(refs_per_block) * refs_per_block;
    uint32_t indirect;
    if (relative < square) {
      indirect = data.indirect_2;
    } else {
      relative -= square;
      indirect = data.indirect_3;
      if (indirect) co_await device->read(uint64_t(indirect) * block_size + (relative / square) * 4, indirect);
      relative %= square;
    }
    if (indirect) co_await device->read(uint64_t(indirect) * block_size + (relative / refs_per_block) * 4, indirect);
    if (indirect) co_await device->read(uint64_t(indirect) * block_size + (relative % refs_per_block) * 4, block_index);
  }

  if (block_index == 0) {
    memset(buffer, 0, block_size);
    co_return;
  }

  auto cnt =
      co_await device->read_all(block_index * block_size, block_size, buffer);
  ASSERT(cnt == block_size);
}

sync<void> Node::write_block(uint32_t, const char *) {
  MISSING();
  co_return;
}

sync<uint32_t> Node::find(const char *name) {
  ASSERT(co_await is_dir());
  uint32_t wanted = 0;
  while (name[wanted]) ++wanted;
  auto buffer = new char[block_size];
  uint32_t found = 0;
  for (uint32_t base = 0; base < data.size_low && !found; base += block_size) {
    co_await read_block(base / block_size, buffer);
    uint32_t limit = data.size_low - base;
    if (limit > block_size) limit = block_size;
    for (uint32_t at = 0; at + 8 <= limit;) {
      uint32_t inode;
      uint16_t record;
      memcpy(&inode, buffer + at, sizeof(inode));
      memcpy(&record, buffer + at + 4, sizeof(record));
      uint32_t length = uint8_t(buffer[at + 6]);
      if (record < 8 || record > limit - at || length > uint32_t(record) - 8) break;
      if (inode && length == wanted) {
        bool same = true;
        for (uint32_t i = 0; i < length; ++i)
          if (buffer[at + 8 + i] != name[i]) { same = false; break; }
        if (same) { found = inode; break; }
      }
      at += record;
    }
  }
  delete[] buffer;
  co_return found;
}

sync<uint32_t> Node::entry_count() {
  ASSERT(co_await is_dir());
  uint32_t count = 0;
  co_await entries([&count](uint32_t, const char *) { count += 1; });
  co_return count;
}

///////////// Ext2::find /////////////

sync<StrongRef<Node>> Ext2::find(StrongRef<Node> dir, const char *path) {
  ASSERT(co_await dir->is_dir());

  StrongRef<Node> node = (path[0] == '/') ? root : dir;

  while (true) {
    while (*path == '/') {
      path++;
    }
    if (*path == 0) {
      co_return node;
    }

    // node is about to be searched, so it must be a directory.
    if (!co_await node->is_dir()) {
      co_return {};
    }

    uint32_t len = 0;
    while (path[len] != 0 && path[len] != '/') {
      len++;
    }

    auto name = new char[len + 1];
    memcpy(name, (void *)path, len);
    name[len] = 0;

    uint32_t number = co_await node->find(name);
    delete[] name;

    if (number == 0) {
      co_return {};
    }
    node = co_await get_node(number);
    path += len;
  }
}


CachedPages::~CachedPages() {
  auto p = pages.unshared();
  while (p) { auto next = p->next; physMem.free(p->frame); delete p; p = next; }
}

sync<uint64_t> CachedPages::get(uint64_t index, Node *file) {
  auto proof = co_await pages;
  for (auto p = proof.data(); p; p = p->next)
    if (p->index == index) co_return p->frame.ppn();
  auto frame = physMem.alloc();
  // Use raw block reads here: going through read_all would reenter the cache.
  if (file && index * FRAME_SIZE < file->data.size_low) {
    uint64_t offset = index * FRAME_SIZE;
    uint64_t count = file->data.size_low - offset;
    if (count > FRAME_SIZE) count = FRAME_SIZE;
    uint64_t done = 0;
    while (done < count) {
      auto n = co_await file->BlockIO::read(offset + done, count - done,
                                            (char *)VA{frame} + done);
      ASSERT(n > 0);
      done += n;
    }
  }
  proof.data() = new Page{index, frame, proof.data()};
  co_return frame.ppn();
}

sync<int64_t> Node::read_all(uint32_t offset, uint32_t n, char *buffer) {
  if (!co_await is_file()) co_return co_await BlockIO::read_all(offset, n, buffer);
  if (offset >= data.size_low) co_return 0;
  uint64_t count = data.size_low - offset;
  if (count > n) count = n;
  uint64_t done = 0;
  while (done < count) {
    uint64_t at = uint64_t(offset) + done;
    PPN frame{co_await cache->get(at / FRAME_SIZE, this)};
    uint64_t part = FRAME_SIZE - at % FRAME_SIZE;
    if (part > count - done) part = count - done;
    memcpy(buffer + done, (char *)VA{frame} + at % FRAME_SIZE, part);
    done += part;
  }
  co_return done;
}

sync<int64_t> Node::write_cached(uint64_t offset, uint64_t n, const char *buffer) {
  // Extend the in-memory file only; neither data nor inode changes reach disk.
  if (!co_await is_file()) co_return -21;
  if (offset > UINT32_MAX || n > UINT32_MAX - offset) co_return -27;
  auto proof = co_await write_lock;
  uint64_t count = n;
  uint64_t done = 0;
  while (done < count) {
    uint64_t at = offset + done;
    PPN frame{co_await cache->get(at / FRAME_SIZE, this)};
    uint64_t part = FRAME_SIZE - at % FRAME_SIZE;
    if (part > count - done) part = count - done;
    memcpy((char *)VA{frame} + at % FRAME_SIZE, buffer + done, part);
    done += part;
  }
  if (done && offset + done > data.size_low) data.size_low = offset + done;
  co_return done;
}
