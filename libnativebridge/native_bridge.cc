/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "nativebridge"

#include "nativebridge/native_bridge.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "android-base/macros.h"
#include "log/log.h"

#ifdef ART_TARGET_ANDROID
#include <bionic/dlext_namespaces.h>
#endif

namespace android {

extern "C" {

void* OpenSystemLibrary(const char* path, int flags) {
#ifdef ART_TARGET_ANDROID
  // The system namespace is called "default" for binaries in /system and
  // "system" for those in the Runtime APEX. Try "system" first since
  // "default" always exists.
  // TODO(b/185587109): Get rid of this error prone logic.
  android_namespace_t* system_ns = android_get_exported_namespace("system");
  if (system_ns == nullptr) {
    system_ns = android_get_exported_namespace("default");
    LOG_ALWAYS_FATAL_IF(system_ns == nullptr,
                        "Failed to get system namespace for loading %s", path);
  }
  const android_dlextinfo dlextinfo = {
      .flags = ANDROID_DLEXT_USE_NAMESPACE,
      .library_namespace = system_ns,
  };
  return android_dlopen_ext(path, flags, &dlextinfo);
#else
  return dlopen(path, flags);
#endif
}

// Environment values required by the apps running with native bridge.
struct NativeBridgeRuntimeValues {
    const char* os_arch;
    const char* cpu_abi;
    const char* cpu_abi2;
    const char* *supported_abis;
    int32_t abi_count;
};

// The symbol name exposed by native-bridge with the type of NativeBridgeCallbacks.
static constexpr const char* kNativeBridgeInterfaceSymbol = "NativeBridgeItf";

enum class NativeBridgeState {
  kNotSetup,                        // Initial state.
  kOpened,                          // After successful dlopen.
  kPreInitialized,                  // After successful pre-initialization.
  kInitialized,                     // After successful initialization.
  kClosed                           // Closed or errors.
};

static constexpr const char* kNotSetupString = "kNotSetup";
static constexpr const char* kOpenedString = "kOpened";
static constexpr const char* kPreInitializedString = "kPreInitialized";
static constexpr const char* kInitializedString = "kInitialized";
static constexpr const char* kClosedString = "kClosed";

static const char* GetNativeBridgeStateString(NativeBridgeState state) {
  switch (state) {
    case NativeBridgeState::kNotSetup:
      return kNotSetupString;

    case NativeBridgeState::kOpened:
      return kOpenedString;

    case NativeBridgeState::kPreInitialized:
      return kPreInitializedString;

    case NativeBridgeState::kInitialized:
      return kInitializedString;

    case NativeBridgeState::kClosed:
      return kClosedString;
  }
}

// Current state of the native bridge.
static NativeBridgeState g_state = NativeBridgeState::kNotSetup;

// The version of NativeBridge implementation.
// Different Nativebridge interface needs the service of different version of
// Nativebridge implementation.
// Used by isCompatibleWith() which is introduced in v2.
enum NativeBridgeImplementationVersion {
  // first version, not used.
  DEFAULT_VERSION = 1,
  // The version which signal semantic is introduced.
  SIGNAL_VERSION = 2,
  // The version which namespace semantic is introduced.
  NAMESPACE_VERSION = 3,
  // The version with vendor namespaces
  VENDOR_NAMESPACE_VERSION = 4,
  // The version with runtime namespaces
  RUNTIME_NAMESPACE_VERSION = 5,
  // The version with pre-zygote-fork hook to support app-zygotes.
  PRE_ZYGOTE_FORK_VERSION = 6,
  // The version with critical_native support
  CRITICAL_NATIVE_SUPPORT_VERSION = 7,
  // The version with native bridge detection fallback for function pointers
  IDENTIFY_NATIVELY_BRIDGED_FUNCTION_POINTERS_VERSION = 8,
};

// Whether we had an error at some point.
static bool g_had_error = false;

// Handle of the loaded library.
static void* g_native_bridge_handle = nullptr;
// Pointer to the callbacks. Available as soon as LoadNativeBridge succeeds, but only initialized
// later.
static const NativeBridgeCallbacks* g_callbacks = nullptr;
// Callbacks provided by the environment to the bridge. Passed to LoadNativeBridge.
static const NativeBridgeRuntimeCallbacks* g_runtime_callbacks = nullptr;

// The app's code cache directory.
static char* g_app_code_cache_dir = nullptr;

// Code cache directory (relative to the application private directory)
// Ideally we'd like to call into framework to retrieve this name. However that's considered an
// implementation detail and will require either hacks or consistent refactorings. We compromise
// and hard code the directory name again here.
static constexpr const char* kCodeCacheDir = "code_cache";

// Characters allowed in a native bridge filename. The first character must
// be in [a-zA-Z] (expected 'l' for "libx"). The rest must be in [a-zA-Z0-9._-].
static bool CharacterAllowed(char c, bool first) {
  if (first) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
  } else {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           (c == '.') || (c == '_') || (c == '-');
  }
}

static void ReleaseAppCodeCacheDir() {
  if (g_app_code_cache_dir != nullptr) {
    delete[] g_app_code_cache_dir;
    g_app_code_cache_dir = nullptr;
  }
}

// We only allow simple names for the library. It is supposed to be a file in
// /system/lib or /vendor/lib. Only allow a small range of characters, that is
// names consisting of [a-zA-Z0-9._-] and starting with [a-zA-Z].
bool NativeBridgeNameAcceptable(const char* nb_library_filename) {
  const char* ptr = nb_library_filename;
  if (*ptr == 0) {
    // Emptry string. Allowed, means no native bridge.
    return true;
  } else {
    // First character must be [a-zA-Z].
    if (!CharacterAllowed(*ptr, true))  {
      // Found an invalid fist character, don't accept.
      ALOGE("Native bridge library %s has been rejected for first character %c",
            nb_library_filename,
            *ptr);
      return false;
    } else {
      // For the rest, be more liberal.
      ptr++;
      while (*ptr != 0) {
        if (!CharacterAllowed(*ptr, false)) {
          // Found an invalid character, don't accept.
          ALOGE("Native bridge library %s has been rejected for %c", nb_library_filename, *ptr);
          return false;
        }
        ptr++;
      }
    }
    return true;
  }
}

// The policy of invoking Nativebridge changed in v3 with/without namespace.
// Suggest Nativebridge implementation not maintain backward-compatible.
static bool isCompatibleWith(const uint32_t version) {
  // Libnativebridge is now designed to be forward-compatible. So only "0" is an unsupported
  // version.
  if (g_callbacks == nullptr || g_callbacks->version == 0 || version == 0) {
    return false;
  }

  // If this is a v2+ bridge, it may not be forwards- or backwards-compatible. Check.
  if (g_callbacks->version >= SIGNAL_VERSION) {
    return g_callbacks->isCompatibleWith(version);
  }

  return true;
}

static void CloseNativeBridge(bool with_error) {
  g_state = NativeBridgeState::kClosed;
  g_had_error |= with_error;
  ReleaseAppCodeCacheDir();
}

bool LoadNativeBridge(const char* nb_library_filename,
                      const NativeBridgeRuntimeCallbacks* runtime_cbs) {
  // We expect only one place that calls LoadNativeBridge: Runtime::Init. At that point we are not
  // multi-threaded, so we do not need locking here.

  if (g_state != NativeBridgeState::kNotSetup) {
    // Setup has been called before. Ignore this call.
    if (nb_library_filename != nullptr) {  // Avoids some log-spam for dalvikvm.
      ALOGW("Called LoadNativeBridge for an already set up native bridge. State is %s.",
            GetNativeBridgeStateString(g_state));
    }
    // Note: counts as an error, even though the bridge may be functional.
    g_had_error = true;
    return false;
  }

  if (nb_library_filename == nullptr || *nb_library_filename == 0) {
    CloseNativeBridge(false);
    return false;
  } else {
    if (!NativeBridgeNameAcceptable(nb_library_filename)) {
      CloseNativeBridge(true);
    } else {
      // Try to open the library. We assume this library is provided by the
      // platform rather than the ART APEX itself, so use the system namespace
      // to avoid requiring a static linker config link to it from the
      // com_android_art namespace.
      void* handle = OpenSystemLibrary(nb_library_filename, RTLD_LAZY);

      if (handle != nullptr) {
        g_callbacks =
            reinterpret_cast<NativeBridgeCallbacks*>(dlsym(handle, kNativeBridgeInterfaceSymbol));
        if (g_callbacks != nullptr) {
          if (isCompatibleWith(NAMESPACE_VERSION)) {
            // Store the handle for later.
            g_native_bridge_handle = handle;
          } else {
            ALOGW("Unsupported native bridge API in %s (is version %d not compatible with %d)",
                  nb_library_filename,
                  g_callbacks->version,
                  NAMESPACE_VERSION);
            g_callbacks = nullptr;
            dlclose(handle);
          }
        } else {
          dlclose(handle);
          ALOGW("Unsupported native bridge API in %s: %s not found",
                nb_library_filename, kNativeBridgeInterfaceSymbol);
        }
      } else {
        ALOGW("Failed to load native bridge implementation: %s", dlerror());
      }

      // Two failure conditions: could not find library (dlopen failed), or could not find native
      // bridge interface (dlsym failed). Both are an error and close the native bridge.
      if (g_callbacks == nullptr) {
        CloseNativeBridge(true);
      } else {
        g_runtime_callbacks = runtime_cbs;
        g_state = NativeBridgeState::kOpened;
      }
    }
    return g_state == NativeBridgeState::kOpened;
  }
}

bool NeedsNativeBridge(const char* instruction_set) {
  if (instruction_set == nullptr) {
    ALOGE("Null instruction set in NeedsNativeBridge.");
    return false;
  }
  return strncmp(instruction_set, ABI_STRING, strlen(ABI_STRING) + 1) != 0;
}

#ifdef ART_TARGET_ANDROID
static bool BindMountCpuinfo(const char* source) {
  const bool proc_mounted =
      TEMP_FAILURE_RETRY(mount(source, "/proc/cpuinfo", nullptr, MS_BIND, nullptr)) == 0;
  if (!proc_mounted) {
    ALOGW("Failed to bind-mount %s as /proc/cpuinfo: %s", source, strerror(errno));
  }

  if (TEMP_FAILURE_RETRY(mount(source, "/etc/cpuinfo", nullptr, MS_BIND, nullptr)) == -1) {
    ALOGW("Failed to bind-mount %s as /etc/cpuinfo: %s", source, strerror(errno));
  }
  return proc_mounted;
}
#endif

static void MountCpuinfoForInstructionSet(const char* instruction_set) {
  if (instruction_set == nullptr || strlen(instruction_set) > 10) {
    if (instruction_set != nullptr) {
      ALOGW("Instruction set %s is malformed", instruction_set);
    }
    return;
  }

#ifdef ART_TARGET_ANDROID
  char path[512];
  snprintf(path, sizeof(path), "/data/downloads/.oh/cpuinfo/%s/cpuinfo", instruction_set);
  if (BindMountCpuinfo(path)) {
    return;
  }

  snprintf(path, sizeof(path), "/system/etc/houdini/cpuinfo.%s.txt", instruction_set);
  if (BindMountCpuinfo(path)) {
    return;
  }

  snprintf(path, sizeof(path), "/system/etc/cpuinfo.%s.txt", instruction_set);
  if (BindMountCpuinfo(path)) {
    return;
  }

#ifdef __LP64__
  snprintf(path, sizeof(path), "/system/lib64/%s/cpuinfo", instruction_set);
#else
  snprintf(path, sizeof(path), "/system/lib/%s/cpuinfo", instruction_set);
#endif
  BindMountCpuinfo(path);
#endif
}

static char g_package_name[256];

extern "C" const char* bst_nbpname() { return g_package_name; }

static void InitializePackageName(const char* app_data_dir) {
  g_package_name[0] = '\0';
  if (app_data_dir == nullptr) {
    return;
  }
  const char* last_separator = strrchr(app_data_dir, '/');
  const char* package_name = last_separator == nullptr ? app_data_dir : last_separator + 1;
  snprintf(g_package_name, sizeof(g_package_name), "%s", package_name);
}

bool PreInitializeNativeBridge(const char* app_data_dir_in, const char* instruction_set) {
  if (g_state != NativeBridgeState::kOpened) {
    ALOGE("Invalid state: native bridge is expected to be opened.");
    CloseNativeBridge(true);
    return false;
  }

  InitializePackageName(app_data_dir_in);
  if (app_data_dir_in != nullptr) {
    // Create the path to the application code cache directory.
    // The memory will be release after Initialization or when the native bridge is closed.
    const size_t len = strlen(app_data_dir_in) + strlen(kCodeCacheDir) + 2;  // '\0' + '/'
    g_app_code_cache_dir = new char[len];
    snprintf(g_app_code_cache_dir, len, "%s/%s", app_data_dir_in, kCodeCacheDir);
  } else {
    ALOGW("Application private directory isn't available.");
    g_app_code_cache_dir = nullptr;
  }

  // Failure is non-fatal. The translated runtime can still initialize with the platform fallback.
  MountCpuinfoForInstructionSet(instruction_set);

  g_state = NativeBridgeState::kPreInitialized;
  return true;
}

void PreZygoteForkNativeBridge() {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(PRE_ZYGOTE_FORK_VERSION)) {
      return g_callbacks->preZygoteFork();
    } else {
      ALOGE("not compatible with version %d, preZygoteFork() isn't invoked",
            PRE_ZYGOTE_FORK_VERSION);
    }
  }
}

static bool UidListedInFile(const char* path, uid_t uid) {
#ifdef ART_TARGET_ANDROID
  FILE* file = fopen(path, "re");
  if (file == nullptr) {
    return false;
  }

  char line[64];
  bool found = false;
  while (fgets(line, sizeof(line), file) != nullptr) {
    errno = 0;
    char* end = nullptr;
    const unsigned long value = strtoul(line, &end, 10);
    if (errno == 0 && end != line && value == uid) {
      found = true;
      break;
    }
  }
  fclose(file);
  return found;
#else
  (void)path;
  (void)uid;
  return false;
#endif
}

static void SetCpuAbi(JNIEnv* env, jclass build_class, const char* field, const char* value) {
  if (value == nullptr) {
    return;
  }
  jfieldID field_id = env->GetStaticFieldID(build_class, field, "Ljava/lang/String;");
  if (field_id == nullptr) {
    env->ExceptionClear();
    ALOGW("Could not find Build.%s", field);
    return;
  }
  jstring string_value = env->NewStringUTF(value);
  if (string_value == nullptr) {
    env->ExceptionClear();
    return;
  }
  env->SetStaticObjectField(build_class, field_id, string_value);
}

static bool SetStringArrayField(JNIEnv* env,
                                jclass build_class,
                                const char* field,
                                const char* const* values,
                                size_t value_count) {
  jfieldID field_id = env->GetStaticFieldID(build_class, field, "[Ljava/lang/String;");
  if (field_id == nullptr) {
    env->ExceptionClear();
    ALOGW("Could not find Build.%s", field);
    return false;
  }
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    env->ExceptionClear();
    return false;
  }
  jobjectArray array = env->NewObjectArray(static_cast<jsize>(value_count), string_class, nullptr);
  if (array == nullptr) {
    env->ExceptionClear();
    return false;
  }
  for (size_t i = 0; i < value_count; ++i) {
    jstring value = env->NewStringUTF(values[i]);
    if (value == nullptr) {
      env->ExceptionClear();
      return false;
    }
    env->SetObjectArrayElement(array, i, value);
  }
  env->SetStaticObjectField(build_class, field_id, array);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  return true;
}

static void SetCpuAbiList(JNIEnv* env, jclass build_class) {
  static constexpr const char* kArm32Abis[] = {"armeabi-v7a", "armeabi"};
#ifdef __LP64__
  static constexpr const char* kArm64Abis[] = {"arm64-v8a"};
  static constexpr const char* kAllArmAbis[] = {"arm64-v8a", "armeabi-v7a", "armeabi"};
  if (!SetStringArrayField(env, build_class, "SUPPORTED_ABIS", kAllArmAbis, 3) ||
      !SetStringArrayField(env, build_class, "SUPPORTED_64_BIT_ABIS", kArm64Abis, 1)) {
    return;
  }
#else
  if (!SetStringArrayField(env, build_class, "SUPPORTED_ABIS", kArm32Abis, 2)) {
    return;
  }
#endif
  SetStringArrayField(env, build_class, "SUPPORTED_32_BIT_ABIS", kArm32Abis, 2);
}

// Set up the environment for the bridged app.
static void SetupEnvironment(const NativeBridgeCallbacks* cbs, JNIEnv* env, const char* isa) {
  // Need a JNIEnv* to do anything.
  if (env == nullptr) {
    ALOGW("No JNIEnv* to set up app environment.");
    return;
  }

  // Query the bridge for environment values.
  const struct NativeBridgeRuntimeValues* env_values = cbs->getAppEnv(isa);
  if (env_values == nullptr) {
    return;
  }

  // Keep the JNIEnv clean.
  jint success = env->PushLocalFrame(16);  // That should be small and large enough.
  if (success < 0) {
    // Out of memory, really borked.
    ALOGW("Out of memory while setting up app environment.");
    env->ExceptionClear();
    return;
  }

  constexpr const char* kXarchApps = "/data/downloads/.tmp/.bstXarchApps";
  constexpr const char* kAbilistApps = "/data/downloads/.tmp/.bstAbilistApps";
  if (!UidListedInFile(kXarchApps, getuid()) &&
      (env_values->cpu_abi != nullptr || env_values->cpu_abi2 != nullptr ||
       env_values->abi_count >= 0)) {
    jclass build_class = env->FindClass("android/os/Build");
    if (build_class != nullptr) {
      SetCpuAbi(env, build_class, "CPU_ABI", env_values->cpu_abi);
      SetCpuAbi(env, build_class, "CPU_ABI2", env_values->cpu_abi2);
      if (UidListedInFile(kAbilistApps, getuid())) {
        SetCpuAbiList(env, build_class);
      }
    } else {
      env->ExceptionClear();
      ALOGW("Could not find Build class");
    }
  }

  if (env_values->os_arch != nullptr) {
    jclass sclass_id = env->FindClass("java/lang/System");
    if (sclass_id != nullptr) {
      jmethodID set_prop_id = env->GetStaticMethodID(sclass_id, "setUnchangeableSystemProperty",
          "(Ljava/lang/String;Ljava/lang/String;)V");
      if (set_prop_id != nullptr) {
        // Init os.arch to the value reqired by the apps running with native bridge.
        env->CallStaticVoidMethod(sclass_id, set_prop_id, env->NewStringUTF("os.arch"),
            env->NewStringUTF(env_values->os_arch));
      } else {
        env->ExceptionClear();
        ALOGW("Could not find System#setUnchangeableSystemProperty.");
      }
    } else {
      env->ExceptionClear();
      ALOGW("Could not find System class.");
    }
  }

  // Make it pristine again.
  env->PopLocalFrame(nullptr);
}

bool InitializeNativeBridge(JNIEnv* env, const char* instruction_set) {
  // We expect only one place that calls InitializeNativeBridge: Runtime::DidForkFromZygote. At that
  // point we are not multi-threaded, so we do not need locking here.

  if (g_state != NativeBridgeState::kPreInitialized) {
    CloseNativeBridge(true);
    return false;
  }

  if (g_app_code_cache_dir != nullptr) {
    // Check for code cache: if it doesn't exist try to create it.
    struct stat st;
    if (stat(g_app_code_cache_dir, &st) == -1) {
      if (errno == ENOENT) {
        if (mkdir(g_app_code_cache_dir, S_IRWXU | S_IRWXG | S_IXOTH) == -1) {
          ALOGW(
              "Cannot create code cache directory %s: %s.", g_app_code_cache_dir, strerror(errno));
          ReleaseAppCodeCacheDir();
        }
      } else {
        ALOGW("Cannot stat code cache directory %s: %s.", g_app_code_cache_dir, strerror(errno));
        ReleaseAppCodeCacheDir();
      }
    } else if (!S_ISDIR(st.st_mode)) {
      ALOGW("Code cache is not a directory %s.", g_app_code_cache_dir);
      ReleaseAppCodeCacheDir();
    }
  }

  if (g_callbacks->initialize(g_runtime_callbacks, g_app_code_cache_dir, instruction_set)) {
    // TODO(b/419835068): SetupEnvironment is likely not needed anymore and can be removed.
    SetupEnvironment(g_callbacks, env, instruction_set);
    g_state = NativeBridgeState::kInitialized;
    // We no longer need the code cache path, release the memory.
    ReleaseAppCodeCacheDir();
  } else {
    // Unload the library.
    dlclose(g_native_bridge_handle);
    CloseNativeBridge(true);
  }

  return g_state == NativeBridgeState::kInitialized;
}

void UnloadNativeBridge() {
  // We expect only one place that calls UnloadNativeBridge: Runtime::DidForkFromZygote. At that
  // point we are not multi-threaded, so we do not need locking here.

  switch (g_state) {
    case NativeBridgeState::kOpened:
    case NativeBridgeState::kPreInitialized:
    case NativeBridgeState::kInitialized:
      // Unload.
      dlclose(g_native_bridge_handle);
      CloseNativeBridge(false);
      break;

    case NativeBridgeState::kNotSetup:
      // Not even set up. Error.
      CloseNativeBridge(true);
      break;

    case NativeBridgeState::kClosed:
      // Ignore.
      break;
  }
}

bool NativeBridgeError() {
  return g_had_error;
}

bool NativeBridgeAvailable() {
  return g_state == NativeBridgeState::kOpened ||
         g_state == NativeBridgeState::kPreInitialized ||
         g_state == NativeBridgeState::kInitialized;
}

bool NativeBridgeInitialized() {
  // Calls of this are supposed to happen in a state where the native bridge is stable, i.e., after
  // Runtime::DidForkFromZygote. In that case we do not need a lock.
  return g_state == NativeBridgeState::kInitialized;
}

void* NativeBridgeLoadLibrary(const char* libpath, int flag) {
  if (NativeBridgeInitialized()) {
    return g_callbacks->loadLibrary(libpath, flag);
  }
  return nullptr;
}

void* NativeBridgeGetTrampoline(void* handle, const char* name, const char* shorty,
                                uint32_t len) {
  return NativeBridgeGetTrampoline2(handle, name, shorty, len, kJNICallTypeRegular);
}

void* NativeBridgeGetTrampoline2(
    void* handle, const char* name, const char* shorty, uint32_t len, JNICallType jni_call_type) {
  if (!NativeBridgeInitialized()) {
    return nullptr;
  }

  // For version 1 isCompatibleWith is always true, even though the extensions
  // are not supported, so we need to handle it separately.
  if (g_callbacks != nullptr && g_callbacks->version == DEFAULT_VERSION) {
    return g_callbacks->getTrampoline(handle, name, shorty, len);
  }

  if (isCompatibleWith(CRITICAL_NATIVE_SUPPORT_VERSION)) {
    return g_callbacks->getTrampolineWithJNICallType(handle, name, shorty, len, jni_call_type);
  }

  return g_callbacks->getTrampoline(handle, name, shorty, len);
}

void* NativeBridgeGetTrampolineForFunctionPointer(const void* method,
                                                  const char* shorty,
                                                  uint32_t len,
                                                  JNICallType jni_call_type) {
  if (!NativeBridgeInitialized()) {
    return nullptr;
  }

  if (isCompatibleWith(CRITICAL_NATIVE_SUPPORT_VERSION)) {
    return g_callbacks->getTrampolineForFunctionPointer(method, shorty, len, jni_call_type);
  } else {
    ALOGE("not compatible with version %d, getTrampolineFnPtrWithJNICallType() isn't invoked",
          CRITICAL_NATIVE_SUPPORT_VERSION);
    return nullptr;
  }
}

bool NativeBridgeIsSupported(const char* libpath) {
  if (NativeBridgeInitialized()) {
    return g_callbacks->isSupported(libpath);
  }
  return false;
}

uint32_t NativeBridgeGetVersion() {
  if (NativeBridgeAvailable()) {
    return g_callbacks->version;
  }
  return 0;
}

NativeBridgeSignalHandlerFn NativeBridgeGetSignalHandler(int signal) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(SIGNAL_VERSION)) {
      return g_callbacks->getSignalHandler(signal);
    } else {
      ALOGE("not compatible with version %d, cannot get signal handler", SIGNAL_VERSION);
    }
  }
  return nullptr;
}

int NativeBridgeUnloadLibrary(void* handle) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return g_callbacks->unloadLibrary(handle);
    } else {
      ALOGE("not compatible with version %d, cannot unload library", NAMESPACE_VERSION);
    }
  }
  return -1;
}

const char* NativeBridgeGetError() {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return g_callbacks->getError();
    } else {
      return "native bridge implementation is not compatible with version 3, cannot get message";
    }
  }
  return "native bridge is not initialized";
}

bool NativeBridgeIsPathSupported(const char* path) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return g_callbacks->isPathSupported(path);
    } else {
      ALOGE("not compatible with version %d, cannot check via library path", NAMESPACE_VERSION);
    }
  }
  return false;
}

native_bridge_namespace_t* NativeBridgeCreateNamespace(const char* name,
                                                       const char* ld_library_path,
                                                       const char* default_library_path,
                                                       uint64_t type,
                                                       const char* permitted_when_isolated_path,
                                                       native_bridge_namespace_t* parent_ns) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return g_callbacks->createNamespace(name,
                                          ld_library_path,
                                          default_library_path,
                                          type,
                                          permitted_when_isolated_path,
                                          parent_ns);
    } else {
      ALOGE("not compatible with version %d, cannot create namespace %s", NAMESPACE_VERSION, name);
    }
  }

  return nullptr;
}

bool NativeBridgeLinkNamespaces(native_bridge_namespace_t* from, native_bridge_namespace_t* to,
                                const char* shared_libs_sonames) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return g_callbacks->linkNamespaces(from, to, shared_libs_sonames);
    } else {
      ALOGE("not compatible with version %d, cannot init namespace", NAMESPACE_VERSION);
    }
  }

  return false;
}

native_bridge_namespace_t* NativeBridgeGetExportedNamespace(const char* name) {
  if (!NativeBridgeInitialized()) {
    return nullptr;
  }

  if (isCompatibleWith(RUNTIME_NAMESPACE_VERSION)) {
    return g_callbacks->getExportedNamespace(name);
  }

  // sphal is vendor namespace name -> use v4 callback in the case NB g_callbacks
  // are not compatible with v5
  if (isCompatibleWith(VENDOR_NAMESPACE_VERSION) && name != nullptr && strcmp("sphal", name) == 0) {
    return g_callbacks->getVendorNamespace();
  }

  return nullptr;
}

class NativeBridgeLibraryHandle {
 public:
  explicit NativeBridgeLibraryHandle(void* handle) : handle_(handle) {}
  ~NativeBridgeLibraryHandle() {
    if (handle_ != nullptr) {
      NativeBridgeUnloadLibrary(handle_);
    }
  }

  NativeBridgeLibraryHandle(const NativeBridgeLibraryHandle&) = delete;
  NativeBridgeLibraryHandle& operator=(const NativeBridgeLibraryHandle&) = delete;

 private:
  void* handle_;
};

#if defined(__LP64__) && defined(ART_TARGET_ANDROID)
static bool ReadSmallFile(const char* path, std::string* contents) {
  constexpr off_t kMaximumSize = 1024 * 1024;
  FILE* file = fopen(path, "re");
  if (file == nullptr) {
    return false;
  }

  struct stat status;
  if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
      status.st_size > kMaximumSize) {
    fclose(file);
    return false;
  }

  contents->resize(status.st_size);
  const size_t bytes_read = fread(contents->data(), 1, contents->size(), file);
  fclose(file);
  if (bytes_read != contents->size()) {
    contents->clear();
    return false;
  }
  return true;
}

static bool PackageListed(const char* path, const char* package_name) {
  if (package_name == nullptr || package_name[0] == '\0') {
    return false;
  }
  std::string contents;
  if (!ReadSmallFile(path, &contents)) {
    return false;
  }

  size_t start = 0;
  while (start <= contents.size()) {
    const size_t end = contents.find(';', start);
    const size_t length = (end == std::string::npos ? contents.size() : end) - start;
    if (length == strlen(package_name) && contents.compare(start, length, package_name) == 0) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}
#endif

static void* LoadPmfLibrary(native_bridge_namespace_t* ns) {
#if defined(__LP64__) && defined(ART_TARGET_ANDROID)
  constexpr const char* kPmfApps = "/data/downloads/.tmp/.pmf";
  if (PackageListed(kPmfApps, g_package_name)) {
    return g_callbacks->loadLibraryExt("/data/downloads/libpmf.so", RTLD_NOW, ns);
  }
#else
  (void)ns;
#endif
  return nullptr;
}

class HotFixHooks {
 public:
  explicit HotFixHooks(const char* package_name) {
#ifdef ART_TARGET_ANDROID
    if (package_name == nullptr || package_name[0] == '\0') {
      return;
    }

    void* binder = OpenSystemLibrary("libbinder.so", RTLD_NOW);
    if (binder == nullptr) {
      return;
    }
    using IsHotFixApp = bool (*)(const char*);
    IsHotFixApp is_hotfix_app = reinterpret_cast<IsHotFixApp>(dlsym(binder, "bstIsHotFixApp"));
    const bool enabled = is_hotfix_app != nullptr && is_hotfix_app(package_name);
    dlclose(binder);
    if (!enabled) {
      return;
    }

    constexpr const char* kLibrary = sizeof(void*) == 8 ? "/data/downloads/.hpp/libhpp64.so"
                                                        : "/data/downloads/.hpp/libhpp32.so";
    handle_ = OpenSystemLibrary(kLibrary, RTLD_NOW);
    if (handle_ == nullptr) {
      return;
    }
    after_load_ = reinterpret_cast<PatchFunction>(dlsym(handle_, "hppa"));
    before_load_ = reinterpret_cast<PatchFunction>(dlsym(handle_, "hppb"));
    if (after_load_ == nullptr || before_load_ == nullptr) {
      dlclose(handle_);
      handle_ = nullptr;
      after_load_ = nullptr;
      before_load_ = nullptr;
    }
#else
    (void)package_name;
#endif
  }

  ~HotFixHooks() {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

  void BeforeLoad(const char* library) const {
    if (before_load_ != nullptr) {
      before_load_(library);
    }
  }

  void AfterLoad(const char* library) const {
    if (after_load_ != nullptr) {
      after_load_(library);
    }
  }

  HotFixHooks(const HotFixHooks&) = delete;
  HotFixHooks& operator=(const HotFixHooks&) = delete;

 private:
  using PatchFunction = void (*)(const char*);
  void* handle_ = nullptr;
  PatchFunction after_load_ = nullptr;
  PatchFunction before_load_ = nullptr;
};

void* NativeBridgeLoadLibraryExt(const char* libpath, int flag, native_bridge_namespace_t* ns) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      static const NativeBridgeLibraryHandle pmf_library(LoadPmfLibrary(ns));
      static const HotFixHooks hotfix_hooks(g_package_name);
      hotfix_hooks.BeforeLoad(libpath);
      void* handle = g_callbacks->loadLibraryExt(libpath, flag, ns);
      if (handle != nullptr) {
        hotfix_hooks.AfterLoad(libpath);
      }
      return handle;
    } else {
      ALOGE("not compatible with version %d, cannot load library in namespace", NAMESPACE_VERSION);
    }
  }
  return nullptr;
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void* method) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(IDENTIFY_NATIVELY_BRIDGED_FUNCTION_POINTERS_VERSION)) {
      return g_callbacks->isNativeBridgeFunctionPointer(method);
    } else {
      ALOGW("not compatible with version %d, unable to call isNativeBridgeFunctionPointer",
            IDENTIFY_NATIVELY_BRIDGED_FUNCTION_POINTERS_VERSION);
    }
  }
  return false;
}

}  // extern "C"

}  // namespace android
