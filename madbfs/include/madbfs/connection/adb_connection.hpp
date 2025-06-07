#pragma once

#include "madbfs/connection/connection.hpp"

namespace madbfs::connection
{
    class AdbConnection final : public Connection
    {
    public:
        AdbConnection() = default;

        AExpect<Gen<ParsedStat>> statdir(path::Path path) override;
        AExpect<data::Stat>      stat(path::Path path) override;
        AExpect<path::PathBuf>   readlink(path::Path path) override;

        AExpect<void> mknod(path::Path path, mode_t mode, dev_t dev) override;
        AExpect<void> mkdir(path::Path path, mode_t mode) override;
        AExpect<void> unlink(path::Path path) override;
        AExpect<void> rmdir(path::Path path) override;
        AExpect<void> rename(path::Path from, path::Path to, u32 flags) override;

        AExpect<u64>  open(path::Path path, int flags) override;
        AExpect<void> close(u64 fd) override;

        AExpect<void>  truncate(path::Path path, off_t size) override;
        AExpect<usize> read(path::Path path, Span<char> out, off_t offset) override;
        AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) override;
        AExpect<void>  utimens(path::Path path, timespec atime, timespec mtime) override;

        AExpect<usize> copy_file_range(path::Path in, off_t in_off, path::Path out, off_t out_off, usize size)
            override;

    private:
        std::atomic<u64> m_fd_counter = 0;
    };
}
