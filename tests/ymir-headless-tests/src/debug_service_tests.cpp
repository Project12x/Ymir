#include <catch2/catch_test_macros.hpp>

#include <debug_service.hpp>

#include <ymir/sys/memory_defs.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef YMIR_HEADLESS_TEST_BINARY
    #error "YMIR_HEADLESS_TEST_BINARY must name the ymir-headless test binary"
#endif

namespace {

struct ScopedFile {
    std::filesystem::path path;

    explicit ScopedFile(std::filesystem::path filePath)
        : path(std::move(filePath)) {}

    ScopedFile(ScopedFile &&other) noexcept
        : path(std::exchange(other.path, {})) {}

    ScopedFile &operator=(ScopedFile &&) = delete;
    ScopedFile(const ScopedFile &) = delete;
    ScopedFile &operator=(const ScopedFile &) = delete;

    ~ScopedFile() {
        if (path.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

ScopedFile CreateZeroIPL() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    ScopedFile file{std::filesystem::temp_directory_path() /
                    ("ymir-headless-zero-ipl-" + std::to_string(suffix) + ".bin")};
    std::ofstream output{file.path, std::ios::binary};
    const std::vector<char> zeroes(ymir::sys::kIPLSize);
    output.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
    return file;
}

ScopedFile CreateTempFile(std::string_view name) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return ScopedFile{std::filesystem::temp_directory_path() /
                      ("ymir-headless-" + std::string{name} + "-" + std::to_string(suffix) + ".txt")};
}

std::string Quote(const std::filesystem::path &path) {
    return "\"" + path.string() + "\"";
}

} // namespace

TEST_CASE("DebugService reports the implemented protocol surface", "[debug-service]") {
    ymir::debug::DebugService service{ymir::debug::HeadlessConfig{}};

    const auto response = service.HandleLine(R"({"jsonrpc":"2.0","method":"debug.version","id":7})");
    REQUIRE(response.has_value());
    CHECK((*response)["id"] == 7);
    CHECK((*response)["result"]["protocol"] == "ymir-debug");
    CHECK((*response)["result"]["protocol_version"] == "0.3.0");
    CHECK((*response)["result"]["capabilities"] ==
          nlohmann::json{"sh2.master", "sh2.slave", "exec.continue", "exec.pause", "exec.run_for", "exec.stepi",
                         "exec.reset", "regs.read", "mem.peek", "video.frame_hash", "video.capture", "instance.status",
                         "instance.shutdown", "event.stopped"});

    const auto notification = service.HandleLine(R"({"jsonrpc":"2.0","method":"debug.version"})");
    CHECK_FALSE(notification.has_value());

    const auto unsupported = service.HandleLine(R"({"jsonrpc":"2.0","method":"disasm.at","id":8})");
    REQUIRE(unsupported.has_value());
    CHECK((*unsupported)["error"]["code"] == static_cast<int>(ymir::debug::JsonRpcError::MethodNotFound));
}

TEST_CASE("DebugService loads an IPL and exposes paused SH-2 state", "[debug-service]") {
    const auto ipl = CreateZeroIPL();
    REQUIRE(std::filesystem::file_size(ipl.path) == ymir::sys::kIPLSize);

    ymir::debug::HeadlessConfig config;
    config.ipl_path = ipl.path;
    ymir::debug::DebugService service{config};

    std::string error;
    REQUIRE(service.Initialize(error));
    CHECK(error.empty());
    CHECK(service.GetState() == ymir::debug::ExecutionState::Paused);

    const auto ready = service.CreateReadyNotification();
    CHECK(ready["method"] == "instance.ready");
    CHECK(ready["params"]["state"] == "paused");
    CHECK(ready["params"]["targets"][0]["enabled"] == true);
    CHECK(ready["params"]["targets"][1]["enabled"] == false);

    const auto noFrame = service.HandleLine(R"({"jsonrpc":"2.0","method":"video.frame_hash","id":"no-frame"})");
    REQUIRE(noFrame.has_value());
    CHECK((*noFrame)["error"]["data"]["debug_code"] == "no_frame");

    const auto registers =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"regs.read","params":{"target":"sh2.master"},"id":"regs"})");
    REQUIRE(registers.has_value());
    CHECK((*registers)["id"] == "regs");
    CHECK((*registers)["result"]["target"] == "sh2.master");
    CHECK((*registers)["result"]["r"].size() == 16);

    const auto memory = service.HandleLine(
        R"({"jsonrpc":"2.0","method":"mem.peek","params":{"address":"0x00000000","count":4},"id":9})");
    REQUIRE(memory.has_value());
    CHECK((*memory)["result"]["address"] == 0);
    CHECK((*memory)["result"]["data"] == nlohmann::json{0, 0, 0, 0});

    const auto step =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.stepi","params":{"target":"sh2.master"},"id":"step"})");
    REQUIRE(step.has_value());
    CHECK((*step)["result"]["reason"] == "step");
    CHECK((*step)["result"]["target"] == "sh2.master");
    CHECK((*step)["result"]["cycles_advanced"].get<uint32_t>() > 0);

    const auto runFor =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.run_for","params":{"frames":2},"id":"run-for"})");
    REQUIRE(runFor.has_value());
    CHECK((*runFor)["result"]["state"] == "paused");
    CHECK((*runFor)["result"]["reason"] == "frame_limit");
    CHECK((*runFor)["result"]["frames_advanced"] == 2);

    const auto frameHash = service.HandleLine(R"({"jsonrpc":"2.0","method":"video.frame_hash","id":"frame-hash"})");
    REQUIRE(frameHash.has_value());
    CHECK((*frameHash)["result"]["width"].get<uint32_t>() > 0);
    CHECK((*frameHash)["result"]["height"].get<uint32_t>() > 0);
    CHECK((*frameHash)["result"]["pixel_format"] == "rgba8888");
    CHECK((*frameHash)["result"]["hash_algorithm"] == "xxh3-128");
    CHECK((*frameHash)["result"]["hash"].get<std::string>().size() == 32);

    const auto capture = service.HandleLine(R"({"jsonrpc":"2.0","method":"video.capture","id":"capture"})");
    REQUIRE(capture.has_value());
    CHECK((*capture)["result"]["sequence"] == (*frameHash)["result"]["sequence"]);
    CHECK((*capture)["result"]["hash"] == (*frameHash)["result"]["hash"]);
    CHECK((*capture)["result"]["mime_type"] == "image/png");
    CHECK((*capture)["result"]["encoding"] == "base64");
    CHECK((*capture)["result"]["byte_count"].get<size_t>() > 8);
    CHECK((*capture)["result"]["data"].get<std::string>().starts_with("iVBORw0KGgo"));

    auto notifications = service.TakePendingNotifications();
    REQUIRE(notifications.size() == 1);
    CHECK(notifications[0]["method"] == "instance.stopped");
    CHECK(notifications[0]["params"]["reason"] == "frame_limit");
    CHECK(notifications[0]["params"]["sequence"] == 1);

    const auto resume = service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.continue","id":"resume"})");
    REQUIRE(resume.has_value());
    CHECK((*resume)["result"]["state"] == "running");

    const auto runningStatus = service.HandleLine(R"({"jsonrpc":"2.0","method":"instance.status","id":"status"})");
    REQUIRE(runningStatus.has_value());
    CHECK((*runningStatus)["result"]["state"] == "running");

    const auto duplicateResume =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.continue","id":"duplicate-resume"})");
    REQUIRE(duplicateResume.has_value());
    CHECK((*duplicateResume)["error"]["data"]["debug_code"] == "invalid_state");

    const auto readWhileRunning =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"regs.read","params":{"target":"sh2.master"},"id":"busy"})");
    REQUIRE(readWhileRunning.has_value());
    CHECK((*readWhileRunning)["error"]["data"]["debug_code"] == "invalid_state");

    const auto captureWhileRunning =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"video.capture","id":"capture-busy"})");
    REQUIRE(captureWhileRunning.has_value());
    CHECK((*captureWhileRunning)["error"]["data"]["debug_code"] == "invalid_state");

    const auto pause = service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.pause","id":"pause"})");
    REQUIRE(pause.has_value());
    CHECK((*pause)["result"]["state"] == "paused");
    notifications = service.TakePendingNotifications();
    REQUIRE(notifications.size() == 1);
    CHECK(notifications[0]["params"]["reason"] == "pause");
    CHECK(notifications[0]["params"]["sequence"] == 2);

    const auto invalidRunFor = service.HandleLine(
        R"({"jsonrpc":"2.0","method":"exec.run_for","params":{"frames":3601},"id":"too-many-frames"})");
    REQUIRE(invalidRunFor.has_value());
    CHECK((*invalidRunFor)["error"]["data"]["debug_code"] == "invalid_params");

    const auto badRange =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"mem.peek","params":{"address":0,"count":65537},"id":10})");
    REQUIRE(badRange.has_value());
    CHECK((*badRange)["error"]["data"]["debug_code"] == "memory_out_of_range");

    const auto resumeForShutdown =
        service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.continue","id":"resume-for-shutdown"})");
    REQUIRE(resumeForShutdown.has_value());
    CHECK((*resumeForShutdown)["result"]["state"] == "running");

    const auto shutdown = service.HandleLine(R"({"jsonrpc":"2.0","method":"instance.shutdown","id":11})");
    REQUIRE(shutdown.has_value());
    CHECK((*shutdown)["result"]["state"] == "stopped");
    CHECK(service.ShutdownRequested());
}

TEST_CASE("DebugService invalidates captured frames on reset", "[debug-service][video]") {
    const auto ipl = CreateZeroIPL();
    ymir::debug::HeadlessConfig config;
    config.ipl_path = ipl.path;
    ymir::debug::DebugService service{config};

    std::string error;
    REQUIRE(service.Initialize(error));
    const auto runFor = service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.run_for","params":{"frames":1},"id":1})");
    REQUIRE(runFor.has_value());
    const auto captured = service.HandleLine(R"({"jsonrpc":"2.0","method":"video.frame_hash","id":2})");
    REQUIRE(captured.has_value());
    REQUIRE((*captured).contains("result"));

    const auto reset = service.HandleLine(R"({"jsonrpc":"2.0","method":"exec.reset","id":3})");
    REQUIRE(reset.has_value());
    const auto staleFrame = service.HandleLine(R"({"jsonrpc":"2.0","method":"video.frame_hash","id":4})");
    REQUIRE(staleFrame.has_value());
    CHECK((*staleFrame)["error"]["data"]["debug_code"] == "no_frame");
}

TEST_CASE("ymir-headless reserves stdout for JSON-RPC", "[debug-service][process]") {
    const auto ipl = CreateZeroIPL();
    const auto requests = CreateTempFile("requests");
    const auto protocolOutput = CreateTempFile("protocol");
    const auto diagnosticOutput = CreateTempFile("diagnostics");

    {
        std::ofstream output{requests.path};
        output << R"({"jsonrpc":"2.0","method":"debug.version","id":1})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"regs.read","params":{"target":"sh2.master"},"id":2})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"exec.stepi","params":{"target":"sh2.master"},"id":3})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"exec.run_for","params":{"frames":1},"id":4})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"video.frame_hash","id":5})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"video.capture","id":6})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"exec.continue","id":7})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"regs.read","params":{"target":"sh2.master"},"id":8})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"exec.pause","id":9})" << '\n';
        output << R"({"jsonrpc":"2.0","method":"instance.shutdown","id":10})" << '\n';
    }

    const std::filesystem::path executable{YMIR_HEADLESS_TEST_BINARY};
    const std::string rawCommand = Quote(executable) + " --ipl " + Quote(ipl.path) + " < " + Quote(requests.path) +
                                   " > " + Quote(protocolOutput.path) + " 2> " + Quote(diagnosticOutput.path);
#if defined(_WIN32)
    const std::string command = "\"" + rawCommand + "\"";
#else
    const std::string &command = rawCommand;
#endif
    INFO(command);
    REQUIRE(std::system(command.c_str()) == 0);

    std::ifstream protocol{protocolOutput.path};
    std::vector<nlohmann::json> messages;
    for (std::string line; std::getline(protocol, line);) {
        REQUIRE_NOTHROW(messages.push_back(nlohmann::json::parse(line)));
    }

    REQUIRE(messages.size() == 13);
    CHECK(messages[0]["method"] == "instance.ready");

    std::vector<int> responseIds;
    std::vector<std::string> stopReasons;
    for (const auto &message : messages) {
        if (message.contains("id") && message["id"].is_number_integer()) {
            responseIds.push_back(message["id"].get<int>());
        }
        if (message.value("method", std::string{}) == "instance.stopped") {
            stopReasons.push_back(message["params"]["reason"].get<std::string>());
        }
    }
    CHECK(responseIds == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    CHECK(stopReasons == std::vector<std::string>{"frame_limit", "pause"});
    CHECK(messages[4]["id"] == 4);
    CHECK(messages[5]["method"] == "instance.stopped");
    CHECK(messages[6]["result"]["hash_algorithm"] == "xxh3-128");
    CHECK(messages[7]["result"]["data"].get<std::string>().starts_with("iVBORw0KGgo"));
    CHECK(messages[11]["method"] == "instance.stopped");

    std::ifstream diagnostics{diagnosticOutput.path};
    const std::string diagnosticText{std::istreambuf_iterator<char>{diagnostics}, {}};
    CHECK(diagnosticText.find("ymir-headless: ipl:") != std::string::npos);
}
