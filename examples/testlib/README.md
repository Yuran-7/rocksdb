静态库：
g++ -c my_thread_lib.cpp -o my_thread_lib.o
ar rcs libmythread.a my_thread_lib.o
g++ main.cpp -L. -lmythread -static -o my_app_static -pthread -lsnappy


动态库：
g++ -c -fPIC my_thread_lib.cpp -o my_thread_lib_pic.o
g++ -shared -o libmythread.so my_thread_lib_pic.o

g++ -shared -fPIC my_thread_lib.cpp -o libmythread.so
g++ main.cpp -L. -lmythread -o my_app_dynamic -pthread -lsnappy

动态库必须加-fPIC，因为它代表“Position-Independent Code”（位置无关代码）。
在构建静态库时，不加-pthread和-lsnappy都不会报错，生成可执行文件时，这个版本的GCC不加-pthread不会报错，不加-lsnappy会报错。不管构建静态库时加没加，生成可执行文件必须加。
加了-static，此时会尽可能地进行静态链接，不要依赖动态库，-L.表示除了标准的系统库路径（如 /lib, /usr/lib, /usr/local/lib 等）之外，也请在当前目录 (.) 中查找我所指定的库文件

在构建动态库时，不加-pthread和-lsnappy都不会报错。如果在构建动态库时加了，生成可执行文件可以不加