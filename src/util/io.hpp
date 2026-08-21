#ifndef ARIA_IO_HPP
#define ARIA_IO_HPP

#include <iostream>
#include <print>
#include "common.hpp"

namespace aria::io {

    using std::print;
    using std::println;

    inline String readline(std::istream &is = std::cin) {
        String line;
        std::getline(is, line);
        return line;
    }

    inline char readchar(std::istream &is) { return static_cast<char>(is.get()); }

    inline char readchar() { return readchar(std::cin); }
} // namespace aria::io


#endif // ARIA_IO_HPP
