#pragma once

#include "config.hpp"

#include <nlohmann/json.hpp>

#include <protocol/json_rpc_adapter.hpp>

#include <ymir/debug/protocol/debug_types.hpp>
#include <ymir/sys/saturn.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ymir::debug {

/// Owns one paused Saturn instance and exposes its initial debugger command set.
///
/// This type is deliberately single-threaded. Continuous execution will require
/// a separate execution controller so JSON-RPC requests never race the core.
class DebugService {
public:
    explicit DebugService(HeadlessConfig config);

    /// Loads the configured IPL, optional disc and optional backup RAM, then
    /// hard-resets the Saturn into a paused state.
    [[nodiscard]] bool Initialize(std::string &outError);

    /// Builds the first protocol message emitted by ymir-headless.
    [[nodiscard]] nlohmann::json CreateReadyNotification() const;

    /// Parses and dispatches one JSON-RPC line. Valid notifications intentionally
    /// return no response, as required by JSON-RPC 2.0.
    [[nodiscard]] std::optional<nlohmann::json> HandleLine(std::string_view line);

    [[nodiscard]] ExecutionState GetState() const noexcept {
        return m_state;
    }

    [[nodiscard]] bool ShutdownRequested() const noexcept {
        return m_shutdownRequested;
    }

private:
    [[nodiscard]] nlohmann::json DispatchRequest(const JsonRpcRequest &request);
    [[nodiscard]] nlohmann::json CreateDebugError(const JsonRpcRequest &request, JsonRpcError rpcError,
                                                  ErrorCode debugError, std::string_view message) const;
    [[nodiscard]] bool IsTargetEnabled(DebugTarget target) const noexcept;

    HeadlessConfig m_config;
    std::unique_ptr<ymir::Saturn> m_saturn;
    ExecutionState m_state{ExecutionState::Starting};
    bool m_shutdownRequested{};
    std::string m_instanceId{"local-001"};
};

} // namespace ymir::debug
