Online Documentation
====================
See online documentation at:
 [http://iss.ices.utexas.edu/?p=projects/galois/doc/current/getting_started](http://iss.ices.utexas.edu/?p=projects/galois/doc/current/getting_started)


Basic Compiling
===============
We use cmake. Run the following commands to set up a basic build configuration:

```Shell
ROOT=`pwd`
mkdir -p build/default; cd build/default; cmake ${ROOT}
```

or

```Shell
mkdir -p build/debug; cd build/debug; cmake -DCMAKE_BUILD_TYPE=Debug ${ROOT}
```

More esoteric systems may require a toolchain file, check ../cmake/Toolchain
if there is a file corresponding to your system. If so, use the following
cmake command:

```Shell
cmake -C ${ROOT}/cmake/Toolchain/${platform}-tryrunresults.cmake \
  -DCMAKE_TOOLCHAIN_FILE=${ROOT}/cmake/Toolchain/${platform}.cmake ${ROOT}
```


Basic Use
=========
You can run the sample applications and make your own Galois programs directly
in the build tree without installing anything. Just add a subdirectory under
apps and copy a CMakeLists.txt file from another application.


Organization
============
Lonestar applications are in lonestar.
Many other applications are in apps.
libsubstrate is the basic threading and machine and OS support functions.
libruntime contains galois proper.
libgraphs contain galois data structures.
libllvm contain a couple pieces of llvm.


Installing
==========
If you want to install Galois as a library,

```Shell
cmake -DCMAKE_INSTALL_PREFIX=${installdir} ${ROOT}
make install
```

or, to speed up compilation,

```Shell
cmake -DCMAKE_INSTALL_PREFIX=${installdir} -DSKIP_COMPILE_APPS=1 ${ROOT}
make install
```


Using Installed Galois
======================
If you are using CMake, put something like the following CMakeLists.txt:

```CMake
set(CMAKE_PREFIX_PATH ${installdir}/lib/cmake/Galois ${CMAKE_PREFIX_PATH})
find_package(Galois REQUIRED)
include_directories(${Galois_INCLUDE_DIRS})
set(CMAKE_CXX_COMPILER ${Galois_CXX_COMPILER})
set(CMAKE_CXX_FLAGS  "${Galois_CXX_FLAGS} ${CMAKE_CXX_FLAGS}")
add_executable(app ...)
target_link_libraries(app ${Galois_LIBRARIES})
```

Using basic commands (although the specific commands vary by system):

```Shell
c++ -std=c++11 app.cpp -I${installdir}/include -L${installdir}/lib -lgalois
```