#include "utilxx.h"
#include <iostream>
#include <string>

void utilxxPrintHello() {
    std::cout << "Hello utilxx!" << std::endl;
}

void _utilxxPrint(const char* str, int size, ...) {
        // 1. 定义 va_list
    va_list para_list; // 类型宏；参数列表

    // 2. 初始化 va_list
    va_start(para_list, size);

    vprintf((std::string{"[utilxx] "} + str + "\n").c_str(), para_list);

    va_end(para_list);
}