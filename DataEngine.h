#pragma once

#include <vector>
#include <list>
#include <unordered_set>
#include <tuple>
#include <type_traits>
#include <algorithm>
#include <exception>
#include <unordered_map>
#include <functional>
#include <bitset>
#include <cstdint>
#include <array>
#include <assert.h>

using
std::vector,
std::list,
std::tuple,
std::is_same_v,
std::ranges::lower_bound,
std::unordered_map,
std::array,
std::get,
std::unordered_set,
std::function,
std::bitset;

template <typename T>
concept boolean_testable = requires (T && t) {
	requires std::convertible_to<T, bool>;
	{ !std::forward<T>(t) } -> std::convertible_to<bool>;
};

namespace dng {

	enum Id : uint64_t {};

	template <typename RowT>
	class Table
	{
		template <typename... RowTypes> friend class DbTables;

		vector<tuple<Id, RowT>> rows;

	private:
		RowT& set(Id id, const RowT& row) noexcept
		{
			auto lower_id = [](auto a, auto b) { return std::get<0>(a) < std::get<0>(b); };

			auto pos = lower_bound(rows, tuple<Id, RowT>{id, {}}, lower_id);
			if (pos != rows.end() && std::get<0>(*pos) == id)
				std::get<1>(*pos) = row;
			else
				pos = rows.insert(pos, tuple<Id, RowT>(id, row));
			return std::get<1>(*pos);
		}

		RowT* get(Id id) noexcept
		{
			auto lower_id = [](auto a, auto b) { return std::get<0>(a) < std::get<0>(b); };

			auto pos = lower_bound(rows, tuple<Id, RowT>{id, {}}, lower_id);
			if (pos != rows.end() && std::get<0>(*pos) == id)
				return &std::get<1>(*pos);
			return nullptr;
		}
	};

	/* Track all ids and the tables they're in
	*
	* When a Query is made entities in those sets of tables will join the query
	*   joining the query means, entities not in the cached set will be inserted
	*   then we'll walk the set and call process
	*
	*
	*/

	template <typename... RowTypes>
	class DbTables
	{
		static int s_instances;

		tuple<Table<RowTypes>...> m_tables;

		vector<tuple<Id, int>> id_members; // what data tables hold data for ID

		static constexpr size_t NumTables = sizeof...(RowTypes);

		// list of table indexes and the query ids using them
		using Qid = bitset<NumTables>;

		int query_stack = 0;

		// array containing the set of QueryIds involved with that base table
		// this could be constexpr.. can I constexpr from query calls
		array<unordered_set<Qid>, NumTables> query_to_tables;

		// pending updates to queries by id
		unordered_map<Qid, unordered_set<Id>> query_insert;
		unordered_map<Qid, unordered_set<Id>> query_remove;

		template <typename QRT, typename... QRTs>
		void registerQueryTables(Qid qid)
		{
			query_to_tables[GetTableIndex<QRT>()].insert(qid);

			if constexpr (sizeof...(QRTs) > 0)
			{
				registerQueryTables<QRTs...>(qid);
			}
		}

		template <typename QRT, typename... QRTs>
		void unRegisterQueryTables(Qid qid)
		{
			query_to_tables[GetTableIndex<QRT>()].erase(qid);
			if constexpr (sizeof...(QRTs) > 0)
			{
				unRegisterQueryTables<QRTs...>(qid);
			}
		}

		template <typename QRT, typename... QRTs>
		static Qid makeQid()
		{
			Qid ret;
			ret[size_t(GetTableIndex<QRT>())] = 1;

			if constexpr (sizeof...(QRTs) == 0)
			{
				return ret;
			}
			else {
				ret |= makeQid<QRTs...>();
				return ret;
			}
		}

		template <int recur, typename QR, typename... QRT>
		void updateCache(auto& row, Id id)
		{
			const auto& base_row = GetTable<QR>().get(id);

			assert(base_row);

			get<recur + 1>(row) = *base_row;

			if constexpr (sizeof...(QRT) > 0)
			{
				updateCache<recur + 1, QRT...>(row, id);
			}
		}

		template <int recur, typename QR, typename... QRT>
		void fill(auto& rows)
		{
			auto& base_table = GetTable<QR>(); // get the base set of data for this cacheline row part

			if constexpr (recur == 0)
			{
				for (const auto& row : base_table.rows)
				{
					auto& n = rows.emplace_back();
					get<0>(n) = get<0>(row);
					get<1>(n) = get<1>(row);
				}
			}
			else
			{
				for (auto itr = rows.begin(); itr != rows.end();)
				{
					const auto base_row = base_table.get(get<0>(*itr));
					if (!base_row)
					{
						itr = rows.erase(itr);
						continue;
					}

					get<recur+1>(*itr) = *base_row;

					++itr;
				}
			}

			if constexpr (sizeof...(QRT) > 0)
			{
				fill<recur+1, QRT...>(rows);
			}
		}

		// Does this entity Id contain all of our base data type
		template <typename QR, typename... QRT>
		bool isInCache(Id id)
		{
			const auto& base_table = GetTable<QR>();

			auto iter = lower_bound(
				base_table.rows,
				id,
				std::less{},
				[](const auto& row) { return get<0>(row); });

			if (iter == base_table.rows.end() || get<0>(*iter) != id)
			{
				return false;
			}

			if constexpr (sizeof...(QRT) > 0)
			{
				return isInCache<QRT...>(id);
			}

			return true;
		}

	public:
		template <typename... QueryRowTypes>
		static auto& getCacheRows()
		{
			static unordered_map<int, vector<tuple<Id, QueryRowTypes...>>> s_instance_cache_rows;
			return s_instance_cache_rows;
		}

		template <typename... QueryRowTypes, typename F>
			requires requires (const tuple<Id, QueryRowTypes...>& tup, F&& f) { { std::apply(std::forward<F>(f), tup) } -> boolean_testable; }
		void QueryOnce(F&& fn)
		{
			vector<tuple<Id, QueryRowTypes...>> rows;
			fill<0, QueryRowTypes...>(rows);

			for (const auto& row : rows)
			{
				if (!std::apply(fn, row))
					break;
			}
		}


		template <typename... QueryRowTypes, typename F>
			requires requires (const tuple<Id, QueryRowTypes...>& tup, F&& f) { { std::apply(std::forward<F>(f), tup) } -> boolean_testable; }
		void Query(F&& fn)
		{
			constexpr size_t nTypes = sizeof...(QueryRowTypes);

			static const Qid queryId = makeQid<QueryRowTypes...>();

			auto& instance_cache_rows = getCacheRows<QueryRowTypes...>();

			static int instance_id = 0;

			bool firstRun = !instance_id;

			if (firstRun) instance_id = s_instances++;

			auto& cache_rows = instance_cache_rows[instance_id];

			if (firstRun)
			{
				registerQueryTables<QueryRowTypes...>(queryId);
				// full join on base data
				fill<0, QueryRowTypes...>(cache_rows);
			}

			if (query_stack == 0) // only apply updates if we're unstacked
			{
				if (query_insert.find(queryId) != query_insert.end() &&
					!query_insert[queryId].empty())
				{
					if (!cache_rows.empty())
					{
						auto& insert_list = query_insert[queryId];
						for (auto id : insert_list)
						{
							auto iter = lower_bound(
								cache_rows,
								id,
								std::less{},
								[](const auto& row) { return get<0>(row); });

							if (get<0>(*iter) != id)
							{
								if (!isInCache<QueryRowTypes...>(id))
									continue;

								iter = cache_rows.emplace(iter);
								get<0>(*iter) = id;
							}

							updateCache<0, QueryRowTypes...>(*iter, id);
						}
					}
					query_insert[queryId].clear();
				}

				if (query_remove.find(queryId) != query_remove.end() &&
					!query_remove[queryId].empty())
				{
					if (!cache_rows.empty())
					{
						for (auto id : query_remove[queryId])
						{
							auto iter = lower_bound(
								cache_rows,
								id,
								std::less{},
								[](const auto& row) { return get<0>(row); });

							if (get<0>(*iter) == id)
							{
								cache_rows.erase(iter);
							}
						}
					}
					query_remove[queryId].clear();
				}
			}

			++query_stack;

			for (const auto& row : cache_rows)
			{
				if (!std::apply(fn, row))
					break;
			}

			--query_stack;
		}

	private:

		void queryTrackInsertForId(Id id, int table)
		{
			for (const auto Qid : query_to_tables[table])
			{
				query_remove[Qid].erase(id);
				query_insert[Qid].insert(id);
			}
		}

		void queryTrackRemoveForId(Id id, int table)
		{
			for (const auto Qid : query_to_tables[table])
			{
				query_insert[Qid].erase(id);
				query_remove[Qid].insert(id);
			}
		}

		template <typename RowT, int n = 0>
		constexpr Table<RowT>& GetTable() noexcept
		{
			if constexpr (is_same_v<std::tuple_element_t<n, decltype(m_tables)>, Table<RowT>>)
			{
				return std::get<n>(m_tables);
			}
			else if constexpr (NumTables - 1 > n)
			{
				return GetTable<RowT, n + 1>();
			}
			else
			{
				static_assert(false, "table not in database");
			}
		}

		template <typename RowT, int n = 0>
		static constexpr int GetTableIndex()
		{
			if constexpr (is_same_v<std::tuple_element_t<n, decltype(m_tables)>, Table<RowT>>)
			{
				return n;
			}
			else if constexpr (NumTables - 1 > n)
			{
				return GetTableIndex<RowT, n + 1>();
			}
			else
			{
				static_assert(false, "table not in database");
			}
		}

	public:

		/*
		* Here is where we possibly put an id into a table with initial data
		*   if it is new we'll track the membership and add to a pending list for that table
		*/
		template <typename RowT>
		void Set(Id id, const RowT& row) noexcept
		{
			auto& tab = GetTable<RowT>();
			tab.set(id, row);

			tuple<Id, int> membership{ id, GetTableIndex<RowT>() };

			auto mem_id = [](const auto& a, const auto& b) { return get<0>(a) < get<0>(b) ? true : get<1>(a) < get<1>(b); };

			auto pos = lower_bound(id_members, membership, mem_id);
			if (pos == id_members.end() || *pos != membership)
			{
				id_members.insert(pos, membership);
			}

			queryTrackInsertForId(get<0>(membership), get<1>(membership));
		}
	};

	template<typename... RowTypes> int DbTables<RowTypes...>::s_instances = 1;
};
