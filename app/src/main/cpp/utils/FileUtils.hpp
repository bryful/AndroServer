#ifndef ANDROSERVER_FILE_UTILS_HPP
#define ANDROSERVER_FILE_UTILS_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <system_error>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

class FileUtils {
public:
    /**
     * パストラバーサル防止を施した安全な絶対パス解決
     * @param rootDir 基底ルートディレクトリ (例: /sdcard)
     * @param relativePath クライアントから指定された相対パス
     * @param resolvedPath 解決された正規化絶対パス
     * @return ルートディレクトリ配下に収まっていれば true
     */
    static bool resolveSafePath(const std::string& rootDir, const std::string& relativePath, std::string& resolvedPath) {
        if (relativePath.find('\0') != std::string::npos) return false;

        const fs::path root(rootDir); // 起動時に正規化済み
        const auto first = relativePath.find_first_not_of('/');
        const fs::path relative = first == std::string::npos ? fs::path{} : fs::path(relativePath.substr(first));
        for (const auto& part : relative) {
            if (isUploadTemporary(part)) return false;
        }

        std::error_code ec;
        const auto target = fs::weakly_canonical(root / relative, ec);
        if (ec) return false;
        auto part = target.begin();
        for (const auto& rootPart : root) {
            if (part == target.end() || *part != rootPart) return false;
            ++part;
        }
        for (; part != target.end(); ++part) {
            if (isUploadTemporary(*part)) return false;
        }
        resolvedPath = target.string();
        return true;
    }

    static bool isUploadTemporary(const fs::path& name) {
        return name.string().rfind(".androserver-upload-", 0) == 0;
    }

    /**
     * シーケンシャルアクセスであることをカーネルに通知（先読み最適化）
     */
    static void adviseSequential(int fd) {
#if defined(__linux__) || defined(__ANDROID__)
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }
};

#endif // ANDROSERVER_FILE_UTILS_HPP

