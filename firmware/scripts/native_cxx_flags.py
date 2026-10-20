# C++-only flags for env:native. build_flags also reach C sources (Unity), where
# these options are invalid and -Werror turns the warning into a build failure.
Import("env")  # noqa: F821  (injected by SCons)

env.Append(CXXFLAGS=["-std=c++17", "-fno-exceptions", "-fno-rtti"])  # noqa: F821
