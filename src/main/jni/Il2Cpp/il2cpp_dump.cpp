//
// Created by Perfare on 2020/7/4.
//

#include "il2cpp_dump.h"
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <jni.h>
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"


#define DO_API(r, n, p) r (*n) p

#include "il2cpp-api-functions.h"
#include "BNMUtils.h"

#undef DO_API

static void *il2cpp_handle = nullptr;
static uint64_t il2cpp_base = 0;

const char *GetPackageName() {
    char *application_id[256];
    FILE *fp = fopen("proc/self/cmdline", "r");
    if (fp) {
        fread(application_id, sizeof(application_id), 1, fp);
        fclose(fp);
    }
    return (const char *) application_id;
}

void init_il2cpp_api() {
#define DO_API(r, n, p) n = (r (*) p)dlsym(il2cpp_handle, #n)

#include "il2cpp-api-functions.h"

#undef DO_API
}

uint64_t get_module_base(const char *module_name) {
    uint64_t addr = 0;
    char line[1024];
    uint64_t start = 0;
    uint64_t end = 0;
    char flags[5];
    char path[PATH_MAX];

    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp)) {
            strcpy(path, "");
            sscanf(line, "%" PRIx64"-%" PRIx64" %s %*" PRIx64" %*x:%*x %*u %s\n", &start, &end,
                   flags, path);
#if defined(__aarch64__)
            if (strstr(flags, "x") == 0) //TODO
                continue;
#endif
            if (strstr(path, module_name)) {
                addr = start;
                break;
            }
        }
        fclose(fp);
    }
    return addr;
}

std::string get_method_modifier(uint32_t flags) {
    std::stringstream outPut;
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:
            outPut << "private ";
            break;
        case METHOD_ATTRIBUTE_PUBLIC:
            outPut << "public ";
            break;
        case METHOD_ATTRIBUTE_FAMILY:
            outPut << "protected ";
            break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:
            outPut << "internal ";
            break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC) {
        outPut << "static ";
    }
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            outPut << "override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT) {
            outPut << "sealed override ";
        }
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT) {
            outPut << "virtual ";
        } else {
            outPut << "override ";
        }
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
        outPut << "extern ";
    }
    return outPut.str();
}

bool _il2cpp_type_is_byref(const Il2CppType *type) {
    auto byref = type->byref;
    if (il2cpp_type_is_byref) {
        byref = il2cpp_type_is_byref(type);
    }
    return byref;
}

std::string dump_method(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Methods\n";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {
        outPut << "\t";
        
        uint32_t iflags = 0;
        auto flags = il2cpp_method_get_flags(method, &iflags);
        outPut << get_method_modifier(flags);
        //TODO genericContainerIndex
        auto return_type = il2cpp_method_get_return_type(method);
        if (_il2cpp_type_is_byref(return_type)) {
            outPut << "ref ";
        }
        auto return_class = il2cpp_class_from_type(return_type);
        outPut << il2cpp_class_get_name(return_class) << " " << il2cpp_method_get_name(method)
               << "(";
        auto param_count = il2cpp_method_get_param_count(method);
        for (int i = 0; i < param_count; ++i) {
            auto param = il2cpp_method_get_param(method, i);
            auto attrs = param->attrs;
            if (_il2cpp_type_is_byref(param)) {
                if (attrs & PARAM_ATTRIBUTE_OUT && !(attrs & PARAM_ATTRIBUTE_IN)) {
                    outPut << "out ";
                } else if (attrs & PARAM_ATTRIBUTE_IN && !(attrs & PARAM_ATTRIBUTE_OUT)) {
                    outPut << "in ";
                } else {
                    outPut << "ref ";
                }
            } else {
                if (attrs & PARAM_ATTRIBUTE_IN) {
                    outPut << "[In] ";
                }
                if (attrs & PARAM_ATTRIBUTE_OUT) {
                    outPut << "[Out] ";
                }
            }
            auto parameter_class = il2cpp_class_from_type(param);
            outPut << il2cpp_class_get_name(parameter_class) << " "
                   << il2cpp_method_get_param_name(method, i);
            outPut << ", ";
        }
        if (param_count > 0) {
            outPut.seekp(-2, outPut.cur);
        }
        outPut << ") { }";
        
        // Add RVA comment on the same line
        if (method->methodPointer) {
            outPut << " //0x";
            outPut << std::hex << (uint64_t) method->methodPointer - il2cpp_base;
        } else {
            outPut << " //0x0";
        }
        outPut << "\n";
        //TODO GenericInstMethod
    }
    return outPut.str();
}

std::string dump_property(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter)) {
        //TODO attribute
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get = il2cpp_property_get_get_method(prop);
        auto set = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);
        outPut << "\t";
        Il2CppClass *prop_class = nullptr;
        uint32_t iflags = 0;
        if (get) {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            prop_class = il2cpp_class_from_type(il2cpp_method_get_return_type(get));
        } else if (set) {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            auto param = il2cpp_method_get_param(set, 0);
            prop_class = il2cpp_class_from_type(param);
        }
        if (prop_class) {
            outPut << il2cpp_class_get_name(prop_class) << " " << prop_name << " { ";
            if (get) {
                outPut << "get; ";
            }
            if (set) {
                outPut << "set; ";
            }
            outPut << "}\n";
        } else {
            if (prop_name) {
                outPut << " // unknown property " << prop_name;
            }
        }
    }
    return outPut.str();
}

std::string dump_field(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Fields\n";
    auto is_enum = il2cpp_class_is_enum(klass);
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        //TODO attribute
        outPut << "\t";
        auto attrs = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:
                outPut << "private ";
                break;
            case FIELD_ATTRIBUTE_PUBLIC:
                outPut << "public ";
                break;
            case FIELD_ATTRIBUTE_FAMILY:
                outPut << "protected ";
                break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:
                outPut << "internal ";
                break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:
                outPut << "protected internal ";
                break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL) {
            outPut << "const ";
        } else {
            if (attrs & FIELD_ATTRIBUTE_STATIC) {
                outPut << "static ";
            }
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) {
                outPut << "readonly ";
            }
        }
        auto field_type = il2cpp_field_get_type(field);
        auto field_class = il2cpp_class_from_type(field_type);
        outPut << il2cpp_class_get_name(field_class) << " " << il2cpp_field_get_name(field);
        //TODO 获取构造函数初始化后的字段值
        if (attrs & FIELD_ATTRIBUTE_LITERAL && is_enum) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            outPut << " = " << std::dec << val;
        }
        outPut << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << "\n";
    }
    return outPut.str();
}

std::string dump_type(const Il2CppType *type) {
    std::stringstream outPut;
    auto *klass = il2cpp_class_from_type(type);
    outPut << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";
    auto flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) {
        outPut << "[Serializable]\n";
    }
    //TODO attribute
    auto is_valuetype = il2cpp_class_is_valuetype(klass);
    auto is_enum = il2cpp_class_is_enum(klass);
    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:
            outPut << "public ";
            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:
            outPut << "internal ";
            break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:
            outPut << "private ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:
            outPut << "protected ";
            break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:
            outPut << "protected internal ";
            break;
    }
    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED) {
        outPut << "static ";
    } else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
    } else if (!is_valuetype && !is_enum && flags & TYPE_ATTRIBUTE_SEALED) {
        outPut << "sealed ";
    }
    if (flags & TYPE_ATTRIBUTE_INTERFACE) {
        outPut << "interface ";
    } else if (is_enum) {
        outPut << "enum ";
    } else if (is_valuetype) {
        outPut << "struct ";
    } else {
        outPut << "class ";
    }
    outPut << il2cpp_class_get_name(klass); //TODO genericContainerIndex
    std::vector<std::string> extends;
    auto parent = il2cpp_class_get_parent(klass);
    if (!is_valuetype && !is_enum && parent) {
        auto parent_type = il2cpp_class_get_type(parent);
        if (parent_type->type != IL2CPP_TYPE_OBJECT) {
            extends.emplace_back(il2cpp_class_get_name(parent));
        }
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter)) {
        extends.emplace_back(il2cpp_class_get_name(itf));
    }
    if (!extends.empty()) {
        outPut << " : " << extends[0];
        for (int i = 1; i < extends.size(); ++i) {
            outPut << ", " << extends[i];
        }
    }
    outPut << "\n{";
    outPut << dump_field(klass);
    outPut << dump_property(klass);
    outPut << dump_method(klass);
    //TODO EventInfo
    outPut << "}\n";
    return outPut.str();
}

void il2cpp_dump(void *handle) {
    LOGI("il2cpp_handle: %p", handle);
    il2cpp_handle = handle;
    init_il2cpp_api();

    if (il2cpp_domain_assembly_open) {
        Dl_info dlInfo;
        if (dladdr((void *) il2cpp_domain_assembly_open, &dlInfo)) {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        } else {
            LOGW("dladdr error, using get_module_base.");
            il2cpp_base = get_module_base("libil2cpp.so");
        }
        LOGI("il2cpp_base: %" PRIx64"", il2cpp_base);
    } else {
        LOGE("Failed to initialize il2cpp api.");
        return;
    }

    auto domain = il2cpp_domain_get();
    LOGI("il2cpp_thread_attach");
    il2cpp_thread_attach(domain);

    // Start dump
    LOGI("Opening assemblies");

    std::stringstream imageOutput;
    std::vector<std::string> outPuts;

    // Replace with specific assembly names if known
    std::vector<const char*> assemblyNames = {
           
        "mscorlib.dll",
        "Assembly-CSharp.dll",
        "System.dll",
        "System.Xml.dll",
        "UnityEngine.UIElementsModule.dll",
        "PlayFab.dll",
        "Newtonsoft.Json.dll",
        "System.Data.dll",
        "System.Core.dll",
        "UnityEngine.CoreModule.dll",
        "Unity.TextMeshPro.dll",
        "DissonanceVoip.dll",
        "Assembly-CSharp-firstpass.dll",
        "UnityEngine.UI.dll",
        "UnityEngine.Purchasing.Stores.dll",
        "UnityEngine.TextCoreTextEngineModule.dll",
        "Mirror.dll",
        "Mono.Security.dll",
        "UnityEngine.IMGUIModule.dll",
        "Mirror.Components.dll",
        "AlmostEngine.dll",
        "Mirror.Examples.dll",
        "Unity.Services.Analytics.dll",
        "UnityEngine.AndroidJNIModule.dll",
        "IngameDebugConsole.Runtime.dll",
        "UnityEngine.dll",
        "Unity.Services.Core.Internal.dll",
        "UnityEngine.Purchasing.dll",
        "UnityEngine.Purchasing.Security.dll",
        "GoogleMobileAds.Android.dll",
        "System.Numerics.dll",
        "SimpleWebTransport.dll",
        "System.Xml.Linq.dll",
        "UnityEngine.PhysicsModule.dll",
        "UnityEngine.UIElementsNativeModule.dll",
        "kcp2k.dll",
        "XNode.dll",
        "UnityEngine.AnimationModule.dll",
        "UnityEngine.UnityWebRequestModule.dll",
        "Unity.Services.Core.Telemetry.dll",
        "UnityEngine.TextCoreFontEngineModule.dll",
        "UnityEngine.Physics2DModule.dll",
        "GoogleMobileAds.dll",
        "UnityEngine.AudioModule.dll",
        "Mirror.Authenticators.dll",
        "UnityEngine.AIModule.dll",
        "UnityEngine.UIModule.dll",
        "UnityEngine.XRModule.dll",
        "UnityEngine.ParticleSystemModule.dll",
        "UnityEngine.TerrainModule.dll",
        "GoogleMobileAds.Common.dll",
        "NavMeshComponentsExamples.dll",
        "UnityEngine.GameCenterModule.dll",
        "Telepathy.dll",
        "UnityEngine.TextRenderingModule.dll",
        "UnityEngine.InputLegacyModule.dll",
        "UnityEngine.SharedInternalsModule.dll",
        "NavMeshComponents.dll",
        "Unity.Services.Core.Registration.dll",
        "UnityEngine.UnityAnalyticsModule.dll",
        "GoogleMobileAds.Core.dll",
        "UnityEngine.SubsystemsModule.dll",
        "Purchasing.Common.dll",
        "Unity.Services.Core.dll",
        "UnityEngine.InputModule.dll",
        "UnityEngine.UnityWebRequestTextureModule.dll",
        "UnityEngine.JSONSerializeModule.dll",
        "UnityEngine.VRModule.dll",
        "Unity.Services.Core.Configuration.dll",
        "Unity.Services.Core.Scheduler.dll",
        "UnityEngine.ImageConversionModule.dll",
        "UnityEngine.TilemapModule.dll",
        "UnityEngine.VehiclesModule.dll",
        "UnityEngine.GridModule.dll",
        "UnityEngine.SpriteShapeModule.dll",
        "UnityEngine.TerrainPhysicsModule.dll",
        "UnityEngine.WindModule.dll",
        "UnityEngine.Purchasing.SecurityCore.dll",
        "Unity.Services.Core.Device.dll",
        "System.Configuration.dll",
        "System.Runtime.Serialization.dll",
        "Unity.Services.Core.Threading.dll",
        "UnityEngine.Purchasing.WinRTCore.dll",
        "where-allocations.dll",
        "System.Globalization.dll",
        "UnityEngine.Purchasing.AppleCore.dll",
        "UnityEngine.Purchasing.AppleMacosStub.dll",
        "UnityEngine.Purchasing.AppleStub.dll",
        "Unity.Services.Core.Environments.Internal.dll",
        "UnityEngine.Purchasing.WinRTStub.dll",
        
        // Add other assemblies as needed
    };

    for (const auto &assemblyName : assemblyNames) {
        LOGI("Opening assembly: %s", assemblyName);
        auto assembly = il2cpp_domain_assembly_open(domain, assemblyName);
        if (!assembly) {
            LOGW("Failed to open assembly: %s", assemblyName);
            continue;
        }

        auto image = il2cpp_assembly_get_image(assembly);
        imageOutput << "// Image: " << il2cpp_image_get_name(image) << "\n";

        if (il2cpp_image_get_class) {
            LOGI("Version greater than 2018.3");
            auto classCount = il2cpp_image_get_class_count(image);
            for (int j = 0; j < classCount; ++j) {
                auto klass = il2cpp_image_get_class(image, j);
                auto type = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
                auto outPut = imageOutput.str() + dump_type(type);
                outPuts.push_back(outPut);
            }
        } else {
            LOGE("il2cpp_image_get_class not supported");
        }
    }

    auto androidDataPath = std::string("/storage/emulated/0/Android/data/").append(GetPackageName()).append("/").append("dump.cs");

    LOGI("Save dump file to %s", androidDataPath.c_str());

    std::ofstream outStream(androidDataPath);
    outStream << imageOutput.str();
    for (const auto &output : outPuts) {
        outStream << output;
    }
    outStream.close();

    LOGI("dump done!");
}

