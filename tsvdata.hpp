#pragma once

#include <filesystem>
#include <functional>
#include <vector>
#include <string>
#include <variant>

#include "sqlite3.h"

namespace data
{

	struct file_not_found { std::string path; };
	struct folder_not_found {};
	struct failed_db_create {};
	struct create_failure{};
	struct insert_failure{};

	using fnLogger = std::function<void(const std::string&)>;

	struct DbTableMetaData
	{
		std::string table_name;
		std::string file_name;
		std::vector<std::string> columns;
		size_t count;
	};

	struct DbMetaData
	{
		std::vector<DbTableMetaData> tables;
	};

	class DbDataSet
	{
	private:
		// throws
		DbTableMetaData LoadTsvFile(const std::filesystem::path& path, const fnLogger& logger);

	private:
		sqlite3* db;
		data::DbMetaData m_meta;
		std::string m_path;
		std::string m_pattern;

	public:
		// throws
		DbDataSet();
		virtual ~DbDataSet();

		using ValType = std::variant<int, std::string>;

		const std::tuple<std::string&, std::string&> GetPath() { return std::make_tuple(std::ref(m_path), std::ref(m_pattern)); }

		const DbMetaData& GetTableMetaData();

		void GetRows(const DbTableMetaData& table, const std::string& sort, std::function<void(const std::vector<ValType>&)> fnOnRow, const fnLogger& logger, int limit = 0, int offset = 0);
		int GetRowCount(const DbTableMetaData& table, const fnLogger& logger);

		// throws
		void LoadFromPath(const std::string& path, const std::string& pattern, const fnLogger& logger);
	};
}
