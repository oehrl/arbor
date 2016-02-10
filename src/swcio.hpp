#pragma once

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace nestmc
{

namespace io
{


class cell_record 
{
public:
    using id_type = int;

    // FIXME: enum's are not completely type-safe, since they can accept
    // anything that can be casted to their underlying type.
    // 
    // More on SWC files: http://research.mssm.edu/cnic/swc.html
    enum kind {
        undefined = 0,
        soma,
        axon,
        dendrite,
        apical_dendrite,
        fork_point,
        end_point,
        custom
    };

    // cell records assume zero-based indexing; root's parent remains -1
    cell_record(kind type, int id, 
                float x, float y, float z, float r,
                int parent_id)
        : type_(type)
        , id_(id)
        , x_(x)
        , y_(y)
        , z_(z)
        , r_(r)
        , parent_id_(parent_id)
    {
        check_consistency();
    }
    
    cell_record()
        : type_(cell_record::undefined)
        , id_(0)
        , x_(0)
        , y_(0)
        , z_(0)
        , r_(0)
        , parent_id_(-1)
    { }

    cell_record(const cell_record &other) = default;
    cell_record &operator=(const cell_record &other) = default;

    // Equality and comparison operators
    friend bool operator==(const cell_record &lhs,
                           const cell_record &rhs)
    {
        return lhs.id_ == rhs.id_;
    }

    friend bool operator<(const cell_record &lhs,
                          const cell_record &rhs)
    {
        return lhs.id_ < rhs.id_;
    }

    friend bool operator<=(const cell_record &lhs,
                           const cell_record &rhs)
    {
        return (lhs < rhs) || (lhs == rhs);
    }

    friend bool operator!=(const cell_record &lhs,
                           const cell_record &rhs)
    {
        return !(lhs == rhs);
    }

    friend bool operator>(const cell_record &lhs,
                          const cell_record &rhs)
    {
        return !(lhs < rhs) && (lhs != rhs);
    }

    friend bool operator>=(const cell_record &lhs,
                           const cell_record &rhs)
    {
        return !(lhs < rhs);
    }

    friend std::ostream &operator<<(std::ostream &os, const cell_record &cell);

    kind type() const
    {
        return type_;
    }

    id_type id() const
    {
        return id_;
    }

    id_type parent() const
    {
        return parent_id_;
    }

    float x() const
    {
        return x_;
    }

    float y() const
    {
        return y_;
    }

    float z() const
    {
        return z_;
    }

    float radius() const
    {
        return r_;
    }

    float diameter() const
    {
        return 2*r_;
    }

    void renumber(id_type new_id, std::map<id_type, id_type> &idmap);

private:
    void check_consistency() const;

    kind type_;         // cell type
    id_type id_;        // cell id
    float x_, y_, z_;   // cell coordinates
    float r_;           // cell radius
    id_type parent_id_; // cell parent's id
};

class swc_parse_error : public std::runtime_error
{
public:
    explicit swc_parse_error(const char *msg)
        : std::runtime_error(msg)
    { }

    explicit swc_parse_error(const std::string &msg)
        : std::runtime_error(msg)
    { }
};

class swc_parser
{
public:
    swc_parser(const std::string &delim,
               std::string comment_prefix)
        : delim_(delim)
        , comment_prefix_(comment_prefix)
    { }

    swc_parser()
        : delim_(" ")
        , comment_prefix_("#")
    { }

    std::istream &parse_record(std::istream &is, cell_record &cell);

private:
    // Read the record from a string stream; will be treated like a single line
    cell_record parse_record(std::istringstream &is);

    std::string delim_;
    std::string comment_prefix_;
    std::string linebuff_;
};


std::istream &operator>>(std::istream &is, cell_record &cell);

//
// Reads cells from an input stream until an eof is encountered and returns a
// cleaned sequence of cell records.
//
// For more information check here:
//   https://github.com/eth-cscs/cell_algorithms/wiki/SWC-file-parsing
//
std::vector<cell_record> swc_read_cells(std::istream &is);

}   // end of nestmc::io
}   // end of nestmc