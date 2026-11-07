CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -fPIC -Iinclude
LDFLAGS ?= ../../libmali_kbase_emu.so -Wl,-rpath,$(abspath ../..) -ldl -lpthread
AR ?= ar

TARGET_SHARED = lib/libr54p3_vulkan_icd.so
TARGET_STATIC = lib/libr54p3_vulkan_icd.a
TARGET_JSON   = lib/r54p3_vulkan_icd.json

all: dirs $(TARGET_SHARED) $(TARGET_STATIC) json

dirs:
	@mkdir -p lib

lib/r54p3_vulkan_icd.o: src/r54p3_vulkan_icd.cpp include/*.h*
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET_SHARED): lib/r54p3_vulkan_icd.o
	$(CXX) $(CXXFLAGS) -shared $< -o $@ $(LDFLAGS)

$(TARGET_STATIC): lib/r54p3_vulkan_icd.o
	$(AR) rcs $@ $<

json: $(TARGET_SHARED)
	@printf '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n    "library_path": "%s/lib/libr54p3_vulkan_icd.so",\n    "api_version": "1.3.250"\n  }\n}\n' "$(CURDIR)" > lib/r54p3_decompiled_icd.x86_64.json

clean:
	rm -rf lib/*.o $(TARGET_SHARED) $(TARGET_STATIC) lib/r54p3_decompiled_icd.x86_64.json

.PHONY: all dirs json clean
