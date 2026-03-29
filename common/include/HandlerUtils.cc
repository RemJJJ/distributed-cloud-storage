#include "HandlerUtils.h"
#include <nlohmann/json.hpp>

void handlerUtils::sendError(const std::shared_ptr<fn::HttpResponse> &resp,
                             const std::string &message,
                             fn::HttpResponse::HttpStatusCode code,
                             const fn::TcpConnectionPtr &conn) {
    nlohmann::json response = {{"code", static_cast<int>(code)},
                               {"message", message}};
    resp->setStatusCode(code);
    resp->setStatusMessage(message);
    resp->setContentType("application/json");
    resp->addHeader("Connection", "close");
    resp->setBody(response.dump());

    if (conn) {
        conn->setWriteCompleteCallback(
            [conn](const fn::TcpConnectionPtr &connection) {
                connection->shutdown();
                return true;
            });
    }
}

std::string handlerUtils::urlDecode(const std::string &encoded) {
    std::string result;
    result.reserve(encoded.length());
    for (size_t i = 0; i < encoded.length(); i++) {
        if (encoded[i] == '%') {
            if (i + 2 < encoded.length()) {
                int value;
                std::sscanf(encoded.substr(i + 1, 2).c_str(), "%x", &value);
                result += static_cast<char>(value);
                i += 2;
            }
        } else if (encoded[i] == '+') {
            result += ' ';
        } else {
            result += encoded[i];
        }
    }
    return result;
}

std::string handlerUtils::getFileType(const std::string &filename) {
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos && dotPos < filename.length() - 1) {
        std::string extension = filename.substr(dotPos + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       ::tolower);

        // 根据扩展名判断文件类型
        if (extension == "jpg" || extension == "jpeg" || extension == "png" ||
            extension == "gif") {
            return "image";
        } else if (extension == "mp4" || extension == "avi" ||
                   extension == "mov" || extension == "wmv") {
            return "video";
        } else if (extension == "pdf") {
            return "pdf";
        } else if (extension == "doc" || extension == "docx") {
            return "word";
        } else if (extension == "xls" || extension == "xlsx") {
            return "excel";
        } else if (extension == "ppt" || extension == "pptx") {
            return "powerpoint";
        } else if (extension == "txt" || extension == "csv") {
            return "text";
        } else {
            return "other";
        }
    }
    return "unknown";
}

// 获取时间
std::string handlerUtils::getCurrentTimeStr() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}