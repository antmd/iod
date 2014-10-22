#pragma once

#include <type_traits>
#include <algorithm>
#include <iod/symbol.hh>
#include <iod/grammar.hh>
#include <iod/utils.hh>
#include <iod/symbols.hh>
#include <iod/foreach.hh>
#include <iod/linq_aggregators.hh>
#include <iod/linq_evaluate.hh>

namespace iod
{
  using namespace s;

  namespace linq_internals
  {

    template <typename R, typename S>
    auto format_record(R& req, const S& r, std::enable_if_t<decltype(req.select)::_size != 0>* = nullptr)
    {
      // Format with a non empty select statement: select(f1 = t1[x], f2 = t2[y])
      return foreach(req.select) |
        [&r] (auto m) { return m.symbol() = evaluate(m.value(), r); };
    }

    template <typename R, typename S>
    auto format_record(R& req, const S& r,
                       std::enable_if_t<decltype(req.select)::_size == 0 and
                       !decltype(req.from)::template _has<_As_t>::value>* = nullptr)
    {
      // Format with an empty select statement and no table alias.
      //return r.template get_nth<0>();
      return r;
    }

    template <typename R, typename S>
    auto format_record(R& req, const S& r,
                       std::enable_if_t<decltype(req.select)::_size == 0 and
                       decltype(req.from)::template _has<_As_t>::value>* = nullptr)
    {
      // Format with an empty select statement and a table alias.
      return r;
    }

    template <typename R>
    auto compute_request_record_type(const R& req)
    {
      const auto& from_table = *req.from.table;
      typedef std::remove_reference_t<decltype(*req.from.table)> from_table_type;
      typedef typename from_table_type::value_type record_type1;

      return static_if<R::template _has<_Join_t>::value>
        ([](const auto& req){
          const auto& from_table = *req.from.table;
          const auto& join_table = *req.join.table;

          auto from_name = req.from.get(_As, s::_1);
          auto join_name = req.join.get(_As, s::_2);
          
          typedef decltype(select_format(req, D(from_name = typename decltype(from_table)::value_type(),
                                                join_name = typename decltype(join_table)::value_type())))
            record_type;
          record_type sample;
          return format_record(req, sample);
        },
        [](const auto& req){
          auto from_name = req.from.get(_As, _1);
          typedef decltype(D(from_name = typename from_table_type::value_type())) record_type1;
          record_type1 sample;
          return format_record(req, sample);
        }, req);
    }

    template <typename R, typename T, typename F>
    void exec_iteration(R& req, T& table, F f)
    {
      static_if<has_symbol<R, _Group_by_t>::value>
        ([] (auto& req, auto& table, auto f) {
          auto gb = req.get(_Group_by, D(_Criteria = 42));
          std::sort(table.begin(), table.end(), [&gb] (const auto& a, const auto& b)
                    {
                      return evaluate(gb.criteria, a) < evaluate(gb.criteria, b);
                    });

          for (int i = 0; i < table.size();)
          {
            int j = i + 1;
            while (j < table.size() and
                   evaluate(gb.criteria, table[i]) == evaluate(gb.criteria, table[i + j]))
              j++;
            T group(table.begin() + i, table.begin() + j);
            f(group);
            i = j;
          }
        },
          [] (auto& req, auto& table, auto f) {
            for (auto& t : table)
              f(t);
          }, req, table, f);
    }
    
    template <typename R, typename T, typename F>
    void exec_simple_iteration(R& req, const T& table, F f)
    {
      for (auto& t : table)
      {
        auto r = D(req.from.get(_As, _1) = t);
        if (evaluate(req.get(_Where, D(_Condition = true)).condition, r))
          f(format_record(req, r));
      }

    }

    template <typename R, typename T, typename F>
    void exec_order_by(R& req, T& table, F f)
    {
      if (req.has(_Order_by))
      {
        auto order = req.get(_Order_by, D(_Order = 1)).order;
        std::sort(table.begin(), table.end(), [&order] (const auto& a, const auto& b)
                  {
                    return evaluate(order, a) < evaluate(order, b);
                  });
      }
    }

    template <typename R, typename T, typename F>
    auto exec_aggregate(R& req, T& group, F f)
    {
      auto aggregators = req.select;
      auto aggrs = foreach(req.select) | [&] (auto& m) {
        //return m.symbol() = aggregate_initialize(m.value(), decltype(group[0])());
        return m.symbol() = aggregate_initialize(m.value(), group[0]);
      };
      for(auto e : group)
        foreach_attribute_value([&] (auto& a) { a.take(e); }, aggrs);

      return foreach(aggrs) | [] (auto& m) {
        return m.symbol() = m.value().result();
      };

    }

    template <typename R, typename F>
    void exec_table(R& req, F f)
    {
      typedef std::remove_reference_t<decltype(*req.from.table)> from_table_type;

      static_if<!has_symbol<R, _Inner_join_t>::value and
                !has_symbol<R, _Order_by_t>::value and
                !has_symbol<R, _Group_by_t>::value and
                decltype(req.select)::_empty
                >
        ([] (auto& req, auto f) {
          exec_simple_iteration(req, *req.from.table, f);
        },
        [] (auto& req, auto f) {

          // Compute { table1_name = table1, table2_name = table2, ...}
          auto tables = static_if<R::template _has<_Inner_join_t>::value>
            ([] (auto& req) {
              auto inner_join_name = req.inner_join.get(_As, _2);
              return D(req.from.get(_As, _1) = req.from.table,
                         req.inner_join.get(_As, _1) = req.inner_join.table);
            },
              [] (auto& req) {
                return D(req.from.get(_As, _1) = req.from.table);
              }, req);

          // The actual intermediate record type.
          auto record_sample = foreach(tables) | [&] (auto m) { return m.symbol() = (*tables[m])[0]; };
          typedef decltype(record_sample) record_type;

          // Compute the intermediate table on which we will iterate.
          auto v = static_if<has_symbol<R, _Inner_join_t>::value>
            ([] (auto& req, auto& tables) { // if join
              const auto& from_table = *tables.template get_nth<0>();
              const auto& join_table = *tables.template get_nth<1>();

              auto on_condition = req.inner_join.get(_On, true);
              auto where_condition = req.get(_Where, D(_Condition = true)).condition;

              auto record_sample = foreach(tables) | [&] (auto m) { return m.symbol() = (*tables[m])[0]; };
              typedef decltype(record_sample) record_type;

              // Bruteforce O(n2) inner join. => optimization?
              std::vector<record_type> out;
              for (int i = 0; i < from_table.size(); i++)
              {
                for (int j = 0; j < join_table.size(); j++)
                {
                  auto r = foreach(tables) | [&] (auto m) {
                    return m.symbol() = (*tables[m])[((void*)tables[m] == (void*)&from_table) ? i : j]; 
                  };
                  if (evaluate(on_condition, r) and evaluate(where_condition, r))
                    out.push_back(r);
                }
              }
              return out;
            },
            [] (auto& req, auto& tables) { // if no join
              auto record_sample = foreach(tables) | [&] (auto m) { return m.symbol() = (*tables[m])[0]; };
              typedef decltype(record_sample) record_type;

              auto table1 = *tables.template get_nth<0>();
              auto where_condition = req.get(_Where, D(_Condition = true)).condition;
              std::vector<record_type> out;
              for (int i = 0; i < table1.size(); i++)
              {
                auto r = foreach(tables) | [&] (auto m) { return m.symbol() = (*tables[m])[i]; };
                if (evaluate(where_condition, r))
                  out.push_back(r);
              }
              return out;
            }, req, tables);

          // Sort if requested.
          if (req.has(_Order_by))
            exec_order_by(req, v, f);

          static_if<has_aggregator<decltype(req.select)>::value>
            ([] (auto& req, auto& v, auto f) // If request contains aggregators.
             {
               static_if<has_symbol<R, _Group_by_t>::value>
                 ([] (auto& req, auto f, auto& v) { // If group_by
                   exec_iteration(req, v, 
                                  [&] (const auto& group) { return f(exec_aggregate(req, group, f)); });
                 },
                 [] (auto& req, auto f, auto& v) { // If no group by
                   f(exec_aggregate(req, v, f));
                 }, req, f, v);
             },
             [] (auto& req, auto& v, auto f) // If no aggregators.
             {

               static_if<has_symbol<R, _Group_by_t>::value>
                 ([] (auto& req, auto f, auto& v) { //If group_by
                   exec_iteration(req, v, 
                                  [&] (const auto& group) { 
                                    return static_if<decltype(req.select)::_empty>
                                      ([] (const auto& group) { return group; },
                                       [&req] (const auto& group) {
                                         std::vector<decltype(format_record(req, group[0]))> v;
                                         for (const auto& t : group) v.push_back(format_record(req, t));
                                         return v;
                                       });
                                  });
                 },
                   [] (auto& req, auto f, auto& v) { // If no group by
                     exec_iteration(req, v,
                                    [&] (const auto& elt) { return f(format_record(req, elt)); });
                   }, req, f, v);

             }, req, v, f);

          }, req, f);
    }

    template <typename T>
    struct query
    {
      query(const T& q) : q(q) {}

      template <typename U>
      auto make_query(U u)
      {
        return query<U>(u);
      }

      template <typename... E>
      auto select(const E&... e) { return make_query(cat(q, _Select = D(e...))); }

      template <typename E>
      auto where(E e) { return make_query(cat(q, _Where = D(_Condition = e))); }
      template <typename E>
      auto group_by(E e) { return make_query(cat(q, _Group_by = D(_Criteria = e))); }
      template <typename Q, typename... E>
      auto from(Q table, const E&... e) { return make_query(cat(q, _From = D(_Table = &table, e...))); }

      template <typename Q, typename... E>
      auto inner_join(Q table, E... e) { return make_query(cat(q,
                                                               _Inner_join = D(_Table = &table, e...))); }
      template <typename Q>
      auto order_by(Q order) { return make_query(cat(q, _Order_by = D(_Order = order))); }

      template <typename F>
      void operator|(F f) { return linq_internals::exec_table(q, f); }

      auto to_array() { 
        typedef decltype(linq_internals::compute_request_record_type(q)) record_type;
        std::vector<record_type> v;
        (*this) | [&v] (const auto& r) { v.push_back(r); };
        return v;
      }

      T q;
    };

  }


  auto linq = linq_internals::query<sio<>>(sio<>());

}
