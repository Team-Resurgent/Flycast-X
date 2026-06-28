// flycast_shim/asio.hpp -- no-op stub of the bits of ASIO that Flycast's
// log/NetworkListener.h uses (UDP log forwarding). Network logging is disabled
// on Xbox, so every operation reports an error/closed socket and the listener
// becomes a no-op. Just enough surface to compile LogManager.cpp.
#pragma once
#include <string>
#include <cstring>
#include <cstddef>

namespace asio
{
    struct error_code
    {
        int value_ = 0;
        error_code() = default;
        explicit operator bool() const { return value_ != 0; }
        std::string message() const { return std::string("network disabled"); }
    };

    struct io_context {};

    struct const_buffer { const void* data_; std::size_t size_; };
    inline const_buffer buffer(const void* d, std::size_t s) { return const_buffer{ d, s }; }

    namespace ip
    {
        struct udp
        {
            struct endpoint {};

            struct resolver
            {
                struct results_type
                {
                    bool empty() const { return true; }
                    const endpoint* begin() const { return nullptr; }
                    const endpoint* end()   const { return nullptr; }
                };
                explicit resolver(io_context&) {}
                results_type resolve(const std::string&, const std::string&, error_code& ec)
                { ec.value_ = 1; return results_type{}; }
            };

            struct socket
            {
                explicit socket(io_context&) {}
                void connect(const endpoint&, error_code& ec) { ec.value_ = 1; }
                bool is_open() const { return false; }
                std::size_t send(const const_buffer&, int, error_code& ec) { ec.value_ = 1; return 0; }
            };
        };
    }
}
