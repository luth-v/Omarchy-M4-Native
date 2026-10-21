// SPDX-License-Identifier: GPL-2.0-or-later
// Read the owner's System volume through an inherited read-only descriptor.
// No mounts, credential input, decryption, or writes to the input.
#include <ApfsLib/ApfsContainer.h>
#include <ApfsLib/ApfsDir.h>
#include <ApfsLib/ApfsVolume.h>
#include <ApfsLib/Decmpfs.h>
#include <ApfsLib/ReaderLimits.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/fs.h>
#include <memory>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using M4Apfs::Require;

static std::string refusal_reason(const char *text)
{
    const size_t size = std::strlen(text);
    if (!size || size > 96) return "PARSER_FAILURE";
    for (size_t n = 0; n < size; ++n)
        if (!((text[n] >= 'A' && text[n] <= 'Z') ||
              (text[n] >= '0' && text[n] <= '9') || text[n] == '_'))
            return "PARSER_FAILURE";
    return text;
}

static void timed_out(int)
{
    const char message[] = "REFUSAL DEADLINE\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(24);
}

#ifndef M4_APFS_SANITIZER_BUILD
static void parser_signal(int signum)
{
    const char message[] = "REFUSAL PARSER_SIGNAL\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(128 + signum);
}
#endif

static void limits()
{
#ifndef M4_APFS_SANITIZER_BUILD
    struct rlimit current;
    Require(getrlimit(RLIMIT_AS, &current) == 0, "RESOURCE_LIMIT_QUERY");
    const rlim_t cap = 1024ULL * 1024 * 1024;
    current.rlim_cur = std::min(current.rlim_cur, cap);
    Require(setrlimit(RLIMIT_AS, &current) == 0, "RESOURCE_LIMIT_SET");
#endif
    struct rlimit core = {0, 0};
    Require(setrlimit(RLIMIT_CORE, &core) == 0, "CORE_LIMIT_SET");
    struct sigaction action = {};
    action.sa_handler = timed_out;
    sigemptyset(&action.sa_mask);
    Require(sigaction(SIGALRM, &action, nullptr) == 0, "DEADLINE_SETUP");
#ifndef M4_APFS_SANITIZER_BUILD
    static char signal_stack[65536];
    stack_t stack = {};
    stack.ss_sp = signal_stack;
    stack.ss_size = sizeof(signal_stack);
    Require(sigaltstack(&stack, nullptr) == 0, "SIGNAL_STACK_SETUP");
    action.sa_handler = parser_signal;
    action.sa_flags = SA_ONSTACK;
    const int caught[] = {SIGSEGV, SIGBUS, SIGABRT, SIGXFSZ, SIGTERM, SIGINT};
    for (unsigned n = 0; n < sizeof(caught) / sizeof(caught[0]); ++n)
        Require(sigaction(caught[n], &action, nullptr) == 0, "SIGNAL_SETUP");
#endif
    alarm(175);
    signal(SIGPIPE, SIG_IGN);
}

class ReadOnlyDevice final : public Device
{
    int fd_ = -1;
    uint64_t size_ = 0;
    uint64_t bytes_ = 0;
public:
    ~ReadOnlyDevice() override { Close(); }
    bool Open(const char *) override { throw std::runtime_error("DEVICE_REOPEN_DISABLED"); }
    void open_input(const char *input, bool image)
    {
#ifdef M4_APFS_TEST_IMAGES
        if (image) fd_ = open(input, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        else
#endif
        {
            char *end = nullptr;
            errno = 0;
            const long supplied = std::strtol(input, &end, 10);
            Require(!errno && input[0] >= '0' && input[0] <= '9' && end && !*end &&
                    supplied >= 3 && supplied <= INT_MAX, "INPUT_DESCRIPTOR");
            fd_ = fcntl(int(supplied), F_DUPFD_CLOEXEC, 3);
        }
        Require(fd_ >= 0, "INPUT_OPEN");
        Require((fcntl(fd_, F_GETFL) & O_ACCMODE) == O_RDONLY, "INPUT_NOT_READ_ONLY");
        struct stat info;
        Require(fstat(fd_, &info) == 0, "INPUT_STAT");
        Require(S_ISBLK(info.st_mode) || (image && S_ISREG(info.st_mode)), "INPUT_NOT_BLOCK");
        Require(!image || S_ISREG(info.st_mode), "IMAGE_REQUIRES_REGULAR_FILE");
        if (S_ISBLK(info.st_mode)) {
            Require(ioctl(fd_, BLKGETSIZE64, &size_) == 0, "INPUT_SIZE_IOCTL");
        } else {
            Require(info.st_size > 0, "IMAGE_EMPTY");
            size_ = info.st_size;
        }
        Require(size_ >= 4096 && size_ <= uint64_t(INT64_MAX), "INPUT_SIZE");
    }
    void Close() override { if (fd_ >= 0) { close(fd_); fd_ = -1; } }
    uint64_t GetSize() const override { return size_; }
    bool Read(void *data, uint64_t offset, uint64_t length) override
    {
        M4Apfs::Step();
        Require(length > 0 && length <= M4Apfs::MaxBytes, "INPUT_READ_LIMIT");
        Require(offset <= size_ && length <= size_ - offset, "INPUT_READ_RANGE");
        const uint64_t budget = 4ULL * 1024 * 1024 * 1024;
        Require(length <= budget - bytes_, "INPUT_READ_BUDGET");
        bytes_ += length;
        const ssize_t got = pread(fd_, data, size_t(length), off_t(offset));
        Require(got >= 0, "INPUT_READ_ERROR");
        Require(uint64_t(got) == length, "SHORT_READ");
        return true;
    }
};

static uint64_t number(const char *text, uint64_t maximum)
{
    Require(text[0] && std::strlen(text) <= 20, "ARGUMENT_NUMBER");
    uint64_t result = 0;
    for (size_t n = 0; text[n]; ++n) {
        Require(text[n] >= '0' && text[n] <= '9', "ARGUMENT_NUMBER");
        unsigned digit = text[n] - '0';
        Require(digit <= maximum && result <= (maximum - digit) / 10, "ARGUMENT_NUMBER_RANGE");
        result = result * 10 + digit;
    }
    return result;
}

static std::string quoted(const uint8_t *text, size_t size)
{
    Require(size <= 4096, "METADATA_STRING_LIMIT");
    std::ostringstream out;
    out << '"';
    for (size_t n = 0; n < size && text[n]; ++n) {
        const unsigned c = text[n];
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c >= 32 && c < 127) out << char(c);
        else out << "\\u00" << std::hex << std::setfill('0') << std::setw(2) << c;
    }
    out << '"';
    return out.str();
}

static bool plaintext(const apfs_superblock_t &sb)
{
    return (sb.apfs_fs_flags & 3) == APFS_FS_UNENCRYPTED;
}

static void compatible(const apfs_superblock_t &sb)
{
    Require(plaintext(sb), "ENCRYPTED_VOLUME");
    Require(!(sb.apfs_incompatible_features & ~APFS_SUPPORTED_INCOMPAT_MASK) &&
            !(sb.apfs_incompatible_features & APFS_INCOMPAT_RESERVED_40), "VOLUME_UNKNOWN_INCOMPAT");
    Require(!(sb.apfs_incompatible_features &
            (APFS_INCOMPAT_ENC_ROLLED | APFS_INCOMPAT_INCOMPLETE_RESTORE)), "VOLUME_INCOMPAT_REFUSED");
}

static void snapshots(ApfsContainer &container, const apfs_superblock_t &sb, unsigned index)
{
    compatible(sb);
    if (!sb.apfs_num_snapshots) return;
    BTree tree(container);
    Require(tree.Init(sb.apfs_snap_meta_tree_oid, sb.apfs_o.o_xid), "SNAPSHOT_TREE_INIT");
    BTreeIterator iterator;
    Require(tree.GetIteratorBegin(iterator), "SNAPSHOT_ITERATOR");
    unsigned count = 0;
    bool ended = false;
    // Metadata and name entries share this tree. Limit both scans and results.
    for (unsigned n = 0; n < 256; ++n) {
        BTreeEntry entry;
        Require(iterator.GetEntry(entry), "SNAPSHOT_ENTRY");
        Require(entry.key_len >= sizeof(j_key_t), "SNAPSHOT_KEY_SIZE");
        const uint64_t key = reinterpret_cast<const j_key_t *>(entry.key)->obj_id_and_type;
        if ((key >> OBJ_TYPE_SHIFT) == APFS_TYPE_SNAP_METADATA) {
            Require(++count <= 64 && entry.val && entry.val_len >= sizeof(j_snap_metadata_val_t), "SNAPSHOT_METADATA_SIZE");
            const auto *value = reinterpret_cast<const j_snap_metadata_val_t *>(entry.val);
            Require(value->name_len > 0 && value->name_len <= 1024 &&
                    value->name_len <= entry.val_len - sizeof(*value), "SNAPSHOT_NAME_SIZE");
            Require(value->name[value->name_len - 1] == 0, "SNAPSHOT_NAME_TERMINATOR");
            const std::string name = quoted(value->name, value->name_len);
            std::printf("{\"type\":\"snapshot\",\"volume\":%u,\"xid\":%llu,\"name\":%s}\n",
                        index, static_cast<unsigned long long>(key & OBJ_ID_MASK), name.c_str());
            Require(count < 64 || sb.apfs_num_snapshots <= 64, "SNAPSHOT_COUNT_LIMIT");
        }
        if (!iterator.next()) { ended = true; break; }
    }
    Require(ended, "SNAPSHOT_SCAN_LIMIT");
    Require(count == sb.apfs_num_snapshots, "SNAPSHOT_COUNT_MISMATCH");
}

static int inspect(ApfsContainer &container)
{
    const auto &nx = container.Superblock();
    std::printf("{\"type\":\"container\",\"checkpoint_xid\":%llu,\"block_size\":%u,\"incompat\":%llu}\n",
                static_cast<unsigned long long>(nx.nx_o.o_xid), container.GetBlocksize(),
                static_cast<unsigned long long>(nx.nx_incompatible_features));
    for (unsigned n = 0; n < NX_MAX_FILE_SYSTEMS; ++n) {
        if (!nx.nx_fs_oid[n]) continue;
        try {
            apfs_superblock_t sb;
            Require(container.GetVolumeInfo(n, sb), "VOLUME_INFO_OMAP_OR_READ");
            const std::string name = quoted(sb.apfs_volname, APFS_VOLNAME_LEN);
            std::printf("{\"type\":\"volume\",\"index\":%u,\"name\":%s,\"role\":%u,"
                        "\"fs_flags\":%llu,\"encrypted\":%s,\"meta_crypto_flags\":%u,"
                        "\"meta_crypto_class\":%u,\"features\":%llu,\"ro_compat\":%llu,"
                        "\"incompat\":%llu,\"unknown_incompat\":%llu,\"sealed\":%s,\"snapshot_count\":%llu}\n",
                        n, name.c_str(), unsigned(sb.apfs_role),
                        static_cast<unsigned long long>(sb.apfs_fs_flags), plaintext(sb) ? "false" : "true",
                        unsigned(sb.apfs_meta_crypto.cpflags), unsigned(sb.apfs_meta_crypto.persistent_class),
                        static_cast<unsigned long long>(sb.apfs_features),
                        static_cast<unsigned long long>(sb.apfs_readonly_compatible_features),
                        static_cast<unsigned long long>(sb.apfs_incompatible_features),
                        static_cast<unsigned long long>(sb.apfs_incompatible_features &
                            (~APFS_SUPPORTED_INCOMPAT_MASK | APFS_INCOMPAT_RESERVED_40)),
                        (sb.apfs_incompatible_features & APFS_INCOMPAT_SEALED_VOLUME) ? "true" : "false",
                        static_cast<unsigned long long>(sb.apfs_num_snapshots));
            // Read no filesystem trees here, including those of plaintext Data volumes.
            if (sb.apfs_role == APFS_VOL_ROLE_SYSTEM) {
                if (!plaintext(sb)) {
                    std::fprintf(stderr, "NOTE ENCRYPTED_SYSTEM_SKIPPED volume=%u\n", n);
                } else snapshots(container, sb, n);
            }
        } catch (const std::exception &error) {
            const std::string reason = refusal_reason(error.what());
            std::fprintf(stderr, "REFUSAL %s volume=%u\n", reason.c_str(), n);
            // This volume contributes no views; other volumes remain usable.
            std::printf("{\"type\":\"refusal\",\"volume\":%u,\"reason\":\"%s\"}\n",
                        n, reason.c_str());
        }
    }
    Require(std::fflush(stdout) == 0 && !std::ferror(stdout), "OUTPUT_WRITE");
    return 0;
}

static int cat(ApfsContainer &container, unsigned index, uint64_t xid, const char *path, bool image)
{
    Require(container.Superblock().nx_fs_oid[index] != 0, "VOLUME_INDEX_MISSING");
    apfs_superblock_t sb;
    Require(container.GetVolumeInfo(index, sb), "VOLUME_INFO_OMAP_OR_READ");
    compatible(sb);
    Require(image || sb.apfs_role == APFS_VOL_ROLE_SYSTEM, "NON_SYSTEM_VOLUME");
    Require(image || std::strcmp(path, "/usr/sbin/appleh16camerad") == 0, "PATH_REFUSED");
    Require(xid <= container.Superblock().nx_o.o_xid, "SNAPSHOT_FUTURE_XID");
    Require(std::strlen(path) <= 4096 && path[0] == '/', "PATH_LIMIT_OR_NOT_ABSOLUTE");
    std::unique_ptr<ApfsVolume> volume(container.GetVolume(index, "", xid));
    Require(bool(volume), xid ? "SNAPSHOT_OMAP_OR_TREE_LOOKUP" : "VOLUME_OMAP_OR_TREE_LOOKUP");
    ApfsDir directory(*volume);
    uint64_t inode = 2;
    ApfsDir::Inode info;
    Require(directory.GetInode(info, inode) && S_ISDIR(info.mode), "ROOT_INODE");
    std::istringstream components(path);
    std::string part;
    unsigned count = 0;
    while (std::getline(components, part, '/')) {
        Require(++count <= 128, "PATH_COMPONENT_LIMIT");
        if (part.empty()) continue;
        Require(part != "." && part != ".." && part.size() <= 255, "PATH_COMPONENT_REFUSED");
        Require(S_ISDIR(info.mode), "NON_DIRECTORY_COMPONENT");
        ApfsDir::DirRec entry;
        // ApfsLib does not distinguish an absent name from failed tree lookup.
        Require(directory.LookupName(entry, inode, part.c_str()), "PATH_LOOKUP_ABSENT_OR_INVALID");
        inode = entry.file_id;
        Require(directory.GetInode(info, inode), "INODE_LOOKUP");
        Require(!S_ISLNK(info.mode), "SYMLINK");
    }
    Require(S_ISREG(info.mode), "NON_REGULAR_FILE");
    Require(!info.ds_default_crypto_id, "ENCRYPTED_FILE_STREAM");
    const bool compressed = info.bsd_flags & 0x20;
    uint64_t logical_size = info.ds_size;
    uint32_t method = 0;
    std::vector<uint8_t> attribute;
    if (compressed) {
        Require(directory.GetAttribute(attribute, inode, "com.apple.decmpfs"), "DECMPFS_ATTRIBUTE");
        Require(attribute.size() >= sizeof(CompressionHeader), "DECMPFS_HEADER_SIZE");
        const auto *header = reinterpret_cast<const CompressionHeader *>(attribute.data());
        method = header->algo;
        logical_size = header->size;
    }
    std::fprintf(stderr, "file inode=%llu mode=0%o bsd_flags=0x%x compressed=%s decmpfs=%u logical_size=%llu\n",
                 static_cast<unsigned long long>(inode), unsigned(info.mode), info.bsd_flags,
                 compressed ? "yes" : "no", method, static_cast<unsigned long long>(logical_size));
    Require(logical_size <= M4Apfs::MaxBytes, "FILE_SIZE_LIMIT");
    std::vector<uint8_t> data;
    if (compressed) Require(DecompressFile(directory, inode, data, attribute), "DECOMPRESSION");
    else {
        Require(info.optional_present_flags & ApfsDir::Inode::INO_HAS_DSTREAM || !logical_size,
                "FILE_DATA_STREAM_MISSING");
        data.resize(logical_size);
        Require(directory.ReadFile(data.data(), info.private_id, 0, data.size()), "FILE_BLOCK_READ");
    }
    Require(data.size() == logical_size, "FILE_LENGTH_MISMATCH");
    Require(std::fwrite(data.data(), 1, data.size(), stdout) == data.size() &&
            std::fflush(stdout) == 0 && !std::ferror(stdout), "OUTPUT_WRITE");
    return 0;
}

int main(int argc, char **argv)
{
    // All library diagnostic streams go to stderr. Only our JSON / file bytes use stdout.
    std::cout.rdbuf(std::cerr.rdbuf());
    g_debug = 0;
    g_lax = false;
    try {
        limits();
        bool image = false;
#ifdef M4_APFS_TEST_IMAGES
        image = argc > 1 && std::strcmp(argv[1], "--image") == 0;
#endif
        const int first = image ? 2 : 1;
        Require(argc > first + 1, "USAGE: m4-apfs-reader inspect FD | cat FD INDEX XID /usr/sbin/appleh16camerad");
        const std::string mode(argv[first]);
        Require((mode == "inspect" && argc == first + 2) || (mode == "cat" && argc == first + 5), "USAGE");
        ReadOnlyDevice device;
        device.open_input(argv[first + 1], image);
        ApfsContainer container(&device, 0, device.GetSize());
        Require(container.Init(), "CONTAINER_INIT");
        if (mode == "inspect") return inspect(container);
        const unsigned index = unsigned(number(argv[first + 2], NX_MAX_FILE_SYSTEMS - 1));
        const uint64_t xid = number(argv[first + 3], UINT64_MAX);
        return cat(container, index, xid, argv[first + 4], image);
    } catch (const std::bad_alloc &) {
        std::fprintf(stderr, "REFUSAL ALLOCATION_LIMIT\n");
        return 22;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "REFUSAL %s\n", error.what());
        return 21;
    }
}
