#include "madbfs/data/cache.hpp"

#include "madbfs/connection/connection.hpp"
#include "madbfs/log.hpp"

namespace madbfs::data
{
    Page::Page(PageKey key, Uniq<char[]> buf, u32 size)
        : m_key{ key }
        , m_data{ std::move(buf) }
        , m_size{ size }
    {
    }

    usize Page::read(Span<char> out, usize offset)
    {
        auto size = std::min((m_size & ~dirty_bit) - offset, out.size());
        std::copy_n(m_data.get() + offset, size, out.data());
        return size;
    }

    usize Page::write(Span<const char> in, usize offset)
    {
        // NOTE: offset + in.size() is always assumed to be less than or equal to page size
        auto size = offset + in.size();
        std::copy_n(in.data(), in.size(), m_data.get() + offset);
        m_size = static_cast<u32>(size) | (m_size & dirty_bit);
        return in.size();
    }

    usize Page::size() const
    {
        return m_size & ~dirty_bit;
    };

    bool Page::is_dirty() const
    {
        return m_size & dirty_bit;
    }

    void Page::set_dirty(bool set)
    {
        if (set) {
            m_size |= dirty_bit;
        } else {
            m_size &= ~dirty_bit;
        }
    }
}

namespace madbfs::data
{
    Cache::Cache(connection::Connection& connection, usize page_size, usize max_pages)
        : m_connection{ connection }
        , m_page_size{ std::bit_ceil(page_size) }
        , m_max_pages{ max_pages }
    {
    }

    AExpect<usize> Cache::read(Id id, path::Path path, Span<char> out, off_t offset)
    {
        auto start = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + out.size() - 1) / m_page_size;

        auto total_read = 0uz;

        // TODO: use parallel group

        for (auto index : sv::iota(start, last + 1)) {
            log_d({ "{}: read [id={}|idx={}]" }, __func__, id.inner(), index);

            auto key = PageKey{ id, index };

            auto queued = m_queue.find(key);
            if (queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            auto entry = m_table.find(key);
            if (entry == m_table.end()) {
                auto promise = saf::promise<Errc>{ co_await async::this_coro::executor };
                auto future  = promise.get_future().share();
                m_queue.emplace(key, std::move(future));

                auto map_entry = m_path_map.find(id);
                if (map_entry == m_path_map.end()) {
                    m_path_map.emplace(id, PathEntry{ 1uz, path.into_buf() });
                } else {
                    ++map_entry->second.count;
                }

                auto data    = std::make_unique<char[]>(m_page_size);
                auto span    = Span{ data.get(), m_page_size };
                auto may_len = co_await on_miss(id, span, static_cast<off_t>(index * m_page_size));
                if (not may_len) {
                    promise.set_value(may_len.error());
                    m_queue.erase(key);
                    co_return Unexpect{ may_len.error() };
                }

                m_lru.emplace_front(key, std::move(data), *may_len);
                auto [p, _] = m_table.emplace(key, m_lru.begin());
                entry       = p;

                promise.set_value(Errc{});
                m_queue.erase(key);

                if (m_table.size() > m_max_pages) {
                    co_await evict(m_table.size() - m_max_pages);
                }
            }

            const auto& [_, page] = *entry;

            if (page != m_lru.begin()) {
                m_lru.splice(m_lru.begin(), m_lru, page);
            }

            auto local_offset = 0uz;
            if (index == start) {
                local_offset = static_cast<usize>(offset) % m_page_size;
            }

            auto out_sub  = Span{ out.data() + total_read, out.size() - total_read };
            total_read   += page->read(out_sub, local_offset);
        }

        co_return total_read;
    }

    AExpect<usize> Cache::write(Id id, path::Path path, Span<const char> in, off_t offset)
    {
        auto start = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + in.size() - 1) / m_page_size;

        auto total_written = 0uz;

        // TODO: use parallel group

        for (auto index : sv::iota(start, last + 1)) {
            log_d({ "{}: write [id={}|idx={}]" }, __func__, id.inner(), index);

            auto key = PageKey{ id, index };

            auto queued = m_queue.find(key);
            if (queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            auto map_entry = m_path_map.find(id);
            if (map_entry == m_path_map.end()) {
                m_path_map.emplace(id, PathEntry{ 1uz, path.into_buf() });
            } else {
                ++map_entry->second.count;
            }

            auto entry = m_table.find(key);
            if (entry == m_table.end()) {
                m_lru.emplace_front(key, std::make_unique<char[]>(m_page_size), 0);
                auto [p, _] = m_table.emplace(key, m_lru.begin());
                entry       = p;
            }

            const auto& [_, page] = *entry;

            auto local_offset = 0uz;
            if (index == start) {
                local_offset = static_cast<usize>(offset) % m_page_size;
            }

            auto write      = std::min(m_page_size - local_offset, in.size() - total_written);
            auto write_span = Span{ in.data() + total_written, write };

            page->write(write_span, local_offset);
            page->set_dirty(true);

            if (page != m_lru.begin()) {
                m_lru.splice(m_lru.begin(), m_lru, page);
            }

            total_written += write;

            if (m_table.size() > m_max_pages) {
                co_await evict(m_table.size() > m_max_pages);
            }
        }

        co_return total_written;
    }

    AExpect<void> Cache::flush(Id id, usize size)
    {
        auto num_pages = size / m_page_size + (size % m_page_size != 0);

        // TODO: use parallel group

        for (auto index : sv::iota(0uz, num_pages)) {
            log_d({ "{}: flush [id={}|idx={}]" }, __func__, id.inner(), index);

            auto key = PageKey{ id, index };

            auto queued = m_queue.find(key);
            if (queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            auto entry = m_table.find(key);
            if (entry == m_table.end()) {
                log_c({ "{}: page skipped [id={}|idx={}]" }, __func__, id.inner(), index);
                continue;
            }

            auto page = entry->second;
            if (page->is_dirty()) {
                auto data = std::make_unique<char[]>(m_page_size);
                auto read = page->read({ data.get(), m_page_size }, 0);
                page->set_dirty(false);

                auto span = Span{ data.get(), read };
                auto res  = co_await on_flush(id, span, static_cast<off_t>(index * m_page_size));
                if (not res.has_value()) {
                    co_return Unexpect{ res.error() };
                }
            }
        }

        co_return Expect<void>{};
    }

    void Cache::invalidate()
    {
        m_table.clear();
        m_lru.clear();

        log_i({ "{}: cache invalidated" }, __func__);
    }

    void Cache::set_page_size(usize new_page_size)
    {
        m_page_size = new_page_size;
        m_table.clear();
        m_lru.clear();

        log_i({ "{}: page size changed to: {}" }, __func__, new_page_size);
    }

    void Cache::set_max_pages(usize new_max_pages)
    {
        m_max_pages = new_max_pages;
        m_table.clear();
        m_lru.clear();

        log_i({ "{}: max pages can be stored changed to: {}" }, __func__, new_max_pages);
    }

    AExpect<usize> Cache::on_miss(Id id, Span<char> out, off_t offset)
    {
        auto found = m_path_map.find(id);
        assert(found != m_path_map.end());

        // WARN: if m_path_map is updated, the path may point to freed memory
        auto path = found->second.path.as_path();
        auto idx  = static_cast<usize>(offset) / m_page_size;

        log_d({ "{}: [id={}|idx={}] cache miss, read from device..." }, __func__, id.inner(), idx, offset);
        return m_connection.read(path, out, offset);
    }

    AExpect<usize> Cache::on_flush(Id id, Span<const char> in, off_t offset)
    {
        auto found = m_path_map.find(id);
        assert(found != m_path_map.end());

        // WARN: if m_path_map is updated, the path may point to freed memory
        auto path = found->second.path.as_path();
        auto idx  = static_cast<usize>(offset) / m_page_size;

        log_d({ "{}: [id={}|idx={}] flush, write to device..." }, __func__, id.inner(), idx, offset);
        return m_connection.write(path, in, offset);
    }

    Await<void> Cache::evict(usize size)
    {
        while (size-- > 0 and not m_lru.empty()) {
            auto page = std::move(m_lru.back());
            m_lru.pop_back();
            m_table.erase(page.key());

            if (not page.is_dirty()) {
                continue;
            }

            auto [id, idx] = page.key();
            log_w({ "{}: force push page [id={}|idx={}" }, __func__, id.inner(), idx);

            auto offset = static_cast<off_t>(idx * m_page_size);
            if (auto res = co_await on_flush(id, page.buf(), offset); not res) {
                log_c({ "{}: failed to force push page [id={}|idx={}" }, __func__, id.inner(), idx);
            }

            auto found = m_path_map.find(id);
            assert(found != m_path_map.end());
            if (--found->second.count == 0) {
                m_path_map.erase(found);
            }
        }
    }
}
