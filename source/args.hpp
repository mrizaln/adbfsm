#pragma once

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>

#include "cmd.hpp"
#include "util.hpp"

namespace adbfsm::args
{
    // NOTE: don't set default value here for string, set them in the parse() function
    struct AdbfsmOpt
    {
        const char* m_serial    = nullptr;
        const char* m_log_level = nullptr;
        const char* m_log_file  = nullptr;
        int         m_cachesize = 500;
        int         m_rescan    = false;
        int         m_help      = false;
        int         m_full_help = false;
    };

    struct ParsedOpt
    {
        std::string               m_serial;
        spdlog::level::level_enum m_log_level;
        std::string               m_log_file;
        usize                     m_cachesize;
        bool                      m_rescan;
    };

    struct ParseResult
    {
        // clang-format off
        struct Opt  { ParsedOpt m_opt; fuse_args m_fuse_args; };
        struct Exit { int m_status; };

        ParseResult(Opt opt)    : m_result{ std::move(opt) } {}
        ParseResult(int status) : m_result{ Exit{ status } } {}

        bool is_opt()  const { return std::holds_alternative<Opt>(m_result);  }
        bool is_exit() const { return std::holds_alternative<Exit>(m_result); }

        Opt&&  opt()  && { return std::move(std::get<Opt>(m_result));  }
        Exit&& exit() && { return std::move(std::get<Exit>(m_result)); }

        std::variant<Opt, Exit> m_result;
        // clang-format on
    };

    enum class SerialStatus
    {
        NotExist,
        Offline,
        Unauthorized,
        Device,
    };

    static constexpr auto adbfsm_opt_spec = std::array<fuse_opt, 9>{ {
        // clang-format off
        { "--serial=%s",    offsetof(AdbfsmOpt, m_serial),    true },
        { "--loglevel=%s",  offsetof(AdbfsmOpt, m_log_level), true },
        { "--logfile=%s",   offsetof(AdbfsmOpt, m_log_file),  true },
        { "--cachesize=%d", offsetof(AdbfsmOpt, m_cachesize), true },
        { "-h",             offsetof(AdbfsmOpt, m_help),      true },
        { "--help",         offsetof(AdbfsmOpt, m_help),      true },
        { "--full-help",    offsetof(AdbfsmOpt, m_full_help), true },
        // clang-format on
        FUSE_OPT_END,
    } };

    inline void show_help(const char* prog, bool cerr)
    {
        auto out = cerr ? stderr : stdout;
        fmt::print(out, "usage: {} [options] <mountpoint>\n\n", prog);
        fmt::print(
            out,
            "Options for adbfsm:\n"
            "    --serial=<s>        serial number of the device to mount\n"
            "                        (default: <auto> [detection is similar to adb])\n"
            "    --loglevel=<l>      log level to use (default: warn)\n"
            "    --logfile=<f>       log file to write to (default: - for stdout)\n"
            "    --cachesize=<n>     maximum size of the cache in MB (default: 500)\n"
            "    -h   --help         show this help message\n"
            "    --full-help         show full help message (includes libfuse options)\n"
        );
    };

    constexpr std::string_view to_string(SerialStatus status)
    {
        switch (status) {
        case SerialStatus::NotExist: return "device not exist";
        case SerialStatus::Offline: return "device offline";
        case SerialStatus::Unauthorized: return "device unauthorized";
        case SerialStatus::Device: return "device ok";
        }
        return "Unknown";
    }

    inline std::optional<spdlog::level::level_enum> parse_level_str(std::string_view level)
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

    inline SerialStatus check_serial(std::string_view serial)
    {
        auto proc = adbfsm::cmd::exec({ "adb", "devices" });
        assert(proc.returncode == 0);

        auto line_splitter = adbfsm::util::StringSplitter{ proc.cout, { '\n' } };
        while (auto str = line_splitter.next()) {
            auto splitter = adbfsm::util::StringSplitter{ *str, { " \t" } };

            auto supposedly_serial = splitter.next();
            auto status            = splitter.next();

            if (not supposedly_serial.has_value() or not status.has_value()) {
                return SerialStatus::NotExist;
            }

            if (supposedly_serial.value() == serial) {
                if (*status == "offline") {
                    return SerialStatus::Offline;
                } else if (*status == "unauthorized") {
                    return SerialStatus::Unauthorized;
                } else if (*status == "device") {
                    return SerialStatus::Device;
                } else {
                    return SerialStatus::NotExist;
                }
            }
        }

        return SerialStatus::NotExist;
    }

    inline std::string get_serial()
    {
        auto proc = adbfsm::cmd::exec({ "adb", "devices" });
        assert(proc.returncode == 0);

        auto serials       = std::vector<std::string>{};
        auto line_splitter = adbfsm::util::StringSplitter{ proc.cout, { '\n' } };

        std::ignore = line_splitter.next();    // skip the first line

        while (auto str = line_splitter.next()) {
            auto splitter = adbfsm::util::StringSplitter{ *str, { " \t" } };

            auto supposedly_serial = splitter.next();
            auto status            = splitter.next();

            if (not supposedly_serial.has_value() or not status.has_value()) {
                break;
            }
            if (*status == "device") {
                serials.emplace_back(*supposedly_serial);
            }
        }

        if (serials.empty()) {
            return {};
        } else if (serials.size() == 1) {
            fmt::println("[adbfsm] only one device found, using serial '{}'", serials[0]);
            return serials[0];
        }

        fmt::println("[adbfsm] multiple devices detected,");
        for (auto i : adbfsm::sv::iota(0u, serials.size())) {
            fmt::println("         - {}: {}", i + 1, serials[i]);
        }
        fmt::print("[adbfsm] please specify which one you would like to use: ");

        auto choice = 1u;
        while (true) {
            std::cin >> choice;
            if (choice > 0 and choice <= serials.size()) {
                break;
            }
            fmt::print("[adbfsm] invalid choice, please enter a number between 1 and {}: ", serials.size());
        }
        fmt::println("[adbfsm] using serial '{}'", serials[choice - 1]);

        return serials[choice - 1];
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
    inline ParseResult parse(int argc, char** argv)
    {
        fuse_args args = FUSE_ARGS_INIT(argc, argv);

        auto get_serial_env = []() -> const char* {
            if (auto serial = ::getenv("ANDROID_SERIAL"); serial != nullptr) {
                fmt::println("[adbfsm] using serial '{}' from env variable 'ANDROID_SERIAL'", serial);
                return ::strdup(serial);
            }
            return nullptr;
        };

        // NOTE: these strings must be malloc-ed since fuse_opt_parse will free them
        auto adbfsm_opt = AdbfsmOpt{
            .m_serial    = get_serial_env(),
            .m_log_level = ::strdup("warn"),
            .m_log_file  = ::strdup("-"),
        };

        if (fuse_opt_parse(&args, &adbfsm_opt, adbfsm_opt_spec.data(), NULL) != 0) {
            fmt::println(stderr, "error: failed to parse options\n");
            fmt::println(stderr, "try '{} --help' for more information", argv[0]);
            fmt::println(stderr, "try '{} --full-help' for full information", argv[0]);
            return 1;
        }

        if (adbfsm_opt.m_help) {
            show_help(argv[0], false);
            ::fuse_opt_free_args(&args);
            return 0;
        } else if (adbfsm_opt.m_full_help) {
            show_help(argv[0], false);
            fmt::println(stdout, "\nOptions for libfuse:");
            ::fuse_cmdline_help();
            ::fuse_lowlevel_help();
            ::fuse_opt_free_args(&args);
            return 0;
        }

        auto log_level = parse_level_str(adbfsm_opt.m_log_level);
        if (not log_level.has_value()) {
            fmt::println(stderr, "error: invalid log level '{}'", adbfsm_opt.m_log_level);
            fmt::println(stderr, "valid log levels: trace, debug, info, warn, error, critical, off");
            ::fuse_opt_free_args(&args);
            return 1;
        }

        if (adbfsm_opt.m_serial == nullptr) {
            auto serial = get_serial();
            if (serial.empty()) {
                fmt::println(stderr, "error: no device found, make sure your device is connected");
                ::fuse_opt_free_args(&args);
                return 1;
            }
            adbfsm_opt.m_serial = ::strdup(serial.c_str());
        } else if (auto status = check_serial(adbfsm_opt.m_serial); status != SerialStatus::Device) {
            fmt::println(
                stderr, "error: serial '{} 'is not valid ({})", adbfsm_opt.m_serial, to_string(status)
            );
            ::fuse_opt_free_args(&args);
            return 1;
        }

        return ParseResult::Opt{
            .m_opt = {
                .m_serial    = adbfsm_opt.m_serial,
                .m_log_level = log_level.value(),
                .m_log_file  = adbfsm_opt.m_log_file,
                .m_cachesize = static_cast<usize>(adbfsm_opt.m_cachesize),
                .m_rescan    = static_cast<bool>(adbfsm_opt.m_rescan),
            },
            .m_fuse_args = args,
        };
    }
}
