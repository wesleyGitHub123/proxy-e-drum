# PlatformIO extra_script for [env:native]: force fully static linking.
#
# PlatformIO routes `-static*` from build_flags into CCFLAGS only, so the
# test binaries were dynamically linked against MinGW's libstdc++-6.dll —
# which this MinGW install doesn't even ship, and an incompatible copy from
# elsewhere on PATH caused STATUS_ENTRYPOINT_NOT_FOUND at load time.
# Static linking makes every native test binary self-contained.
Import("env")

env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
