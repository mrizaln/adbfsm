#include "madbfs/tree/node.hpp"

#include <madbfs-common/util/overload.hpp>

namespace madbfs::tree
{
    u64 Directory::NodeHash::operator()(const Uniq<Node>& node) const
    {
        return (*this)(node->name());
    }

    bool Directory::NodeEq::operator()(Str lhs, const Uniq<Node>& rhs) const
    {
        return lhs == rhs->name();
    }

    bool Directory::NodeEq::operator()(const Uniq<Node>& lhs, Str rhs) const
    {
        return lhs->name() == rhs;
    }

    bool Directory::NodeEq::operator()(const Uniq<Node>& lhs, const Uniq<Node>& rhs) const
    {
        return lhs->name() == rhs->name();
    }

    Expect<Ref<Node>> Directory::find(Str name) const
    {
        auto found = m_children.find(name);
        if (found == m_children.end()) {
            return Unexpect{ Errc::no_such_file_or_directory };
        }
        return *found->get();
    }

    bool Directory::erase(Str name)
    {
        return m_children.erase(name) > 0;
    }

    Expect<Pair<Ref<Node>, Uniq<Node>>> Directory::insert(Uniq<Node> node, bool overwrite)
    {
        auto found = m_children.find(node->name());
        if (found != m_children.end() and not overwrite) {
            return Unexpect{ Errc::file_exists };
        }

        auto released = Uniq<Node>{};
        if (found != m_children.end()) {
            released = m_children.extract(found);
        }

        auto [back, _] = m_children.emplace(std::move(node));
        return Pair{ std::ref(*back->get()), std::move(released) };
    }

    Expect<Uniq<Node>> Directory::extract(Str name)
    {
        auto found = m_children.find(name);
        if (found == m_children.end()) {
            return Unexpect{ Errc::no_such_file_or_directory };
        }
        return m_children.extract(found);
    }
}

namespace madbfs::tree
{
    Expect<Ref<const data::Stat>> Node::stat() const
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return m_stat;
    }

    const Error* Node::as_error() const
    {
        auto err = as<Error>();
        return err ? &err->get() : nullptr;
    }

    path::PathBuf Node::build_path() const
    {
        auto path = m_name | sv::reverse | sr::to<String>();
        auto iter = std::back_inserter(path);

        for (auto current = m_parent; current != nullptr; current = current->m_parent) {
            *iter = '/';
            sr::copy(current->m_name | sv::reverse, iter);
        }

        // if the last path is root, we need to remove the last /
        if (path.size() > 2 and path[path.size() - 1] == '/' and path[path.size() - 2] == '/') {
            path.pop_back();
        }
        sr::reverse(path);

        return path::create_buf(std::move(path)).value();
    }

    void Node::refresh_stat(timespec atime, timespec mtime)
    {
        auto now  = Clock::now().time_since_epoch();
        auto sec  = std::chrono::duration_cast<std::chrono::seconds>(now);
        auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now - sec);

        auto time = timespec{ .tv_sec = sec.count(), .tv_nsec = nsec.count() };

        if (atime.tv_nsec != UTIME_OMIT) {
            m_stat.atime = atime.tv_nsec == UTIME_NOW ? time : atime;
        }
        if (mtime.tv_nsec != UTIME_OMIT) {
            m_stat.mtime = mtime.tv_nsec == UTIME_NOW ? time : mtime;
        }
        m_stat.ctime = time;
    }

    bool Node::has_synced() const
    {
        // should have locked but bool is atomic anyway in x64
        auto visit = util::Overload{
            [](const Directory& dir) { return dir.has_readdir(); },
            [](const auto&) { return true; },
        };
        return std::visit(visit, m_value);
    }

    void Node::set_synced()
    {
        // should have locked but bool is atomic anyway in x64
        std::ignore = as<Directory>().transform(&Directory::set_readdir);
    }

    Expect<Ref<Node>> Node::traverse(Str name) const
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then(proj(&Directory::find, name));
    }

    Expect<void> Node::list(std::move_only_function<void(Str)>&& fn) const
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().transform([&](const Directory& dir) {
            for (const auto& node : dir.children()) {
                if (not node->is<Error>()) {
                    fn(node->name());
                }
            }
        });
    }

    Expect<Ref<Node>> Node::build(Str name, data::Stat stat, File file)
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>()
            .and_then(proj(
                &Directory::insert,
                std::make_unique<Node>(name, this, std::move(stat), std::move(file)),
                false
            ))
            .transform([](auto&& pair) { return pair.first; });
    }

    Expect<Uniq<Node>> Node::extract(Str name)
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then(proj(&Directory::extract, name));
    }

    Expect<Pair<Ref<Node>, Uniq<Node>>> Node::insert(Uniq<Node> node, bool overwrite)
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then(proj(&Directory::insert, std::move(node), overwrite));
    }

    Expect<Ref<Node>> Node::symlink(Str name, Node* target)
    {
        if (auto err = as<Error>(); err.has_value()) {
            return Unexpect{ err->get().error };
        }
        return as<Directory>().and_then([&](Directory& dir) -> Expect<Ref<Node>> {
            if (dir.find(name).has_value()) {
                return Unexpect{ Errc::file_exists };
            }

            // NOTE: we can't really make a symlink on android from adb (unless rooted device iirc), so this
            // operation actually not creating any link on the adb device, just on the in-memory filetree.

            auto now  = Clock::now().time_since_epoch();
            auto sec  = std::chrono::duration_cast<std::chrono::seconds>(now);
            auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now - sec);

            auto time = timespec{ .tv_sec = sec.count(), .tv_nsec = nsec.count() };

            // dummy stat for symlink based on
            // lrw-r--r--  root root 21 2024-10-05 09:19:29.000000000 +0700 /sdcard -> /storage/self/primary
            auto dummy_stat = data::Stat{
                .links = 1,
                .size  = 21,
                .mtime = time,
                .atime = time,
                .ctime = time,
                .mode  = S_IFLNK | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH,    // mode: "lrw-r--r--"
                .uid   = 0,
                .gid   = 0,
            };

            auto node = std::make_unique<Node>(name, this, std::move(dummy_stat), Link{ target });
            return dir.insert(std::move(node), false).transform([&](auto&& pair) { return pair.first; });
        });
    }

    AExpect<Ref<Node>> Node::mknod(Context context, mode_t mode, dev_t dev)
    {
        if (auto err = as<Error>(); err.has_value()) {
            co_return Unexpect{ err->get().error };
        }

        auto may_dir = as<Directory>();
        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir  = may_dir->get();
        auto  name = context.path.filename();

        auto overwrite = false;
        if (auto node = dir.find(name); node.has_value()) {
            if (not node->get().is<Error>()) {
                co_return Unexpect{ Errc::file_exists };
            }
            overwrite = true;
        }

        if (auto created = co_await context.connection.mknod(context.path, mode, dev); not created) {
            co_return Unexpect{ created.error() };
        }

        co_return (co_await context.connection.stat(context.path))
            .and_then([&](data::Stat stat) {
                auto node = std::make_unique<Node>(name, this, std::move(stat), RegularFile{});
                return dir.insert(std::move(node), overwrite);
            })
            .transform([&](auto&& pair) { return pair.first; });
    }

    AExpect<Ref<Node>> Node::mkdir(Context context, mode_t mode)
    {
        if (auto err = as<Error>(); err.has_value()) {
            co_return Unexpect{ err->get().error };
        }

        auto may_dir = as<Directory>();
        if (not may_dir) {
            co_return Unexpect{ may_dir.error() };
        }

        auto& dir  = may_dir->get();
        auto  name = context.path.filename();

        auto overwrite = false;
        if (auto node = dir.find(name); node.has_value()) {
            if (not node->get().is<Error>()) {
                co_return Unexpect{ Errc::file_exists };
            }
            overwrite = true;
        }

        auto may_mkdir = co_await context.connection.mkdir(context.path, mode);
        if (not may_mkdir) {
            co_return Unexpect{ may_mkdir.error() };
        }

        co_return (co_await context.connection.stat(context.path))
            .and_then([&](data::Stat stat) {
                auto node = std::make_unique<Node>(name, this, std::move(stat), Directory{});
                return dir.insert(std::move(node), overwrite);
            })
            .transform([&](auto&& pair) { return pair.first; });
    }

    AExpect<void> Node::unlink(Context context)
    {
        if (auto err = as<Error>(); err.has_value()) {
            co_return Unexpect{ err->get().error };
        }

        auto res = as<Directory>().and_then([&](Directory& dir) -> Expect<void> {
            auto name = context.path.filename();
            return dir.find(name).and_then([&](Node& node) -> Expect<void> {
                if (node.is<Directory>()) {
                    return Unexpect{ Errc::is_a_directory };
                }
                auto success = dir.erase(name);
                assert(success);
                return {};
            });
        });

        if (not res) {
            co_return Unexpect{ res.error() };
        }
        co_return co_await context.connection.unlink(context.path);
    }

    AExpect<void> Node::rmdir(Context context)
    {
        if (auto err = as<Error>(); err.has_value()) {
            co_return Unexpect{ err->get().error };
        }

        auto name = context.path.filename();

        auto res = as<Directory>().and_then([&](Directory& dir) {
            return dir.find(name).and_then([&](Node& node) {
                return node.as<Directory>().and_then([&](Directory& target) -> Expect<Ref<Directory>> {
                    if (not target.children().empty()) {
                        for (const auto& child : target.children()) {
                            if (not child->is<Error>()) {
                                Unexpect{ Errc::directory_not_empty };
                            }
                        }
                    }
                    return Expect<Ref<Directory>>{ dir };
                });
            });
        });

        if (not res) {
            co_return Unexpect{ res.error() };
        }

        co_return (co_await context.connection.rmdir(context.path)).transform([&] {
            res->get().erase(name);
        });
    }

    AExpect<void> Node::truncate(Context context, off_t size)
    {
        if (auto may_file = regular_file_prelude(); not may_file) {
            co_return Unexpect{ may_file.error() };
        }
        co_return (co_await context.connection.truncate(context.path, size)).transform([&] {
            m_stat.size = size;
            refresh_stat({ .tv_sec = 0, .tv_nsec = UTIME_OMIT }, { .tv_sec = 0, .tv_nsec = UTIME_NOW });
        });
    }

    AExpect<u64> Node::open(Context context, int flags)
    {
        auto may_file = regular_file_prelude();
        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }
        auto& file = may_file->get();

        co_return (co_await context.connection.open(context.path, flags)).transform([&](u64 fd) {
            auto res = file.open(fd);
            assert(res);
            return fd;
        });
    }

    AExpect<usize> Node::read(Context context, u64 fd, Span<char> out, off_t offset)
    {
        auto may_file = regular_file_prelude();
        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }
        auto& file = may_file->get();

        if (not file.is_open(fd)) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        co_return (co_await context.cache.read(id(), context.path, out, offset)).transform([&](usize ret) {
            refresh_stat({ .tv_sec = 0, .tv_nsec = UTIME_NOW }, { .tv_sec = 0, .tv_nsec = UTIME_OMIT });
            return ret;
        });
    }

    AExpect<usize> Node::write(Context context, u64 fd, Str in, off_t offset)
    {
        auto may_file = regular_file_prelude();
        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }
        auto& file = may_file->get();

        if (not file.is_open(fd)) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        file.set_dirty(true);
        co_return (co_await context.cache.write(id(), context.path, in, offset)).transform([&](usize ret) {
            // the file size is defined as offset + size from last write if it's higher than previous size
            // NOTE: this may be different for sparse files but I don't think Android has it
            auto new_size = offset + static_cast<off_t>(ret);
            m_stat.size   = std::max(m_stat.size, new_size);
            refresh_stat({ .tv_sec = 0, .tv_nsec = UTIME_OMIT }, { .tv_sec = 0, .tv_nsec = UTIME_NOW });
            return ret;
        });
    }

    AExpect<void> Node::flush(Context context, u64 fd)
    {
        auto may_file = regular_file_prelude();
        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }
        auto& file = may_file->get();

        if (not file.is_open(fd)) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }
        if (not file.is_dirty()) {
            co_return Expect<void>{};    // no write, do nothing
        }

        file.set_dirty(false);
        auto filesize = static_cast<usize>(stat()->get().size);
        co_return co_await context.cache.flush(id(), filesize);
    }

    AExpect<void> Node::release(Context context, u64 fd)
    {
        auto may_file = regular_file_prelude();
        if (not may_file) {
            co_return Unexpect{ may_file.error() };
        }
        auto& file = may_file->get();

        if (not file.close(fd)) {
            co_return Unexpect{ Errc::bad_file_descriptor };
        }
        if (not file.is_dirty()) {
            co_return Expect<void>{};    // no write, do nothing
        }
        file.set_dirty(false);
        auto filesize = static_cast<usize>(stat()->get().size);

        auto res = co_await context.cache.flush(id(), filesize);
        if (not res) {
            co_return Unexpect{ res.error() };
        }

        co_return context.connection.close(fd);
    }

    AExpect<void> Node::utimens(Context context, timespec atime, timespec mtime)
    {
        if (auto res = co_await context.connection.utimens(context.path, atime, mtime); not res) {
            co_return Unexpect{ res.error() };
        }
        co_return (co_await context.connection.stat(context.path)).transform([&](data::Stat stat) {
            m_stat = std::move(stat);
        });
    }

    Expect<Ref<Node>> Node::readlink()
    {
        auto current = this;
        while (current->is<Link>()) {
            current = &current->as<Link>()->get().target();
        }
        return *current;
    }
}
