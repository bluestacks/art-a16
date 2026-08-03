/*
 * Copyright (C) 2019 The Android Open Source Project
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

#if defined(ART_TARGET_ANDROID)

#define LOG_TAG "nativeloader"

#include "native_loader_namespace.h"

#include <android-base/strings.h>
#include <bionic/dlext_namespaces.h>
#include <dlfcn.h>
#include <log/log.h>
#include <nativebridge/native_bridge.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <functional>

using android::base::Error;

namespace android {

namespace {

constexpr const char* kDefaultNamespaceName = "default";
constexpr const char* kSystemNamespaceName = "system";

std::string GetLinkerError(bool is_bridged) {
  const char* msg = is_bridged ? NativeBridgeGetError() : dlerror();
  if (msg == nullptr) {
    return "no error";
  }
  return std::string(msg);
}

}  // namespace

Result<NativeLoaderNamespace> NativeLoaderNamespace::GetExportedNamespace(const std::string& name,
                                                                          bool is_bridged) {
  if (!is_bridged) {
    android_namespace_t* raw = android_get_exported_namespace(name.c_str());
    if (raw != nullptr) {
      return NativeLoaderNamespace(name, raw);
    }
  } else {
    native_bridge_namespace_t* raw = NativeBridgeGetExportedNamespace(name.c_str());
    if (raw != nullptr) {
      return NativeLoaderNamespace(name, raw);
    }
  }
  return Errorf("namespace {} does not exist or exported", name);
}

// The system namespace is called "default" for binaries in /system and
// "system" for those in the Runtime APEX. Try "system" first since
// "default" always exists.
Result<NativeLoaderNamespace> NativeLoaderNamespace::GetSystemNamespace(bool is_bridged) {
  if (Result<NativeLoaderNamespace> ns = GetExportedNamespace(kSystemNamespaceName, is_bridged);
      ns.ok()) {
    return ns;
  }
  if (Result<NativeLoaderNamespace> ns = GetExportedNamespace(kDefaultNamespaceName, is_bridged);
      ns.ok()) {
    return ns;
  }

  // If nothing is found, return NativeLoaderNamespace constructed from nullptr.
  // nullptr also means default namespace to the linker.
  if (!is_bridged) {
    return NativeLoaderNamespace(kDefaultNamespaceName, static_cast<android_namespace_t*>(nullptr));
  } else {
    return NativeLoaderNamespace(kDefaultNamespaceName,
                                 static_cast<native_bridge_namespace_t*>(nullptr));
  }
}

Result<NativeLoaderNamespace> NativeLoaderNamespace::Create(
    const std::string& name, const std::string& search_paths, const std::string& permitted_paths,
    const NativeLoaderNamespace* parent, bool is_shared, bool is_exempt_list_enabled,
    bool also_used_as_anonymous) {
  bool is_bridged = false;
  if (parent != nullptr) {
    is_bridged = parent->IsBridged();
  } else if (!search_paths.empty()) {
    is_bridged = NativeBridgeIsPathSupported(search_paths.c_str());
  }

  // Fall back to the system namespace if no parent is set.
  Result<NativeLoaderNamespace> system_ns = GetSystemNamespace(is_bridged);
  if (!system_ns.ok()) {
    return system_ns.error();
  }
  const NativeLoaderNamespace& effective_parent = parent != nullptr ? *parent : *system_ns;

  // All namespaces for apps are isolated
  uint64_t type = ANDROID_NAMESPACE_TYPE_ISOLATED;

  // The namespace is also used as the anonymous namespace
  // which is used when the linker fails to determine the caller address
  if (also_used_as_anonymous) {
    type |= ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS;
  }

  // Bundled apps have access to all system libraries that are currently loaded
  // in the default namespace
  if (is_shared) {
    type |= ANDROID_NAMESPACE_TYPE_SHARED;
  }
  if (is_exempt_list_enabled) {
    type |= ANDROID_NAMESPACE_TYPE_EXEMPT_LIST_ENABLED;
  }

  if (!is_bridged) {
    android_namespace_t* raw =
        android_create_namespace(name.c_str(), nullptr, search_paths.c_str(), type,
                                 permitted_paths.c_str(), effective_parent.ToRawAndroidNamespace());
    if (raw != nullptr) {
      return NativeLoaderNamespace(name, raw);
    }
  } else {
    native_bridge_namespace_t* raw = NativeBridgeCreateNamespace(
        name.c_str(), nullptr, search_paths.c_str(), type, permitted_paths.c_str(),
        effective_parent.ToRawNativeBridgeNamespace());
    if (raw != nullptr) {
      return NativeLoaderNamespace(name, raw);
    }
  }
  return Errorf("failed to create {} namespace name:{}, search_paths:{}, permitted_paths:{}",
                is_bridged ? "bridged" : "native", name, search_paths, permitted_paths);
}

Result<void> NativeLoaderNamespace::Link(const NativeLoaderNamespace* target,
                                         const std::string& shared_libs) const {
  LOG_ALWAYS_FATAL_IF(shared_libs.empty(), "empty share lib when linking %s to %s",
                      this->name().c_str(), target == nullptr ? "default" : target->name().c_str());
  if (!IsBridged()) {
    if (android_link_namespaces(this->ToRawAndroidNamespace(),
                                target == nullptr ? nullptr : target->ToRawAndroidNamespace(),
                                shared_libs.c_str())) {
      return {};
    }
  } else {
    if (NativeBridgeLinkNamespaces(this->ToRawNativeBridgeNamespace(),
                                   target == nullptr ? nullptr : target->ToRawNativeBridgeNamespace(),
                                   shared_libs.c_str())) {
      return {};
    }
  }
  return Error() << GetLinkerError(IsBridged());
}

namespace {

void* OpenSystemLibrary(const char* path, int flags) {
  android_namespace_t* system_ns = android_get_exported_namespace(kSystemNamespaceName);
  if (system_ns == nullptr) {
    system_ns = android_get_exported_namespace(kDefaultNamespaceName);
    LOG_ALWAYS_FATAL_IF(
        system_ns == nullptr, "Failed to get system namespace for loading %s", path);
  }
  const android_dlextinfo extinfo = {
      .flags = ANDROID_DLEXT_USE_NAMESPACE,
      .library_namespace = system_ns,
  };
  return android_dlopen_ext(path, flags, &extinfo);
}

bool IsHighFpsEnabled() {
  char value[PROP_VALUE_MAX] = {};
  return __system_property_get("bst.enable_high_fps", value) == 1 && value[0] == '1';
}

int GetExtendedPerformanceMode(bool check_process_name) {
  if (getuid() < 10000) {
    return 0;
  }
  const char* process_name = getprogname();
  if (check_process_name && process_name != nullptr && strcmp(process_name, "chrome_zygote") == 0) {
    return 0;
  }

  void* handle = OpenSystemLibrary("libbinder.so", RTLD_NOLOAD);
  if (handle == nullptr) {
    return 0;
  }
  using GetMode = int (*)(int);
  GetMode get_mode = reinterpret_cast<GetMode>(dlsym(handle, "bst_getXPerfMode"));
  const int mode = get_mode != nullptr ? get_mode(getuid()) : 0;
  dlclose(handle);
  return mode;
}

template <bool kUseNativeBridge>
class HighFpsHookLibrary {
 public:
  explicit HighFpsHookLibrary(const NativeLoaderNamespace& ns) {
    const int mode = GetExtendedPerformanceMode(!kUseNativeBridge);
    if (!((IsHighFpsEnabled() && mode == 1) || mode == 2)) {
      return;
    }

    constexpr const char* kArmLibrary =
        sizeof(void*) == 8 ? "/data/downloads/.xp/lib64/arm64/libstagefright_httpcommon.so"
                           : "/data/downloads/.xp/lib/arm/libstagefright_httpcommon.so";
    constexpr const char* kX86Library =
        sizeof(void*) == 8 ? "/data/downloads/.xp/lib64/libstagefright_httpcommon.so"
                           : "/data/downloads/.xp/lib/libstagefright_httpcommon.so";
    const char* library = kUseNativeBridge ? kArmLibrary : kX86Library;
    if (access(library, F_OK) != 0) {
      return;
    }

    if constexpr (kUseNativeBridge) {
      handle_ = NativeBridgeLoadLibraryExt(library, RTLD_NOW, ns.ToRawNativeBridgeNamespace());
    } else {
      const android_dlextinfo extinfo = {
          .flags = ANDROID_DLEXT_USE_NAMESPACE,
          .library_namespace = ns.ToRawAndroidNamespace(),
      };
      handle_ = android_dlopen_ext(library, RTLD_NOW, &extinfo);
    }
  }

  ~HighFpsHookLibrary() {
    if (handle_ == nullptr) {
      return;
    }
    if constexpr (kUseNativeBridge) {
      NativeBridgeUnloadLibrary(handle_);
    } else {
      dlclose(handle_);
    }
  }

  HighFpsHookLibrary(const HighFpsHookLibrary&) = delete;
  HighFpsHookLibrary& operator=(const HighFpsHookLibrary&) = delete;

 private:
  void* handle_ = nullptr;
};

bool IsHotFixApp() {
  if (getuid() < 10000) {
    return false;
  }
  const char* process_name = getprogname();
  if (process_name != nullptr && strcmp(process_name, "chrome_zygote") == 0) {
    return false;
  }

  void* handle = OpenSystemLibrary("libbinder.so", RTLD_NOLOAD);
  if (handle == nullptr) {
    return false;
  }
  using IsHotFixAppByUid = bool (*)(int32_t);
  IsHotFixAppByUid is_hotfix_app =
      reinterpret_cast<IsHotFixAppByUid>(dlsym(handle, "bstIsHotFixAppByUid"));
  const bool result = is_hotfix_app != nullptr && is_hotfix_app(getuid());
  dlclose(handle);
  return result;
}

class HotFixHookLibrary {
 public:
  explicit HotFixHookLibrary(const NativeLoaderNamespace& ns) {
    if (!IsHotFixApp()) {
      return;
    }
    constexpr const char* kLibrary = sizeof(void*) == 8 ? "/data/downloads/.hpp/libhpp64.so"
                                                        : "/data/downloads/.hpp/libhpp32.so";
    if (access(kLibrary, F_OK) != 0) {
      return;
    }

    const android_dlextinfo extinfo = {
        .flags = ANDROID_DLEXT_USE_NAMESPACE,
        .library_namespace = ns.ToRawAndroidNamespace(),
    };
    handle_ = android_dlopen_ext(kLibrary, RTLD_NOW, &extinfo);
    if (handle_ != nullptr) {
      using InstallHooks = void (*)();
      InstallHooks install_hooks = reinterpret_cast<InstallHooks>(dlsym(handle_, "hook_x86App"));
      if (install_hooks != nullptr) {
        install_hooks();
      }
    }
  }

  ~HotFixHookLibrary() {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

  HotFixHookLibrary(const HotFixHookLibrary&) = delete;
  HotFixHookLibrary& operator=(const HotFixHookLibrary&) = delete;

 private:
  void* handle_ = nullptr;
};

}  // namespace

Result<void*> NativeLoaderNamespace::Load(const char* lib_name) const {
  if (!IsBridged()) {
    static const HighFpsHookLibrary<false> high_fps_hook(*this);
    static const HotFixHookLibrary hotfix_hook(*this);
    android_dlextinfo extinfo;
    extinfo.flags = ANDROID_DLEXT_USE_NAMESPACE;
    extinfo.library_namespace = this->ToRawAndroidNamespace();
    void* handle = android_dlopen_ext(lib_name, RTLD_NOW, &extinfo);
    if (handle != nullptr) {
      return handle;
    }
  } else {
    static const HighFpsHookLibrary<true> high_fps_hook(*this);
    void* handle =
        NativeBridgeLoadLibraryExt(lib_name, RTLD_NOW, this->ToRawNativeBridgeNamespace());
    if (handle != nullptr) {
      return handle;
    }
  }
  return Error() << GetLinkerError(IsBridged());
}

}  // namespace android

#endif  // defined(ART_TARGET_ANDROID)
