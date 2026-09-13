#ifndef ARIA_IO_HPP
#define ARIA_IO_HPP

#include <iostream>
#include <print>
#include "common.hpp"

namespace aria::io {

    using std::print;
    using std::println;

    // 流结束与「读到空行/合法字节」在返回值上无差别（EOF 不单独表达），调用方需自行查流状态。
    inline String readline(std::istream& is = std::cin) {
        String line;
        std::getline(is, line);
        return line;
    }

    // get() 返回 int 再窄化为 char：EOF（traits::eof）与合法字节 0xFF 不可区分，同样需另查流状态。
    inline char readchar(std::istream& is) { return static_cast<char>(is.get()); }

    inline char readchar() { return readchar(std::cin); }
} // namespace aria::io


#endif // ARIA_IO_HPP
