// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstddef>
#include <ApfsLib/DiskStruct.h>
#include <cstdio>
#include <cstring>
#include <vector>

static void checksum(uint8_t *block)
{
    const uint64_t mod = 0xffffffff;
    uint64_t a = 0, b = 0;
    for (size_t n = 8; n < 4096; n += 4) {
        uint32_t word;
        std::memcpy(&word, block + n, 4);
        a = (a + word) % mod;
        b = (b + a) % mod;
    }
    const uint32_t low = mod - ((a + b) % mod);
    const uint32_t high = mod - ((a + low) % mod);
    std::memcpy(block, &low, 4);
    std::memcpy(block + 4, &high, 4);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        // A wholly synthetic empty container. No filesystem or third-party
        // payloads; enough valid metadata to exercise the complete Init path.
        std::vector<uint8_t> data(8 * 4096);
        auto &nx = *reinterpret_cast<nx_superblock_t *>(data.data());
        nx.nx_o.o_oid = 1;
        nx.nx_o.o_xid = 1;
        nx.nx_o.o_type = OBJECT_TYPE_NX_SUPERBLOCK;
        nx.nx_magic = NX_MAGIC;
        nx.nx_block_size = 4096;
        nx.nx_block_count = 8;
        nx.nx_xp_desc_base = 1;
        nx.nx_xp_desc_blocks = 2;
        nx.nx_xp_desc_len = 2;
        nx.nx_omap_oid = 3;
        auto &map = *reinterpret_cast<checkpoint_map_phys_t *>(data.data() + 4096);
        map.cpm_o.o_oid = 2;
        map.cpm_o.o_xid = 1;
        map.cpm_o.o_type = OBJECT_TYPE_CHECKPOINT_MAP;
        std::memcpy(data.data() + 8192, data.data(), 4096);
        auto &omap = *reinterpret_cast<omap_phys_t *>(data.data() + 3 * 4096);
        omap.om_o.o_oid = 3;
        omap.om_o.o_xid = 1;
        omap.om_o.o_type = OBJECT_TYPE_OMAP;
        omap.om_tree_oid = 4;
        auto &root = *reinterpret_cast<btree_node_phys_t *>(data.data() + 4 * 4096);
        root.btn_o.o_oid = 4;
        root.btn_o.o_xid = 1;
        root.btn_o.o_type = OBJECT_TYPE_BTREE;
        root.btn_o.o_subtype = OBJECT_TYPE_OMAP;
        root.btn_flags = BTNODE_ROOT | BTNODE_LEAF | BTNODE_FIXED_KV_SIZE;
        auto &info = *reinterpret_cast<btree_info_t *>(data.data() + 5 * 4096 - sizeof(btree_info_t));
        info.bt_fixed.bt_node_size = 4096;
        info.bt_fixed.bt_key_size = sizeof(omap_key_t);
        info.bt_fixed.bt_val_size = sizeof(omap_val_t);
        for (size_t n = 0; n < 5; ++n) checksum(data.data() + n * 4096);
        FILE *out = std::fopen(argv[1], "wb");
        if (!out) return 1;
        const bool ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
        return std::fclose(out) || !ok;
    }
    std::printf("{\"role\":%zu,\"fs_flags\":%zu,\"incompat\":%zu,\"fs_root\":%zu,"
                "\"snap_tree\":%zu,\"snap_count\":%zu,\"keylocker\":%zu,\"nx_incompat\":%zu,"
                "\"checkpoint_blocks\":%zu,\"checkpoint_base\":%zu,\"checkpoint_index\":%zu,"
                "\"checkpoint_length\":%zu,\"fext_tree\":%zu}\n",
                offsetof(apfs_superblock_t, apfs_role), offsetof(apfs_superblock_t, apfs_fs_flags),
                offsetof(apfs_superblock_t, apfs_incompatible_features), offsetof(apfs_superblock_t, apfs_root_tree_oid),
                offsetof(apfs_superblock_t, apfs_snap_meta_tree_oid), offsetof(apfs_superblock_t, apfs_num_snapshots),
                offsetof(nx_superblock_t, nx_keylocker), offsetof(nx_superblock_t, nx_incompatible_features),
                offsetof(nx_superblock_t, nx_xp_desc_blocks), offsetof(nx_superblock_t, nx_xp_desc_base),
                offsetof(nx_superblock_t, nx_xp_desc_index), offsetof(nx_superblock_t, nx_xp_desc_len),
                offsetof(apfs_superblock_t, apfs_fext_tree_oid));
}
