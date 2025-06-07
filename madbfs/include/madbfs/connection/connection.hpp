#pragma once

#include "madbfs/data/stat.hpp"

#include <madbfs-common/async/async.hpp>

namespace madbfs::path
{
    class Path;
    class PathBuf;
}

namespace madbfs::connection
{
    struct ParsedStat
    {
        data::Stat stat;
        Str        path;
    };

    class Connection
    {
    public:
        // directory operations
        // --------------------

        /**
         * @brief List
         *
         * @param path Path to a directory.
         *
         * @return A generator if successful, or an error if it fails.
         */
        virtual AExpect<Gen<ParsedStat>> statdir(path::Path path) = 0;

        /**
         * @brief Get the stat of a file or directory.
         *
         * @param path The path to the file or directory.
         *
         * @return The stat of the file or directory.
         */
        virtual AExpect<data::Stat> stat(path::Path path) = 0;

        /**
         * @brief Get the real file pointed by a symlink.
         *
         * @param path The path to the file or directory.
         *
         * @return Target of the link.
         */
        virtual AExpect<path::PathBuf> readlink(path::Path path) = 0;

        /**
         * @brief Create a new empty file.
         *
         * @param path Path of the new file.
         */
        virtual AExpect<void> mknod(path::Path path, mode_t mode, dev_t dev) = 0;

        /**
         * @brief Make a directory on the device.
         *
         * @param path Path to the directory.
         */
        virtual AExpect<void> mkdir(path::Path path, mode_t mode) = 0;

        /**
         * @brief Remove a file on the device.
         *
         * @param path Path to the file on the device.
         * @param bool Whether to remove recursively or not.
         */
        virtual AExpect<void> unlink(path::Path path) = 0;

        /**
         * @brief Remove a directory on the device.
         *
         * @param path Path to the directory on the device.
         */
        virtual AExpect<void> rmdir(path::Path path) = 0;

        /**
         * @brief Move a file on the device.
         *
         * @param from Target file.
         * @param to Destination file.
         */
        virtual AExpect<void> rename(path::Path from, path::Path to, u32 flags) = 0;

        // --------------------

        // file operations
        // ---------------

        /**
         * @brief Open a file on the device.
         *
         * @param path Path to the file on the device.
         * @param flags Open flags.
         *
         * @return File descriptor.
         */
        virtual AExpect<u64> open(path::Path path, int flags) = 0;

        /**
         * @brief Close a file on the device.
         *
         * @param fd File descriptor to the file.
         */
        virtual AExpect<void> close(u64 fd) = 0;

        /**
         * @brief Truncate a file on the device.
         *
         * @param path Path to the file on the device.
         * @param size Size to truncate to.
         */
        virtual AExpect<void> truncate(path::Path path, off_t size) = 0;

        /**
         * @brief Read from a file on the device.
         *
         * @param path Path to the file on the device.
         * @param out Buffer to read into.
         * @param offset Offset to read from.
         *
         * @return Number of bytes read.
         */
        virtual AExpect<usize> read(path::Path path, Span<char> out, off_t offset) = 0;

        /**
         * @brief Write to a file on the device.
         *
         * @param path Path to the file on the device.
         * @param in Buffer to write from.
         * @param offset Offset to write to.
         *
         * @return Number of bytes written.
         */
        virtual AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) = 0;

        /**
         * @brief Update change time and modification time of a file
         *
         * @param path Path to the file on the device.
         * @param atime Access time.
         * @param mtime Modification time.
         */
        virtual AExpect<void> utimens(path::Path path, timespec atime, timespec mtime) = 0;

        /**
         * @brief Copy file server-side.
         *
         * @param in Input file path.
         * @param in_off Input offset.
         * @param out Output file path.
         * @param out_off Output offset.
         * @param size Number of bytes to be copied.
         *
         * @return Number of bytes copied.
         */
        virtual AExpect<usize> copy_file_range(
            path::Path in,
            off_t      in_off,
            path::Path out,
            off_t      out_off,
            usize      size
        ) = 0;

        // ---------------

        virtual ~Connection() = default;
    };

    enum class AdbError
    {
        Unknown,
        NoDev,
        PermDenied,
        NoSuchFileOrDir,
        NotADir,
        Inaccessible,
        ReadOnly,
        TryAgain,
    };

    enum class DeviceStatus
    {
        Device,
        Offline,
        Unauthorized,
        Unknown,
    };

    struct Device
    {
        String       serial;
        DeviceStatus status;
    };

    /**
     * @brief Get human readable description of DeviceStatus.
     */
    Str to_string(DeviceStatus status);

    /**
     * @brief Execute a command.
     *
     * @param exe Executable name.
     * @param args Arguments to the command.
     * @param in Input data to be piped as stdin.
     * @param check Check return value.
     * @param merge_err Append stderr to stdout.
     */
    madbfs::Await<madbfs::Expect<madbfs::String>> exec_async(
        madbfs::Str                     exe,
        madbfs::Span<const madbfs::Str> args,
        madbfs::Str                     in,
        bool                            check,
        bool                            merge_err = false
    );

    /**
     * @brief Start connection with the devices.
     */
    AExpect<void> start_connection();

    /**
     * @brief List connected devices.
     */
    AExpect<Vec<Device>> list_devices();
}
