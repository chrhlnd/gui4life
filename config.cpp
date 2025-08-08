
#include "config.hpp"

#include <locale>
#include <assert.h>
#include <array>
#include <variant>

#include <windows.h>
#include <stringapiset.h>

namespace
{
	std::string ws2s(const std::wstring& wstr)
	{
		BOOL usedDef = false;
		int amt = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wstr.c_str(), int(wstr.size()), nullptr, 0, "?", &usedDef);

		std::string ret;
		ret.resize(amt+1);
		int amt2 = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wstr.c_str(), int(wstr.size()), ret.data(), int(ret.size()), "?", &usedDef);
		assert(amt == amt2);

		return ret;
	}

	using blobType = std::tuple<const char*, size_t>;
	using valueType = std::variant<const char*, int64_t, double, blobType, std::nullopt_t>;

	void insertRow(sqlite3* db, const std::string& table, const std::vector<valueType>& coldata)
	{
		assert(db);
		constexpr const char* insertBegin = "INSERT INTO `";
		constexpr const char* insertAfter = "` VALUES(";
		constexpr const char* insertEnd = ");";

		std::string sql;
		sql.append(insertBegin);
		sql.append(table);
		sql.append(insertAfter);

		for (size_t i = 0; i < coldata.size(); i++)
		{
			sql.append("?,");
		}
		sql.resize(sql.size() - 1);
		sql.append(insertEnd);

		sqlite3_stmt* stmt = nullptr;
		auto rc = sqlite3_prepare_v2(db, sql.c_str(), int(sql.size()), &stmt, nullptr);
		if (rc != SQLITE_OK)
		{
			auto exc = new config::config_setup_failed(rc, sqlite3_errmsg(db));
			throw exc;
		}

		int nparm = 1;
		for (const auto& val : coldata)
		{
			if (std::holds_alternative<const char*>(val))
			{
				sqlite3_bind_text(stmt, nparm++, std::get<const char*>(val), int(strlen(std::get<const char*>(val))), SQLITE_STATIC);
			}
			else if (std::holds_alternative<int64_t>(val))
			{
				sqlite3_bind_int64(stmt, nparm++, std::get<int64_t>(val));
			}
			else if (std::holds_alternative<double>(val))
			{
				sqlite3_bind_double(stmt, nparm++, std::get<double>(val));
			}
			else if (std::holds_alternative<blobType>(val))
			{
				auto& v = std::get<blobType>(val);
				sqlite3_bind_blob(stmt, nparm++, std::get<0>(v), int(std::get<1>(v)), SQLITE_STATIC);
			}
			else if (std::holds_alternative<std::nullopt_t>(val))
			{
				sqlite3_bind_null(stmt, nparm++);
			}
			else
			{
				throw new config::unknown_type_error();
			}
		}

		rc = sqlite3_step(stmt);
		switch (rc)
		{
		case SQLITE_DONE:
			break;
		default:
			throw new config::config_setup_failed(rc);
		}

		sqlite3_finalize(stmt);
	}

	void createTable(sqlite3* db, const std::string& table, const std::vector<std::array<std::string,3>>& cols)
	{
		assert(db);

		constexpr const char* createBegin = "CREATE TABLE IF NOT EXISTS `";
		constexpr const char* createContinue = "` (";
		constexpr const char* createColName = " `";
		constexpr const char* createColType = "` ";
		constexpr const char* createEnd = ");";

		std::string sql;
		sql.append(createBegin);
		sql.append(table);
		sql.append(createContinue);

		for (size_t i = 0; i < cols.size(); ++i)
		{
			sql.append(createColName);
			sql.append(cols[i].at(0));
			sql.append(createColType);
			sql.append(cols[i].at(1));
			sql.append(" ");
			sql.append(cols[i].at(2));
			sql.append(",\n");
		}

		sql.resize(sql.size() - 2);

		sql.append(createEnd);

		sqlite3_stmt* stmt = nullptr;
		auto rc = sqlite3_prepare_v2(db, sql.c_str(), int(sql.size()), & stmt, nullptr);
		if (rc != SQLITE_OK)
		{
			auto exc = new config::config_setup_failed(rc, sqlite3_errmsg(db));
			throw exc;
		}

		rc = sqlite3_step(stmt);
		switch (rc)
		{
		case SQLITE_DONE:
			break;
		default:
			auto ret = new config::config_setup_failed(rc, sqlite3_errmsg(db));
			throw ret;
		}

		sqlite3_finalize(stmt);
	}

	using FnOnData = std::function<void(const void* pData, int dLen, int dType, int colIdx, int rowIdx)>;

	void queryRows(sqlite3* db, const std::string& table, const std::vector<std::string> columns, const std::string& filter, const std::string& order, const std::string& limit, const FnOnData& fnOnData)
	{
		assert(db);

		constexpr const char* selectBegin = "SELECT ";
		constexpr const char* selectFrom = " FROM ";
		constexpr const char* selectWhere = " WHERE \n";
		constexpr const char* selectOrder = " ORDER BY \n";
		constexpr const char* selectLimit = " LIMIT \n";
		constexpr const char* selectEnd = ";";


		std::string sql;
		sql.append(selectBegin);

		for (size_t i = 0; i < columns.size(); ++i)
		{
			sql.append("`");
			sql.append(columns[i]);
			sql.append("`,\n");
		}

		sql.resize(sql.size() - 2);

		sql.append(selectFrom);
		sql.append("`");
		sql.append(table);
		sql.append("` \n");
		if (!filter.empty())
		{
			sql.append(selectWhere);
			sql.append(filter);
		}
		if (!order.empty())
		{
			sql.append(selectOrder);
			sql.append(order);
		}
		if (!limit.empty())
		{
			sql.append(selectLimit);
			sql.append(limit);
		}

		sql.append(selectEnd);

		sqlite3_stmt* stmt = nullptr;
		auto rc = sqlite3_prepare_v2(db, sql.c_str(), int(sql.size()), &stmt, nullptr);
		if (rc != SQLITE_OK)
		{
			auto exc = new config::config_query_failed(rc, sqlite3_errmsg(db));
			throw exc;
		}

		int row_index = 0;

		bool done = false;
		while (!done)
		{
			rc = sqlite3_step(stmt);
			switch (rc)
			{
			case SQLITE_ROW:
			{
				int colCount = sqlite3_column_count(stmt);

				for (int col_index = 0; col_index < colCount; col_index++)
				{
					int colType = sqlite3_column_type(stmt, col_index);
					switch (colType)
					{
					case SQLITE_INTEGER:
					{
						auto val = sqlite3_column_int64(stmt, col_index);
						fnOnData(&val, sizeof(val), colType, col_index, row_index);
					}
					break;
					case SQLITE_FLOAT:
					{
						auto val = sqlite3_column_double(stmt, col_index);
						fnOnData(&val, sizeof(val), colType, col_index, row_index);
					}
					break;
					case SQLITE_TEXT:
					{
						const auto val = sqlite3_column_text(stmt, col_index);
						fnOnData(val, sqlite3_column_bytes(stmt, col_index), colType, col_index, row_index);
					}
					break;
					case SQLITE_BLOB:
					{
						auto val = sqlite3_column_blob(stmt, col_index);
						fnOnData(&val, sqlite3_column_bytes(stmt, col_index), colType, col_index, row_index);
					}
					break;
					case SQLITE_NULL:
					{
						fnOnData(nullptr, 0, colType, col_index, row_index);
					}
					break;
					}
				}
			}
			break;
			case SQLITE_DONE:
				done = true;
				break;
			default:
				throw new config::config_query_failed(rc);
			}
		}

		sqlite3_finalize(stmt);
	}

	void remRows(sqlite3* db, const std::string& table, const std::string filter, const std::vector<valueType>& vals)
	{
		assert(db);

		constexpr const char* begin = "DELETE FROM ";
		constexpr const char* end = ";";

		std::string sql;
		sql.append(begin);
		sql.append("`");
		sql.append(table);
		sql.append("`");
		sql.append(filter);
		sql.append(end);

		sqlite3_stmt* stmt = nullptr;
		auto rc = sqlite3_prepare_v2(db, sql.c_str(), int(sql.size()), &stmt, nullptr);
		if (rc != SQLITE_OK)
		{
			auto exc = new config::config_setup_failed(rc, sqlite3_errmsg(db));
			throw exc;
		}

		int nparm = 1;
		for (const auto& val : vals)
		{
			if (std::holds_alternative<const char*>(val))
			{
				sqlite3_bind_text(stmt, nparm++, std::get<const char*>(val), int(strlen(std::get<const char*>(val))), SQLITE_STATIC);
			}
			else if (std::holds_alternative<int64_t>(val))
			{
				sqlite3_bind_int64(stmt, nparm++, std::get<int64_t>(val));
			}
			else if (std::holds_alternative<double>(val))
			{
				sqlite3_bind_double(stmt, nparm++, std::get<double>(val));
			}
			else if (std::holds_alternative<blobType>(val))
			{
				auto& v = std::get<blobType>(val);
				sqlite3_bind_blob(stmt, nparm++, std::get<0>(v), int(std::get<1>(v)), SQLITE_STATIC);
			}
			else if (std::holds_alternative<std::nullopt_t>(val))
			{
				sqlite3_bind_null(stmt, nparm++);
			}
			else
			{
				throw new config::unknown_type_error();
			}
		}

		bool done = false;
		while (!done)
		{
			rc = sqlite3_step(stmt);
			switch (rc)
			{
			case SQLITE_DONE:
				done = true;
				break;
			default:
				throw new config::delete_query_failed(rc, sqlite3_errmsg(db));
			}
		}
	}
}

namespace config
{
	Config::~Config()
	{
		if (db)
		{
			sqlite3_close(db);
		}
	}

	bool Config::CreatePaths(const std::filesystem::path& loc) noexcept
	{
		if (loc.parent_path().empty())
			return true;

		if (!std::filesystem::exists(loc.parent_path()))
		{
			if (!std::filesystem::create_directories(loc.parent_path()))
			{
				return false;
			}
		}
		return true;
	}

	void Config::Load(const std::filesystem::path& loc)
	{
		if (db)
		{
			sqlite3_close(db);
		}

		if (!loc.parent_path().empty() && !std::filesystem::exists(loc.parent_path()))
		{
			throw new file_not_found(loc.parent_path());
		}


		auto path = ws2s(loc);

		auto rc = sqlite3_open(path.c_str(), &db);
		if (rc)
		{
			throw new failed_db_create(rc);
		}

		createTable(db, "history", { { "row_id", "INTEGER", "PRIMARY KEY ASC AUTOINCREMENT"}, {"path", "TEXT", ""}});
	}

	void Config::HistoryAdd(const std::string& path)
	{
		assert(db);

		insertRow(db, "history", { valueType(std::nullopt), valueType(path.c_str()) });
	}


	void Config::HistoryGet(const std::function<void(int, const char*)>& fnOnRow)
	{
		int64_t rowId = 0;

		queryRows(db, "history", { "row_id", "path" }, "", "", "", [&](const void* colData, int dataLen, int colType, int colIndex, int rowIndex)
			{
				assert(colType == SQLITE_TEXT || colType == SQLITE_INTEGER);
				switch (colType)
				{
				case SQLITE_TEXT:
					fnOnRow(int(rowId), reinterpret_cast<const char*>(colData));
					break;
				case SQLITE_INTEGER:
					rowId = *reinterpret_cast<const int64_t*>(colData);
					break;
				}
			});

	}

	void Config::HistoryRem(int id)
	{
		remRows(db, "history", "WHERE row_id = ?", { valueType(id) });
	}


}