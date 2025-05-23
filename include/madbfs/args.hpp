#pragma once

#include "madbfs/data/connection.hpp"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#include <spdlog/spdlog.h>

#include <iostream>

namespace madbfs::args
{
    // NOTE: don't set default value here for string, set them in the parse() function
    struct MadbfsOpt
    {
        const char* serial     = nullptr;
        const char* log_level  = nullptr;
        const char* log_file   = nullptr;
        int         cache_size = 512;    // in MiB
        int         page_size  = 128;    // in KiB
        int         help       = false;
        int         full_help  = false;
    };

    struct ParsedOpt
    {
        String                    serial;
        spdlog::level::level_enum log_level;
        String                    log_file;
        usize                     cachesize;
        usize                     pagesize;
    };

    struct ParseResult
    {
        // clang-format off
        struct Opt  { ParsedOpt opt; fuse_args args; };
        struct Exit { int status; };

        ParseResult() : result{ Exit{ 0}} {}
        ParseResult(Opt opt)    : result{ std::move(opt) } {}
        ParseResult(int status) : result{ Exit{ status } } {}

        bool is_opt()  const { return std::holds_alternative<Opt>(result);  }
        bool is_exit() const { return std::holds_alternative<Exit>(result); }

        Opt&&  opt()  && { return std::move(std::get<Opt>(result));  }
        Exit&& exit() && { return std::move(std::get<Exit>(result)); }

        Var<Opt, Exit> result;
        // clang-format on
    };

    static constexpr auto madbfs_opt_spec = Array<fuse_opt, 9>{ {
        // clang-format off
        { "--serial=%s",     offsetof(MadbfsOpt, serial),     true },
        { "--log-level=%s",  offsetof(MadbfsOpt, log_level),  true },
        { "--log-file=%s",   offsetof(MadbfsOpt, log_file),   true },
        { "--cache-size=%d", offsetof(MadbfsOpt, cache_size), true },
        { "--page-size=%d",  offsetof(MadbfsOpt, page_size),  true },
        { "-h",              offsetof(MadbfsOpt, help),       true },
        { "--help",          offsetof(MadbfsOpt, help),       true },
        { "--full-help",     offsetof(MadbfsOpt, full_help),  true },
        // clang-format on
        FUSE_OPT_END,
    } };

    inline void show_help(const char* prog, bool cerr)
    {
        auto out = cerr ? stderr : stdout;
        fmt::print(out, "usage: {} [options] <mountpoint>\n\n", prog);
        fmt::print(
            out,
            "Options for madbfs:\n"
            "    --serial=<s>         serial number of the device to mount\n"
            "                           (you can omit this [detection is similar to adb])\n"
            "                           (will prompt if more than one device exists)\n"
            "    --log-level=<l>      log level to use (default: warn)\n"
            "    --log-file=<f>       log file to write to (default: - for stdout)\n"
            "    --cache-size=<n>     maximum size of the cache in MiB\n"
            "                           (default: 512)\n"
            "                           (minimum: 128)\n"
            "                           (value will be rounded to the next power of 2)\n"
            "    --page-size=<n>      page size for cache & transfer in KiB\n"
            "                           (default: 128)\n"
            "                           (minimum: 64)\n"
            "                           (value will be rounded to the next power of 2)\n"
            "    -h   --help          show this help message\n"
            "    --full-help          show full help message (includes libfuse options)\n"
        );
    };

    inline Opt<spdlog::level::level_enum> parse_level_str(Str level)
    {
        // clang-format off
        if      (level == "trace")      return spdlog::level::trace;
        else if (level == "debug")      return spdlog::level::debug;
        else if (level == "info")       return spdlog::level::info;
        else if (level == "warn")       return spdlog::level::warn;
        else if (level == "error")      return spdlog::level::err;
        else if (level == "critical")   return spdlog::level::critical;
        else if (level == "off")        return spdlog::level::off;
        else                            return std::nullopt;
        // clang-format on
    }

    inline Await<data::DeviceStatus> check_serial(Str serial)
    {
        if (auto maybe_devices = co_await data::list_devices(); maybe_devices.has_value()) {
            auto found = sr::find(*maybe_devices, serial, &data::Device::serial);
            if (found != maybe_devices->end()) {
                co_return found->status;
            }
        }
        co_return data::DeviceStatus::Unknown;
    }

    inline Await<String> get_serial()
    {
        auto maybe_devices = co_await data::list_devices();
        if (not maybe_devices.has_value()) {
            co_return "";
        }
        auto devices = *maybe_devices                                                                 //
                     | sv::filter([](auto&& d) { return d.status == data::DeviceStatus::Device; })    //
                     | sr::to<Vec<data::Device>>();

        if (devices.empty()) {
            co_return "";
        } else if (devices.size() == 1) {
            fmt::println("[madbfs] only one device found, using serial '{}'", devices[0].serial);
            co_return devices[0].serial;
        }

        fmt::println("[madbfs] multiple devices detected,");
        for (auto i : madbfs::sv::iota(0u, devices.size())) {
            fmt::println("         - {}: {}", i + 1, devices[i].serial);
        }
        fmt::print("[madbfs] please specify which one you would like to use: ");

        auto choice = 1u;
        while (true) {
            std::cin >> choice;
            if (choice > 0 and choice <= devices.size()) {
                break;
            }
            fmt::print("[madbfs] invalid choice, please enter a number between 1 and {}: ", devices.size());
        }
        fmt::println("[madbfs] using serial '{}'", devices[choice - 1].serial);

        co_return devices[choice - 1].serial;
    }

    /**
     * @brief Parse the command line arguments; show help message if needed.
     *
     * @param argc Number of arguments.
     * @param argv Array of arguments.
     *
     * If the return value is `ParseResult::Opt`, the `fuse_args` member must be freed using
     * `fuse_opt_free_args` after use.
     */
    inline Await<ParseResult> parse(int argc, char** argv)
    {
        fmt::println("[madbfs] checking adb availability...");
        if (auto status = co_await data::start_connection(); not status.has_value()) {
            const auto msg = std::make_error_code(status.error()).message();
            fmt::println(stderr, "\nerror: failed to start adb server [{}].", msg);
            fmt::println(stderr, "\nnote: make sure adb is installed and in PATH.");
            fmt::println(stderr, "note: make sure phone debugging permission is enabled.");
            fmt::println(stderr, "      phone with its screen locked might denies adb connection.");
            fmt::println(stderr, "      you might need to unlock your device first to be able to use adb.");

            co_return ParseResult{ 1 };
        }

        fuse_args args = FUSE_ARGS_INIT(argc, argv);

        auto get_serial_env = []() -> const char* {
            if (auto serial = ::getenv("ANDROID_SERIAL"); serial != nullptr) {
                fmt::println("[madbfs] using serial '{}' from env variable 'ANDROID_SERIAL'", serial);
                return ::strdup(serial);
            }
            return nullptr;
        };

        // NOTE: these strings must be malloc-ed since fuse_opt_parse will free them
        auto madbfs_opt = MadbfsOpt{
            .serial    = get_serial_env(),
            .log_level = ::strdup("warn"),
            .log_file  = ::strdup("-"),
        };

        if (fuse_opt_parse(&args, &madbfs_opt, madbfs_opt_spec.data(), NULL) != 0) {
            fmt::println(stderr, "error: failed to parse options\n");
            fmt::println(stderr, "try '{} --help' for more information", argv[0]);
            fmt::println(stderr, "try '{} --full-help' for full information", argv[0]);
            co_return ParseResult{ 1 };
        }

        if (madbfs_opt.help) {
            show_help(argv[0], false);
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 0 };
        } else if (madbfs_opt.full_help) {
            show_help(argv[0], false);
            fmt::println(stdout, "\nOptions for libfuse:");
            ::fuse_cmdline_help();
            ::fuse_lowlevel_help();
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 0 };
        }

        auto log_level = parse_level_str(madbfs_opt.log_level);
        if (not log_level.has_value()) {
            fmt::println(stderr, "error: invalid log level '{}'", madbfs_opt.log_level);
            fmt::println(stderr, "valid log levels: trace, debug, info, warn, error, critical, off");
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 1 };
        }

        if (madbfs_opt.serial == nullptr) {
            auto serial = co_await get_serial();
            if (serial.empty()) {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                ::fuse_opt_free_args(&args);
                co_return ParseResult{ 1 };
            }
            madbfs_opt.serial = ::strdup(serial.c_str());
        } else if (auto r = co_await check_serial(madbfs_opt.serial); r != data::DeviceStatus::Device) {
            fmt::println(stderr, "error: serial '{} 'is not valid ({})", madbfs_opt.serial, to_string(r));
            ::fuse_opt_free_args(&args);
            co_return ParseResult{ 1 };
        }

        co_return ParseResult::Opt{
            .opt = {
                .serial    = madbfs_opt.serial,
                .log_level = log_level.value(),
                .log_file  = madbfs_opt.log_file,
                .cachesize = std::bit_ceil(std::max(static_cast<usize>(madbfs_opt.cache_size), 128uz)),
                .pagesize  = std::bit_ceil(std::max(static_cast<usize>(madbfs_opt.page_size), 64uz)),
            },
            .args = args,
        };
    }
}
