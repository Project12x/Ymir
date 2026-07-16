#include "config_parser.hpp"
#include "debug_service.hpp"

#include <fmt/format.h>

#include <protocol/json_rpc_adapter.hpp>
#include <protocol/line_framer.hpp>

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#if defined(_WIN32)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace {

int FileNumber(FILE *stream) {
#if defined(_WIN32)
    return ::_fileno(stream);
#else
    return ::fileno(stream);
#endif
}

int DuplicateFileDescriptor(int descriptor) {
#if defined(_WIN32)
    return ::_dup(descriptor);
#else
    return ::dup(descriptor);
#endif
}

bool DuplicateFileDescriptorTo(int source, int destination) {
#if defined(_WIN32)
    return ::_dup2(source, destination) == 0;
#else
    return ::dup2(source, destination) >= 0;
#endif
}

void CloseFileDescriptor(int descriptor) {
#if defined(_WIN32)
    ::_close(descriptor);
#else
    ::close(descriptor);
#endif
}

FILE *OpenFileDescriptor(int descriptor) {
#if defined(_WIN32)
    return ::_fdopen(descriptor, "w");
#else
    return ::fdopen(descriptor, "w");
#endif
}

/// Preserves the process's original stdout for JSON-RPC and redirects the
/// stdout descriptor used by Ymir's development logger to stderr.
class ProtocolWriter {
public:
    ~ProtocolWriter() {
        if (m_stream != nullptr) {
            std::fclose(m_stream);
        }
    }

    [[nodiscard]] bool Initialize(std::string &outError) {
        std::fflush(stdout);
        const int stdoutDescriptor = FileNumber(stdout);
        const int stderrDescriptor = FileNumber(stderr);
        const int protocolDescriptor = DuplicateFileDescriptor(stdoutDescriptor);
        if (protocolDescriptor < 0) {
            outError = std::error_code{errno, std::generic_category()}.message();
            return false;
        }

        if (!DuplicateFileDescriptorTo(stderrDescriptor, stdoutDescriptor)) {
            outError = std::error_code{errno, std::generic_category()}.message();
            CloseFileDescriptor(protocolDescriptor);
            return false;
        }

        m_stream = OpenFileDescriptor(protocolDescriptor);
        if (m_stream == nullptr) {
            outError = std::error_code{errno, std::generic_category()}.message();
            DuplicateFileDescriptorTo(protocolDescriptor, stdoutDescriptor);
            CloseFileDescriptor(protocolDescriptor);
            return false;
        }
        return true;
    }

    void Write(const nlohmann::json &message) const {
        fmt::print(m_stream, "{}\n", message.dump());
        std::fflush(m_stream);
    }

private:
    FILE *m_stream{};
};

} // namespace

int main(int argc, char **argv) {
    auto config = ymir::debug::LoadConfig(argc, argv);
    if (!ymir::debug::ValidateConfig(config)) {
        return 1;
    }

    fmt::print(stderr, "ymir-headless: ipl: {}\n", config.ipl_path.string());
    if (config.game_path) {
        fmt::print(stderr, "ymir-headless: game: {}\n", config.game_path->string());
    }
    if (config.bram_path) {
        fmt::print(stderr, "ymir-headless: bram: {}\n", config.bram_path->string());
    }
    fmt::print(stderr, "ymir-headless: slave: {}\n", config.slave_enabled ? "enabled" : "disabled");

    ProtocolWriter protocolWriter;
    std::string protocolError;
    if (!protocolWriter.Initialize(protocolError)) {
        fmt::print(stderr, "ymir-headless: failed to reserve stdout for JSON-RPC: {}\n", protocolError);
        return 2;
    }

    ymir::debug::DebugService service{std::move(config)};
    std::string initializationError;
    if (!service.Initialize(initializationError)) {
        fmt::print(stderr, "ymir-headless: {}\n", initializationError);
        return 3;
    }

    protocolWriter.Write(service.CreateReadyNotification());

    ymir::debug::LineFramer framer(
        [&](std::string_view line) {
            if (auto response = service.HandleLine(line)) {
                protocolWriter.Write(*response);
            }
        },
        [&](ymir::debug::LineFramerError) {
            protocolWriter.Write(ymir::debug::JsonRpcAdapter::CreateErrorResponse(
                std::monostate{}, ymir::debug::JsonRpcError::InvalidRequest,
                "request line exceeds the 1 MiB transport limit"));
        });

    char input;
    while (!service.ShutdownRequested() && std::cin.get(input)) {
        framer.Push(&input, 1);
    }

    return 0;
}
