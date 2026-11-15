#ifndef ANDROSERVER_DTOS_HPP
#define ANDROSERVER_DTOS_HPP

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/Types.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * ファイル・ディレクトリエントリDTO
 */
class FileItemDto : public oatpp::DTO {
    DTO_INIT(FileItemDto, DTO)

    DTO_FIELD(String, name);
    DTO_FIELD(Boolean, is_dir);
    DTO_FIELD(Int64, size);
    DTO_FIELD(Int64, modified_at);
};

/**
 * ファイル一覧レスポンスDTO
 */
class FileListResponseDto : public oatpp::DTO {
    DTO_INIT(FileListResponseDto, DTO)

    DTO_FIELD(String, path);
    DTO_FIELD(Vector<oatpp::Object<FileItemDto>>, items);
};

/**
 * 汎用ステータスレスポンスDTO
 */
class StatusResponseDto : public oatpp::DTO {
    DTO_INIT(StatusResponseDto, DTO)

    DTO_FIELD(Boolean, success);
    DTO_FIELD(String, message);
    DTO_FIELD(Int64, bytes);
};

#include OATPP_CODEGEN_END(DTO)

#endif // ANDROSERVER_DTOS_HPP

