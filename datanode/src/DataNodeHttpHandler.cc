#include "DataNodeHttpHandler.h"
#include "Config.h"
#include "DataNode.h"
#include "LocalFileStorage.h"
#include "MasterClient.h"
#include "PrefetchCache.h"
#include "TokenManager.h"
#include "base/ThreadPool.h"
#include "net/Callbacks.h"
#include "net/HttpRequest.h"
#include "net/HttpResponse.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace {
constexpr uintmax_t kDownloadChunkSize = 1024 * 1024; // 1MB
constexpr uintmax_t kLearningLargeFileThreshold = 50ULL * 1024ULL * 1024ULL;
constexpr uintmax_t kVipPrefetchWindowBytes = 20ULL * 1024ULL * 1024ULL;
constexpr uintmax_t kLearningPrefetchWindowBytes = 10ULL * 1024ULL * 1024ULL;
constexpr uintmax_t kPreviewFileSizeLimit = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kPreviewWarmupHdThreshold =
    1024ULL * 1024ULL * 1024ULL; // 1GB 以内顺手预热 HD

std::mutex g_videoVariantMutex;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> g_videoVariantLocks;
std::unordered_map<std::string, bool> g_hlsGenerationInProgress;

uint64_t qosServiceWeight(const std::string &serviceLevel) {
    if (serviceLevel == "svip") {
        return 6;
    }
    if (serviceLevel == "vip") {
        return 3;
    }
    return 1;
}

bool isVipOrSvipLevel(const std::string &serviceLevel) {
    return serviceLevel == "vip" || serviceLevel == "svip";
}

bool isSvipLevel(const std::string &serviceLevel) {
    return serviceLevel == "svip";
}

fs::path buildCodeSnapshotPath(const std::string &uploadDir,
                               const std::string &serverFilename) {
    return fs::path(uploadDir) / ".code_snapshots" /
           (serverFilename + ".snapshot");
}

fs::path buildCodeSnapshotMetaPath(const std::string &uploadDir,
                                   const std::string &serverFilename) {
    return fs::path(uploadDir) / ".code_snapshots" /
           (serverFilename + ".snapshot.json");
}

fs::path buildCodeExperimentDir(const std::string &uploadDir,
                                const std::string &serverFilename) {
    return fs::path(uploadDir) / ".code_experiments" / serverFilename;
}

fs::path buildCodeExperimentPath(const std::string &uploadDir,
                                 const std::string &serverFilename,
                                 const std::string &branchId) {
    return buildCodeExperimentDir(uploadDir, serverFilename) /
           (branchId + ".experiment");
}

fs::path buildCodeExperimentMetaPath(const std::string &uploadDir,
                                     const std::string &serverFilename,
                                     const std::string &branchId) {
    return buildCodeExperimentDir(uploadDir, serverFilename) /
           (branchId + ".experiment.json");
}

bool isSafeExperimentId(const std::string &branchId) {
    if (branchId.empty() || branchId.size() > 80) {
        return false;
    }
    return std::all_of(branchId.begin(), branchId.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

std::string makeExperimentId() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return "exp_" + std::to_string(now) + "_" + std::to_string(::getpid());
}

nlohmann::json readCodeSnapshotMeta(const fs::path &metaPath) {
    if (!fs::exists(metaPath) || !fs::is_regular_file(metaPath)) {
        return nlohmann::json::object();
    }
    std::ifstream in(metaPath);
    if (!in.is_open()) {
        return nlohmann::json::object();
    }
    try {
        nlohmann::json meta;
        in >> meta;
        return meta;
    } catch (...) {
        return nlohmann::json::object();
    }
}

nlohmann::json readJsonFile(const fs::path &path) {
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        return nlohmann::json::object();
    }
    std::ifstream in(path);
    if (!in.is_open()) {
        return nlohmann::json::object();
    }
    try {
        nlohmann::json value;
        in >> value;
        return value;
    } catch (...) {
        return nlohmann::json::object();
    }
}

std::shared_ptr<TokenBucketRateLimiter>
buildRateLimiter(const TokenManager::QoSPolicy &qosPolicy, DataNode *datanode) {
    uint64_t rateLimitBps = qosPolicy.rate_limit_bps;
    uint64_t bucketCapacity = qosPolicy.bucket_capacity_bytes;
    bool throttleEnabled = qosPolicy.throttle_enabled;

    if (datanode) {
        const uint64_t serviceWeight = qosServiceWeight(qosPolicy.service_level);
        if (datanode->getEffectiveBandwidthLimitBps() > 0) {
            throttleEnabled = true;
            if (bucketCapacity == 0) {
                bucketCapacity = 256ULL * 1024ULL * serviceWeight;
            } else {
                bucketCapacity = std::max<uint64_t>(bucketCapacity,
                                                    64ULL * 1024ULL *
                                                        serviceWeight);
            }
            return std::make_shared<TokenBucketRateLimiter>(
                0, bucketCapacity,
                [datanode, serviceLevel = qosPolicy.service_level,
                 qosMode = qosPolicy.qos_mode]() {
                    return datanode->getWeightedRateLimitBps(serviceLevel,
                                                             qosMode);
                });
        }

        const uint64_t effectiveLimit =
            datanode->getEffectiveBandwidthLimitBps();
        if (effectiveLimit > 0) {
            throttleEnabled = true;
            rateLimitBps =
                rateLimitBps > 0 ? std::min(rateLimitBps, effectiveLimit)
                                 : effectiveLimit;
            bucketCapacity = bucketCapacity > 0
                                 ? std::min(bucketCapacity, effectiveLimit)
                                 : effectiveLimit;
        }
    }

    if (!throttleEnabled || rateLimitBps == 0 || bucketCapacity == 0) {
        return nullptr;
    }

    return std::make_shared<TokenBucketRateLimiter>(
        rateLimitBps, bucketCapacity);
}

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool isVideoFile(const std::string &filename) {
    const auto dotPos = filename.find_last_of('.');
    if (dotPos == std::string::npos || dotPos + 1 >= filename.size()) {
        return false;
    }
    const std::string ext = toLowerCopy(filename.substr(dotPos + 1));
    return ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov";
}

bool hasFfmpeg() {
    return ::system("command -v ffmpeg >/dev/null 2>&1") == 0;
}

std::string fnv1aHex(const std::string &input) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash = kOffsetBasis;
    for (unsigned char ch : input) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= kPrime;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

std::string makeVideoResponseFilename(const std::string &originalFilename,
                                      const std::string &quality,
                                      const std::string &watermarkMode) {
    const auto dotPos = originalFilename.find_last_of('.');
    const std::string baseName =
        dotPos == std::string::npos ? originalFilename
                                    : originalFilename.substr(0, dotPos);
    std::string suffix;
    if (quality == "sd") {
        suffix += "_sd";
    } else if (quality == "hd") {
        suffix += "_hd";
    }
    if (watermarkMode != "none") {
        suffix += "_wm";
    }
    if (suffix.empty()) {
        return originalFilename;
    }
    return baseName + suffix + ".mp4";
}

std::string buildHlsPackageKey(const std::string &sourcePath,
                               const std::string &quality) {
    const auto fileSize = fs::exists(sourcePath) ? fs::file_size(sourcePath) : 0;
    const auto mtime =
        fs::exists(sourcePath)
            ? fs::last_write_time(sourcePath).time_since_epoch().count()
            : 0;
    return fnv1aHex(sourcePath + "|" + std::to_string(fileSize) + "|" +
                    std::to_string(mtime) + "|hls|" + quality);
}

std::shared_ptr<std::mutex> getVariantLock(const std::string &key) {
    std::lock_guard<std::mutex> lock(g_videoVariantMutex);
    auto &lockPtr = g_videoVariantLocks[key];
    if (!lockPtr) {
        lockPtr = std::make_shared<std::mutex>();
    }
    return lockPtr;
}

bool tryMarkHlsGeneration(const std::string &key) {
    std::lock_guard<std::mutex> lock(g_videoVariantMutex);
    auto &flag = g_hlsGenerationInProgress[key];
    if (flag) {
        return false;
    }
    flag = true;
    return true;
}

void clearHlsGeneration(const std::string &key) {
    std::lock_guard<std::mutex> lock(g_videoVariantMutex);
    g_hlsGenerationInProgress.erase(key);
}

bool isHlsReady(const std::string &playlistPath) {
    if (!fs::exists(playlistPath) || fs::file_size(playlistPath) == 0) {
        return false;
    }

    std::ifstream playlist(playlistPath);
    std::string line;
    while (std::getline(playlist, line)) {
        if (!line.empty() && line[0] != '#') {
            return fs::exists(fs::path(playlistPath).parent_path() / line);
        }
    }
    return false;
}

std::string buildVideoFilter(const std::string &quality,
                             const std::string &watermarkMode,
                             const std::string & /*watermarkText*/,
                             const std::string &watermarkTextFile) {
    std::vector<std::string> filters;
    if (quality == "sd") {
        filters.emplace_back("scale=-2:480");
    } else if (quality == "hd") {
        filters.emplace_back("scale=-2:720");
    }

    if (watermarkMode != "none") {
        std::ostringstream filter;
        filter << "drawtext=textfile=" << watermarkTextFile
               << ":reload=0:fontcolor=white@0.34:fontsize=26"
               << ":box=1:boxcolor=black@0.18:boxborderw=12"
               << ":x=w-tw-28:y=h-th-28";
        filters.push_back(filter.str());
    }

    std::ostringstream joined;
    for (size_t i = 0; i < filters.size(); ++i) {
        if (i > 0) {
            joined << ",";
        }
        joined << filters[i];
    }
    return joined.str();
}

int runProcess(const std::vector<std::string> &args,
               const std::string &logPath) {
    pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int fd = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return status;
}

std::string quoteArg(const std::string &arg) {
    if (arg.empty()) {
        return "''";
    }

    bool needsQuotes = false;
    for (char ch : arg) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '\'' ||
            ch == '"' || ch == '\\' || ch == '(' || ch == ')' || ch == '[' ||
            ch == ']' || ch == '{' || ch == '}' || ch == '&' || ch == '|' ||
            ch == ';' || ch == '<' || ch == '>' || ch == '*' || ch == '?') {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes) {
        return arg;
    }

    std::string quoted = "'";
    for (char ch : arg) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += "'";
    return quoted;
}

std::string joinCommandForLog(const std::vector<std::string> &args) {
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << quoteArg(args[i]);
    }
    return oss.str();
}

std::string describeWaitStatus(int status) {
    if (status < 0) {
        return "waitpid_failed";
    }
    if (WIFEXITED(status)) {
        return "exit_code=" + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "signal=" + std::to_string(WTERMSIG(status));
    }
    if (WIFSTOPPED(status)) {
        return "stopped_signal=" + std::to_string(WSTOPSIG(status));
    }
    return "raw_status=" + std::to_string(status);
}

std::string readLogTail(const std::string &logPath, size_t maxBytes = 8192) {
    std::ifstream input(logPath, std::ios::binary);
    if (!input.is_open()) {
        return "";
    }

    input.seekg(0, std::ios::end);
    const std::streamoff fileSize = input.tellg();
    if (fileSize <= 0) {
        return "";
    }

    const std::streamoff start =
        fileSize > static_cast<std::streamoff>(maxBytes)
            ? fileSize - static_cast<std::streamoff>(maxBytes)
            : 0;
    input.seekg(start, std::ios::beg);

    std::string content(static_cast<size_t>(fileSize - start), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<size_t>(input.gcount()));
    return content;
}

bool commandOutputContains(const std::string &command,
                           const std::string &keyword) {
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return false;
    }

    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output.append(buffer);
    }
    ::pclose(pipe);
    return output.find(keyword) != std::string::npos;
}

struct VideoTranscodeConfig {
    bool enableGpu = true;
    bool fallbackToCpu = true;
    std::string gpuEncoder = "h264_nvenc";
    int gpuIndex = 0;
    std::string cpuEncoder = "libx264";
};

const VideoTranscodeConfig &getVideoTranscodeConfig() {
    static const VideoTranscodeConfig config = [] {
        VideoTranscodeConfig value;
        value.enableGpu =
            Config::instance().getBool("datanode.transcode.enable_gpu", true);
        value.fallbackToCpu =
            Config::instance().getBool("datanode.transcode.fallback_to_cpu",
                                       true);
        value.gpuEncoder = Config::instance().getString(
            "datanode.transcode.gpu_encoder", "h264_nvenc");
        value.gpuIndex =
            Config::instance().getInt("datanode.transcode.gpu_index", 0);
        value.cpuEncoder = Config::instance().getString(
            "datanode.transcode.cpu_encoder", "libx264");
        return value;
    }();
    return config;
}

bool isGpuEncoderAvailable(const std::string &encoder) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, bool> cache;

    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = cache.find(encoder);
    if (it != cache.end()) {
        return it->second;
    }

    const bool available = commandOutputContains(
        "ffmpeg -hide_banner -encoders 2>/dev/null", encoder);
    cache[encoder] = available;
    return available;
}

std::vector<std::string> buildTranscodeArgs(
    const std::string &sourcePath, const std::string &filter,
    const std::string &tmpOutputPath, const std::string &preset,
    const std::string &qualityValue, bool useGpu) {
    const auto &config = getVideoTranscodeConfig();

    std::vector<std::string> ffmpegArgs = {"ffmpeg",      "-hide_banner",
                                           "-loglevel",   "info",
                                           "-y",          "-threads",
                                           "0",           "-i",
                                           sourcePath};
    if (!filter.empty()) {
        ffmpegArgs.emplace_back("-vf");
        ffmpegArgs.push_back(filter);
    }

    if (useGpu) {
        const std::string nvPreset = preset == "ultrafast" ? "p1" : "p3";
        ffmpegArgs.insert(ffmpegArgs.end(),
                          {"-c:v",
                           config.gpuEncoder,
                           "-gpu",
                           std::to_string(config.gpuIndex),
                           "-preset",
                           nvPreset,
                           "-tune",
                           "ll",
                           "-rc",
                           "vbr",
                           "-cq",
                           qualityValue,
                           "-b:v",
                           "0",
                           "-pix_fmt",
                           "yuv420p"});
    } else {
        ffmpegArgs.insert(ffmpegArgs.end(),
                          {"-c:v", config.cpuEncoder, "-preset", preset,
                           "-crf", qualityValue});
    }

    ffmpegArgs.insert(ffmpegArgs.end(),
                      {"-c:a", "aac", "-movflags", "+faststart",
                       tmpOutputPath});
    return ffmpegArgs;
}

bool prepareVideoVariant(const std::string &sourcePath,
                         const std::string &cacheRoot,
                         const TokenManager::downloadTokenPayload &payload,
                         std::string &servedPath,
                         std::string &servedFilename,
                         std::string &errorMessage) {
    servedPath = sourcePath;
    servedFilename = payload.original_filename;

    if (!isVideoFile(payload.original_filename)) {
        return true;
    }

    if (payload.access_mode == "preview") {
        return true;
    }

    if (!fs::exists(sourcePath)) {
        errorMessage = "源视频文件不存在: " + sourcePath;
        LOG_WARN << "Video transcode source file missing. path=" << sourcePath
                 << ", original_filename=" << payload.original_filename
                 << ", server_filename=" << payload.server_filename;
        return false;
    }

    const std::string effectiveWatermarkMode = payload.watermark_mode;
    const std::string effectiveWatermarkText = payload.watermark_text;

    const bool needsTranscode = effectiveWatermarkMode != "none";
    if (!needsTranscode) {
        return true;
    }

    if (!hasFfmpeg()) {
        errorMessage = "当前 DataNode 未安装 ffmpeg，无法提供视频转码服务";
        return false;
    }

    fs::create_directories(cacheRoot);

    const auto fileSize = fs::exists(sourcePath) ? fs::file_size(sourcePath) : 0;
    const auto mtime =
        fs::exists(sourcePath)
            ? fs::last_write_time(sourcePath).time_since_epoch().count()
            : 0;
    const std::string variantKey = fnv1aHex(
        sourcePath + "|" + std::to_string(fileSize) + "|" +
        std::to_string(mtime) + "|" + payload.video_quality + "|" +
        effectiveWatermarkMode + "|" + effectiveWatermarkText + "|" +
        payload.access_mode);
    const std::string outputPath = cacheRoot + "/" + variantKey + ".mp4";
    const std::string tmpOutputPath = outputPath + ".tmp.mp4";
    const std::string watermarkTextPath = cacheRoot + "/" + variantKey + ".txt";
    const std::string logPath = cacheRoot + "/" + variantKey + ".log";

    servedFilename = makeVideoResponseFilename(payload.original_filename,
                                               payload.video_quality,
                                               effectiveWatermarkMode);

    if (fs::exists(outputPath) && fs::file_size(outputPath) > 0) {
        servedPath = outputPath;
        return true;
    }

    auto variantLock = getVariantLock(variantKey);
    std::lock_guard<std::mutex> guard(*variantLock);
    if (fs::exists(outputPath) && fs::file_size(outputPath) > 0) {
        servedPath = outputPath;
        return true;
    }

    if (effectiveWatermarkMode != "none") {
        std::ofstream watermarkFile(watermarkTextPath, std::ios::trunc);
        watermarkFile << (effectiveWatermarkText.empty()
                              ? std::string("Distributed-FM")
                              : effectiveWatermarkText);
    }

    const std::string filter = buildVideoFilter(payload.video_quality,
                                                effectiveWatermarkMode,
                                                effectiveWatermarkText,
                                                watermarkTextPath);

    const std::string preset = "veryfast";
    const std::string crf = "23";

    const auto &transcodeConfig = getVideoTranscodeConfig();
    const bool shouldTryGpu = transcodeConfig.enableGpu &&
                              isGpuEncoderAvailable(
                                  transcodeConfig.gpuEncoder);
    bool success = false;

    if (shouldTryGpu) {
        const auto gpuArgs = buildTranscodeArgs(sourcePath, filter,
                                                tmpOutputPath, preset, crf,
                                                true);
        LOG_INFO << "Trying dedicated GPU transcoding for watermarked video. "
                 << "encoder="
                 << transcodeConfig.gpuEncoder
                 << ", gpu_index=" << transcodeConfig.gpuIndex
                 << ", quality=" << payload.video_quality
                 << ", watermark_mode=" << effectiveWatermarkMode
                 << ", source=" << sourcePath << ", output=" << outputPath
                 << ", ffmpeg_log=" << logPath
                 << ", command=" << joinCommandForLog(gpuArgs);
        const int gpuStatus = runProcess(gpuArgs, logPath);
        success = gpuStatus == 0 && fs::exists(tmpOutputPath) &&
                  fs::file_size(tmpOutputPath) > 0;
        if (!success && fs::exists(tmpOutputPath)) {
            fs::remove(tmpOutputPath);
        }
        if (!success) {
            LOG_WARN << "Dedicated GPU transcoding failed for watermarked "
                        "video. status="
                     << gpuStatus << " (" << describeWaitStatus(gpuStatus)
                     << ")"
                     << ", encoder=" << transcodeConfig.gpuEncoder
                     << ", gpu_index=" << transcodeConfig.gpuIndex
                     << ", source=" << sourcePath
                     << ", ffmpeg_log=" << logPath
                     << ", ffmpeg_log_tail=" << readLogTail(logPath);
        }
    } else {
        LOG_INFO << "Skip dedicated GPU transcoding for watermarked video. "
                 << "enable_gpu=" << transcodeConfig.enableGpu
                 << ", gpu_encoder=" << transcodeConfig.gpuEncoder
                 << ", gpu_encoder_available="
                 << isGpuEncoderAvailable(transcodeConfig.gpuEncoder)
                 << ", source=" << sourcePath;
    }

    if (!success && (!shouldTryGpu || transcodeConfig.fallbackToCpu)) {
        if (shouldTryGpu) {
            LOG_INFO << "Falling back to CPU transcoding for watermarked "
                        "video. source="
                     << sourcePath;
        }
        const auto cpuArgs = buildTranscodeArgs(sourcePath, filter,
                                                tmpOutputPath, preset, crf,
                                                false);
        LOG_INFO << "Running CPU ffmpeg for watermarked video. encoder="
                 << transcodeConfig.cpuEncoder
                 << ", quality=" << payload.video_quality
                 << ", watermark_mode=" << effectiveWatermarkMode
                 << ", source=" << sourcePath << ", output=" << outputPath
                 << ", ffmpeg_log=" << logPath
                 << ", command=" << joinCommandForLog(cpuArgs);
        const int cpuStatus = runProcess(cpuArgs, logPath);
        success = cpuStatus == 0 && fs::exists(tmpOutputPath) &&
                  fs::file_size(tmpOutputPath) > 0;
        if (!success && fs::exists(tmpOutputPath)) {
            fs::remove(tmpOutputPath);
        }
        if (!success) {
            LOG_WARN << "CPU transcoding failed for watermarked video. status="
                     << cpuStatus << " (" << describeWaitStatus(cpuStatus)
                     << ")"
                     << ", encoder=" << transcodeConfig.cpuEncoder
                     << ", source=" << sourcePath
                     << ", ffmpeg_log=" << logPath
                     << ", ffmpeg_log_tail=" << readLogTail(logPath);
        }
    }

    if (!success) {
        errorMessage = shouldTryGpu
                           ? "GPU/CPU 转码均失败，请检查 DataNode 日志"
                           : "ffmpeg 处理失败，请检查 DataNode 日志";
        return false;
    }

    fs::rename(tmpOutputPath, outputPath);
    LOG_INFO << "Watermarked video generated successfully. source="
             << sourcePath << ", output=" << outputPath
             << ", watermark_mode=" << effectiveWatermarkMode
             << ", ffmpeg_log=" << logPath;
    servedPath = outputPath;
    return true;
}
} // namespace

// --------------FileUploadContext--------------
FileUploadContext::FileUploadContext(
    uint64_t fileID, const std::string &filename,
    std::shared_ptr<FileStorage> &&storage,
    std::shared_ptr<TokenBucketRateLimiter> rateLimiter)
    : fileID_(fileID), filename_(filename), storage_(std::move(storage)),
      rateLimiter_(std::move(rateLimiter)), totalBytes_(0),
      state_(State::kExpectHeaders), boundary_("") {
    if (!storage_) {
        LOG_ERROR << "FileStorage not initialized";
    }
    if (!storage_->open(filename_)) {
        throw std::runtime_error("Failed to open file: " + filename_);
    }
    LOG_INFO << "FileUploadContext created: " << filename;
}

FileUploadContext::~FileUploadContext() {
    if (storage_ && storage_->isOpen()) {
        storage_->close();
    }
}

// 写入数据：转发给LocalStorage
void FileUploadContext::writeData(const char *data, size_t len) {
    if (!storage_) {
        LOG_ERROR << "FileStorage not initialized";
        throw std::runtime_error("FileStorage not initialized");
    }
    try {
        storage_->write(data, len);
        totalBytes_ += len;
        LOG_INFO << "Wrote " << len
                 << "bytes to DataNode, total: " << totalBytes_;
    } catch (const std::exception &e) {
        LOG_ERROR << "FileStorage: write failed - " << e.what();
        throw; // 向上层抛出异常
    }
}

void FileUploadContext::throttleIfNeeded(size_t len) {
    if (rateLimiter_) {
        rateLimiter_->consume(len);
    }
}

bool FileUploadContext::releaseTransferCounter() {
    if (transferCounterReleased_) {
        return false;
    }
    transferCounterReleased_ = true;
    return true;
}

// --------------FileDownContext--------------
FileDownContext::FileDownContext(
    const std::string &filepath, const std::string &originalFilename,
    const std::string &sceneTag, const std::string &serviceLevel,
    std::shared_ptr<TokenBucketRateLimiter> rateLimiter,
    std::shared_ptr<PrefetchCache> prefetchCache, ThreadPool *ioThreadPool)
    : filepath_(filepath), originalFilename_(originalFilename),
      sceneTag_(sceneTag), serviceLevel_(serviceLevel), fd_(-1),
      rateLimiter_(std::move(rateLimiter)),
      prefetchCache_(std::move(prefetchCache)), fileSize_(0),
      currentPosition_(0), isComplete_(false), ioThreadPool_(ioThreadPool) {
    fileSize_ = fs::file_size(filepath_);
    fd_ = ::open(filepath_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        LOG_ERROR << "Failed to open file: " << filepath_
                  << ", errno=" << std::strerror(errno);
        throw std::runtime_error("Failed to open file: " + filepath_);
    }
    LOG_INFO << "Opening file for download: " << filepath_
             << ", size: " << fileSize_ << ", scene_tag=" << sceneTag_;
}

FileDownContext::~FileDownContext() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void FileDownContext::seekTo(uintmax_t position) {
    if (fd_ < 0) {
        throw std::runtime_error("File is not open: " + filepath_);
    }
    currentPosition_ = position;
    isComplete_ = false;
}

bool FileDownContext::readNextChunk(std::string &chunk) {
    if (fd_ < 0 || isComplete_) {
        return false;
    }

    uintmax_t remainingBytes = fileSize_ - currentPosition_;
    uintmax_t bytesToRead = std::min(kDownloadChunkSize, remainingBytes);
    if (bytesToRead == 0) {
        isComplete_ = true;
        return false;
    }

    std::shared_ptr<const PrefetchCache::Buffer> cachedBuffer;
    std::vector<char> buffer;
    bool cacheHit = false;

    // 如果是学习场景，先去缓存里找找看
    if (shouldUsePrefetchCache()) {
        cacheHit =
            prefetchCache_->get(buildCacheKey(currentPosition_), cachedBuffer);
        if (cacheHit && cachedBuffer && cachedBuffer->size() > bytesToRead) {
            chunk.assign(cachedBuffer->data(), bytesToRead);
        } else if (cacheHit && cachedBuffer) {
            chunk.assign(cachedBuffer->data(), cachedBuffer->size());
        }
    }

    if (!cacheHit) {
        buffer.resize(bytesToRead);
        ssize_t readBytes = ::pread(fd_, buffer.data(), bytesToRead,
                                    static_cast<off_t>(currentPosition_));
        if (readBytes <= 0) {
            isComplete_ = true;
            return false;
        }
        buffer.resize(static_cast<size_t>(readBytes));

        // 主线程读完磁盘后，如果开启了预取，顺手把这块数据也塞进缓存
        if (shouldUsePrefetchCache()) {
            prefetchCache_->put(buildCacheKey(currentPosition_), buffer,
                                isVipUser());
        }
        chunk.assign(buffer.data(), buffer.size());
    }

    currentPosition_ += chunk.size();
    if (currentPosition_ >= fileSize_) {
        isComplete_ = true;
    }

    // 核心创新点：触发异步预取！
    // 如果是学习场景，不仅把当前这块发给用户，还调用线程池把后面的数据提前读出来！
    if (shouldUsePrefetchCache()) {
        schedulePrefetchWindow(currentPosition_);
    }

    LOG_DEBUG << (cacheHit ? "Prefetch cache hit" : "Disk read")
              << ", bytes: " << chunk.size()
              << ", current position: " << currentPosition_ << "/" << fileSize_;
    return true;
}

bool FileDownContext::shouldUsePrefetchCache() const {
    if (!prefetchCache_) {
        return false;
    }

    if (isVipUser()) {
        return true;
    }

    return isLearningUser() && fileSize_ > kLearningLargeFileThreshold;
}

bool FileDownContext::isVipUser() const {
    return serviceLevel_ == "vip" || serviceLevel_ == "svip";
}

bool FileDownContext::isLearningUser() const { return sceneTag_ == "learning"; }

uintmax_t FileDownContext::getPrefetchWindowBytes() const {
    if (isVipUser()) {
        return kVipPrefetchWindowBytes;
    }
    if (isLearningUser()) {
        return kLearningPrefetchWindowBytes;
    }
    return 0;
}

uintmax_t FileDownContext::getPrefetchLowWaterBytes() const {
    const uintmax_t windowBytes = getPrefetchWindowBytes();
    return std::max<uintmax_t>(kDownloadChunkSize, windowBytes * 0.4);
}

std::string FileDownContext::buildCacheKey(uintmax_t offset) const {
    return filepath_ + "_" + std::to_string(offset);
}

void FileDownContext::schedulePrefetchWindow(uintmax_t nextOffset) {
    if (!shouldUsePrefetchCache() || nextOffset >= fileSize_) {
        return;
    }

    const uintmax_t lowWaterBytes = getPrefetchLowWaterBytes();
    const uintmax_t prefetchWindowBytes = getPrefetchWindowBytes();

    // 向全局缓存查询，从nextOffset 开始，后面连续有多少数据是安全的？
    uintmax_t safeBytes = prefetchCache_->getContinuousCachedBytes(
        filepath_, nextOffset, prefetchWindowBytes, kDownloadChunkSize);

    // 如果安全的数据量还高于低水位线，说明缓存充足，不需要唤醒后台线程
    if (safeBytes > lowWaterBytes) {
        LOG_INFO << "Global cache is sufficient, skip prefetch. safe_bytes="
                 << safeBytes << ", low_water=" << lowWaterBytes;
        return;
    }
    // 确保线程池指针不为空
    if (!ioThreadPool_) {
        LOG_WARN << "IO ThreadPool is null, skipping prefetch";
        return;
    }

    const std::string filepath = filepath_;
    const uintmax_t fileSize = fileSize_;
    const bool isVip = isVipUser();
    auto cache = prefetchCache_;

    uintmax_t actualStartOffset = nextOffset + safeBytes;

    ioThreadPool_->run([filepath, fileSize, actualStartOffset, safeBytes,
                        prefetchWindowBytes, isVip, cache]() {
        int fd = ::open(filepath.c_str(), O_RDONLY);
        if (fd < 0) {
            return;
        }

        uintmax_t offset = actualStartOffset;
        // 还要读多少字节(总窗口-已经安全的字节)
        uintmax_t remainingBytes = prefetchWindowBytes - safeBytes;
        uintmax_t prefetchedBytes = 0;
        while (offset < fileSize && prefetchedBytes < remainingBytes) {
            uintmax_t bytesToRead =
                std::min(kDownloadChunkSize, fileSize - offset);
            std::string key = filepath + "_" + std::to_string(offset);
            if (!cache->shouldAdmitPrefetch(static_cast<size_t>(bytesToRead),
                                            isVip)) {
                break;
            }
            if (!cache->tryMarkLoading(key)) {
                offset += bytesToRead;
                continue;
            }

            std::vector<char> buffer(bytesToRead);
            ssize_t readBytes = ::pread(fd, buffer.data(), bytesToRead,
                                        static_cast<off_t>(offset));
            if (readBytes <= 0) {
                cache->cancelLoading(key);
                offset += bytesToRead;
                continue;
            }

            buffer.resize(static_cast<size_t>(readBytes));
            cache->put(key, std::move(buffer), isVip);
            offset += static_cast<uintmax_t>(readBytes);
            prefetchedBytes += static_cast<uintmax_t>(readBytes);
            LOG_INFO << "prefetched:" << prefetchedBytes
                     << " bytes; total:" << cache->currentBytes() << " bytes";
        }

        ::close(fd);
    });
}

// --------------Handler--------------
DataNodeHttpHandler::DataNodeHttpHandler(DataNode *datanode, int numThreads)
    : uploadDir_("uploads"), datanode_(datanode),
      prefetchCache_(std::make_shared<PrefetchCache>(1000 * 1024 * 1024)),
      threadPool_("DNHttpHandlerThreadPool") {
    threadPool_.start(numThreads);
    maxConcurrentVideoTranscodes_ = static_cast<size_t>(
        std::max(1, Config::instance().getInt(
                        "datanode.transcode.max_background_jobs", 2)));
    // 创建上传目录
    if (!fs::exists(uploadDir_)) {
        LOG_DEBUG << "创建上传目录: " << uploadDir_;
        fs::create_directory(uploadDir_);
    }

    // 初始化路由表
    initRoutes();
    const auto &transcodeConfig = getVideoTranscodeConfig();
    LOG_INFO << "Video transcode config: enable_gpu="
             << transcodeConfig.enableGpu
             << ", gpu_encoder=" << transcodeConfig.gpuEncoder
             << ", gpu_index=" << transcodeConfig.gpuIndex
             << ", fallback_to_cpu=" << transcodeConfig.fallbackToCpu
             << ", max_background_jobs=" << maxConcurrentVideoTranscodes_
             << ", gpu_encoder_available="
             << isGpuEncoderAvailable(transcodeConfig.gpuEncoder);
    LOG_INFO << "Prefetch cache initialized, bytes="
             << prefetchCache_->currentBytes();
}

DataNodeHttpHandler::~DataNodeHttpHandler() {}

std::string DataNodeHttpHandler::getMimeType(const std::string &filename) {
    static std::unordered_map<std::string, std::string> mimeMap = {
        {".mp4", "video/mp4"},   {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".png", "image/png"},
        {".gif", "image/gif"},   {".pdf", "application/pdf"},
        {".txt", "text/plain"},  {".zip", "application/zip"},
        {".html", "text/html"},  {".mp3", "audio/mpeg"}};

    std::string ext;
    size_t pos = filename.find_last_of('.');
    if (pos != std::string::npos) {
        ext = filename.substr(pos);
    }

    if (mimeMap.count(ext)) {
        return mimeMap[ext];
    }

    return "application/octet-stream";
}

/// TODO: 初始化路由表
void DataNodeHttpHandler::initRoutes() {
    addRoute("/api/datanode/upload", HttpRequest::kOptions,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileUpload(conn, req, resp);
             });
    addRoute("/api/datanode/upload", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileUpload(conn, req, resp);
             });
    addRoute("/api/datanode/download", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleFileDownload(conn, req, resp);
             });
    addRoute("/api/datanode/text_preview", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextPreview(conn, req, resp);
             });
    addRoute("/api/datanode/text_edit", HttpRequest::kOptions,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextEdit(conn, req, resp);
             });
    addRoute("/api/datanode/text_edit", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextEdit(conn, req, resp);
             });
    addRoute("/api/datanode/text_snapshot", HttpRequest::kOptions,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextSnapshot(conn, req, resp);
             });
    addRoute("/api/datanode/text_snapshot", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextSnapshot(conn, req, resp);
             });
    addRoute("/api/datanode/text_snapshot", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextSnapshot(conn, req, resp);
             });
    addRoute("/api/datanode/text_rollback", HttpRequest::kOptions,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextRollback(conn, req, resp);
             });
    addRoute("/api/datanode/text_rollback", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextRollback(conn, req, resp);
             });
    addRoute("/api/datanode/text_experiment", HttpRequest::kOptions,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextExperiment(conn, req, resp);
             });
    addRoute("/api/datanode/text_experiment", HttpRequest::kGet,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextExperiment(conn, req, resp);
             });
    addRoute("/api/datanode/text_experiment", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleTextExperiment(conn, req, resp);
             });
    addRoute("/api/datanode/hot_cache", HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleHotCache(conn, req, resp);
             });
    addRoute("/api/datanode/delete", fileserver::net::HttpRequest::kPost,
             [this](const TcpConnectionPtr &conn, HttpRequest &req,
                    std::shared_ptr<HttpResponse> &resp) {
                 return handleDeleteFile(conn, req, resp);
             });
}

void DataNodeHttpHandler::onConnection(const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        LOG_INFO << "New connection from " << conn->peerAddress().toIpPort();
        // 为每一个新连接创建一个HttpContext
        conn->setContext(std::make_shared<HttpContext>());
    } else {
        LOG_INFO << "Connection closed from " << conn->peerAddress().toIpPort();
        // 清理上下文

        std::shared_ptr<ConnectionTransferState> transferState;

        {
            std::lock_guard<std::mutex> lock(transferMutex_);
            auto it = activeTransfers_.find(conn->name());
            if (it != activeTransfers_.end()) {
                transferState = it->second;
                activeTransfers_.erase(it); // 移除记录
            }
        }

        // 如果找到了状态，说明任务被异常中断，执行减扣
        if (transferState) {
            if (transferState->uploadContext &&
                transferState->uploadContext->releaseTransferCounter()) {
                if (!transferState->uploadContext->getSessionId().empty()) {
                    datanode_->unregisterTransferSession(
                        transferState->uploadContext->getSessionId());
                }
                LOG_INFO << "Abnormal disconnection: cleaning up upload "
                            "context and reducing the active count";
                datanode_->decActiveUpload();
            }
            if (transferState->downloadContext &&
                transferState->downloadContext->releaseTransferCounter()) {
                if (!transferState->downloadContext->getSessionId().empty()) {
                    datanode_->unregisterTransferSession(
                        transferState->downloadContext->getSessionId());
                }
                LOG_INFO << "Abnormal disconnection: cleaning up download "
                            "context and reducing the active count";
                datanode_->decActiveDownload();
            }
        }
        conn->setContext(std::shared_ptr<void>());
    }
}

bool DataNodeHttpHandler::handleFileUpload(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {

    // ==========================================
    // 1. 拦截并处理浏览器的 CORS 预检请求 (OPTIONS)
    // ==========================================
    if (req.method() == fn::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        // 允许所有域名跨域（生产环境可以改成具体的域名如
        // http://localhost:8000）
        resp->addHeader("Access-Control-Allow-Origin", "*");
        // 允许的方法
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        // 允许前端携带的自定义请求头
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type, X-File-Name");
        // 让浏览器缓存这个预检结果 24 小时，不用每次上传都发 OPTIONS
        resp->addHeader("Access-Control-Max-Age", "86400");

        // OPTIONS 请求不需要 Body，直接返回 true 结束处理
        return true;
    }

    // 即使是真正的 POST响应，也必须带上跨域头，否则前端拿不到响应数据！
    resp->addHeader("Access-Control-Allow-Origin", "*");

    // 验证Token
    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "缺少上传 Token", fn::HttpResponse::k401Unauthorized,
                  conn);
        return true;
    }

    TokenManager::uploadTokenPayload uploadPayload;
    std::string uploadToken = authHeader.substr(7);
    if (!TokenManager::instance().verifyUploadToken(uploadToken,
                                                    uploadPayload)) {
        sendError(resp, "上传 Token 无效或已过期",
                  HttpResponse::k401Unauthorized, conn);
        return true;
    }

    // 获取HttpContext
    auto httpContext =
        std::static_pointer_cast<HttpContext>(conn->getContext());
    if (!httpContext) {
        LOG_ERROR << "HttpContext is null";
        sendError(resp, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
    LOG_INFO << "body.size() = " << req.body().size();

    std::shared_ptr<ConnectionTransferState> transferState;
    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        auto it = activeTransfers_.find(conn->name());
        if (it != activeTransfers_.end()) {
            transferState = it->second;
        } else {
            transferState = std::make_shared<ConnectionTransferState>();
            activeTransfers_[conn->name()] = transferState;
        }
    }

    // 尝试获取已经存在的上传上下文
    std::shared_ptr<FileUploadContext> uploadContext =
        transferState->uploadContext;

    if (!uploadContext) {
        try {
            std::string filePath =
                uploadDir_ + "/" + uploadPayload.server_filename;
            auto localStorage = std::make_shared<LocalFileStorage>();
            uploadContext = std::make_shared<FileUploadContext>(
                uploadPayload.file_id, filePath, std::move(localStorage),
                buildRateLimiter(uploadPayload.qos_policy, datanode_));
            const std::string sessionId = conn->name() + ":upload";
            uploadContext->setSessionId(sessionId);
            transferState->uploadContext = uploadContext;
            datanode_->registerTransferSession(
                sessionId, uploadPayload.user_id, uploadPayload.username,
                uploadPayload.qos_policy.service_level, uploadPayload.scene_tag,
                "upload", uploadPayload.original_filename,
                getCurrentTimeStr());
            LOG_INFO << "开始接收文件: " << uploadPayload.server_filename
                     << ", service_level="
                     << uploadPayload.qos_policy.service_level
                     << ", qos_mode=" << uploadPayload.qos_policy.qos_mode
                     << ", throttle="
                     << uploadPayload.qos_policy.throttle_enabled;

            // 新的上传任务开始，计数器+1
            datanode_->incActiveUpload();
        } catch (std::exception &e) {
            sendError(resp, "无法创建文件",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    }

    if (!req.body().empty()) {
        try {
            uploadContext->throttleIfNeeded(req.body().size());
            uploadContext->writeData(req.body().data(), req.body().size());
            if (!uploadContext->getSessionId().empty()) {
                datanode_->recordTransferBytes(uploadContext->getSessionId(),
                                              req.body().size());
            }
            req.setBody(""); // 清空内存，防止 OOM
        } catch (std::exception &e) {
            sendError(resp, "写入磁盘失败",
                      HttpResponse::k500InternalServerError, conn);
            return true;
        }
    }

    uintmax_t declaredContentLength = 0;
    const std::string contentLengthHeader = req.getHeader("Content-Length");
    if (!contentLengthHeader.empty()) {
        try {
            declaredContentLength =
                static_cast<uintmax_t>(std::stoull(contentLengthHeader));
        } catch (...) {
            declaredContentLength = 0;
        }
    }

    const bool contentLengthSatisfied =
        declaredContentLength > 0 &&
        uploadContext->getTotalBytes() >= declaredContentLength;

    // 检查是否完成
    if (uploadContext->getState() == FileUploadContext::State::kComplete ||
        httpContext->gotAll() || contentLengthSatisfied) {
        // 上传任务完成，计数器-1
        if (uploadContext->releaseTransferCounter()) {
            if (!uploadContext->getSessionId().empty()) {
                datanode_->unregisterTransferSession(
                    uploadContext->getSessionId());
            }
            datanode_->decActiveUpload();
        }

        // 从Map中安全移除状态
        {
            std::lock_guard<std::mutex> lock(transferMutex_);
            activeTransfers_.erase(conn->name());
        }

        auto file_id = uploadContext->getFileID();
        auto server_filename = uploadContext->getFilename();
        auto stored_size = uploadContext->getTotalBytes();
        auto localStorage = uploadContext->getStorage<LocalFileStorage>();
        if (localStorage && localStorage->isOpen()) {
            localStorage->close();
        }

        json respJson = {{"code", 0},
                         {"msg", "Upload success"},
                         {"file",
                          {{"id", file_id},
                           {"name", server_filename.substr(8)},
                           {"originalName", uploadPayload.original_filename},
                           {"size", stored_size},
                           {"createdAt", uploadPayload.created_time}}}};
        //   respJson["data"]["file_md5"] = uploadContext->getFileMd5();
        // 优化：只 dump 一次
        std::string bodyStr = respJson.dump();
        resp->setStatusCode(fileserver::net::HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->addHeader("Connection", "close");
        resp->setBody(bodyStr);
        resp->addHeader("Content-Length", std::to_string(bodyStr.size()));

        // 清理上下文
        transferState->uploadContext.reset();
        if (!transferState->downloadContext) {
            httpContext->setContext(nullptr);
        }

        // 设置写完成回调以关闭连接
        conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
            connection->shutdown();
            return true;
        });

        // ---------------- 第二步：异步通知 Master ----------------
        // 不要在当前 onMessage 线程里同步调用 Master，避免阻塞网络线程
        // 用 muduo 的 EventLoop::runInLoop 或者线程池异步发送
        // notifyMasterAsync(uploadContext);
        auto masterClient = datanode_->getMasterClient();
        masterClient->notifyUploadFinish(std::to_string(file_id),
                                         server_filename, stored_size);
        scheduleVideoPreviewWarmup(server_filename,
                                   uploadPayload.original_filename,
                                   stored_size);

        return true;
    } else {
        LOG_INFO << "Waiting for more data, current state: "
                 << static_cast<uint8_t>(uploadContext->getState());
        return false;
    }
}

bool DataNodeHttpHandler::handleFileDownload(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {

    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        // 处理跨域 (重要：视频播放需要 Range 头，必须在允许列表里)
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Headers", "Range, Authorization");
        resp->addHeader("Access-Control-Expose-Headers",
                        "Content-Range, Content-Length, Accept-Ranges");
        return true;
    }

    // 即使是真正的 POST响应，也必须带上跨域头，否则前端拿不到响应数据！
    resp->addHeader("Access-Control-Allow-Origin", "*");

    std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    std::string filepath = uploadDir_ + "/" + downloadPayload.server_filename;
    if (!fs::exists(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }

    std::string servedPath;
    std::string servedFilename;
    std::string variantError;
    if (!prepareVideoVariant(filepath, uploadDir_ + "/.video_variants",
                             downloadPayload, servedPath, servedFilename,
                             variantError)) {
        sendError(resp, variantError, HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    uintmax_t fileSize = fs::file_size(servedPath);

    std::string rangeHeader = req.getHeader("Range");
    uintmax_t startPos = 0;
    uintmax_t endPos = fileSize - 1;
    bool isRange = false;

    if (!rangeHeader.empty()) {
        std::regex rangeRegex("bytes=(\\d+)-(\\d*)");
        std::smatch matches;
        if (std::regex_search(rangeHeader, matches, rangeRegex)) {
            startPos = std::stoull(matches[1]);
            if (!matches[2].str().empty())
                endPos = std::stoull(matches[2]);
            isRange = true;
        }
    }

    resp->setStatusCode(isRange ? HttpResponse::k206PartialContent
                                : HttpResponse::k200Ok);
    resp->addHeader("Accept-Ranges", "bytes");
    const std::string dispositionType =
        (downloadPayload.access_mode == "preview" ||
         downloadPayload.access_mode == "edit")
            ? "inline"
            : "attachment";
    resp->addHeader("Content-Disposition",
                    dispositionType + "; filename=\"" +
                        servedFilename + "\"");
    resp->setContentType(getMimeType(servedFilename));
    resp->addHeader("X-Video-Quality", downloadPayload.video_quality);
    resp->addHeader("X-Watermark-Mode", downloadPayload.watermark_mode);
    resp->addHeader("X-Watermark-Text", downloadPayload.watermark_text);
    if (isRange) {
        resp->addHeader("Content-Range", "bytes " + std::to_string(startPos) +
                                             "-" + std::to_string(endPos) +
                                             "/" + std::to_string(fileSize));
    }
    resp->addHeader("Content-Length", std::to_string(endPos - startPos + 1));

    auto httpContext =
        std::static_pointer_cast<HttpContext>(conn->getContext());
    if (!httpContext) {
        sendError(resp, "Internal Server Error",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }

    auto transferState = std::make_shared<ConnectionTransferState>();

    auto downContext = std::make_shared<FileDownContext>(
        servedPath, servedFilename, downloadPayload.scene_tag,
        downloadPayload.qos_policy.service_level,
        buildRateLimiter(downloadPayload.qos_policy, datanode_), prefetchCache_,
        &threadPool_);
    const std::string sessionId = conn->name() + ":download";
    downContext->setSessionId(sessionId);
    downContext->seekTo(startPos);
    transferState->downloadContext = downContext;
    datanode_->registerTransferSession(
        sessionId, downloadPayload.user_id, downloadPayload.username,
        downloadPayload.qos_policy.service_level, downloadPayload.scene_tag,
        "download", downloadPayload.original_filename, getCurrentTimeStr());

    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        activeTransfers_[conn->name()] = transferState;
    }
    datanode_->incActiveDownload();

    LOG_INFO << "Start download: " << downloadPayload.server_filename
             << ", service_level=" << downloadPayload.qos_policy.service_level
             << ", qos_mode=" << downloadPayload.qos_policy.qos_mode
             << ", throttle=" << downloadPayload.qos_policy.throttle_enabled;

    conn->setWriteCompleteCallback(
        [this, conn_name = conn->name(),
         downContext](const TcpConnectionPtr &connection) {
            std::string chunk;
            if (downContext->readNextChunk(chunk)) {
                downContext->throttleIfNeeded(chunk.size());
                if (!downContext->getSessionId().empty()) {
                    datanode_->recordTransferBytes(
                        downContext->getSessionId(), chunk.size());
                }
                connection->send(chunk);
                return true;
            } else {
                if (downContext->releaseTransferCounter()) {
                    if (!downContext->getSessionId().empty()) {
                        datanode_->unregisterTransferSession(
                            downContext->getSessionId());
                    }
                    datanode_->decActiveDownload();
                }
                {
                    std::lock_guard<std::mutex> lock(transferMutex_);
                    activeTransfers_.erase(conn_name);
                }
                connection->shutdown();
                return true;
            }
            return true;
        });

    return true;
}

bool DataNodeHttpHandler::isPreviewableTextFile(
    const std::string &filename) const {
    static const std::unordered_set<std::string> previewableExts = {
        "cpp", "cc",  "c",    "h",  "hpp", "py",  "js", "json",
        "ts",  "tsx", "java", "go", "rs",  "txt", "md"};

    auto pos = filename.find_last_of('.');
    if (pos == std::string::npos || pos + 1 >= filename.size()) {
        return false;
    }

    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return previewableExts.count(ext) > 0;
}

bool DataNodeHttpHandler::handleTextPreview(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Authorization");
        return true;
    }

    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    resp->addHeader("Pragma", "no-cache");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    const bool canPreviewText =
        (downloadPayload.access_mode == "preview" &&
         downloadPayload.scene_tag == "development") ||
        downloadPayload.access_mode == "edit";
    if (!canPreviewText) {
        sendError(resp, "当前请求不支持文本预览", HttpResponse::k403Forbidden,
                  conn);
        return true;
    }

    if (!isPreviewableTextFile(downloadPayload.original_filename)) {
        sendError(resp, "该文件类型不支持文本预览",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    const std::string filepath =
        uploadDir_ + "/" + downloadPayload.server_filename;
    if (!fs::exists(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }

    const uintmax_t fileSize = fs::file_size(filepath);
    if (fileSize > kPreviewFileSizeLimit) {
        sendError(resp, "预览仅支持 2MB 以内的文本文件",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        sendError(resp, "文件读取失败", HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("text/plain; charset=utf-8");
    resp->setBody(content);
    resp->addHeader("Content-Length", std::to_string(content.size()));
    return true;
}

bool DataNodeHttpHandler::handleHlsPlaylist(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.access_mode != "preview" ||
        !isVideoFile(downloadPayload.original_filename)) {
        sendError(resp, "当前请求不支持 HLS 视频预览",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }

    const std::string sourcePath =
        uploadDir_ + "/" + downloadPayload.server_filename;
    if (!fs::exists(sourcePath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }
    if (!hasFfmpeg()) {
        sendError(resp, "当前 DataNode 未安装 ffmpeg，无法提供 HLS 预览",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }

    scheduleHlsGeneration(sourcePath, downloadPayload);

    const std::string packageKey =
        buildHlsPackageKey(sourcePath, downloadPayload.video_quality);
    const std::string playlistPath =
        uploadDir_ + "/.hls_cache/" + packageKey + "/index.m3u8";

    if (!isHlsReady(playlistPath)) {
        resp->setStatusCode(static_cast<HttpResponse::HttpStatusCode>(202));
        resp->setStatusMessage("Accepted");
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody("preparing");
        resp->addHeader("Content-Length", "9");
        return true;
    }

    std::ifstream playlistFile(playlistPath);
    if (!playlistFile.is_open()) {
        sendError(resp, "HLS 播放列表读取失败",
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }

    std::ostringstream rewritten;
    std::string line;
    while (std::getline(playlistFile, line)) {
        if (!line.empty() && line[0] != '#') {
            rewritten << "/api/datanode/hls_segment?token=" << token
                      << "&segment=" << line << "\n";
        } else {
            rewritten << line << "\n";
        }
    }

    const std::string body = rewritten.str();
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("application/vnd.apple.mpegurl");
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
    return true;
}

bool DataNodeHttpHandler::handleHlsSegment(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store");

    const std::string token = req.getQuery("token");
    const std::string segmentName = req.getQuery("segment");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.access_mode != "preview" ||
        !isVideoFile(downloadPayload.original_filename)) {
        sendError(resp, "当前请求不支持 HLS 分片",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }
    if (segmentName.empty() || segmentName.find("..") != std::string::npos ||
        segmentName.find('/') != std::string::npos ||
        segmentName.find('\\') != std::string::npos) {
        sendError(resp, "非法分片请求", HttpResponse::k400BadRequest, conn);
        return true;
    }

    const std::string sourcePath =
        uploadDir_ + "/" + downloadPayload.server_filename;
    const std::string packageKey =
        buildHlsPackageKey(sourcePath, downloadPayload.video_quality);
    const fs::path segmentPath =
        fs::path(uploadDir_) / ".hls_cache" / packageKey / segmentName;

    if (!fs::exists(segmentPath) || fs::is_directory(segmentPath)) {
        resp->setStatusCode(HttpResponse::k404NotFound);
        resp->setContentType("text/plain; charset=utf-8");
        resp->setBody("segment_not_ready");
        resp->addHeader("Content-Length", "17");
        return true;
    }

    std::ifstream segmentFile(segmentPath, std::ios::binary);
    if (!segmentFile.is_open()) {
        sendError(resp, "分片读取失败", HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    std::string body((std::istreambuf_iterator<char>(segmentFile)),
                     std::istreambuf_iterator<char>());
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("video/mp2t");
    resp->setBody(body);
    resp->addHeader("Content-Length", std::to_string(body.size()));
    return true;
}

bool DataNodeHttpHandler::handleTextEdit(const TcpConnectionPtr &conn,
                                         HttpRequest &req,
                                         std::shared_ptr<HttpResponse> &resp) {
    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type");
        return true;
    }

    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    resp->addHeader("Pragma", "no-cache");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.access_mode != "edit" ||
        !isVipOrSvipLevel(downloadPayload.qos_policy.service_level) ||
        downloadPayload.scene_tag != "development") {
        sendError(resp, "当前用户不支持在线编辑",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (!isPreviewableTextFile(downloadPayload.original_filename)) {
        sendError(resp, "该文件类型不支持在线编辑",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    const std::string filepath =
        uploadDir_ + "/" + downloadPayload.server_filename;
    if (!fs::exists(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }

    try {
        const auto reqJson = nlohmann::json::parse(req.body());
        const std::string content = reqJson.value("content", "");
        if (content.size() > kPreviewFileSizeLimit) {
            sendError(resp, "在线编辑仅支持 2MB 以内的文本文件",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }

        const std::string tmpPath = filepath + ".edit.tmp";
        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                sendError(resp, "临时文件写入失败",
                          HttpResponse::k500InternalServerError, conn);
                return true;
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        fs::rename(tmpPath, filepath);
        if (prefetchCache_) {
            prefetchCache_->invalidateFile(filepath);
        }

        const std::string body =
            nlohmann::json({{"code", 0}, {"message", "保存成功"}}).dump();
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "在线编辑失败: " + std::string(e.what()),
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool DataNodeHttpHandler::handleTextSnapshot(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type");
        return true;
    }

    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    resp->addHeader("Pragma", "no-cache");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.access_mode != "edit" ||
        !isSvipLevel(downloadPayload.qos_policy.service_level) ||
        downloadPayload.scene_tag != "development") {
        sendError(resp, "仅开发者模式下的 SVIP 用户支持代码快照",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }
    if (!isPreviewableTextFile(downloadPayload.original_filename)) {
        sendError(resp, "该文件类型不支持代码快照",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    const fs::path filepath = fs::path(uploadDir_) /
                              downloadPayload.server_filename;
    if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }
    if (req.method() == fileserver::net::HttpRequest::kPost &&
        fs::file_size(filepath) > kPreviewFileSizeLimit) {
        sendError(resp, "代码快照仅支持 2MB 以内的文本文件",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    try {
        const fs::path snapshotPath =
            buildCodeSnapshotPath(uploadDir_, downloadPayload.server_filename);
        const fs::path metaPath =
            buildCodeSnapshotMetaPath(uploadDir_,
                                      downloadPayload.server_filename);

        if (req.method() == fileserver::net::HttpRequest::kGet) {
            if (!fs::exists(snapshotPath) || !fs::is_regular_file(snapshotPath)) {
                sendError(resp, "尚未生成代码快照",
                          HttpResponse::k404NotFound, conn);
                return true;
            }
            if (fs::file_size(snapshotPath) > kPreviewFileSizeLimit) {
                sendError(resp, "快照文件过大，无法预览差异",
                          HttpResponse::k400BadRequest, conn);
                return true;
            }
            std::ifstream snapshotIn(snapshotPath, std::ios::binary);
            std::string snapshotContent(
                (std::istreambuf_iterator<char>(snapshotIn)),
                std::istreambuf_iterator<char>());
            nlohmann::json meta = readCodeSnapshotMeta(metaPath);
            meta["sizeBytes"] = static_cast<uint64_t>(fs::file_size(snapshotPath));
            const std::string body =
                nlohmann::json(
                    {{"code", 0},
                     {"message", "snapshot found"},
                     {"snapshot", meta},
                     {"content", snapshotContent}})
                    .dump();
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(body);
            resp->addHeader("Content-Length", std::to_string(body.size()));
            return true;
        }

        const auto reqJson = req.body().empty()
                                 ? nlohmann::json::object()
                                 : nlohmann::json::parse(req.body());
        std::string content;
        if (reqJson.contains("content") && reqJson["content"].is_string()) {
            content = reqJson.value("content", "");
        } else {
            std::ifstream in(filepath, std::ios::binary);
            content.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
        }
        if (content.size() > kPreviewFileSizeLimit) {
            sendError(resp, "代码快照仅支持 2MB 以内的文本文件",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }
        std::string note = reqJson.value("note", "");
        if (note.size() > 500) {
            note = note.substr(0, 500);
        }

        fs::create_directories(snapshotPath.parent_path());
        {
            std::ofstream out(snapshotPath,
                              std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                sendError(resp, "快照文件写入失败",
                          HttpResponse::k500InternalServerError, conn);
                return true;
            }
            out.write(content.data(),
                      static_cast<std::streamsize>(content.size()));
        }

        nlohmann::json meta = {
            {"note", note},
            {"createdAt", getCurrentTimeStr()},
            {"originalFilename", downloadPayload.original_filename},
            {"serverFilename", downloadPayload.server_filename},
            {"sizeBytes", static_cast<uint64_t>(content.size())}};
        {
            std::ofstream metaOut(metaPath, std::ios::trunc);
            if (metaOut.is_open()) {
                metaOut << meta.dump(2);
            }
        }

        const std::string body =
            nlohmann::json({{"code", 0},
                            {"message", "代码快照已生成"},
                            {"snapshot", meta}})
                .dump();
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "生成代码快照失败: " + std::string(e.what()),
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool DataNodeHttpHandler::handleTextRollback(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type");
        return true;
    }

    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    resp->addHeader("Pragma", "no-cache");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.access_mode != "edit" ||
        !isSvipLevel(downloadPayload.qos_policy.service_level) ||
        downloadPayload.scene_tag != "development") {
        sendError(resp, "仅开发者模式下的 SVIP 用户支持代码回滚",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }
    if (!isPreviewableTextFile(downloadPayload.original_filename)) {
        sendError(resp, "该文件类型不支持代码回滚",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    const fs::path filepath = fs::path(uploadDir_) /
                              downloadPayload.server_filename;
    const fs::path snapshotPath =
        buildCodeSnapshotPath(uploadDir_, downloadPayload.server_filename);
    const fs::path metaPath =
        buildCodeSnapshotMetaPath(uploadDir_, downloadPayload.server_filename);
    if (!fs::exists(snapshotPath) || !fs::is_regular_file(snapshotPath)) {
        sendError(resp, "尚未生成代码快照，无法回滚",
                  HttpResponse::k404NotFound, conn);
        return true;
    }
    if (fs::file_size(snapshotPath) > kPreviewFileSizeLimit) {
        sendError(resp, "快照文件过大，拒绝回滚",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    try {
        fs::copy_file(snapshotPath, filepath,
                      fs::copy_options::overwrite_existing);
        if (prefetchCache_) {
            prefetchCache_->invalidateFile(filepath.string());
        }

        std::ifstream in(filepath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        nlohmann::json meta = readCodeSnapshotMeta(metaPath);
        meta["sizeBytes"] = static_cast<uint64_t>(fs::file_size(snapshotPath));
        const std::string body =
            nlohmann::json({{"code", 0},
                            {"message", "已回滚到代码快照"},
                            {"content", content},
                            {"snapshot", meta}})
                .dump();
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setContentType("application/json");
        resp->setBody(body);
        resp->addHeader("Content-Length", std::to_string(body.size()));
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "代码回滚失败: " + std::string(e.what()),
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool DataNodeHttpHandler::handleTextExperiment(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() == fileserver::net::HttpRequest::kOptions) {
        resp->setStatusCode(fn::HttpResponse::k200Ok);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers",
                        "Authorization, Content-Type");
        return true;
    }

    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    resp->addHeader("Pragma", "no-cache");

    const std::string token = req.getQuery("token");
    TokenManager::downloadTokenPayload downloadPayload;
    if (!TokenManager::instance().verifyDownloadToken(token, downloadPayload)) {
        sendError(resp, "非法请求", HttpResponse::k403Forbidden, conn);
        return true;
    }

    if (downloadPayload.access_mode != "edit" ||
        !isSvipLevel(downloadPayload.qos_policy.service_level) ||
        downloadPayload.scene_tag != "development") {
        sendError(resp, "仅开发者模式下的 SVIP 用户支持实验分支",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }
    if (!isPreviewableTextFile(downloadPayload.original_filename)) {
        sendError(resp, "该文件类型不支持实验分支",
                  HttpResponse::k400BadRequest, conn);
        return true;
    }

    const fs::path filepath = fs::path(uploadDir_) /
                              downloadPayload.server_filename;
    if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) {
        sendError(resp, "文件丢失", HttpResponse::k404NotFound, conn);
        return true;
    }

    try {
        const fs::path experimentDir =
            buildCodeExperimentDir(uploadDir_, downloadPayload.server_filename);

        if (req.method() == fileserver::net::HttpRequest::kGet) {
            const std::string branchId = req.getQuery("branchId");
            if (!branchId.empty()) {
                if (!isSafeExperimentId(branchId)) {
                    sendError(resp, "实验分支 ID 非法",
                              HttpResponse::k400BadRequest, conn);
                    return true;
                }
                const fs::path experimentPath = buildCodeExperimentPath(
                    uploadDir_, downloadPayload.server_filename, branchId);
                const fs::path metaPath = buildCodeExperimentMetaPath(
                    uploadDir_, downloadPayload.server_filename, branchId);
                if (!fs::exists(experimentPath) ||
                    !fs::is_regular_file(experimentPath)) {
                    sendError(resp, "实验分支不存在",
                              HttpResponse::k404NotFound, conn);
                    return true;
                }
                if (fs::file_size(experimentPath) > kPreviewFileSizeLimit) {
                    sendError(resp, "实验分支内容过大，无法载入",
                              HttpResponse::k400BadRequest, conn);
                    return true;
                }
                std::ifstream in(experimentPath, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                nlohmann::json meta = readJsonFile(metaPath);
                meta["branchId"] = branchId;
                meta["sizeBytes"] =
                    static_cast<uint64_t>(fs::file_size(experimentPath));
                const std::string body =
                    nlohmann::json({{"code", 0},
                                    {"experiment", meta},
                                    {"content", content}})
                        .dump();
                resp->setStatusCode(HttpResponse::k200Ok);
                resp->setContentType("application/json");
                resp->setBody(body);
                resp->addHeader("Content-Length", std::to_string(body.size()));
                return true;
            }

            nlohmann::json experiments = nlohmann::json::array();
            if (fs::exists(experimentDir) && fs::is_directory(experimentDir)) {
                for (const auto &entry : fs::directory_iterator(experimentDir)) {
                    if (!entry.is_regular_file() ||
                        entry.path().extension() != ".json") {
                        continue;
                    }
                    const std::string filename =
                        entry.path().filename().string();
                    const std::string suffix = ".experiment.json";
                    if (filename.size() <= suffix.size() ||
                        filename.substr(filename.size() - suffix.size()) !=
                            suffix) {
                        continue;
                    }
                    const std::string branchId =
                        filename.substr(0, filename.size() - suffix.size());
                    nlohmann::json meta = readJsonFile(entry.path());
                    meta["branchId"] = branchId;
                    const fs::path contentPath = buildCodeExperimentPath(
                        uploadDir_, downloadPayload.server_filename, branchId);
                    if (fs::exists(contentPath)) {
                        meta["sizeBytes"] =
                            static_cast<uint64_t>(fs::file_size(contentPath));
                    }
                    experiments.push_back(meta);
                }
            }
            const std::string body =
                nlohmann::json({{"code", 0}, {"experiments", experiments}})
                    .dump();
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(body);
            resp->addHeader("Content-Length", std::to_string(body.size()));
            return true;
        }

        const auto reqJson = req.body().empty()
                                 ? nlohmann::json::object()
                                 : nlohmann::json::parse(req.body());
        const std::string action = reqJson.value("action", "");
        std::string branchId = reqJson.value("branchId", "");

        if (action == "create") {
            branchId = makeExperimentId();
            std::string name = reqJson.value("name", "");
            std::string note = reqJson.value("note", "");
            if (name.size() > 80) {
                name = name.substr(0, 80);
            }
            if (note.size() > 500) {
                note = note.substr(0, 500);
            }
            std::string content;
            if (reqJson.contains("content") && reqJson["content"].is_string()) {
                content = reqJson.value("content", "");
            } else {
                std::ifstream in(filepath, std::ios::binary);
                content.assign(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
            }
            if (content.size() > kPreviewFileSizeLimit) {
                sendError(resp, "实验分支仅支持 2MB 以内的文本文件",
                          HttpResponse::k400BadRequest, conn);
                return true;
            }

            fs::create_directories(experimentDir);
            const fs::path experimentPath = buildCodeExperimentPath(
                uploadDir_, downloadPayload.server_filename, branchId);
            const fs::path metaPath = buildCodeExperimentMetaPath(
                uploadDir_, downloadPayload.server_filename, branchId);
            {
                std::ofstream out(experimentPath,
                                  std::ios::binary | std::ios::trunc);
                out.write(content.data(),
                          static_cast<std::streamsize>(content.size()));
            }
            nlohmann::json meta = {
                {"branchId", branchId},
                {"name", name.empty() ? "未命名实验" : name},
                {"note", note},
                {"originalFilename", downloadPayload.original_filename},
                {"serverFilename", downloadPayload.server_filename},
                {"createdAt", getCurrentTimeStr()},
                {"updatedAt", getCurrentTimeStr()},
                {"sizeBytes", static_cast<uint64_t>(content.size())}};
            {
                std::ofstream metaOut(metaPath, std::ios::trunc);
                metaOut << meta.dump(2);
            }
            const std::string body =
                nlohmann::json({{"code", 0},
                                {"message", "实验分支已创建"},
                                {"experiment", meta},
                                {"content", content}})
                    .dump();
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(body);
            resp->addHeader("Content-Length", std::to_string(body.size()));
            return true;
        }

        if (!isSafeExperimentId(branchId)) {
            sendError(resp, "实验分支 ID 非法",
                      HttpResponse::k400BadRequest, conn);
            return true;
        }

        const fs::path experimentPath = buildCodeExperimentPath(
            uploadDir_, downloadPayload.server_filename, branchId);
        const fs::path metaPath = buildCodeExperimentMetaPath(
            uploadDir_, downloadPayload.server_filename, branchId);
        if (!fs::exists(experimentPath) || !fs::is_regular_file(experimentPath)) {
            sendError(resp, "实验分支不存在", HttpResponse::k404NotFound, conn);
            return true;
        }

        if (action == "save") {
            const std::string content = reqJson.value("content", "");
            if (content.size() > kPreviewFileSizeLimit) {
                sendError(resp, "实验分支仅支持 2MB 以内的文本文件",
                          HttpResponse::k400BadRequest, conn);
                return true;
            }
            {
                std::ofstream out(experimentPath,
                                  std::ios::binary | std::ios::trunc);
                out.write(content.data(),
                          static_cast<std::streamsize>(content.size()));
            }
            nlohmann::json meta = readJsonFile(metaPath);
            meta["branchId"] = branchId;
            meta["updatedAt"] = getCurrentTimeStr();
            meta["sizeBytes"] = static_cast<uint64_t>(content.size());
            {
                std::ofstream metaOut(metaPath, std::ios::trunc);
                metaOut << meta.dump(2);
            }
            const std::string body =
                nlohmann::json({{"code", 0},
                                {"message", "实验分支已保存"},
                                {"experiment", meta}})
                    .dump();
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(body);
            resp->addHeader("Content-Length", std::to_string(body.size()));
            return true;
        }

        if (action == "merge") {
            if (fs::file_size(experimentPath) > kPreviewFileSizeLimit) {
                sendError(resp, "实验分支内容过大，无法合并",
                          HttpResponse::k400BadRequest, conn);
                return true;
            }
            fs::copy_file(experimentPath, filepath,
                          fs::copy_options::overwrite_existing);
            if (prefetchCache_) {
                prefetchCache_->invalidateFile(filepath.string());
            }
            std::ifstream in(filepath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            nlohmann::json meta = readJsonFile(metaPath);
            meta["branchId"] = branchId;
            const std::string body =
                nlohmann::json({{"code", 0},
                                {"message", "实验分支已合并到主文件"},
                                {"experiment", meta},
                                {"content", content}})
                    .dump();
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(body);
            resp->addHeader("Content-Length", std::to_string(body.size()));
            return true;
        }

        if (action == "discard") {
            fs::remove(experimentPath);
            if (fs::exists(metaPath)) {
                fs::remove(metaPath);
            }
            const std::string body =
                nlohmann::json({{"code", 0},
                                {"message", "实验分支已丢弃"}})
                    .dump();
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setContentType("application/json");
            resp->setBody(body);
            resp->addHeader("Content-Length", std::to_string(body.size()));
            return true;
        }

        sendError(resp, "未知实验分支操作", HttpResponse::k400BadRequest, conn);
        return true;
    } catch (const std::exception &e) {
        sendError(resp, "实验分支操作失败: " + std::string(e.what()),
                  HttpResponse::k500InternalServerError, conn);
        return true;
    }
}

bool DataNodeHttpHandler::handleHotCache(const TcpConnectionPtr &conn,
                                         HttpRequest &req,
                                         std::shared_ptr<HttpResponse> &resp) {
    if (req.method() != HttpRequest::kPost ||
        req.path() != "/api/datanode/hot_cache") {
        return false;
    }

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "未授权", HttpResponse::k401Unauthorized, conn);
        return true;
    }

    TokenManager::hotCacheTokenPayload hotPayload;
    if (!TokenManager::instance().verifyHotCacheToken(authHeader.substr(7),
                                                      hotPayload)) {
        sendError(resp, "热点缓存 Token 无效或已过期",
                  HttpResponse::k403Forbidden, conn);
        return true;
    }

    const std::string filepath = uploadDir_ + "/" + hotPayload.server_filename;
    if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) {
        sendError(resp, "文件不存在", HttpResponse::k404NotFound, conn);
        return true;
    }

    scheduleHotCacheWarmup(hotPayload.server_filename, hotPayload.preload_bytes,
                           hotPayload.vip_priority);

    nlohmann::json body = {{"code", 0},
                           {"message", "hot cache scheduled"},
                           {"server_filename", hotPayload.server_filename},
                           {"preload_bytes", hotPayload.preload_bytes},
                           {"vip_priority", hotPayload.vip_priority}};
    const std::string bodyStr = body.dump();
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("application/json");
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}

void DataNodeHttpHandler::scheduleHotCacheWarmup(
    const std::string &serverFilename, uint64_t preloadBytes,
    bool vipPriority) {
    if (!prefetchCache_ || preloadBytes == 0) {
        return;
    }

    const std::string filepath = uploadDir_ + "/" + serverFilename;
    const auto cache = prefetchCache_;

    threadPool_.run([filepath, preloadBytes, vipPriority, cache]() {
        LOG_INFO << "preloadBytes:" << preloadBytes;
        try {
            if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) {
                LOG_WARN << "Skip hot cache warmup because file is missing: "
                         << filepath;
                return;
            }

            const uintmax_t fileSize = fs::file_size(filepath);
            const uintmax_t targetBytes =
                std::min<uintmax_t>(fileSize, preloadBytes);
            if (targetBytes == 0) {
                return;
            }

            int fd = ::open(filepath.c_str(), O_RDONLY);
            if (fd < 0) {
                LOG_WARN << "Open file for hot cache warmup failed: "
                         << filepath << ", errno=" << std::strerror(errno);
                return;
            }

            uintmax_t offset = 0;
            uintmax_t warmedBytes = 0;
            while (offset < targetBytes) {
                const uintmax_t bytesToRead = std::min<uintmax_t>(
                    kDownloadChunkSize, targetBytes - offset);
                const std::string key = filepath + "_" + std::to_string(offset);

                if (!cache->shouldAdmitPrefetch(
                        static_cast<size_t>(bytesToRead), vipPriority)) {
                    break;
                }
                if (!cache->tryMarkLoading(key)) {
                    offset += bytesToRead;
                    continue;
                }

                std::vector<char> buffer(bytesToRead);
                const ssize_t readBytes = ::pread(
                    fd, buffer.data(), bytesToRead, static_cast<off_t>(offset));
                if (readBytes <= 0) {
                    cache->cancelLoading(key);
                    break;
                }

                buffer.resize(static_cast<size_t>(readBytes));
                cache->put(key, std::move(buffer), vipPriority);
                offset += static_cast<uintmax_t>(readBytes);
                warmedBytes += static_cast<uintmax_t>(readBytes);
            }

            ::close(fd);
            LOG_INFO << "Hot cache warmup finished. file=" << filepath
                     << ", warmed_bytes=" << warmedBytes
                     << ", vip_priority=" << vipPriority
                     << ", cache_bytes=" << cache->currentBytes();
        } catch (const std::exception &e) {
            LOG_ERROR << "Hot cache warmup failed for " << filepath << ": "
                      << e.what();
        }
    });
}

void DataNodeHttpHandler::scheduleHlsGeneration(
    const std::string &sourcePath,
    const TokenManager::downloadTokenPayload &payload) {
    if (!hasFfmpeg() || !isVideoFile(payload.original_filename)) {
        return;
    }

    const std::string packageKey =
        buildHlsPackageKey(sourcePath, payload.video_quality);
    const std::string cacheRoot = uploadDir_ + "/.hls_cache";
    const std::string packageDir = cacheRoot + "/" + packageKey;
    const std::string playlistPath = packageDir + "/index.m3u8";
    if (isHlsReady(playlistPath) || !tryMarkHlsGeneration(packageKey)) {
        return;
    }

    threadPool_.run([sourcePath, payload, cacheRoot, packageKey]() {
        const std::string packageDir = cacheRoot + "/" + packageKey;
        const std::string playlistPath = packageDir + "/index.m3u8";
        const std::string segmentPattern = packageDir + "/seg_%03d.ts";
        const std::string logPath = packageDir + ".log";
        auto variantLock = getVariantLock(packageKey);
        std::lock_guard<std::mutex> guard(*variantLock);

        try {
            if (isHlsReady(playlistPath)) {
                clearHlsGeneration(packageKey);
                return;
            }

            fs::create_directories(packageDir);
            const auto &transcodeConfig = getVideoTranscodeConfig();
            const bool shouldTryGpu =
                transcodeConfig.enableGpu &&
                isGpuEncoderAvailable(transcodeConfig.gpuEncoder);

            auto appendCommonHlsArgs =
                [&](std::vector<std::string> &args) {
                    args.insert(args.end(),
                                {"-c:a", "aac",
                                 "-g", "48",
                                 "-sc_threshold", "0",
                                 "-hls_time", "4",
                                 "-hls_list_size", "0",
                                 "-hls_playlist_type", "event",
                                 "-hls_flags", "independent_segments",
                                 "-hls_segment_filename", segmentPattern,
                                 playlistPath});
                };

            auto buildBaseArgs = [&]() {
                std::vector<std::string> args = {"ffmpeg", "-y", "-threads",
                                                 "0",      "-i", sourcePath};
                if (payload.video_quality == "sd") {
                    args.emplace_back("-vf");
                    args.emplace_back("scale=-2:480");
                } else if (payload.video_quality == "hd") {
                    args.emplace_back("-vf");
                    args.emplace_back("scale=-2:720");
                }
                return args;
            };

            bool success = false;
            if (shouldTryGpu) {
                std::vector<std::string> gpuArgs = buildBaseArgs();
                gpuArgs.insert(gpuArgs.end(),
                               {"-c:v",
                                transcodeConfig.gpuEncoder,
                                "-gpu",
                                std::to_string(transcodeConfig.gpuIndex),
                                "-preset",
                                "p1",
                                "-tune",
                                "ll",
                                "-rc",
                                "vbr",
                                "-cq",
                                payload.video_quality == "sd" ? "31"
                                                              : payload.video_quality == "hd" ? "27"
                                                                                               : "23",
                                "-b:v",
                                "0",
                                "-pix_fmt",
                                "yuv420p"});
                appendCommonHlsArgs(gpuArgs);
                const int gpuStatus = runProcess(gpuArgs, logPath);
                success = gpuStatus == 0 && isHlsReady(playlistPath);
                if (!success) {
                    LOG_WARN << "HLS GPU generation failed. file=" << sourcePath
                             << ", quality=" << payload.video_quality
                             << ", encoder=" << transcodeConfig.gpuEncoder
                             << ", gpu_index=" << transcodeConfig.gpuIndex;
                }
            }

            if (!success && (!shouldTryGpu || transcodeConfig.fallbackToCpu)) {
                std::vector<std::string> cpuArgs = buildBaseArgs();
                cpuArgs.insert(cpuArgs.end(),
                               {"-c:v",
                                transcodeConfig.cpuEncoder,
                                "-preset",
                                "ultrafast",
                                "-crf",
                                payload.video_quality == "sd" ? "31"
                                                              : payload.video_quality == "hd" ? "27"
                                                                                               : "23"});
                appendCommonHlsArgs(cpuArgs);
                const int cpuStatus = runProcess(cpuArgs, logPath);
                success = cpuStatus == 0 && isHlsReady(playlistPath);
            }

            if (!success) {
                LOG_WARN << "HLS generation failed. file=" << sourcePath
                         << ", quality=" << payload.video_quality
                         << ", log=" << logPath;
            }
        } catch (const std::exception &e) {
            LOG_ERROR << "scheduleHlsGeneration failed. file=" << sourcePath
                      << ", quality=" << payload.video_quality
                      << ", error=" << e.what();
        }

        clearHlsGeneration(packageKey);
    });
}

void DataNodeHttpHandler::enqueueVideoTranscodeJob(
    const std::string &sourcePath, const std::string &serverFilename,
    const std::string &originalFilename, const std::string &quality) {
    const std::string jobKey = sourcePath + "|" + quality;
    {
        std::lock_guard<std::mutex> lock(videoTranscodeMutex_);
        if (pendingVideoTranscodes_.count(jobKey) > 0) {
            return;
        }
        pendingVideoTranscodes_.insert(jobKey);
        videoTranscodeQueue_.push_back(
            VideoTranscodeJob{sourcePath, serverFilename, originalFilename,
                              quality});
    }
    scheduleNextVideoTranscode();
}

void DataNodeHttpHandler::scheduleNextVideoTranscode() {
    VideoTranscodeJob job;
    bool hasJob = false;

    {
        std::lock_guard<std::mutex> lock(videoTranscodeMutex_);
        if (activeVideoTranscodes_ >= maxConcurrentVideoTranscodes_ ||
            videoTranscodeQueue_.empty()) {
            return;
        }
        job = videoTranscodeQueue_.front();
        videoTranscodeQueue_.pop_front();
        ++activeVideoTranscodes_;
        hasJob = true;
    }

    if (!hasJob) {
        return;
    }

    threadPool_.run([this, job]() {
        const std::string jobKey = job.sourcePath + "|" + job.quality;
        TokenManager::downloadTokenPayload payload;
        payload.original_filename = job.originalFilename;
        payload.server_filename = job.serverFilename;
        payload.access_mode = "preview";
        payload.video_quality = job.quality;
        payload.watermark_mode = "none";
        payload.watermark_text = "";

        std::string servedPath;
        std::string servedFilename;
        std::string errorMessage;
        if (!prepareVideoVariant(job.sourcePath, uploadDir_ + "/.video_variants",
                                 payload, servedPath, servedFilename,
                                 errorMessage)) {
            LOG_WARN << "Background preview transcode failed. file="
                     << job.sourcePath << ", quality=" << job.quality
                     << ", error=" << errorMessage;
        } else {
            LOG_INFO << "Background preview transcode finished. file="
                     << job.sourcePath << ", quality=" << job.quality;
        }

        {
            std::lock_guard<std::mutex> lock(videoTranscodeMutex_);
            pendingVideoTranscodes_.erase(jobKey);
            if (activeVideoTranscodes_ > 0) {
                --activeVideoTranscodes_;
            }
        }
        scheduleNextVideoTranscode();
    });
}

void DataNodeHttpHandler::scheduleVideoPreviewWarmup(
    const std::string &serverFilename, const std::string &originalFilename,
    uint64_t fileSize) {
    static_cast<void>(serverFilename);
    static_cast<void>(originalFilename);
    static_cast<void>(fileSize);
    LOG_DEBUG << "Preview warmup disabled: preview now serves original video "
                 "without transcoding";
}

bool DataNodeHttpHandler::handleDeleteFile(
    const TcpConnectionPtr &conn, HttpRequest &req,
    std::shared_ptr<HttpResponse> &resp) {
    if (req.method() != HttpRequest::kPost ||
        req.path() != "/api/datanode/delete")
        return true;

    std::string authHeader = req.getHeader("Authorization");
    if (authHeader.empty() || authHeader.substr(0, 7) != "Bearer ") {
        sendError(resp, "未授权", HttpResponse::k401Unauthorized, conn);
        return true;
    }

    std::string token = authHeader.substr(7);
    std::string serverFilename;
    if (!TokenManager::instance().verifyDeleteToken(token, serverFilename)) {
        sendError(resp, "删除 Token 无效或已过期", HttpResponse::k403Forbidden,
                  conn);
        return true;
    }

    std::string filepath = uploadDir_ + "/" + serverFilename;
    try {
        if (std::filesystem::exists(filepath)) {
            std::filesystem::remove(filepath);
            LOG_INFO << "DataNode 成功物理删除文件: " << filepath;
        } else {
            LOG_WARN << "DataNode 物理文件不存在 (可能已被删除): " << filepath;
            sendError(resp, "Physial file not found",
                      HttpResponse::k404NotFound, conn);
            return true;
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "DataNode 删除物理文件失败: " << e.what();
        sendError(resp, "物理删除失败", HttpResponse::k500InternalServerError,
                  conn);
        return true;
    }

    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setContentType("application/json");
    std::string bodyStr = R"({"code": 0, "message": "物理删除成功"})";
    resp->setBody(bodyStr);
    resp->addHeader("Content-Length", std::to_string(bodyStr.size()));
    conn->setWriteCompleteCallback([](const TcpConnectionPtr &connection) {
        connection->shutdown();
        return true;
    });
    return true;
}
