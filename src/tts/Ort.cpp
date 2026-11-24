// SPDX-License-Identifier: GPL-3.0-only
#include "Ort.h"

#include "../../third_party/onnxruntime/onnxruntime_c_api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <numeric>

namespace veyra::tts {
namespace {

// The oldest API version whose calls are used here (ONNX Runtime 1.14). A newer runtime
// answers for any older version too; the vendored header only adds calls at the end.
constexpr uint32_t oldestApi = 14;

void* openLibrary(const std::filesystem::path& file) {
#ifdef _WIN32
    // By absolute path, with its own folder first for its dependencies, as RIFE loads its runtime.
    return LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    return dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void closeLibrary(void* library) {
    if (!library) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

} // namespace

size_t Tensor::elements() const {
    return size_t(std::accumulate(shape.begin(), shape.end(), int64_t(1), std::multiplies<>()));
}

OrtRuntime::OrtRuntime(const std::filesystem::path& library) {
    library_ = openLibrary(library);
    if (!library_) throw OrtError("cannot load " + library.string());
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto base = reinterpret_cast<GetApiBase>(symbol(library_, "OrtGetApiBase"));
    if (!base || !base()) {
        closeLibrary(library_);
        throw OrtError("OrtGetApiBase missing in " + library.string());
    }
    version_ = base()->GetVersionString();
    for (uint32_t version = ORT_API_VERSION; version >= oldestApi && !api_; --version)
        api_ = base()->GetApi(version);
    if (!api_) {
        closeLibrary(library_);
        throw OrtError("ONNX Runtime " + version_ + " is too old");
    }
    check(api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "veyra-voice", &env_));
    check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu_));
}

OrtRuntime::~OrtRuntime() {
    if (api_) {
        if (cpu_) api_->ReleaseMemoryInfo(cpu_);
        if (env_) api_->ReleaseEnv(env_);
    }
    // The library stays loaded: ONNX Runtime keeps worker threads that must not lose their code.
}

void OrtRuntime::check(OrtStatus* status) const {
    if (!status) return;
    std::string message = api_->GetErrorMessage(status);
    api_->ReleaseStatus(status);
    throw OrtError(message);
}

OrtModel::OrtModel(const OrtRuntime& runtime, const std::filesystem::path& file, int threads, bool spin) : runtime_(runtime) {
    const auto& api = runtime.api();
    OrtSessionOptions* options = nullptr;
    runtime.check(api.CreateSessionOptions(&options));
    struct Release { const OrtApi& api; OrtSessionOptions* o; ~Release() { api.ReleaseSessionOptions(o); } } release{api, options};
    if (threads > 0) runtime.check(api.SetIntraOpNumThreads(options, threads));
    runtime.check(api.SetInterOpNumThreads(options, 1));
    if (!spin) runtime.check(api.AddSessionConfigEntry(options, "session.intra_op.allow_spinning", "0"));
    runtime.check(api.SetSessionExecutionMode(options, ORT_SEQUENTIAL));
    runtime.check(api.SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
#ifdef _WIN32
    runtime.check(api.CreateSession(runtime.env(), file.wstring().c_str(), options, &session_));
#else
    runtime.check(api.CreateSession(runtime.env(), file.string().c_str(), options, &session_));
#endif
    OrtAllocator* allocator = nullptr;
    runtime.check(api.GetAllocatorWithDefaultOptions(&allocator));
    size_t count = 0;
    runtime.check(api.SessionGetInputCount(session_, &count));
    for (size_t i = 0; i < count; ++i) {
        char* name = nullptr;
        runtime.check(api.SessionGetInputName(session_, i, allocator, &name));
        inputNames_.emplace_back(name);
        allocator->Free(allocator, name);
    }
}

OrtModel::~OrtModel() {
    if (session_) runtime_.api().ReleaseSession(session_);
}

std::vector<std::string> OrtModel::outputNames() const {
    const auto& api = runtime_.api();
    OrtAllocator* allocator = nullptr;
    runtime_.check(api.GetAllocatorWithDefaultOptions(&allocator));
    size_t count = 0;
    runtime_.check(api.SessionGetOutputCount(session_, &count));
    std::vector<std::string> names;
    for (size_t i = 0; i < count; ++i) {
        char* name = nullptr;
        runtime_.check(api.SessionGetOutputName(session_, i, allocator, &name));
        names.emplace_back(name);
        allocator->Free(allocator, name);
    }
    return names;
}

std::string OrtModel::metadata(const char* key) const {
    const auto& api = runtime_.api();
    OrtAllocator* allocator = nullptr;
    runtime_.check(api.GetAllocatorWithDefaultOptions(&allocator));
    OrtModelMetadata* meta = nullptr;
    runtime_.check(api.SessionGetModelMetadata(session_, &meta));
    char* value = nullptr;
    OrtStatus* status = api.ModelMetadataLookupCustomMetadataMap(meta, allocator, key, &value);
    api.ReleaseModelMetadata(meta);
    runtime_.check(status);
    if (!value) return {};
    std::string text(value);
    allocator->Free(allocator, value);
    return text;
}

Tensor OrtModel::run(const std::vector<std::pair<const char*, const Tensor*>>& inputs, const char* output) const {
    return std::move(runMany(inputs, {output}).front());
}

std::vector<Tensor> OrtModel::runMany(const std::vector<std::pair<const char*, const Tensor*>>& inputs,
                                      const std::vector<const char*>& outputs) const {
    const auto& api = runtime_.api();
    std::vector<const char*> names;
    std::vector<OrtValue*> values;
    struct Values {
        const OrtApi& api; std::vector<OrtValue*>& list;
        ~Values() { for (auto* v : list) if (v) api.ReleaseValue(v); }
    } releaseInputs{api, values};
    for (const auto& [name, tensor] : inputs) {
        OrtValue* value = nullptr;
        const bool isFloat = tensor->type == Tensor::Type::Float;
        void* data = isFloat ? static_cast<void*>(const_cast<float*>(tensor->floats.data()))
                             : static_cast<void*>(const_cast<int64_t*>(tensor->ints.data()));
        const size_t bytes = tensor->elements() * (isFloat ? sizeof(float) : sizeof(int64_t));
        runtime_.check(api.CreateTensorWithDataAsOrtValue(runtime_.cpu(), data, bytes, tensor->shape.data(),
            tensor->shape.size(), isFloat ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT : ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
            &value));
        names.push_back(name);
        values.push_back(value);
    }
    std::vector<OrtValue*> results(outputs.size(), nullptr);
    runtime_.check(api.Run(session_, nullptr, names.data(), values.data(), values.size(), outputs.data(),
                           outputs.size(), results.data()));
    Values releaseOutputs{api, results};
    std::vector<Tensor> tensors;
    for (size_t index = 0; index < results.size(); ++index) {
        OrtValue* result = results[index];
        const char* output = outputs[index];
        OrtTensorTypeAndShapeInfo* info = nullptr;
        runtime_.check(api.GetTensorTypeAndShape(result, &info));
        size_t count = 0;
        ONNXTensorElementDataType type{};
        Tensor out;
        OrtStatus* status = api.GetDimensionsCount(info, &count);
        if (!status) {
            out.shape.resize(count);
            status = api.GetDimensions(info, out.shape.data(), count);
        }
        if (!status) status = api.GetTensorElementType(info, &type);
        api.ReleaseTensorTypeAndShapeInfo(info);
        runtime_.check(status);
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) throw OrtError(std::string("output is not float: ") + output);
        float* data = nullptr;
        runtime_.check(api.GetTensorMutableData(result, reinterpret_cast<void**>(&data)));
        out.type = Tensor::Type::Float;
        out.floats.assign(data, data + out.elements());
        tensors.push_back(std::move(out));
    }
    return tensors;
}

} // namespace veyra::tts
