#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"

#include <sys/stat.h>
#include <sys/types.h>

namespace madbfs::rpc
{
    /*
     * This RPC is opaque from both the client and the server. The stub is provided as is and provides correct
     * semantics if provided that the caller and callee conforms to the contract:
     *
     * client:
     * - call to `send_req_procedure` with certain procedure
     * - must be followed by a call to `send_req_param` with appropriate param in req namespace
     * - read the response by calling `recv_resp_procedure`
     *
     * server:
     * - call to `recv_req_procedure`
     * - followed by call to `recv_req_param` with previously obtained procedure enum
     * - response with `send_resp_procedure`
     *
     * For Listdir procedure only:
     * - server must send dirent continuously using `listdir_channel::Sender::send_next`, send EOF on complete
     * - client must recv dirent continuously using `listdir_channel::Receiver::recv_next` until EOF
     */

    using Socket = async::tcp::Socket;

    // NOTE: if you decided to add/remove one or more entries, do update domain check in peek_req
    enum class Procedure : u8
    {
        Listdir = 1,
        Stat,
        Readlink,
        Mknod,
        Mkdir,
        Unlink,
        Rmdir,
        Rename,
        Truncate,
        Read,
        Write,
        Utimens,
        CopyFileRange,
    };

    // NOTE: network error won't overlap with procedure error. procedure error are only limited to the
    // integral values defined by Status
    enum class Status : u8
    {
        Success               = 0,
        NoSuchFileOrDirectory = ENOENT,
        PermissionDenied      = EACCES,
        FileExists            = EEXIST,
        NotADirectory         = ENOTDIR,
        IsADirectory          = EISDIR,
        InvalidArgument       = EINVAL,    // generic error
        DirectoryNotEmpty     = ENOTEMPTY,
    };

    namespace req
    {
        // NOTE: server after receiving this request must immediately use `listdir_channel::Sender` and send
        // the directory data by that channel.
        struct Listdir
        {
            Str path;
        };

        // clang-format off
        struct Stat          { Str path; };
        struct Readlink      { Str path; };
        struct Mknod         { Str path; };
        struct Mkdir         { Str path; };
        struct Unlink        { Str path; };
        struct Rmdir         { Str path; };
        struct Rename        { Str from; Str to; };
        struct Truncate      { Str path; i64 size; };
        struct Read          { Str path; i64 offset; u64 size; };
        struct Write         { Str path; i64 offset; Span<const u8> in; };
        struct Utimens       { Str path; timespec atime; timespec mtime; };
        struct CopyFileRange { Str in_path; i64 in_offset; Str out_path; i64 out_offset; u64 size; };
        // clang-format on
    }

    using Request = Var<
        req::Listdir,
        req::Stat,
        req::Readlink,
        req::Mknod,
        req::Mkdir,
        req::Unlink,
        req::Rmdir,
        req::Rename,
        req::Truncate,
        req::Read,
        req::Write,
        req::Utimens,
        req::CopyFileRange>;

    namespace resp
    {
        struct Stat;

        struct Listdir
        {
            Vec<Pair<Str, Stat>> entries;
        };

        struct Stat
        {
            off_t    size;
            nlink_t  links;
            timespec mtime;
            timespec atime;
            timespec ctime;
            mode_t   mode;
            uid_t    uid;
            gid_t    gid;
        };

        // clang-format off
        struct Readlink         { Str target; };
        struct Mkdir            { };
        struct Mknod            { };
        struct Unlink           { };
        struct Rmdir            { };
        struct Rename           { };
        struct Truncate         { };
        struct Read             { Span<const u8> read; };
        struct Write            { usize size; };
        struct Utimens          { };
        struct CopyFileRange    { usize size; };
        // clang-format on
    }

    using Response = Var<
        resp::Listdir,
        resp::Stat,
        resp::Readlink,
        resp::Mknod,
        resp::Mkdir,
        resp::Unlink,
        resp::Rmdir,
        resp::Rename,
        resp::Truncate,
        resp::Read,
        resp::Write,
        resp::Utimens,
        resp::CopyFileRange>;

    class Client
    {
    public:
        Client(Socket& socket, Vec<u8>& buffer)
            : m_socket{ socket }
            , m_buffer{ buffer }
        {
        }

        Socket&  sock() noexcept { return m_socket; }
        Vec<u8>& buf() noexcept { return m_buffer; }

        // clang-format off
        AExpect<resp::Listdir>       send_req_listdir        (req::Listdir       req);
        AExpect<resp::Stat>          send_req_stat           (req::Stat          req);
        AExpect<resp::Readlink>      send_req_readlink       (req::Readlink      req);
        AExpect<resp::Mknod>         send_req_mknod          (req::Mknod         req);
        AExpect<resp::Mkdir>         send_req_mkdir          (req::Mkdir         req);
        AExpect<resp::Unlink>        send_req_unlink         (req::Unlink        req);
        AExpect<resp::Rmdir>         send_req_rmdir          (req::Rmdir         req);
        AExpect<resp::Rename>        send_req_rename         (req::Rename        req);
        AExpect<resp::Truncate>      send_req_truncate       (req::Truncate      req);
        AExpect<resp::Read>          send_req_read           (req::Read          req);
        AExpect<resp::Write>         send_req_write          (req::Write         req);
        AExpect<resp::Utimens>       send_req_utimens        (req::Utimens       req);
        AExpect<resp::CopyFileRange> send_req_copy_file_range(req::CopyFileRange req);
        // clang-format on

    private:
        Socket&  m_socket;
        Vec<u8>& m_buffer;
    };

    class Server
    {
    public:
        Server(Socket& socket, Vec<u8>& buffer)
            : m_socket{ socket }
            , m_buffer{ buffer }
        {
        }

        Socket&  sock() noexcept { return m_socket; }
        Vec<u8>& buf() noexcept { return m_buffer; }

        AExpect<Procedure> peek_req();
        AExpect<void>      send_resp(Var<Status, Response> response);

        AExpect<req::Listdir>       recv_req_listdir();
        AExpect<req::Stat>          recv_req_stat();
        AExpect<req::Readlink>      recv_req_readlink();
        AExpect<req::Mknod>         recv_req_mknod();
        AExpect<req::Mkdir>         recv_req_mkdir();
        AExpect<req::Unlink>        recv_req_unlink();
        AExpect<req::Rmdir>         recv_req_rmdir();
        AExpect<req::Rename>        recv_req_rename();
        AExpect<req::Truncate>      recv_req_truncate();
        AExpect<req::Read>          recv_req_read();
        AExpect<req::Write>         recv_req_write();
        AExpect<req::Utimens>       recv_req_utimens();
        AExpect<req::CopyFileRange> recv_req_copy_file_range();

    private:
        Socket&  m_socket;
        Vec<u8>& m_buffer;
    };

    static constexpr Str server_ready_string = "SERVER_IS_READY";

    /**
     * @brief Return string representation of enum Procedure.
     *
     * The string lifetime is static.
     */
    Str to_string(Procedure procedure);

    /**
     * @brief Return the type name of the contained Request variant.
     *
     * The string lifetime is static.
     */
    Str to_string(Request request);

    /**
     * @brief Return the type name of the contained Response variant.
     *
     * The string lifetime is static.
     */
    Str to_string(Response response);
}
