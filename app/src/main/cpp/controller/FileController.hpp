#ifndef ANDROSERVER_FILE_CONTROLLER_HPP
#define ANDROSERVER_FILE_CONTROLLER_HPP

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/outgoing/StreamingBody.hpp"
#include "oatpp/core/data/stream/FileStream.hpp"
#include "oatpp/encoding/Url.hpp"

#include "../dto/DTOs.hpp"
#include "../utils/FileUtils.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <string_view>
#include <sstream>
#include <filesystem>
#include <chrono>

#include OATPP_CODEGEN_BEGIN(ApiController)

/**
 * 正確な Content-Length と Range レジュームに対応した高性能ファイルストリーミング Body
 */
class FileStreamBody : public oatpp::web::protocol::http::outgoing::Body {
private:
    std::shared_ptr<oatpp::data::stream::FileInputStream> m_stream;
    v_int64 m_contentLength;
    v_int64 m_bytesReadTotal{0};

public:
    FileStreamBody(const std::shared_ptr<oatpp::data::stream::FileInputStream>& stream, v_int64 contentLength)
        : m_stream(stream), m_contentLength(contentLength) {}

    oatpp::v_io_size read(void *buffer, v_buff_size count, oatpp::async::Action& action) override {
        if (m_bytesReadTotal >= m_contentLength) {
            return 0; // EOF (指定バイト数転送完了)
        }
        v_buff_size desired = count;
        v_int64 remaining = m_contentLength - m_bytesReadTotal;
        if (static_cast<v_int64>(desired) > remaining) {
            desired = static_cast<v_buff_size>(remaining);
        }
        oatpp::v_io_size res = m_stream->read(buffer, desired, action);
        if (res > 0) {
            m_bytesReadTotal += res;
        }
        return res;
    }

    void declareHeaders(Headers& headers) override {
        // Content-Length は getKnownSize() から自動的に宣言されます
    }

    p_char8 getKnownData() override {
        return nullptr;
    }

    v_int64 getKnownSize() override {
        return m_contentLength;
    }
};

namespace {
oatpp::String decodePath(const oatpp::String& encoded) {
    const auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    };
    for (size_t i = 0; i < encoded->size(); ++i) {
        if ((*encoded)[i] != '%') continue;
        if (encoded->size() - i < 3 || !isHex((*encoded)[i + 1]) || !isHex((*encoded)[i + 2])) {
            throw oatpp::web::protocol::http::HttpError(
                oatpp::web::protocol::http::Status::CODE_400, "Invalid path encoding");
        }
        i += 2;
    }
    auto decoded = oatpp::encoding::Url::decode(encoded);
    if (decoded->find('\0') != std::string::npos) {
        throw oatpp::web::protocol::http::HttpError(
            oatpp::web::protocol::http::Status::CODE_400, "Invalid path");
    }
    return decoded;
}

bool parseNonnegative(std::string_view value, int64_t& result) {
    if (value.empty() || value.front() < '0' || value.front() > '9') return false;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

class PendingUpload {
public:
    explicit PendingUpload(const fs::path& directory)
        : m_path((directory / ".androserver-upload-XXXXXX").string()) {
        const int fd = mkstemp(m_path.data());
        if (fd < 0) {
            m_path.clear();
            return;
        }
        m_file = fdopen(fd, "wb");
        if (!m_file) close(fd);
    }

    PendingUpload(const PendingUpload&) = delete;
    PendingUpload& operator=(const PendingUpload&) = delete;

    ~PendingUpload() {
        if (m_file) fclose(m_file);
        if (!m_path.empty()) unlink(m_path.c_str());
    }

    FILE* file() const { return m_file; }

    bool commit(const std::string& destination) {
        const auto file = m_file;
        m_file = nullptr;
        if (fclose(file) != 0 || rename(m_path.c_str(), destination.c_str()) != 0) return false;
        m_path.clear();
        return true;
    }

private:
    std::string m_path;
    FILE* m_file{nullptr};
};

std::string attachmentName(const std::string& name) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string header = "attachment; filename*=UTF-8''";
    for (const unsigned char ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_') {
            header += static_cast<char>(ch);
        } else {
            header += '%';
            header += HEX[ch >> 4];
            header += HEX[ch & 15];
        }
    }
    return header;
}
}

/**
 * REST API ファイル転送コントローラー
 */
class FileController : public oatpp::web::server::api::ApiController {
public:
    using Roots = std::map<std::string, std::string>;

private:
    Roots m_roots;
    static constexpr oatpp::v_io_size STREAM_BUFFER_SIZE = 256 * 1024;

    bool resolvePath(const std::string& path, std::string& resolved, bool allowRoot = false) const {
        const auto first = path.find_first_not_of('/');
        if (first == std::string::npos) return false;
        const auto slash = path.find('/', first);
        const auto root = m_roots.find(path.substr(first, slash - first));
        if (root == m_roots.end()) return false;
        const auto relative = slash == std::string::npos ? "" : path.substr(slash + 1);
        return FileUtils::resolveSafePath(root->second, relative, resolved) &&
               (allowRoot || resolved != root->second);
    }

public:
    FileController(const std::shared_ptr<ObjectMapper>& objectMapper, Roots roots)
        : oatpp::web::server::api::ApiController(objectMapper), m_roots(std::move(roots)) {}

    static std::shared_ptr<FileController> createShared(Roots roots) {
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper);
        return std::make_shared<FileController>(objectMapper, std::move(roots));
    }

    /**
     * 1. ファイル一覧取得 (GET /api/files/list)
     */
    ENDPOINT("GET", "/api/files/list", listFiles,
             QUERY(String, encodedPath, "path", "")) {

        const std::string relPath = *decodePath(encodedPath);
        auto responseDto = FileListResponseDto::createShared();
        responseDto->path = relPath.empty() ? "/" : relPath.c_str();
        responseDto->items = oatpp::Vector<oatpp::Object<FileItemDto>>::createShared();

        if (relPath.find_first_not_of('/') == std::string::npos) {
            for (const auto& entry : m_roots) {
                auto item = FileItemDto::createShared();
                item->name = entry.first.c_str();
                item->is_dir = true;
                item->size = static_cast<v_int64>(0);
                item->modified_at = static_cast<v_int64>(0);
                responseDto->items->push_back(item);
            }
            return createDtoResponse(Status::CODE_200, responseDto);
        }

        std::string resolved;

        if (!resolvePath(relPath, resolved, true)) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Invalid or restricted path";
            return createDtoResponse(Status::CODE_400, errDto);
        }

        std::error_code ec;
        if (!fs::exists(resolved, ec) || !fs::is_directory(resolved, ec)) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Directory not found";
            return createDtoResponse(Status::CODE_404, errDto);
        }

        for (const auto& entry : fs::directory_iterator(resolved, fs::directory_options::skip_permission_denied, ec)) {
            if (FileUtils::isUploadTemporary(entry.path().filename())) continue;
            auto itemDto = FileItemDto::createShared();
            itemDto->name = entry.path().filename().string().c_str();
            itemDto->is_dir = entry.is_directory(ec);

            if (itemDto->is_dir) {
                itemDto->size = static_cast<v_int64>(0);
            } else {
                itemDto->size = static_cast<v_int64>(entry.file_size(ec));
            }

            auto ftime = entry.last_write_time(ec);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            itemDto->modified_at = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();

            responseDto->items->push_back(itemDto);
        }

        return createDtoResponse(Status::CODE_200, responseDto);
    }

    /**
     * 2. ファイルダウンロード (GET /api/files/download)
     * Rangeヘッダーによる部分転送・レジューム対応
     */
    ENDPOINT("GET", "/api/files/download", downloadFile,
             QUERY(String, encodedPath, "path"),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {

        const auto rangeHeader = request->getHeader("Range");
        const auto path = decodePath(encodedPath);

        if (!path || path->empty()) {
            return createResponse(Status::CODE_400, "Path parameter is required");
        }

        std::string resolved;
        if (!resolvePath(*path, resolved)) {
            return createResponse(Status::CODE_403, "Access denied");
        }

        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) {
            return createResponse(Status::CODE_404, "File not found");
        }

        auto fileStream = std::make_shared<oatpp::data::stream::FileInputStream>(resolved.c_str());
        if (!fileStream->getFile()) {
            return createResponse(Status::CODE_500, "Failed to open file");
        }
        const int fd = fileno(fileStream->getFile());
        struct stat info{};
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            return createResponse(Status::CODE_500, "Failed to inspect file");
        }
        FileUtils::adviseSequential(fd);

        const int64_t fileSize = info.st_size;
        int64_t start = 0;
        int64_t end = fileSize - 1;
        bool partial = false;
        if (rangeHeader && rangeHeader->compare(0, 6, "bytes=") == 0) {
            const auto spec = std::string_view(*rangeHeader).substr(6);
            const auto dash = spec.find('-');
            if (dash != std::string_view::npos && spec.find(',') == std::string_view::npos) {
                const auto first = spec.substr(0, dash);
                const auto last = spec.substr(dash + 1);
                int64_t lower = 0;
                int64_t upper = 0;
                const bool suffix = first.empty();
                const bool valid = suffix ? parseNonnegative(last, upper)
                    : parseNonnegative(first, lower) &&
                      (last.empty() || (parseNonnegative(last, upper) && upper >= lower));
                if (valid) {
                    if (fileSize == 0 || lower >= fileSize || (suffix && upper == 0)) {
                        auto response = createResponse(Status::CODE_416, "Range not satisfiable");
                        response->putHeader("Content-Range", ("bytes */" + std::to_string(fileSize)).c_str());
                        return response;
                    }
                    partial = true;
                    if (suffix) {
                        start = fileSize - std::min(upper, fileSize);
                    } else {
                        start = lower;
                        if (!last.empty()) end = std::min(upper, end);
                    }
                }
            }
        }
        if (partial && fseeko(fileStream->getFile(), start, SEEK_SET) != 0) {
            return createResponse(Status::CODE_500, "Failed to seek file");
        }

        auto body = std::make_shared<FileStreamBody>(fileStream, end - start + 1);
        auto response = OutgoingResponse::createShared(partial ? Status::CODE_206 : Status::CODE_200, body);
        response->putHeader("Content-Type", "application/octet-stream");
        response->putHeader("Accept-Ranges", "bytes");
        response->putHeader("Content-Disposition", attachmentName(fs::path(resolved).filename().string()).c_str());
        if (partial) {
            std::ostringstream range;
            range << "bytes " << start << '-' << end << '/' << fileSize;
            response->putHeader("Content-Range", range.str().c_str());
        }
        return response;
    }

    /**
     * 3. ファイルアップロード (POST /api/files/upload)
     * ストリーミング書き込み後に保存先を更新
     */
    ENDPOINT("POST", "/api/files/upload", uploadFile,
             QUERY(String, encodedPath, "path"),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {

        const auto path = decodePath(encodedPath);
        const auto error = [this](const Status& status, const char* message) {
            auto dto = StatusResponseDto::createShared();
            dto->success = false;
            dto->message = message;
            auto response = createDtoResponse(status, dto);
            response->putHeader("Connection", "close");
            return response;
        };
        if (!path || path->empty()) return error(Status::CODE_400, "Path parameter is required");
        if (request->getHeader("Transfer-Encoding")) {
            return error(Status::CODE_400, "Transfer-Encoding is unsupported");
        }
        const auto length = request->getHeader("Content-Length");
        int64_t expected = 0;
        if (!length) return error(Status::CODE_411, "Content-Length is required");
        if (!parseNonnegative(*length, expected)) {
            return error(Status::CODE_400, "Invalid Content-Length");
        }

        std::string resolved;
        if (!resolvePath(*path, resolved)) {
            return error(Status::CODE_403, "Invalid destination path");
        }
        std::error_code ec;
        const auto directory = fs::path(resolved).parent_path();
        fs::create_directories(directory, ec);
        if (ec) return error(Status::CODE_500, "Failed to create destination directory");

        PendingUpload pending(directory);
        if (!pending.file()) return error(Status::CODE_500, "Failed to create temporary file");

        auto input = request->getBodyStream();
        if (expected > 0 && !input) return error(Status::CODE_400, "Missing request body");
        std::vector<v_char8> buffer(STREAM_BUFFER_SIZE);
        int64_t remaining = expected;
        while (remaining > 0) {
            const auto count = std::min<int64_t>(remaining, buffer.size());
            const auto received = input->readSimple(buffer.data(), static_cast<v_buff_size>(count));
            if (received <= 0) return error(Status::CODE_400, "Incomplete upload");
            const auto bytes = static_cast<size_t>(received);
            if (fwrite(buffer.data(), 1, bytes, pending.file()) != bytes) {
                return error(Status::CODE_500, "Failed to write upload");
            }
            remaining -= received;
        }
        if (!pending.commit(resolved)) return error(Status::CODE_500, "Failed to save upload");

        auto dto = StatusResponseDto::createShared();
        dto->success = true;
        dto->message = "Upload completed successfully";
        dto->bytes = static_cast<v_int64>(expected);
        return createDtoResponse(Status::CODE_200, dto);
    }

    /**
     * 4. ファイル / フォルダ削除 (DELETE /api/files)
     */
    ENDPOINT("DELETE", "/api/files", deleteFile,
             QUERY(String, encodedPath, "path")) {

        const auto path = decodePath(encodedPath);
        if (!path || path->empty()) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Path parameter is required";
            return createDtoResponse(Status::CODE_400, errDto);
        }

        std::string resolved;
        if (!resolvePath(*path, resolved)) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Access denied";
            return createDtoResponse(Status::CODE_403, errDto);
        }

        std::error_code ec;
        if (!fs::exists(resolved, ec)) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Target not found";
            return createDtoResponse(Status::CODE_404, errDto);
        }

        const fs::path target(resolved);
        for (const auto& entry : m_roots) {
            const fs::path root(entry.second);
            if (std::mismatch(target.begin(), target.end(), root.begin(), root.end()).first == target.end()) {
                auto dto = StatusResponseDto::createShared();
                dto->success = false;
                dto->message = "Cannot delete a shared folder or its parent";
                return createDtoResponse(Status::CODE_403, dto);
            }
        }

        fs::remove_all(resolved, ec);
        if (ec) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Failed to delete: " + ec.message();
            return createDtoResponse(Status::CODE_500, errDto);
        }

        auto resDto = StatusResponseDto::createShared();
        resDto->success = true;
        resDto->message = "Deleted successfully";
        return createDtoResponse(Status::CODE_200, resDto);
    }

    /**
     * 5. ディレクトリ作成 (POST /api/directories)
     */
    ENDPOINT("POST", "/api/directories", createDirectory,
             QUERY(String, encodedPath, "path")) {

        const auto path = decodePath(encodedPath);
        if (!path || path->empty()) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Path parameter is required";
            return createDtoResponse(Status::CODE_400, errDto);
        }

        std::string resolved;
        if (!resolvePath(*path, resolved)) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Invalid path";
            return createDtoResponse(Status::CODE_403, errDto);
        }

        std::error_code ec;
        fs::create_directories(resolved, ec);
        if (ec) {
            auto errDto = StatusResponseDto::createShared();
            errDto->success = false;
            errDto->message = "Failed to create directory: " + ec.message();
            return createDtoResponse(Status::CODE_500, errDto);
        }

        auto resDto = StatusResponseDto::createShared();
        resDto->success = true;
        resDto->message = "Directory created successfully";
        return createDtoResponse(Status::CODE_200, resDto);
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // ANDROSERVER_FILE_CONTROLLER_HPP
