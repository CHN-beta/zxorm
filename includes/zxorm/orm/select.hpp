#pragma once
#include <sqlite3.h>
#include "zxorm/common.hpp"
#include "zxorm/result.hpp"
#include "zxorm/orm/query.hpp"
#include "zxorm/orm/record_iterator.hpp"

namespace zxorm {
    namespace __select_detail {
        template <typename From, typename... U>
        struct SelectColumnClause {
            friend std::ostream & operator<< (std::ostream &out, const SelectColumnClause<From, U...>&) {
                out << "SELECT ";
                out << "`" << From::name << "`.* ";
                    std::apply([&](const auto&... a) {
                        ([&]() {
                            using table_t = std::remove_reference_t<decltype(a)>;
                            out << ", ";
                            out << "`" << table_t::name << "`.* ";
                         }(), ...);
                    }, std::tuple<U...>{});
                return out;
            }
        };

        template <typename T, size_t s>
        struct ColumnOffset{
            using type = T;
            static constexpr size_t offset = s;
        };


        template <size_t idx, typename... Ts>
        struct find_offset {
        private:
            static constexpr std::array<size_t, sizeof...(Ts)> _n_columns = { Ts::n_columns... };

            static constexpr size_t _sum (size_t i = 0U)
            {
                if (i >= idx || i >= sizeof...(Ts)) {
                    return 0;
                }
                return _n_columns[i] + _sum(i+1U);
            }

        public:
            static constexpr size_t value = _sum();
        };

        template<typename Target, typename ListHead, typename... ListTails>
        constexpr size_t find_index_of_type()
        {
            if constexpr (std::is_same<Target, ListHead>::value)
                return 0;
            else
                return 1 + find_index_of_type<Target, ListTails...>();
        };

        // unused base case
        template <typename T>
        struct with_offsets : std::false_type {};

        template <typename... T>
        struct with_offsets <std::tuple<T...>> {
            template <typename Needle, typename... Haystack>
            struct elem : std::type_identity<
                ColumnOffset<Needle, find_offset<find_index_of_type<Needle, Haystack...>(), Haystack...>::value>
            > { };

            using type = std::tuple<typename elem<T, T...>::type...>;
        };

        template <typename T>
        using with_offsets_t = typename with_offsets<T>::type;
    };

    template <class From, typename SelectedTablesTuple, typename ColumnClause, typename... JoinedTables>
    class Select : public Query<From, ColumnClause, JoinedTables...> {
    private:
        using Super = Query<From, ColumnClause, JoinedTables...>;
        static constexpr bool is_multi_table_select = std::tuple_size_v<SelectedTablesTuple> > 1;

        std::string _limit_clause;
        std::string _order_clause;

        virtual void serialize_limits(std::ostream& ss) override {
            ss << _order_clause << " " << _limit_clause;
        }

        // unused base case
        template <typename T>
        struct tuple_return : std::false_type {};

        template <typename... T>
        struct tuple_return <std::tuple<T...>> : std::type_identity<
            std::tuple<typename T::object_class...>
        > {};

        template <typename T>
        using tuple_return_t = typename tuple_return<T>::type;

        using return_t = std::conditional_t<
            is_multi_table_select,
            tuple_return_t<SelectedTablesTuple>,
            typename From::object_class
        >;

        // unused base case
        template <typename T>
        struct result_tuple : std::false_type {};

        template <typename... T>
        struct result_tuple <std::tuple<T...>> : std::type_identity<
            std::tuple<Result<typename T::object_class>...>
        > {};

        using result_tuple_t = typename result_tuple<SelectedTablesTuple>::type;

        static auto read_row(Statement& s) -> Result<return_t>
        {
            if constexpr (!is_multi_table_select) {
                return From::get_row(s);
            } else {
                auto us_res = std::apply([&](const auto&... a) {
                    auto get_row = [&](const auto& pair) {
                        using pair_t = std::remove_reference_t<decltype(pair)>;
                        using table_t = pair_t::type;
                        constexpr size_t offset = pair_t::offset;
                        return table_t::get_row(s, offset);
                    };


                    return result_tuple_t {
                        get_row(a)...
                    };
                }, __select_detail::with_offsets_t<SelectedTablesTuple>{});


                OptionalResult<return_t> us = std::nullopt;

                std::apply([&](const auto&... a) {
                    // if an error is in here we should return it
                    ([&]() {
                        if (!us.is_error() && a.is_error()) {
                            us = a.error();
                        }
                     }(), ...);

                    // if there is no error, set the values
                    if (!us.is_error()) {
                        us = { a.value()... };
                    }
                }, us_res);

                if (us.is_error()) {
                    return us.error();
                }

                return us.value();
            }
        }

    public:
        Select(sqlite3* handle, Logger logger) :
            Super(handle, logger) {}

        auto where(auto&&... args) {
            Super::where(std::forward<decltype(args)>(args)...);
            return *this;
        }

        template <Field field_a, Field field_b>
        auto join(join_type_t type = join_type_t::INNER) {
            Super::template join<field_a, field_b>(type);
            return *this;
        }

        template <FixedLengthString foreign_table>
        auto join(join_type_t type = join_type_t::INNER) {
            Super::template join<foreign_table>(type);
            return *this;
        }

        auto limit(unsigned long limit) {
            std::stringstream ss;
            ss << "LIMIT " << limit;
            _limit_clause = ss.str();
            return *this;
        }

        template <FixedLengthString field>
        auto order_by(order_t ord) {
            static_assert(not std::is_same_v<typename From::column_by_name<field>::type, std::false_type>,
                "ORDER BY clause must use a field beloning to the Table"
            );
            std::stringstream ss;
            ss << "ORDER BY `" << field.value << "` " << ord;
            _order_clause = ss.str();
            return *this;
        }

        auto one() -> OptionalResult<return_t>
        {
            assert(_limit_clause.empty());
            limit(1);
            ZXORM_GET_RESULT(Statement s, Super::prepare());
            ZXORM_TRY(s.step());
            if (s.done()) {
                return std::nullopt;
            }

            return read_row(s);
        }

        Result<RecordIterator<return_t>> many() {
            auto s = Super::prepare();
            if (s.is_error()) {
                return s.error();
            }
            return RecordIterator<return_t>(std::move(s.value()), read_row);
        }
    };
};
