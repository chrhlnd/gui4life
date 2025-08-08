#include "tsvdata.hpp"

#include <algorithm>
#include <fstream>
#include <assert.h>

namespace
{
	struct TableDesc
	{
		std::string name;
		std::vector<std::string> columns;
	};

	void parseTabs(const std::string& line, std::vector<std::string>& parts)
	{
		size_t iter = 0;
		while (true)
		{
			auto end = line.find('\t', iter);

			auto col = line.substr(iter, end - iter);

			while (!col.empty() && (col[col.size() - 1] == '\n' || col[col.size() - 1] == '\r'))
				col.resize(col.size() - 1);

			parts.push_back(col);
			if (end != std::string::npos)
				iter = end + 1;
			else
				break;
		}
	}

	static int callback(void* NotUsed, int argc, char** argv, char** azColName) {
		int i;
		for (i = 0; i < argc; i++) {
			printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
		}
		printf("\n");
		return 0;
	}

	TableDesc createTable(sqlite3 *db, std::string name, const std::string& line)
	{
		assert(db);

		auto escapeName = [](std::string s) -> const std::string
			{
				auto pos = s.find("-");
				while (pos != std::string::npos)
				{
					s[pos] = '_';
					pos = s.find("-", pos + 1);
				}
				return s;

			};


		TableDesc ret;
		ret.name = escapeName(std::move(name));
		ret.columns.push_back("row_id");
		parseTabs(line, ret.columns);

		std::stringstream ss;

		auto escape = [](std::string s) -> const std::string
			{
				auto pos = s.find(" ");
				while (pos != std::string::npos)
				{
					s[pos] = '_';
					pos = s.find(" ", pos + 1);
				}
				return s;

			};

		ss << "CREATE TABLE " << ret.name << " (\n";
		ss << "'" << "row_id" << "' INT,\n";
		for (auto it = ret.columns.begin() + 1; it != ret.columns.end(); ++it)
		{
			ss << "'" << escape(*it) << "' TEXT,\n";
		}

		auto str = ss.str();
		str.resize(str.size() - 2);
		str.append(");");

		char* zErrMsg = nullptr;
		auto rc = sqlite3_exec(db, str.c_str(), callback, 0, &zErrMsg);
		if (rc != SQLITE_OK) {
			throw new data::create_failure{};
		}

		return ret;
	}

	struct insertContext
	{
		std::string prefix;
		std::stringstream ss;
		size_t count = 0;
		size_t wrote = 0;
	};

	insertContext insertBegin(const TableDesc& info)
	{
		insertContext ret;

		std::stringstream ss;
		ss << "INSERT INTO `" << info.name << "` VALUES \n";

		ret.prefix = ss.str();

		return ret;
	}

	void insertEnd(sqlite3* db, const TableDesc& info, insertContext& ctx)
	{
		if (ctx.count)
		{
			char* zErrMsg = nullptr;
			auto str = ctx.ss.str();
			auto rc = sqlite3_exec(db, str.c_str(), callback, 0, &zErrMsg);
			if (rc != SQLITE_OK) {
				throw new data::insert_failure{};
			}

			ctx.wrote += ctx.count;
			ctx.count = 0;
			ctx.ss.clear();
		}
	}

	constexpr size_t batchSize = 2000;

	void insertTable(sqlite3* db, int id, TableDesc info, insertContext& ctx, const std::string& line)
	{
		assert(db);

		std::vector<std::string> values;
		parseTabs(line, values);

		auto escape = [](std::string s) -> const std::string
			{
				auto pos = s.find("\'");
				while (pos != std::string::npos)
				{
					s.insert(pos, "\'");
					pos = s.find("\'",pos + 2);
				}
				return s;

			};

		if (!ctx.count)
		{
			ctx.ss << ctx.prefix;
		}
		else
		{
			ctx.ss << ",";
		}

		ctx.ss << "(";

		ctx.ss << id;
		ctx.ss << ",\n";

		for (const auto& val : values)
		{
			ctx.ss << "\'" << escape(val) << "\',\n";
		}
		for (auto i = values.size()+1; i < info.columns.size(); i++)
		{
			ctx.ss << "NULL,\n";
		}

		ctx.ss.seekp(size_t(ctx.ss.tellp()) - 2);

		ctx.ss << ")\n";

		if (++ctx.count >= batchSize)
		{
			char* zErrMsg = nullptr;
			auto str = ctx.ss.str();
			auto rc = sqlite3_exec(db, str.c_str(), callback, 0, &zErrMsg);
			if (rc != SQLITE_OK) {
				throw new data::insert_failure{};
			}

			ctx.wrote += ctx.count;
			ctx.count = 0;
			std::stringstream ss;
			std::swap(ctx.ss, ss);
		}
	}

}

namespace data
{

	DbDataSet::DbDataSet()
	{
		auto rc = sqlite3_open(":memory:", &db);
		if (rc)
		{
			throw new data::failed_db_create();
		}
	}

	DbDataSet::~DbDataSet()
	{
		assert(db);
		sqlite3_close(db);

	}

#define LOG_TO(l, ...)        \
	{                         \
		std::stringstream ss; \
		ss << __VA_ARGS__;    \
        l(ss.str());          \
	}

	const DbMetaData& DbDataSet::GetTableMetaData()
	{
		return m_meta;
	}

	int DbDataSet::GetRowCount(const DbTableMetaData& table, const fnLogger& logger)
	{
		int retCount = 0;

		std::string sql;
		sql.append("SELECT COUNT(*) FROM `");
		sql.append(table.table_name);
		sql.append("`");
		sql.append(";");

		sqlite3_stmt* stmt = nullptr;

		int ret = sqlite3_prepare_v2(
			db,
			sql.c_str(),
			int(sql.size()),
			&stmt,
			nullptr);

		if (ret != SQLITE_OK)
		{
			LOG_TO(logger, "Failed to exec " << sql << " error " << ret << "\n");
			sqlite3_finalize(stmt);
			return retCount;
		}

		bool stepping = true;
		while (stepping)
		{
			ret = sqlite3_step(stmt);
			switch (ret)
			{
			case SQLITE_ROW:
				ret = sqlite3_column_int(stmt, 0);
				break;
			case SQLITE_DONE:
				stepping = false;
				break;
			default:
				LOG_TO(logger, "Failed stepping " << sql << " error " << ret << "\n");
				stepping = false;
				break;
			}
		}

		return retCount;
	}

	void DbDataSet::GetRows(const DbTableMetaData& table, const std::string& sort, std::function<void(const std::vector<ValType>&)> fnOnRow, const fnLogger& logger, int limit, int offset)
	{
		static std::vector<ValType> row_data;

		std::stringstream ss;

		ss << "SELECT * FROM `" << table.table_name << "`";
		if (!sort.empty())
		{
			ss << " ORDER BY " << sort;
		}
		if (limit)
		{
			ss << " LIMIT " << limit;
			if (offset)
			{
				ss << " OFFSET " << offset;
			}
		}
		ss << ";";

		sqlite3_stmt* stmt = nullptr;

		int ret = sqlite3_prepare_v2(
			db,
			ss.str().c_str(),
			int(ss.str().size()),
			&stmt,
			nullptr);

		if (ret != SQLITE_OK)
		{
			LOG_TO(logger, "Failed to exec " << ss.str() << " error " << ret << "\n");
			sqlite3_finalize(stmt);
			return;
		}

		bool stepping = true;
		while (stepping)
		{
			row_data.clear();
			row_data.reserve(table.columns.size());

			ret = sqlite3_step(stmt);
			switch (ret)
			{
			case SQLITE_ROW:

				row_data.emplace_back(sqlite3_column_int(stmt, 0));

				for (int i = 1; i < int(table.columns.size()); ++i)
				{
					auto sz = sqlite3_column_bytes(stmt, i);
					const unsigned char* data = sqlite3_column_text(stmt, i);
					row_data.emplace_back(std::string(data, data + size_t(sz)));
				}

				fnOnRow(row_data);
				break;
			case SQLITE_DONE:
				stepping = false;
				break;
			default:
				LOG_TO(logger, "Failed stepping " << ss.str() << " error " << ret << "\n");
				stepping = false;
				break;
			}
		}
	}

	void DbDataSet::LoadFromPath(const std::string& path, const std::string& pattern, const fnLogger& logger)
	{
		DbMetaData ret;
		if (!std::filesystem::exists(path))
		{
			LOG_TO(logger, "Couldn't find file " << path << "\n");
			throw new folder_not_found();
		}

		LOG_TO(logger, "Loading from path " << path << "\n");
		for (auto const& item : std::filesystem::directory_iterator(path))
		{
			if (item.is_regular_file())
			{
				std::wstring wide_path(item.path());
				auto found = std::ranges::search(wide_path, pattern);
				if (!found.empty())
				{
					ret.tables.emplace_back(LoadTsvFile(item.path(), logger));
				}
			}
		}

		m_meta = ret;
		m_path = path;
		m_pattern = pattern;
	}

	DbTableMetaData DbDataSet::LoadTsvFile(const std::filesystem::path& path, const fnLogger& logger)
	{
		DbTableMetaData ret{};

		ret.file_name = path.string();

		std::ifstream in(path.string(), std::ios::binary);
		if (!in.is_open())
		{
			LOG_TO(logger, "Failed to open: " << path << "\n");
			throw new file_not_found{path.string()};
		}

		int lineNo = 0;
		TableDesc desc{};

		insertContext ctx;

		int nextId = 0;

		for (std::string line; std::getline(in, line);)
		{
			if (lineNo++ == 0)
			{
				auto name = path.filename().string();
				name = name.substr(0, name.find("."));
				desc = createTable(db, name, line);
				
				ret.table_name = desc.name;
				ret.columns = desc.columns;

				ctx = insertBegin(desc);
			}
			else
			{
				insertTable(db, nextId++, desc, ctx, line);
			}
			// LOG_TO(logger, path << ": line: " << ++lineNo << " Had len " << line.size() << "\n");
		}

		insertEnd(db, desc, ctx);

		ret.count = ctx.wrote;

		LOG_TO(logger, path << " loaded " << ctx.wrote << " lines\n");

		return ret;
	}

}