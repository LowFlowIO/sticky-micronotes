Import("env")
# IDF injects -Werror after platformio.ini flags. Re-assert at the end.
for key in ("CCFLAGS", "CXXFLAGS", "CFLAGS"):
    env.Append(**{key: ["-Wno-error", "-Wno-format-truncation", "-Wno-error=format-truncation"]})
