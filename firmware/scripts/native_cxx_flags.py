# Extra flags for the native envs (pre-script, see platformio.ini).
#
# 1. C++-only flags. build_flags also reach C sources (Unity), where these options are invalid
#    and -Werror turns the warning into a build failure.
# 2. Sanitizers, selected per env with `custom_sanitizers = address,undefined` or `thread`.
#    They must go to the compiler (C and C++) AND the linker: build_flags only passes -f... to
#    the compiler, and a binary linked without -fsanitize lacks the sanitizer runtime.
Import("env")  # noqa: F821  (injected by SCons)

env.Append(CXXFLAGS=["-std=c++17", "-fno-exceptions", "-fno-rtti"])  # noqa: F821

sanitizers = env.GetProjectOption("custom_sanitizers", "").strip()  # noqa: F821
if sanitizers:
    flags = [
        "-fsanitize=" + sanitizers,
        "-fno-omit-frame-pointer",  # readable stack traces in reports
        "-g",
        "-O1",  # recommended for ASan/TSan: fast enough, still precise reports
    ]
    if "undefined" in sanitizers:
        flags += [
            "-fno-sanitize-recover=all",  # any UB report aborts, so the test fails
            "-fno-sanitize=vptr",  # needs RTTI; the core builds with -fno-rtti
        ]
    env.Append(CCFLAGS=flags, LINKFLAGS=flags)  # noqa: F821
