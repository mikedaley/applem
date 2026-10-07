# ApplEm, the native macOS app. Builds go in build-macos; four jobs at most.

BUILD := build-macos
APP   := $(BUILD)/native/ApplEm.app

.PHONY: build run test release clean submodules

build: submodules
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) --target ApplEmNative -j 4

run: build
	open $(APP)

# The app's own tests. The core's are run in the core (cd core; see its README).
test: submodules
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j 4
	cd $(BUILD) && ctest --output-on-failure

# Signed with Developer ID, notarised and stapled (scripts/build-native-mac.sh).
release: submodules
	scripts/build-native-mac.sh

submodules:
	@test -f core/CMakeLists.txt -a -f native/third_party/imgui/imgui.h || git submodule update --init

clean:
	rm -rf $(BUILD)
