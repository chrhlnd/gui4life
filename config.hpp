#pragma once

#include <filesystem>
#include <functional>
#include "sqlite3.h"


namespace config
{
	struct file_not_found { std::filesystem::path path; };
	struct failed_db_create { int code; const char* msg; };
	struct config_setup_failed { int code; const char* msg; };
	struct config_query_failed { int code; const char* msg; };
	struct unknown_type_error {};
	struct delete_query_failed { int code; const char* msg; };

	class Config
	{
	private:
		sqlite3* db = nullptr;

	public:
		[[nodiscard]] bool CreatePaths(const std::filesystem::path& loc) noexcept;
		// throws
		void Load(const std::filesystem::path& loc);

		void HistoryAdd(const std::string& path);

		void HistoryGet(const std::function<void(int, const char*)>& fnOnRow);
		void HistoryRem(int id);

		virtual ~Config();
	};
}