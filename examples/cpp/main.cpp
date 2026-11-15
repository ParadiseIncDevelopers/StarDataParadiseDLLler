#include <StarDataParadiseDLLler.hpp>

#include <array>
#include <charconv>
#include <iostream>
#include <string_view>

struct Password
{
    std::array<char, 1026> bytes{};
    ~Password() { clear(); }
    void clear() noexcept
    {
        volatile char* output = bytes.data();
        for (std::size_t index = 0; index < bytes.size(); ++index) output[index] = 0;
    }
};

int main(int argc, char** argv)
{
    if (argc != 6)
    {
        std::cerr << "Usage: native_example address port server-name ca-file username\n"
                     "Supply the password through the first line of standard input.\n";
        return 2;
    }
    const std::string_view port_text = argv[2];
    uint16_t port{};
    const auto end = port_text.data() + port_text.size();
    const auto port_result = std::from_chars(port_text.data(), end, port);
    if (port_result.ec != std::errc{} || port_result.ptr != end || !port) return 2;

    Password password;
    if (!std::cin.getline(password.bytes.data(), password.bytes.size())) return 2;
    try
    {
        using sdp::client::text;
        sdp_open_options options{};
        options.struct_size = sizeof(options);
        options.abi_version = SDP_ABI_VERSION;
        options.address = text(argv[1]);
        options.port = port;
        options.server_name = text(argv[3]);
        options.ca_file = text(argv[4]);
        options.username = text(argv[5]);
        options.password = text(password.bytes.data());
        options.timeout_ms = 3000;

        const sdp::client::Connection connection(options);
        password.clear();
        connection.health();
        std::cout << connection.session_info().view() << '\n' << connection.capabilities().view() << '\n';
    }
    catch (const sdp::client::Error& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
