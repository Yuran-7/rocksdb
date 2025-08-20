#include "my_thread_lib.h"
#include <iostream> // C++ 的输入输出流
#include <string>   // C++ 字符串

int main() {
    std::cout << "Calling create_my_thread from main program..." << std::endl;
    create_my_thread("This is a message from main!");

    std::cout << "\nCalling compress_data_snappy from main program..." << std::endl;
    const char* long_text = "This is a long string that we will try to compress using the Snappy library. It contains some repetitive words to potentially show good compression ratios. Let's see how well Snappy does with this example text.";
    compress_data_snappy(long_text);

    std::cout << "\nMain program finished." << std::endl;
    return 0;
}