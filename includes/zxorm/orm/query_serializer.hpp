#pragma once

namespace zxorm {
    enum query_type_t {
        SELECT,
        DELETE,
    };

    static inline std::ostream & operator<< (std::ostream &out, const query_type_t& type) {
        switch (type) {
            case query_type_t::SELECT:
                out << "SELECT ";
                break;
            case query_type_t::DELETE:
                out << "DELETE ";
                break;
        };
        return out;
    };

    class QuerySerializer {
    public:
    private:
        query_type_t _type;
        const char* _from;

        std::ostream& column_clause(std::ostream& out) const {
            if (_type == query_type_t::SELECT) {
                out << "* ";
            }
            return out;
        }

        std::ostream& from_clause(std::ostream& out) const {
            out << "FROM `" << _from << "` ";
            return out;
        }

    public:
        QuerySerializer(query_type_t t, const char* from) : _type{t}, _from{from} {}

        std::string serialize(std::function<void(std::ostream&)> append) const {
            std::stringstream ss;
            ss << _type;
            column_clause(ss);
            from_clause(ss);
            append(ss);

            ss << ";";

            return ss.str();
        }
    };
};