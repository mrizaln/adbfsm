#pragma once

#include "madbfs/connection/connection.hpp"
#include "madbfs/data/cache.hpp"
#include "madbfs/data/stat.hpp"
#include "madbfs/path/path.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <atomic>
#include <functional>

namespace madbfs::tree
{
    class Node;
    class RegularFile;
    class Directory;
    class Link;
    struct Other;
    struct Error;

    using File = Var<RegularFile, Directory, Link, Other, Error>;

    // TODO: add open flags, use them to determine if file has suitable read/write permission
    class RegularFile
    {
    public:
        friend Node;

        RegularFile() = default;

        RegularFile(RegularFile&& other)
            : m_open_fds{ std::move(other.m_open_fds) }
            , m_dirty{ other.m_dirty.load(std::memory_order::acquire) }
        {
        }

        /**
         * @brief Insert `fd` into list of open files.
         *
         * @param fd File descriptor.
         * @param flags Open flags.
         *
         * @return True if the fd has not been inserted before, false otherwise.
         */
        bool open(u64 fd)
        {
            if (is_open(fd)) {
                return false;
            }
            m_open_fds.emplace_back(fd);
            return true;
        }

        /**
         * @brief Remove `fd` from list of open files.
         *
         * @param fd File descriptor.
         *
         * @return True if `fd` actually removed, false otherwise.
         */
        bool close(u64 fd) { return std::erase(m_open_fds, fd) > 0; }

        bool is_open(u64 fd) { return sr::find(m_open_fds, fd) != m_open_fds.end(); }
        bool has_open_fds() const { return not m_open_fds.empty(); }
        bool is_dirty() const { return m_dirty.load(std::memory_order::acquire); }
        void set_dirty(bool val) { m_dirty.store(val, std::memory_order::release); }

    private:
        Vec<u64>          m_open_fds;    // used to track open files
        std::atomic<bool> m_dirty = false;
    };

    class Directory
    {
    public:
        struct NodeHash
        {
            using is_transparent = void;
            using is_avalanching = void;

            u64 operator()(Str str) const { return ankerl::unordered_dense::hash<Str>{}(str); }
            u64 operator()(const Uniq<Node>& node) const;
        };

        struct NodeEq
        {
            using is_transparent = void;
            using is_avalanching = void;

            bool operator()(Str lhs, Str rhs) const { return lhs == rhs; }
            bool operator()(Str lhs, const Uniq<Node>& rhs) const;
            bool operator()(const Uniq<Node>& lhs, Str rhs) const;
            bool operator()(const Uniq<Node>& lhs, const Uniq<Node>& rhs) const;
        };

        // NOTE: use with caution, Node::m_name field must not be modified unless the node is extracted
        using List = ankerl::unordered_dense::set<Uniq<Node>, NodeHash, NodeEq>;

        Directory() = default;

        bool has_readdir() const { return m_has_readdir; }
        void set_readdir() { m_has_readdir = true; }

        /**
         * @brief Check if a node with the given name exists.
         *
         * @param name The name of the node to check.
         */
        Expect<Ref<Node>> find(Str name) const;

        /**
         * @brief Erase a file by its name.
         *
         * @param name The file name.
         * @return True if file deleted, false if the file does not exist.
         */
        bool erase(Str name);

        /**
         * @brief Add a new node.
         *
         * @param node The node to add.
         * @param overwrite If true, overwrite the existing node with the same name.
         *
         * @return A pair containing the added node and overwriten node if overwrite happens.
         *
         * - If insertion happen without overwrite, right will be null.
         * - If insertion happen with overwrite flag set, right will be non-null.
         * - If overwrite happen when overwrite flag not set, error will be returned.
         */
        Expect<Pair<Ref<Node>, Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

        /**
         * @brief Extract a node.
         *
         * @param name The name of the node.
         */
        Expect<Uniq<Node>> extract(Str name);

        const List& children() const { return m_children; }

    private:
        List m_children;
        bool m_has_readdir = false;
    };

    class Link
    {
    public:
        Link(Node* target)
            : m_target{ target }
        {
        }

        /**
         * @brief Get immediate target of the link.
         */
        Node& target() const { return *m_target; }

    private:
        Node* m_target;    // how do I signal node not exist anymore?
    };

    /**
     * @class Other
     * @brief For files other than regular file, directory, or link.
     *
     * Block files, character files, socket file, etc. should use this type.
     */
    struct Other
    {
    };

    /**
     * @class Error
     * @brief For error operations.
     *
     * For optimization purpose this class cache the last failed node operation.
     */
    struct Error
    {
        Errc error;
    };

    class Node
    {
    public:
        struct Context
        {
            connection::Connection& connection;
            data::Cache&            cache;
            const path::Path&       path;    // path for connection
        };

        Node(Str name, Node* parent, data::Stat stat, File value)
            : m_parent{ parent }
            , m_name{ name }
            , m_stat{ std::move(stat) }
            , m_value{ std::move(value) }
        {
        }

        Node(Node&&)                  = delete;
        Node& operator=(Node&& other) = delete;

        Node(const Node&)            = delete;
        Node& operator=(const Node&) = delete;

        data::Id id() const { return m_stat.id; };

        void set_name(Str name) { m_name = name; }
        void set_parent(Node* parent) { m_parent = parent; }
        void set_stat(data::Stat stat) { m_stat = stat; }

        Str         name() const { return m_name; }
        Node*       parent() const { return m_parent; }
        const File& value() const { return m_value; }

        Expect<Ref<const data::Stat>> stat() const;

        /**
         * @brief Get Error ptr value if the variant is an Error.
         *
         * This function is different from `as<Error>` since it's intended for use outside of Node. It will
         * return nullptr if the Node is not an Error instance.
         */
        const Error* as_error() const;

        /**
         * @brief Build path from node.
         */
        path::PathBuf build_path() const;

        /**
         * @brief Update time metadata.
         *
         * @param atime Access time (special values apply).
         * @param mtime Modification time (special values apply)
         */
        void refresh_stat(timespec atime, timespec mtime);

        /**
         * @brief Check whether node is synced with device.
         *
         * For directory this depends on whether user has set the synced flag or not. For example, directory
         * can be created but without never doing any readdir opeartion on it, thus not synced. This one is
         * user's responsibility. For other kinds this value always true since it assume that the stats
         * are synced.
         */
        bool has_synced() const;

        /**
         * @brief Set synced flag.
         *
         * You can set this after readdir operation for example to make sure that there is no need to do any
         * readdir again in the future.
         */
        void set_synced();

        // operations on Directory
        // -----------------------

        /**
         * @brief Traverse into child node.
         *
         * @param name The name of the child node.
         *
         * @return The child node.
         */
        Expect<Ref<Node>> traverse(Str name) const;

        /**
         * @brief List children of this node.
         *
         * @param Function to operate on these children.
         */
        Expect<void> list(std::move_only_function<void(Str)>&& fn) const;

        /**
         * @brief Create a new node without any call to connection or cache with this node as its parent.
         *
         * @param name The name of the new node.
         * @param stat Stat of the new node.
         * @param file The kind of the node.
         *
         * @return The new node.
         *
         * This function is used to build a new node from existing filetree on the device. Instead of
         * operating on the file on the device itself, this function just modify the nodes. This function
         * assume that the build process won't overwrite any node.
         */
        Expect<Ref<Node>> build(Str name, data::Stat stat, File file);

        /**
         * @brief Extract a child node.
         *
         * @param name The name of the node.
         *
         * @return The extracted node.
         *
         * Since it extracts a child node, it only works on Directory.
         */
        Expect<Uniq<Node>> extract(Str name);

        /**
         * @brief Insert a node into another node.
         *
         * @param node The node to add.
         * @param overwrite If true, overwrite the existing node with the same name.
         *
         * @return A pair containing the added node and overwriten node if overwrite happens.
         *
         * Since the insertion is basically adding a child node, it only works on Directory.
         *
         * - If insertion happen without overwrite, left will be non-null whereas right will be null.
         * - If insertion happen with overwrite and flag enabled both will be non-null.
         * - If overwrite happen when flag not enabled, error will be returned.
         */
        Expect<Pair<Ref<Node>, Uniq<Node>>> insert(Uniq<Node> node, bool overwrite);

        /**
         * @brief Create a new child node as Link.
         *
         * @param name The name of the link.
         * @param target The target of the link.
         *
         * @return The new link node.
         */
        Expect<Ref<Node>> symlink(Str name, Node* target);

        /**
         * @brief Create a new child node as RegularFile.
         *
         * @param context Context needed to communicate with device and local.
         * @param mode File mode to use and the type of node to be created.
         *
         * @return The new regular file node.
         */
        AExpect<Ref<Node>> mknod(Context context, mode_t mode, dev_t dev);

        /**
         * @brief Create a new child node as Directory.
         *
         * @param context Context needed to communicate with device and local.
         * @param mode The mode of the new directory.
         *
         * @return The new directory node.
         */
        AExpect<Ref<Node>> mkdir(Context context, mode_t mode);

        /**
         * @brief Remove a child node by its name (RegularFile or Directory).
         *
         * @param context Context needed to communicate with device and local.
         */
        AExpect<void> unlink(Context context);

        /**
         * @brief Remove a child node by its name (Directory).
         *
         * @param context Context needed to communicate with device and local.
         */
        AExpect<void> rmdir(Context context);

        // -----------------------

        // operations on RegularFile
        // -------------------------

        /**
         * @brief Truncate file.
         *
         * @param context Context needed to communicate with device and local.
         * @param size The final size to truncate to.
         */
        AExpect<void> truncate(Context context, off_t size);

        /**
         * @brief Open file.
         *
         * @param context Context needed to communicate with device and local.
         * @param flags Open mode flags.
         *
         * @return File descriptor.
         */
        AExpect<u64> open(Context context, int flags);

        /**
         * @brief Read data from file.
         *
         * @param context Context needed to communicate with device and local.
         * @param fd File descriptor.
         * @param out The output of the data.
         * @param offset Offset to the data to be read.
         *
         * @return The number of bytes read.
         */
        AExpect<usize> read(Context context, u64 fd, Span<char> out, off_t offset);

        /**
         * @brief Write data to file.
         *
         * @param context Context needed to communicate with device and local.
         * @param fd File descriptor.
         * @param in Data to be written.
         * @param offset Offset of the write pointer.
         *
         * @return The number of bytes written.
         */
        AExpect<usize> write(Context context, u64 fd, Str in, off_t offset);

        /**
         * @brief Flush buffer of the file.
         *
         * @param context Context needed to communicate with device and local.
         * @param fd File descriptor.
         *
         * Basically forcing writing to the file itself instead of to the buffer in memory.
         */
        AExpect<void> flush(Context context, u64 fd);

        /**
         * @brief Release file.
         *
         * @param context Context needed to communicate with device and local.
         */
        AExpect<void> release(Context context, u64 fd);

        /**
         * @brief Update the timestamps of a file.
         *
         * @param context Context needed to communicate with device and local.
         */
        AExpect<void> utimens(Context context, timespec atime, timespec mtime);

        // -------------------------

        // operations on Link
        // ------------------

        /**
         * @brief Read a link.
         */
        Expect<Ref<Node>> readlink();

        // ------------------

    private:
        inline static std::atomic<u64> s_id_counter = 0;

        template <typename T>
        bool is() const;

        template <typename T>
        Expect<Ref<T>> as();

        template <typename T>
        Expect<Ref<const T>> as() const;

        Expect<Ref<RegularFile>> regular_file_prelude()
        {
            auto current = this;
            if (is<Link>()) {
                current = &readlink()->get();
            }

            if (auto err = as<Error>(); err.has_value()) {
                return Unexpect{ err->get().error };
            }

            if (current->is<Directory>()) {
                return Unexpect{ Errc::is_a_directory };
            } else if (current->is<Other>()) {
                return Unexpect{ Errc::permission_denied };
            }

            return current->as<RegularFile>();
        }

        Node*      m_parent = nullptr;
        String     m_name   = {};
        data::Stat m_stat   = {};
        File       m_value;
    };
}

// -----------------------
// template implementation
// -----------------------

namespace madbfs::tree
{
    template <typename T>
    bool Node::is() const
    {
        return std::holds_alternative<T>(m_value);
    }

    template <typename T>
    Expect<Ref<T>> Node::as()
    {
        constexpr auto errc = [&] {
            if constexpr (std::same_as<Directory, T>) {
                return Errc::not_a_directory;
            } else {
                return Errc::invalid_argument;
            }
        }();
        if (auto* val = std::get_if<T>(&m_value)) {
            return *val;
        }
        return Unexpect{ errc };
    }

    template <typename T>
    Expect<Ref<const T>> Node::as() const
    {
        constexpr auto errc = [] {
            if constexpr (std::same_as<Directory, T>) {
                return Errc::not_a_directory;
            } else {
                return Errc::invalid_argument;
            }
        }();
        if (auto* val = std::get_if<T>(&m_value)) {
            return *val;
        }
        return Unexpect{ errc };
    }
}
