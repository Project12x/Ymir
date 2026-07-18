#include "debug_service.hpp"

#include <protocol/json_rpc_adapter.hpp>

#include <ymir/debug/protocol/debug_command.hpp>
#include <ymir/debug/protocol/protocol_version.hpp>
#include <ymir/hw/vdp/vdp2_defs.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_impl_control_pad.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/version.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <span>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

namespace ymir::debug {

namespace {

    constexpr uint32_t kMaxPeekBytes = 64_KiB;
    constexpr uint32_t kMaxRunForFrames = 3600;

    const std::vector<std::string> &Capabilities() {
        static const std::vector<std::string> capabilities{
            "sh2.master",    "sh2.slave",       "exec.continue",     "exec.pause",    "exec.run_for",
            "exec.stepi",    "exec.reset",      "regs.read",         "mem.peek",      "mem.poke", "input.pulse",
            "video.frame_hash",
            "video.capture", "instance.status", "instance.shutdown", "event.stopped",
        };
        return capabilities;
    }

    bool HasOnlyKeys(const nlohmann::json &params, std::initializer_list<std::string_view> allowedKeys) {
        if (!params.is_object()) {
            return false;
        }
        for (const auto &[key, value] : params.items()) {
            (void)value;
            const bool allowed = std::ranges::any_of(allowedKeys, [&](std::string_view item) { return item == key; });
            if (!allowed) {
                return false;
            }
        }
        return true;
    }

    bool HasNoParams(const nlohmann::json &params) {
        return (params.is_object() || params.is_array()) && params.empty();
    }

    std::optional<uint32_t> ParseUint32(const nlohmann::json &value) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<uint64_t>();
            if (number <= std::numeric_limits<uint32_t>::max()) {
                return static_cast<uint32_t>(number);
            }
            return std::nullopt;
        }
        if (value.is_number_integer()) {
            const auto number = value.get<int64_t>();
            if (number >= 0 && static_cast<uint64_t>(number) <= std::numeric_limits<uint32_t>::max()) {
                return static_cast<uint32_t>(number);
            }
            return std::nullopt;
        }
        if (!value.is_string()) {
            return std::nullopt;
        }

        const std::string &text = value.get_ref<const std::string &>();
        std::string_view digits{text};
        int base = 10;
        if (digits.starts_with("0x") || digits.starts_with("0X")) {
            digits.remove_prefix(2);
            base = 16;
        }
        if (digits.empty()) {
            return std::nullopt;
        }

        uint32_t number{};
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number, base);
        if (error != std::errc{} || end != digits.data() + digits.size()) {
            return std::nullopt;
        }
        return number;
    }

    std::optional<DebugTarget> ParseTarget(const nlohmann::json &params) {
        if (!params.contains("target")) {
            return DebugTarget::Sh2Master;
        }
        if (!params["target"].is_string()) {
            return std::nullopt;
        }
        const std::string &target = params["target"].get_ref<const std::string &>();
        if (target == ToString(DebugTarget::Sh2Master)) {
            return DebugTarget::Sh2Master;
        }
        if (target == ToString(DebugTarget::Sh2Slave)) {
            return DebugTarget::Sh2Slave;
        }
        return std::nullopt;
    }

    nlohmann::json TargetJson(DebugTarget target) {
        return std::string{ToString(target)};
    }

} // namespace

DebugService::DebugService(HeadlessConfig config)
    : m_config(std::move(config))
    , m_saturn(std::make_unique<ymir::Saturn>()) {}

DebugService::~DebugService() {
    StopExecutionThread();
    m_saturn.reset();
}

bool DebugService::Initialize(std::string &outError) {
    outError.clear();
    if (m_config.ipl_path.empty()) {
        outError = "IPL path is required";
        return false;
    }

    try {
        std::ifstream iplFile{m_config.ipl_path, std::ios::binary | std::ios::ate};
        if (!iplFile) {
            outError = "failed to open IPL image: " + m_config.ipl_path.string();
            return false;
        }

        const auto iplSize = iplFile.tellg();
        if (iplSize != static_cast<std::streamoff>(ymir::sys::kIPLSize)) {
            outError = "IPL size mismatch: expected " + std::to_string(ymir::sys::kIPLSize) + " bytes, got " +
                       std::to_string(static_cast<std::streamoff>(iplSize));
            return false;
        }

        std::vector<uint8_t> ipl(ymir::sys::kIPLSize);
        iplFile.seekg(0);
        if (!iplFile.read(reinterpret_cast<char *>(ipl.data()), static_cast<std::streamsize>(ipl.size()))) {
            outError = "failed to read IPL image: " + m_config.ipl_path.string();
            return false;
        }
        m_saturn->LoadIPL(std::span<uint8_t, ymir::sys::kIPLSize>{ipl.data(), ipl.size()});
        // Headless automation needs a deterministic standard pad for BIOS
        // first-boot prompts and later game input pulses.
        m_saturn->SMPC.GetPeripheralPort1().ConnectControlPad();

        if (m_config.bram_path) {
            std::error_code error;
            m_saturn->LoadInternalBackupMemoryImage(*m_config.bram_path, false, error);
            if (error) {
                outError = "failed to load backup RAM image '" + m_config.bram_path->string() + "': " + error.message();
                return false;
            }
        }

        if (m_config.game_path) {
            ymir::media::Disc disc;
            std::string loaderError;
            const bool loaded = ymir::media::LoadDisc(
                *m_config.game_path, disc, false, [&](ymir::media::MessageType type, std::string message) {
                    if (type == ymir::media::MessageType::Error || type == ymir::media::MessageType::NotValid) {
                        loaderError = std::move(message);
                    }
                });
            if (!loaded) {
                outError = "failed to load disc image '" + m_config.game_path->string() + "'";
                if (!loaderError.empty()) {
                    outError += ": " + loaderError;
                }
                return false;
            }
            m_saturn->LoadDisc(std::move(disc));
        }

        m_videoPixels.resize(static_cast<size_t>(vdp::kMaxResH) * vdp::kMaxResV);
        auto *videoRenderer = m_saturn->VDP.UseSoftwareRenderer();
        if (videoRenderer == nullptr) {
            outError = "failed to initialize the software video renderer";
            return false;
        }
        videoRenderer->EnableThreadedVDP1(false);
        videoRenderer->EnableThreadedVDP2(false);
        videoRenderer->EnableThreadedDeinterlacer(false);
        m_saturn->VDP.SetSoftwareRenderCallback({
            this,
            [](uint32 *framebuffer, uint32 width, uint32 height, void *context) {
                static_cast<DebugService *>(context)->CaptureVideoFrame(framebuffer, width, height);
            },
        });

        ClearVideoFrame();
        m_saturn->Reset(true);
        m_slaveTargetEnabled.store(IsTargetEnabled(DebugTarget::Sh2Slave), std::memory_order_release);
        m_state.store(ExecutionState::Paused, std::memory_order_release);
        StartExecutionThread();
        return true;
    } catch (const std::exception &error) {
        outError = std::string{"initialization failed: "} + error.what();
        m_state.store(ExecutionState::Crashed, std::memory_order_release);
        return false;
    }
}

nlohmann::json DebugService::CreateReadyNotification() const {
    const bool slaveEnabled = IsTargetEnabled(DebugTarget::Sh2Slave);
    return JsonRpcAdapter::CreateNotification(
        "instance.ready", {
                              {"protocol", kProtocolName},
                              {"protocol_version", kProtocolVersion},
                              {"transport", kStdioJsonRpcLinesTransport},
                              {"instance_id", m_instanceId},
                              {"state", ToString(GetState())},
                              {"capabilities", Capabilities()},
                              {"targets",
                               {{{"target", ToString(DebugTarget::Sh2Master)}, {"enabled", true}},
                                {{"target", ToString(DebugTarget::Sh2Slave)}, {"enabled", slaveEnabled}}}},
                          });
}

std::optional<nlohmann::json> DebugService::HandleLine(std::string_view line) {
    nlohmann::json parseError;
    auto request = JsonRpcAdapter::ParseRequest(line, parseError);
    if (!request) {
        return parseError;
    }

    nlohmann::json response;
    try {
        response = DispatchRequest(*request);
    } catch (const nlohmann::json::exception &error) {
        response = CreateDebugError(*request, JsonRpcError::InvalidParams, ErrorCode::InvalidParams, error.what());
    } catch (const std::exception &error) {
        response = CreateDebugError(*request, JsonRpcError::InternalError, ErrorCode::InternalError, error.what());
    }

    if (request->is_notification) {
        return std::nullopt;
    }
    return response;
}

std::vector<nlohmann::json> DebugService::TakePendingNotifications() {
    std::lock_guard lock{m_executionMutex};
    std::vector<nlohmann::json> notifications;
    notifications.swap(m_pendingNotifications);
    return notifications;
}

void DebugService::CaptureVideoFrame(const uint32_t *framebuffer, uint32_t width, uint32_t height) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (framebuffer == nullptr || width == 0 || height == 0 || pixelCount > m_videoPixels.size()) {
        return;
    }

    std::lock_guard lock{m_videoMutex};
    std::copy_n(framebuffer, pixelCount, m_videoPixels.begin());
    m_videoWidth = width;
    m_videoHeight = height;
    ++m_videoFrameSequence;
    m_hasVideoFrame = true;
}

void DebugService::ClearVideoFrame() {
    std::lock_guard lock{m_videoMutex};
    m_videoWidth = 0;
    m_videoHeight = 0;
    m_hasVideoFrame = false;
}

std::optional<CapturedVideoFrame> DebugService::SnapshotVideoFrame() {
    std::lock_guard lock{m_videoMutex};
    if (!m_hasVideoFrame) {
        return std::nullopt;
    }

    CapturedVideoFrame frame;
    frame.width = m_videoWidth;
    frame.height = m_videoHeight;
    frame.sequence = m_videoFrameSequence;
    const size_t pixelCount = static_cast<size_t>(frame.width) * frame.height;
    frame.xbgr8888Pixels.assign(m_videoPixels.begin(), m_videoPixels.begin() + pixelCount);
    return frame;
}

nlohmann::json DebugService::CreateDebugError(const JsonRpcRequest &request, JsonRpcError rpcError,
                                              ErrorCode debugError, std::string_view message) const {
    return JsonRpcAdapter::CreateErrorResponse(request.id, rpcError, message, {{"debug_code", ToString(debugError)}});
}

bool DebugService::IsTargetEnabled(DebugTarget target) const noexcept {
    if (target == DebugTarget::Sh2Master) {
        return true;
    }
    if (m_executionThread.joinable()) {
        return m_slaveTargetEnabled.load(std::memory_order_acquire);
    }
    return m_config.slave_enabled && m_saturn->slaveSH2Enabled;
}

void DebugService::StartExecutionThread() {
    std::lock_guard lock{m_executionMutex};
    if (m_executionThread.joinable()) {
        return;
    }
    m_workerStopRequested = false;
    m_executionThread = std::thread{[this] { ExecutionThreadMain(); }};
}

void DebugService::StopExecutionThread() {
    {
        std::lock_guard lock{m_executionMutex};
        m_workerStopRequested = true;
        m_continueRequested = false;
        m_pauseRequested = false;
    }
    m_executionCondition.notify_all();
    m_pausedCondition.notify_all();
    if (m_executionThread.joinable()) {
        m_executionThread.join();
    }
}

void DebugService::QueueStoppedNotificationLocked(StopReason reason) {
    ++m_stopSequence;
    m_pendingNotifications.push_back(
        JsonRpcAdapter::CreateNotification("instance.stopped", {
                                                                   {"instance_id", m_instanceId},
                                                                   {"reason", ToString(reason)},
                                                                   {"target", ToString(DebugTarget::Sh2Master)},
                                                                   {"pc", m_saturn->masterSH2.GetProbe().PC()},
                                                                   {"sequence", m_stopSequence},
                                                               }));
}

void DebugService::ExecutionThreadMain() {
    for (;;) {
        std::unique_lock lock{m_executionMutex};
        m_executionCondition.wait(lock, [&] { return m_workerStopRequested || m_continueRequested; });
        if (m_workerStopRequested) {
            return;
        }

        if (m_pauseRequested) {
            m_continueRequested = false;
            m_pauseRequested = false;
            m_state.store(ExecutionState::Paused, std::memory_order_release);
            QueueStoppedNotificationLocked(StopReason::Pause);
            lock.unlock();
            m_pausedCondition.notify_all();
            continue;
        }

        lock.unlock();
        try {
            m_saturn->RunFrame();
            m_slaveTargetEnabled.store(m_config.slave_enabled && m_saturn->slaveSH2Enabled, std::memory_order_release);
        } catch (...) {
            lock.lock();
            m_continueRequested = false;
            m_pauseRequested = false;
            m_state.store(ExecutionState::Crashed, std::memory_order_release);
            QueueStoppedNotificationLocked(StopReason::Error);
            lock.unlock();
            m_pausedCondition.notify_all();
            return;
        }

        lock.lock();
        if (m_pauseRequested) {
            m_continueRequested = false;
            m_pauseRequested = false;
            m_state.store(ExecutionState::Paused, std::memory_order_release);
            QueueStoppedNotificationLocked(StopReason::Pause);
            lock.unlock();
            m_pausedCondition.notify_all();
        }
    }
}

nlohmann::json DebugService::DispatchRequest(const JsonRpcRequest &request) {
    const auto success = [&](nlohmann::json result) {
        return JsonRpcAdapter::CreateSuccessResponse(request.id, result);
    };
    const auto invalidParams = [&](ErrorCode code, std::string_view message) {
        return CreateDebugError(request, JsonRpcError::InvalidParams, code, message);
    };
    const auto serverError = [&](ErrorCode code, std::string_view message) {
        return CreateDebugError(request, JsonRpcError::ServerError, code, message);
    };
    const auto requirePaused = [&]() -> std::optional<nlohmann::json> {
        if (GetState() != ExecutionState::Paused) {
            return serverError(ErrorCode::InvalidState, "operation requires a paused instance");
        }
        return std::nullopt;
    };
    const auto captureVideo = [&](bool includeImage) {
        const auto frame = SnapshotVideoFrame();
        if (!frame) {
            return serverError(ErrorCode::NoFrame, "no completed video frame is available");
        }

        const auto rgba8888 = ConvertToRGBA8888(*frame);
        nlohmann::json result{
            {"sequence", frame->sequence}, {"width", frame->width},        {"height", frame->height},
            {"pixel_format", "rgba8888"},  {"hash_algorithm", "xxh3-128"}, {"hash", HashRGBA8888(rgba8888)},
        };
        if (includeImage) {
            const auto png = EncodePNG(rgba8888, frame->width, frame->height);
            result["mime_type"] = "image/png";
            result["encoding"] = "base64";
            result["byte_count"] = png.size();
            result["data"] = EncodeBase64(png);
        }
        return success(std::move(result));
    };

    if (request.method == ToString(CommandMethod::DebugVersion)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "debug.version takes no parameters");
        }
        return success({
            {"protocol", kProtocolName},
            {"protocol_version", kProtocolVersion},
            {"transport", kStdioJsonRpcLinesTransport},
            {"application", {{"name", "ymir-headless"}, {"version", ymir::version::string}, {"git_sha", "unknown"}}},
            {"capabilities", Capabilities()},
        });
    }

    if (request.method == ToString(CommandMethod::InstanceStatus)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "instance.status takes no parameters");
        }
        return success({
            {"protocol", kProtocolName},
            {"instance_id", m_instanceId},
            {"state", ToString(GetState())},
            {"slave_enabled", IsTargetEnabled(DebugTarget::Sh2Slave)},
            {"capabilities", Capabilities()},
        });
    }

    if (request.method == ToString(CommandMethod::InstanceShutdown)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "instance.shutdown takes no parameters");
        }
        StopExecutionThread();
        m_state.store(ExecutionState::Stopped, std::memory_order_release);
        m_shutdownRequested.store(true, std::memory_order_release);
        return success({{"state", ToString(GetState())}});
    }

    if (request.method == ToString(CommandMethod::ExecContinue)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "exec.continue takes no parameters");
        }
        if (auto error = requirePaused()) {
            return *error;
        }

        {
            std::lock_guard lock{m_executionMutex};
            m_pauseRequested = false;
            m_continueRequested = true;
            m_state.store(ExecutionState::Running, std::memory_order_release);
        }
        m_executionCondition.notify_one();
        return success({{"state", ToString(GetState())}});
    }

    if (request.method == ToString(CommandMethod::ExecPause)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "exec.pause takes no parameters");
        }

        std::unique_lock lock{m_executionMutex};
        if (GetState() != ExecutionState::Running || !m_continueRequested) {
            return serverError(ErrorCode::InvalidState, "operation requires a continuously running instance");
        }
        m_pauseRequested = true;
        m_executionCondition.notify_one();
        m_pausedCondition.wait(lock, [&] { return m_workerStopRequested || GetState() != ExecutionState::Running; });
        if (GetState() == ExecutionState::Crashed) {
            return serverError(ErrorCode::InternalError, "emulator execution failed while pausing");
        }
        return success({{"state", ToString(GetState())}});
    }

    if (request.method == ToString(CommandMethod::ExecRunFor)) {
        if (!HasOnlyKeys(request.params, {"frames"}) || !request.params.contains("frames")) {
            return invalidParams(ErrorCode::InvalidParams, "exec.run_for expects an object containing frames");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        const auto frames = ParseUint32(request.params["frames"]);
        if (!frames || *frames == 0 || *frames > kMaxRunForFrames) {
            return invalidParams(ErrorCode::InvalidParams, "frames must be between 1 and 3600");
        }

        uint32_t framesAdvanced = 0;
        m_state.store(ExecutionState::Running, std::memory_order_release);
        try {
            for (; framesAdvanced < *frames; ++framesAdvanced) {
                m_saturn->RunFrame();
                // exec.run_for is deliberately synchronous for deterministic
                // debugging, but CD-block host I/O is serviced by another
                // host thread. Yield after a completed frame so a long bounded
                // run does not starve that worker and stall the BIOS handoff.
                // This affects host scheduling only, never emulated cycles.
                std::this_thread::yield();
            }
            m_slaveTargetEnabled.store(m_config.slave_enabled && m_saturn->slaveSH2Enabled, std::memory_order_release);
            m_state.store(ExecutionState::Paused, std::memory_order_release);
            std::lock_guard lock{m_executionMutex};
            QueueStoppedNotificationLocked(StopReason::FrameLimit);
        } catch (...) {
            m_state.store(ExecutionState::Crashed, std::memory_order_release);
            std::lock_guard lock{m_executionMutex};
            QueueStoppedNotificationLocked(StopReason::Error);
            throw;
        }

        return success({
            {"state", ToString(GetState())},
            {"reason", ToString(StopReason::FrameLimit)},
            {"frames_requested", *frames},
            {"frames_advanced", framesAdvanced},
        });
    }

    if (request.method == ToString(CommandMethod::ExecReset)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "exec.reset takes no parameters");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        ClearVideoFrame();
        m_saturn->Reset(true);
        m_slaveTargetEnabled.store(m_config.slave_enabled && m_saturn->slaveSH2Enabled, std::memory_order_release);
        return success({{"state", ToString(GetState())}});
    }

    if (request.method == ToString(CommandMethod::VideoFrameHash)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "video.frame_hash takes no parameters");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        return captureVideo(false);
    }

    if (request.method == ToString(CommandMethod::VideoCapture)) {
        if (!HasNoParams(request.params)) {
            return invalidParams(ErrorCode::InvalidParams, "video.capture takes no parameters");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        return captureVideo(true);
    }

    if (request.method == ToString(CommandMethod::ExecStepI)) {
        if (!HasOnlyKeys(request.params, {"target"})) {
            return invalidParams(ErrorCode::InvalidParams, "exec.stepi expects an object containing only target");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        const auto target = ParseTarget(request.params);
        if (!target) {
            return invalidParams(ErrorCode::InvalidTarget, "target must be sh2.master or sh2.slave");
        }
        if (!IsTargetEnabled(*target)) {
            return serverError(ErrorCode::TargetDisabled, "target is not currently enabled");
        }

        auto &cpu = *target == DebugTarget::Sh2Master ? m_saturn->masterSH2 : m_saturn->slaveSH2;
        const uint32_t pcBefore = cpu.GetProbe().PC();
        const bool counterpartAdvanced = *target == DebugTarget::Sh2Slave || m_saturn->slaveSH2Enabled;
        const uint64_t cycles =
            *target == DebugTarget::Sh2Master ? m_saturn->StepMasterSH2() : m_saturn->StepSlaveSH2();
        const uint32_t pcAfter = cpu.GetProbe().PC();

        return success({
            {"reason", ToString(StopReason::Step)},
            {"target", TargetJson(*target)},
            {"pc_before", pcBefore},
            {"pc_after", pcAfter},
            {"cycles_advanced",
             static_cast<uint32_t>(std::min<uint64_t>(cycles, std::numeric_limits<uint32_t>::max()))},
            {"counterpart_advanced", counterpartAdvanced},
            {"breakpoint_id", nullptr},
        });
    }

    if (request.method == ToString(CommandMethod::RegsRead)) {
        if (!HasOnlyKeys(request.params, {"target"})) {
            return invalidParams(ErrorCode::InvalidParams, "regs.read expects an object containing only target");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        const auto target = ParseTarget(request.params);
        if (!target) {
            return invalidParams(ErrorCode::InvalidTarget, "target must be sh2.master or sh2.slave");
        }
        if (!IsTargetEnabled(*target)) {
            return serverError(ErrorCode::TargetDisabled, "target is not currently enabled");
        }

        const auto &cpu = *target == DebugTarget::Sh2Master ? m_saturn->masterSH2 : m_saturn->slaveSH2;
        const auto &probe = cpu.GetProbe();
        const auto sr = probe.SR();
        const auto mac = probe.MAC();
        nlohmann::json registers = nlohmann::json::array();
        for (const uint32_t value : probe.R()) {
            registers.push_back(value);
        }

        return success({
            {"target", TargetJson(*target)},
            {"r", std::move(registers)},
            {"pc", probe.PC()},
            {"pr", probe.PR()},
            {"sr", sr.u32},
            {"sr_t", static_cast<bool>(sr.T)},
            {"sr_s", static_cast<bool>(sr.S)},
            {"sr_ilevel", sr.ILevel},
            {"sr_q", static_cast<bool>(sr.Q)},
            {"sr_m", static_cast<bool>(sr.M)},
            {"gbr", probe.GBR()},
            {"vbr", probe.VBR()},
            {"mach", mac.H},
            {"macl", mac.L},
            {"is_delay_slot", probe.IsInDelaySlot()},
        });
    }

    if (request.method == ToString(CommandMethod::MemPeek)) {
        if (!HasOnlyKeys(request.params, {"target", "address", "count"}) || !request.params.contains("address") ||
            !request.params.contains("count")) {
            return invalidParams(ErrorCode::InvalidParams, "mem.peek expects target (optional), address and count");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        const auto target = ParseTarget(request.params);
        if (!target) {
            return invalidParams(ErrorCode::InvalidTarget, "target must be sh2.master or sh2.slave");
        }
        if (!IsTargetEnabled(*target)) {
            return serverError(ErrorCode::TargetDisabled, "target is not currently enabled");
        }

        const auto address = ParseUint32(request.params["address"]);
        const auto count = ParseUint32(request.params["count"]);
        if (!address || !count || *count == 0 || *count > kMaxPeekBytes) {
            return invalidParams(ErrorCode::MemoryOutOfRange, "count must be between 1 and 65536 bytes");
        }
        if (*address > std::numeric_limits<uint32_t>::max() - (*count - 1)) {
            return invalidParams(ErrorCode::MemoryOutOfRange, "memory range wraps the 32-bit address space");
        }

        const auto &cpu = *target == DebugTarget::Sh2Master ? m_saturn->masterSH2 : m_saturn->slaveSH2;
        const auto &probe = cpu.GetProbe();
        nlohmann::json data = nlohmann::json::array();
        for (uint32_t offset = 0; offset < *count; ++offset) {
            data.push_back(probe.MemPeekByte(*address + offset, true));
        }
        return success({
            {"target", TargetJson(*target)},
            {"address", *address},
            {"data", std::move(data)},
        });
    }

    if (request.method == ToString(CommandMethod::MemPoke)) {
        if (!HasOnlyKeys(request.params, {"target", "address", "data"}) || !request.params.contains("address") ||
            !request.params.contains("data")) {
            return invalidParams(ErrorCode::InvalidParams, "mem.poke expects target (optional), address and data");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        const auto target = ParseTarget(request.params);
        if (!target) {
            return invalidParams(ErrorCode::InvalidTarget, "target must be sh2.master or sh2.slave");
        }
        if (!IsTargetEnabled(*target)) {
            return serverError(ErrorCode::TargetDisabled, "target is not currently enabled");
        }
        const auto address = ParseUint32(request.params["address"]);
        if (!address || !request.params["data"].is_array() || request.params["data"].empty() ||
            request.params["data"].size() > kMaxPeekBytes ||
            *address > std::numeric_limits<uint32_t>::max() - (request.params["data"].size() - 1)) {
            return invalidParams(ErrorCode::MemoryOutOfRange, "data must contain 1..65536 bytes without address wrap");
        }

        auto &cpu = *target == DebugTarget::Sh2Master ? m_saturn->masterSH2 : m_saturn->slaveSH2;
        auto &probe = cpu.GetProbe();
        uint32_t offset = 0;
        for (const auto &item : request.params["data"]) {
            const auto value = ParseUint32(item);
            if (!value || *value > 0xFFU) {
                return invalidParams(ErrorCode::InvalidParams, "mem.poke data values must be bytes");
            }
            probe.MemPokeByte(*address + offset, static_cast<uint8_t>(*value), true);
            ++offset;
        }
        return success({
            {"target", TargetJson(*target)},
            {"address", *address},
            {"bytes_written", offset},
        });
    }

    if (request.method == ToString(CommandMethod::InputPulse)) {
        if (!HasOnlyKeys(request.params, {"port", "buttons"}) || !request.params.contains("buttons")) {
            return invalidParams(ErrorCode::InvalidParams, "input.pulse expects buttons and optional port");
        }
        if (auto error = requirePaused()) {
            return *error;
        }
        if (request.params.contains("port") &&
            (!request.params["port"].is_string() || request.params["port"].get<std::string>() != "port1")) {
            return invalidParams(ErrorCode::InvalidParams, "only port1 is supported");
        }
        const auto buttons = ParseUint32(request.params["buttons"]);
        if (!buttons || (*buttons & ~0xFFF8U) != 0U) {
            return invalidParams(ErrorCode::InvalidParams, "buttons must be a Saturn Button bitmask");
        }
        auto *pad = dynamic_cast<ymir::peripheral::ControlPad *>(
            &m_saturn->SMPC.GetPeripheralPort1().GetPeripheral());
        if (pad == nullptr) {
            return serverError(ErrorCode::InvalidState, "port1 is not connected to a control pad");
        }
        pad->SetButtons(static_cast<ymir::peripheral::Button>(*buttons));
        return success({{"port", "port1"}, {"buttons", *buttons}, {"state", ToString(GetState())}});
    }

    return JsonRpcAdapter::CreateMethodNotFoundResponse(request.id, request.method);
}

} // namespace ymir::debug
