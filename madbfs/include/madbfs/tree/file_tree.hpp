#pragma once

#include "madbfs/connection/connection.hpp"
#include "madbfs/path/path.hpp"
#include "madbfs/tree/node.hpp"

#include <functional>

namespace madbfs::tree
{
    /**
     * @class FileTree
     * @brief A class representing a file tree structure.
     *
     * This data structure is a Trie
     */
    class FileTree
    {
    public:
        using Filler = std::move_only_function<void(const char* name)>;

        FileTree(connection::Connection& connection, data::Cache& cache);
        ~FileTree() = default;

        FileTree(Node&& root)            = delete;
        FileTree& operator=(Node&& root) = delete;

        FileTree(const Node& root)            = delete;
        FileTree& operator=(const Node& root) = delete;

        /**
         * @brief Get a node by the given path.
         *
         * @param path The path to the node.
         */
        Expect<Ref<Node>> traverse(path::Path path);

        AExpect<void>                  readdir(path::Path path, Filler filler);
        AExpect<Ref<const data::Stat>> getattr(path::Path path);

        AExpect<Ref<Node>> readlink(path::Path path);
        AExpect<Ref<Node>> mknod(path::Path path, mode_t mode, dev_t dev);
        AExpect<Ref<Node>> mkdir(path::Path path, mode_t mode);
        AExpect<void>      unlink(path::Path path);
        AExpect<void>      rmdir(path::Path path);
        AExpect<void>      rename(path::Path from, path::Path to, u32 flags);

        AExpect<void>  truncate(path::Path path, off_t size);
        AExpect<u64>   open(path::Path path, int flags);
        AExpect<usize> read(path::Path path, u64 fd, Span<char> out, off_t offset);
        AExpect<usize> write(path::Path path, u64 fd, Str in, off_t offset);
        AExpect<void>  flush(path::Path path, u64 fd);
        AExpect<void>  release(path::Path path, u64 fd);
        AExpect<void>  utimens(path::Path path, timespec atime, timespec mtime);

        AExpect<usize> copy_file_range(
            path::Path in_path,
            u64        in_fd,
            off_t      in_off,
            path::Path path_out,
            u64        out_fd,
            off_t      offset_out,
            size_t     size
        );

        // this function only used to link already existing files, user can't use it
        Expect<void> symlink(path::Path path, path::Path target);

        const Node& root() const { return m_root; }

    private:
        /**
         * @brief Traverse the node or build a new node.
         *
         * @param path Path to the node.
         */
        AExpect<Ref<Node>> traverse_or_build(path::Path path);

        Node::Context make_context(const path::Path& path)
        {
            return {
                .connection = m_connection,
                .cache      = m_cache,
                .path       = path,
            };
        }

        Node                    m_root;
        connection::Connection& m_connection;
        data::Cache&            m_cache;
        bool                    m_root_initialized = false;
    };
}
