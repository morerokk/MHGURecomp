# The APK (Makefile, PLATFORM=android; settings in android/android.mk), made with the SDK's own tools, no Gradle:
# SDL's Java sources (javac + d8 -> classes.dex), the manifest (aapt2), the assets, then the libraries (without their
# debug info, stored uncompressed and 16 KB aligned: Android maps them straight from the APK; the build folder keeps
# them whole, for symbols in crash backtraces), aligned (zipalign) and signed (apksigner).
#   make PLATFORM=android ABI=x86_64            build-android/mhgurecomp.apk: every ABI built so far
#                                                 (build-android/lib/<abi>/), signed with the SDK's debug key
#   make PLATFORM=android ABI=x86_64 install    that APK onto the running emulator / the connected phone (adb)
#   make PLATFORM=android dist VERSION=0.13.0   the release: dist/Android-MHGURecomp.<version>.apk, arm64-v8a only,
#                                                 signed with the release key (on Windows, make dist does this too)
#   make PLATFORM=android release-key           that key, once ever (below)
# The assets are what a PC release has beside the exe: pipelines.list, licenses/ and bonus/system (BONUS), which the
# app copies into its folder (runtime/platform/android.c).
BUILD_TOOLS := $(lastword $(sort $(wildcard $(ANDROID_SDK)/build-tools/*)))
ANDROID_JAR := $(lastword $(sort $(wildcard $(ANDROID_SDK)/platforms/android-*/android.jar)))
TARGET_API  := $(patsubst android-%,%,$(notdir $(patsubst %/,%,$(dir $(ANDROID_JAR)))))
ADB         := $(ANDROID_SDK)/platform-tools/adb
APK         := $(B)/mhgurecomp.apk
APK_O       := obj-android/apk
JAVA_SRC    := $(SDL3_DIR)/src/android-project/app/src/main/java/org/libsdl/app
DEBUG_KEY   := $(HOME)/.android/debug.keystore
# The release key: every release must be signed with the same one, or Android refuses it as an update, and players
# would have to uninstall first (losing the app's folder: their game files and saves). Made once, by make release-key,
# with a random password in the .pass file beside it: keep both safe, and copy them to every PC that makes releases.
# (In the Windows profile's .android, where the SDK keeps its own: MSYS2's HOME isn't the same in every shell, and
# make dist never makes a new key by itself.)
RELEASE_KEY ?= $(or $(call to_unix,$(USERPROFILE)),$(HOME))/.android/mhgurecomp-release.keystore
RELEASE_PASS = $(RELEASE_KEY).pass
# versionCode: the commits so far (grows with every build of a newer commit); versionName: the build's VERSION
APK_VERSION_CODE := $(shell git rev-list --count HEAD 2>/dev/null || echo 1)
# (the .bat tools of the SDK need the JDK, found through JAVA_HOME)
SDK_ENV     := JAVA_HOME="$(call win_path,$(JDK))" PATH="$(JDK)/bin:$$PATH"
APK_DEPS     = android/AndroidManifest.xml $(APK_O)/classes.dex $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(SDL_DEPS) \
               android/apk.py extras/pipelines.list $(LICENSE_TXT) $(BONUS)
DEBUG_SIGN   = --ks "$(call win_path,$(DEBUG_KEY))" --ks-pass pass:android --key-pass pass:android
# (no --key-pass: the keystore's; apksigner would read a second line of the same file for it. No v4 signature: it only
# serves adb's incremental install, and would leave an .idsig file beside the APK in dist/)
RELEASE_SIGN = --ks "$(call win_path,$(RELEASE_KEY))" --ks-pass "file:$(call win_path,$(RELEASE_PASS))" \
               --v4-signing-enabled false
DIST_APK     = $(DIST)/Android-MHGURecomp.$(VERSION).apk

# $(call pack_apk,<apk>,<ABIs>,<apksigner's key options>)
define pack_apk
@mkdir -p $(B)/lib/$(ABI) $(APK_O) $(dir $(1))
cp $(SDL_DEPS) $(B)/lib/$(ABI)/
rm -rf $(APK_O)/assets && mkdir -p $(APK_O)/assets/bonus && cp extras/pipelines.list $(APK_O)/assets/ && \
    cp -r $(B)/licenses $(APK_O)/assets/ $(if $(BONUS),&& cp "$(BONUS)" $(APK_O)/assets/bonus/system)
"$(BUILD_TOOLS)/aapt2" link -o $(APK_O)/base.apk --manifest android/AndroidManifest.xml -I "$(call win_path,$(ANDROID_JAR))" \
    -A $(APK_O)/assets --min-sdk-version $(API) --target-sdk-version $(TARGET_API) \
    --version-code $(APK_VERSION_CODE) --version-name $(VERSION)
rm -rf $(APK_O)/lib && for a in $(2); do mkdir -p $(APK_O)/lib/$$a; \
    for f in $(B)/lib/$$a/*.so; do "$(LLVM)/llvm-strip" --strip-debug -o $(APK_O)/lib/$$a/$$(basename $$f) $$f || exit 1; done; done
$(PY) android/apk.py $(APK_O)/base.apk $(APK_O)/unaligned.apk $(APK_O)/classes.dex $(APK_O)/lib
"$(BUILD_TOOLS)/zipalign" -f -P 16 4 $(APK_O)/unaligned.apk $(APK_O)/aligned.apk
$(SDK_ENV) "$(BUILD_TOOLS)/apksigner.bat" sign $(3) --out $(1) $(APK_O)/aligned.apk
@echo "apk: $(1) ($$(du -h $(1) | cut -f1), ABIs: $(2))"
endef

apk: $(APK)

$(APK_O)/classes.dex: $(SDL3_DIR)/.built
	rm -rf $(APK_O)/classes $(APK_O)/dex && mkdir -p $(APK_O)/classes $(APK_O)/dex
	"$(JDK)/bin/javac" -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(call win_path,$(ANDROID_JAR))" \
	    -d $(APK_O)/classes $(JAVA_SRC)/*.java
	$(SDK_ENV) "$(BUILD_TOOLS)/d8.bat" --release --min-api $(API) --lib "$(call win_path,$(ANDROID_JAR))" --output $(APK_O)/dex \
	    $$(find $(APK_O)/classes -name '*.class')
	cp $(APK_O)/dex/classes.dex $@

$(DEBUG_KEY):
	@mkdir -p $(dir $@)
	"$(JDK)/bin/keytool" -genkeypair -keystore "$(call win_path,$@)" -storepass android -alias androiddebugkey \
	    -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"

release-key:
	@test ! -e "$(RELEASE_KEY)" || { echo "apk: $(RELEASE_KEY) exists already: releases keep that one"; exit 1; }
	@mkdir -p $(dir $(RELEASE_KEY))
	@$(PY) -c "import secrets; print(secrets.token_urlsafe(24), end='')" > "$(RELEASE_PASS)"
	"$(JDK)/bin/keytool" -genkeypair -keystore "$(call win_path,$(RELEASE_KEY))" -storetype PKCS12 \
	    -storepass:file "$(call win_path,$(RELEASE_PASS))" -keypass:file "$(call win_path,$(RELEASE_PASS))" \
	    -alias mhgurecomp -keyalg RSA -keysize 4096 -validity 36500 -dname "CN=MHGURecomp"
	@echo "apk: made the release key $(RELEASE_KEY) (password in $(RELEASE_PASS)): back both up, every release needs them"
$(RELEASE_KEY):
	@echo "dist: no release key $@: copy it (with its .pass) from the PC that made the earlier releases,"; \
	 echo "      or for the very first release make it: make PLATFORM=android release-key"; exit 1

# (the libraries: whichever ABIs build-android/lib/ has; rebuilt whenever this ABI's change)
$(APK): $(APK_DEPS) $(DEBUG_KEY)
	$(call pack_apk,$@,$(notdir $(wildcard $(B)/lib/*)),$(DEBUG_SIGN))

dist: $(APK_DEPS) $(RELEASE_KEY)
	$(CHECK_VERSION)
	@test "$(ABI)" = arm64-v8a || { echo "dist: the release is arm64-v8a (phones): ABI=arm64-v8a"; exit 1; }
	$(call pack_apk,$(DIST_APK),arm64-v8a,$(RELEASE_SIGN))
	@echo "dist: $(DIST_APK)"; echo "      $$(stat -c %s $(DIST_APK)) bytes, sha256:$$(sha256sum $(DIST_APK) | cut -d' ' -f1)"

install: $(APK)
	"$(ADB)" install -r $(APK)

.PHONY: apk install release-key
