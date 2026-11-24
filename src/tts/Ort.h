// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// ONNX Runtime through its C API, loaded at run time from a given library file: the voice
// uses the onnxruntime.dll that live speech recognition already downloads (sherpa-onnx's
// runtime), so nothing is linked at build time and only the API header is vendored.
// Only calls present since the oldest API version asked for are used.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtStatus;

namespace veyra::tts {

struct OrtError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One tensor handed to or returned by a network.
struct Tensor {
    enum class Type { Float, Int64 } type = Type::Float;
    std::vector<int64_t> shape;
    std::vector<float> floats;
    std::vector<int64_t> ints;

    static Tensor of(std::vector<int64_t> shape, std::vector<float> values) {
        Tensor t; t.type = Type::Float; t.shape = std::move(shape); t.floats = std::move(values); return t;
    }
    static Tensor of(std::vector<int64_t> shape, std::vector<int64_t> values) {
        Tensor t; t.type = Type::Int64; t.shape = std::move(shape); t.ints = std::move(values); return t;
    }
    size_t elements() const;
};

class OrtRuntime {
public:
    // Throws OrtError when the library cannot be loaded or is too old.
    explicit OrtRuntime(const std::filesystem::path& library);
    ~OrtRuntime();
    OrtRuntime(const OrtRuntime&) = delete;
    OrtRuntime& operator=(const OrtRuntime&) = delete;

    const OrtApi& api() const { return *api_; }
    OrtEnv* env() const { return env_; }
    OrtMemoryInfo* cpu() const { return cpu_; }
    std::string version() const { return version_; }
    void check(OrtStatus* status) const; // throws with the runtime's message

private:
    void* library_ = nullptr;
    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtMemoryInfo* cpu_ = nullptr;
    std::string version_;
};

class OrtModel {
public:
    // `threads`: intra-op threads on the CPU (0 = the runtime's choice). `spin`: whether idle
    // threads keep spinning after a call (quicker for a network called back to back; RUAccent's
    // networks, called now and then, would only take the CPU from the voice).
    OrtModel(const OrtRuntime& runtime, const std::filesystem::path& file, int threads, bool spin = true);
    ~OrtModel();
    OrtModel(const OrtModel&) = delete;
    OrtModel& operator=(const OrtModel&) = delete;

    // Inputs by name, in the given order; returns the named output as float.
    Tensor run(const std::vector<std::pair<const char*, const Tensor*>>& inputs, const char* output) const;
    // The same with several float outputs, in the order asked for.
    std::vector<Tensor> runMany(const std::vector<std::pair<const char*, const Tensor*>>& inputs,
                                const std::vector<const char*>& outputs) const;
    const std::vector<std::string>& inputNames() const { return inputNames_; }
    // The network's output names, in its own order.
    std::vector<std::string> outputNames() const;
    // A custom metadata value the model file carries (PP-OCR recognisers keep their
    // alphabet under "character"); empty when there is none.
    std::string metadata(const char* key) const;

private:
    const OrtRuntime& runtime_;
    OrtSession* session_ = nullptr;
    std::vector<std::string> inputNames_;
};

} // namespace veyra::tts
