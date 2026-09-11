/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/d3d12_provider.h"

#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"
#include "xenia/ui/d3d12/d3d12_immediate_drawer.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_util.h"
#include "xenia/ui/redist_installer_wx.h"
DEFINE_bool(d3d12_debug, false, "Enable Direct3D 12 and DXGI debug layer.",
            "D3D12");
DEFINE_bool(d3d12_gpu_validation, false,
            "Enable Direct3D 12 GPU-based validation to catch out-of-bounds "
            "shader resource access. Requires --d3d12_debug. Very slow.",
            "D3D12");
DEFINE_bool(d3d12_dred, false,
            "Enable Direct3D 12 Device Removed Extended Data (DRED) to log the "
            "operation and allocations involved in a device removal. Works "
            "without the debug layer.",
            "D3D12");
DEFINE_bool(d3d12_break_on_error, false,
            "Break on Direct3D 12 validation errors.", "D3D12");
DEFINE_bool(d3d12_break_on_warning, false,
            "Break on Direct3D 12 validation warnings.", "D3D12");
DEFINE_int32(d3d12_adapter, -1,
             "Index of the DXGI adapter to use. "
             "-1 for any physical adapter, -2 for WARP software rendering.",
             "D3D12");
DEFINE_int32(
    d3d12_queue_priority, 1,
    "Graphics (direct) command queue scheduling priority, 0 - normal, 1 - "
    "high, 2 - global realtime (requires administrator privileges, may impact "
    "system responsibility)",
    "D3D12");

namespace xe {
namespace ui {
namespace d3d12 {

bool D3D12Provider::IsD3D12APIAvailable() {
  HMODULE library_d3d12 = LoadLibraryW(L"D3D12.dll");
  if (!library_d3d12) {
    return false;
  }
  FreeLibrary(library_d3d12);
  return true;
}

const std::string& D3D12Provider::GetAdapterDescription() const {
  return adapter_description_;
}

void D3D12Provider::DumpDeviceRemovedData() const {
  ID3D12DeviceRemovedExtendedData* dred;
  HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dred));
  if (FAILED(hr)) {
    XELOGW("DRED: not available on this device (HRESULT 0x{:08X})",
           uint32_t(hr));
    return;
  }
  // Breadcrumbs and page-fault data are captured independently, so each is
  // reported on its own - a missing half is itself a finding.
  bool breadcrumbs_captured = false;
  // Breadcrumbs identify the last GPU operation that ran before the removal.
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs;
  hr = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
  if (FAILED(hr)) {
    XELOGW("DRED: breadcrumbs unavailable (HRESULT 0x{:08X})", uint32_t(hr));
  } else {
    uint32_t node_count = 0, unfinished_count = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node =
             breadcrumbs.pHeadAutoBreadcrumbNode;
         node; node = node->pNext) {
      ++node_count;
      uint32_t op_count = node->BreadcrumbCount;
      uint32_t completed =
          node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      if (completed >= op_count) {
        // This command list finished, so it is not where the GPU stopped.
        continue;
      }
      ++unfinished_count;
      XELOGE(
          "DRED: command list '{}' on queue '{}' stopped after {} of {} ops, "
          "next op was {}",
          node->pCommandListDebugNameA ? node->pCommandListDebugNameA
                                       : "<unnamed>",
          node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA
                                        : "<unnamed>",
          completed, op_count, uint32_t(node->pCommandHistory[completed]));
    }
    breadcrumbs_captured = node_count != 0;
    if (!node_count) {
      XELOGW("DRED: the breadcrumb ring is empty - no command list recorded");
    } else if (!unfinished_count) {
      XELOGW(
          "DRED: all {} recorded command lists completed - the fault is not "
          "in a recorded operation",
          node_count);
    }
  }
  // Page-fault data names the allocation a bad GPU address belonged to.
  bool page_fault_captured = false;
  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault;
  hr = dred->GetPageFaultAllocationOutput(&page_fault);
  if (FAILED(hr)) {
    XELOGW("DRED: page-fault data unavailable (HRESULT 0x{:08X})",
           uint32_t(hr));
  } else if (!page_fault.PageFaultVA) {
    XELOGW("DRED: the removal was not a GPU page fault");
  } else {
    page_fault_captured = true;
    XELOGE("DRED: GPU page fault at virtual address 0x{:016X}",
           uint64_t(page_fault.PageFaultVA));
    uint32_t allocation_count = 0;
    for (const D3D12_DRED_ALLOCATION_NODE* node =
             page_fault.pHeadExistingAllocationNode;
         node; node = node->pNext) {
      ++allocation_count;
      XELOGE("DRED:   live allocation '{}' (type {})",
             node->ObjectNameA ? node->ObjectNameA : "<unnamed>",
             uint32_t(node->AllocationType));
    }
    for (const D3D12_DRED_ALLOCATION_NODE* node =
             page_fault.pHeadRecentFreedAllocationNode;
         node; node = node->pNext) {
      ++allocation_count;
      XELOGE("DRED:   recently freed allocation '{}' (type {})",
             node->ObjectNameA ? node->ObjectNameA : "<unnamed>",
             uint32_t(node->AllocationType));
    }
    if (!allocation_count) {
      XELOGE(
          "DRED:   no tracked allocation - the address belongs to nothing we "
          "own");
    }
  }
  if (!breadcrumbs_captured && !page_fault_captured && !cvars::d3d12_dred) {
    XELOGW("DRED: nothing captured; restart with --d3d12_dred");
  }
  dred->Release();
}

// Check for Intel Arc cards and Intel Graphics iGPUs which use
// the same architecture.
bool D3D12Provider::IsIntelArcGpu() const {
  if (adapter_vendor_id_ != GpuVendorID::kIntel) {
    return false;
  }
  return adapter_description_.starts_with("Intel(R) Arc(TM)") ||
         adapter_description_.starts_with("Intel(R) Graphics");
}

std::unique_ptr<D3D12Provider> D3D12Provider::Create() {
  std::unique_ptr<D3D12Provider> provider(new D3D12Provider);
  if (!provider->Initialize()) {
    xe::FatalError(
        "Unable to initialize Direct3D 12 graphics subsystem.\n"
        "\n"
        "Ensure that you have the latest drivers for your GPU and it supports "
        "Direct3D 12 with the feature level of at least 11_0.\n"
        "\n"
        "See https://xenia.jp/faq/ for more information and a list of "
        "supported GPUs.");
    return nullptr;
  }

  return provider;
}

D3D12Provider::~D3D12Provider() {
  if (graphics_analysis_ != nullptr) {
    graphics_analysis_->Release();
  }
  if (direct_queue_ != nullptr) {
    direct_queue_->Release();
  }
  if (device_ != nullptr) {
    device_->Release();
  }
  if (dxgi_factory_ != nullptr) {
    dxgi_factory_->Release();
  }

  if (cvars::d3d12_debug && pfn_dxgi_get_debug_interface1_) {
    Microsoft::WRL::ComPtr<IDXGIDebug> dxgi_debug;
    if (SUCCEEDED(
            pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&dxgi_debug)))) {
      dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
    }
  }

  if (library_dxil_ != nullptr) {
    FreeLibrary(library_dxil_);
  }
  if (library_d3d12_ != nullptr) {
    FreeLibrary(library_d3d12_);
  }
  if (library_dxgi_ != nullptr) {
    FreeLibrary(library_dxgi_);
  }
}

bool D3D12Provider::EnableIncreaseBasePriorityPrivilege() {
  TOKEN_PRIVILEGES privileges;
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValue(nullptr, SE_INC_BASE_PRIORITY_NAME,
                            &privileges.Privileges[0].Luid)) {
    return false;
  }
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
    return false;
  }
  bool enabled = AdjustTokenPrivileges(token, false, &privileges,
                                       sizeof(privileges), nullptr, nullptr) &&
                 GetLastError() != ERROR_NOT_ALL_ASSIGNED;
  CloseHandle(token);
  return enabled;
}

bool D3D12Provider::Initialize() {
  // Load the core libraries.
  library_dxgi_ = LoadLibraryW(L"dxgi.dll");
  library_d3d12_ = LoadLibraryW(L"D3D12.dll");
  if (!library_dxgi_ || !library_d3d12_) {
    XELOGE("Failed to load dxgi.dll or D3D12.dll");
    return false;
  }
  bool libraries_loaded = true;
  libraries_loaded &=
      (pfn_create_dxgi_factory2_ = PFNCreateDXGIFactory2(
           GetProcAddress(library_dxgi_, "CreateDXGIFactory2"))) != nullptr;
  libraries_loaded &=
      (pfn_dxgi_get_debug_interface1_ = PFNDXGIGetDebugInterface1(
           GetProcAddress(library_dxgi_, "DXGIGetDebugInterface1"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_get_debug_interface_ = PFN_D3D12_GET_DEBUG_INTERFACE(
           GetProcAddress(library_d3d12_, "D3D12GetDebugInterface"))) !=
      nullptr;
  libraries_loaded &=
      (pfn_d3d12_create_device_ = PFN_D3D12_CREATE_DEVICE(
           GetProcAddress(library_d3d12_, "D3D12CreateDevice"))) != nullptr;
  libraries_loaded &=
      (pfn_d3d12_serialize_root_signature_ = PFN_D3D12_SERIALIZE_ROOT_SIGNATURE(
           GetProcAddress(library_d3d12_, "D3D12SerializeRootSignature"))) !=
      nullptr;
  if (!libraries_loaded) {
    XELOGE("Failed to get DXGI or Direct3D 12 functions");
    return false;
  }

  // Load the required DXIL validator (dxil.dll) from the D3D12 folder next to
  // the executable. It signs every shader Mesa emits, which D3D12 rejects
  // unsigned, so offer to download it if it's missing.
  auto d3d12_dir = xe::filesystem::GetExecutablePath().parent_path() / "D3D12";
  {
    EnsureShaderCompilerRuntime(d3d12_dir);

    // Load by full path, since the signer's own plain-name load skips D3D12/.
    auto dxil_path_utf16 = xe::path_to_utf16(d3d12_dir / "dxil.dll");
    library_dxil_ =
        LoadLibraryW(reinterpret_cast<LPCWSTR>(dxil_path_utf16.c_str()));
    if (library_dxil_) {
      XELOGI("Loaded dxil.dll from the D3D12 directory");
    } else {
      // Fall back to the system search path (system-wide, or next to the exe).
      library_dxil_ = LoadLibraryW(L"dxil.dll");
    }
  }
  if (!library_dxil_) {
    XELOGW(
        "Failed to load dxil.dll (error {}), DXIL shaders will be unavailable "
        "- download from "
        "https://github.com/microsoft/DirectXShaderCompiler/releases",
        GetLastError());
  }

  // The D3D12SDKVersion exports make d3d12.dll load D3D12Core.dll at the first
  // device creation, which fails outright if it's missing, so fetch it first.
  std::error_code ec;
  if (!std::filesystem::exists(d3d12_dir / "D3D12Core.dll", ec)) {
    // Returns only on decline or failure. On success it restarts.
    EnsureAgilityRuntime(d3d12_dir);
    if (!std::filesystem::exists(d3d12_dir / "D3D12Core.dll", ec)) {
      XELOGE(
          "The DirectX 12 Agility SDK runtime (D3D12Core.dll) is required but "
          "was not installed");
      return false;
    }
  }

  // Configure the DXGI debug info queue.
  if (cvars::d3d12_break_on_error || cvars::d3d12_break_on_warning) {
    IDXGIInfoQueue* dxgi_info_queue;
    if (SUCCEEDED(pfn_dxgi_get_debug_interface1_(
            0, IID_PPV_ARGS(&dxgi_info_queue)))) {
      if (cvars::d3d12_break_on_error) {
        dxgi_info_queue->SetBreakOnSeverity(
            DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
        dxgi_info_queue->SetBreakOnSeverity(
            DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
      }
      if (cvars::d3d12_break_on_warning) {
        dxgi_info_queue->SetBreakOnSeverity(
            DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, true);
      }
      dxgi_info_queue->Release();
    }
  }

  // Enable the debug layer.
  bool debug = cvars::d3d12_debug;
  if (debug) {
    ID3D12Debug* debug_interface;
    HRESULT debug_interface_hr =
        pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&debug_interface));
    if (SUCCEEDED(debug_interface_hr)) {
      debug_interface->EnableDebugLayer();
      // GPU-based validation catches out-of-bounds shader resource access that
      // the CPU-side layer misses, but is very slow, so keep it opt-in.
      bool gpu_validation = false;
      if (cvars::d3d12_gpu_validation) {
        ID3D12Debug1* debug_interface1;
        HRESULT debug_interface1_hr =
            debug_interface->QueryInterface(IID_PPV_ARGS(&debug_interface1));
        if (SUCCEEDED(debug_interface1_hr)) {
          debug_interface1->SetEnableGPUBasedValidation(TRUE);
          debug_interface1->Release();
          gpu_validation = true;
        } else {
          XELOGW(
              "GPU-based validation was requested but is unavailable, "
              "continuing without it (HRESULT 0x{:08X})",
              uint32_t(debug_interface1_hr));
        }
      }
      debug_interface->Release();
      XELOGI("Direct3D 12 debug layer enabled{}",
             gpu_validation ? " with GPU-based validation" : "");
    } else {
      // The debug layer (D3D12SDKLayers.dll) isn't redistributable on its own.
      // Offer to fetch it from the Agility SDK and restart.
      EnsureDebugLayer(d3d12_dir);
      XELOGW("Failed to enable the Direct3D 12 debug layer (HRESULT 0x{:08X})",
             uint32_t(debug_interface_hr));
      debug = false;
    }
  }

  // Enable Device Removed Extended Data. Must be set before device creation,
  // and unlike the debug layer it does not need the Graphics Tools feature.
  if (cvars::d3d12_dred) {
    ID3D12DeviceRemovedExtendedDataSettings* dred_settings;
    if (SUCCEEDED(
            pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&dred_settings)))) {
      dred_settings->SetAutoBreadcrumbsEnablement(
          D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred_settings->Release();
      XELOGI("Direct3D 12 DRED enabled");
    } else {
      XELOGW("Failed to enable Direct3D 12 DRED");
    }
  }

  // Create the DXGI factory.
  IDXGIFactory2* dxgi_factory;
  if (FAILED(pfn_create_dxgi_factory2_(debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                       IID_PPV_ARGS(&dxgi_factory)))) {
    XELOGE("Failed to create a DXGI factory");
    return false;
  }

  // Choose the adapter.
  uint32_t adapter_index = 0;
  IDXGIAdapter1* adapter = nullptr;
  while (dxgi_factory->EnumAdapters1(adapter_index, &adapter) == S_OK) {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc))) {
      if (SUCCEEDED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                             _uuidof(ID3D12Device), nullptr))) {
        if (cvars::d3d12_adapter >= 0) {
          if (adapter_index == cvars::d3d12_adapter) {
            break;
          }
        } else if (cvars::d3d12_adapter == -2) {
          if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            break;
          }
        } else {
          if (!(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
          }
        }
      }
    }
    adapter->Release();
    adapter = nullptr;
    ++adapter_index;
  }
  if (adapter == nullptr) {
    XELOGE(
        "Failed to get an adapter supporting Direct3D 12 with the feature "
        "level of at least 11_0");
    dxgi_factory->Release();
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc;
  if (FAILED(adapter->GetDesc(&adapter_desc))) {
    XELOGE("Failed to get the DXGI adapter description");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter_vendor_id_ = GpuVendorID(adapter_desc.VendorId);
  adapter_device_id_ = adapter_desc.DeviceId;

  int adapter_name_mb_size = WideCharToMultiByte(
      CP_UTF8, 0, adapter_desc.Description, -1, nullptr, 0, nullptr, nullptr);
  if (adapter_name_mb_size != 0) {
    char* adapter_name_mb =
        reinterpret_cast<char*>(alloca(adapter_name_mb_size));
    if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
                            adapter_name_mb, adapter_name_mb_size, nullptr,
                            nullptr) != 0) {
      adapter_description_ = adapter_name_mb;
      XELOGD3D("DXGI adapter: {} (vendor 0x{:04X}, device 0x{:04X})",
               adapter_name_mb, adapter_desc.VendorId, adapter_desc.DeviceId);
    }
  }

  // Create the Direct3D 12 device.
  ID3D12Device* device;
  if (FAILED(pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&device)))) {
    XELOGE("Failed to create a Direct3D 12 feature level 11_0 device");
    adapter->Release();
    dxgi_factory->Release();
    return false;
  }
  adapter->Release();

  // Safety net: the Agility runtime should provide Shader Model 6.6 for DXIL.
  {
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model;
    shader_model.HighestShaderModel = D3D_SHADER_MODEL_6_6;
    bool shader_model_6_6_supported =
        SUCCEEDED(device->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) &&
        shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
    if (!shader_model_6_6_supported) {
      device->Release();
      dxgi_factory->Release();
      XELOGE(
          "The Direct3D 12 runtime lacks Shader Model 6.6 required for DXIL "
          "shaders");
      return false;
    }
  }

  // Configure the Direct3D 12 debug info queue.
  ID3D12InfoQueue* d3d12_info_queue;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d12_info_queue)))) {
    // Increase message storage limit for debugging.
    d3d12_info_queue->SetMessageCountLimit(1024);
    D3D12_MESSAGE_SEVERITY d3d12_info_queue_denied_severities[] = {
        D3D12_MESSAGE_SEVERITY_INFO,
    };
    D3D12_MESSAGE_ID d3d12_info_queue_denied_messages[] = {
        // Xbox 360 vertex fetch is explicit in shaders.
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT,
        // Bug in the debug layer (fixed in some version of Windows) - gaps in
        // render target bindings must be represented with a fully typed RTV
        // descriptor and DXGI_FORMAT_UNKNOWN in the pipeline state, but older
        // debug layer versions give a format mismatch error in this case.
        D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE,
        // Render targets and shader exports don't have to match on the Xbox
        // 360.
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
        // Arbitrary scissor can be specified by the guest, also it can be
        // explicitly used to disable drawing.
        D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
        // Arbitrary clear values can be specified by the guest.
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
    };
    D3D12_INFO_QUEUE_FILTER d3d12_info_queue_filter = {};
    d3d12_info_queue_filter.DenyList.NumSeverities =
        UINT(xe::countof(d3d12_info_queue_denied_severities));
    d3d12_info_queue_filter.DenyList.pSeverityList =
        d3d12_info_queue_denied_severities;
    d3d12_info_queue_filter.DenyList.NumIDs =
        UINT(xe::countof(d3d12_info_queue_denied_messages));
    d3d12_info_queue_filter.DenyList.pIDList = d3d12_info_queue_denied_messages;
    d3d12_info_queue->PushStorageFilter(&d3d12_info_queue_filter);
    if (cvars::d3d12_break_on_error) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION,
                                           true);
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
    }
    if (cvars::d3d12_break_on_warning) {
      d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,
                                           true);
    }
    d3d12_info_queue->Release();
  }

  // Create the command queue for graphics.
  D3D12_COMMAND_QUEUE_DESC queue_desc;
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (cvars::d3d12_queue_priority >= 2) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    if (!EnableIncreaseBasePriorityPrivilege()) {
      XELOGW(
          "Failed to enable SeIncreaseBasePriorityPrivilege for global "
          "realtime Direct3D 12 command queue priority, falling back to high "
          "priority, try launching Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    }
  } else if (cvars::d3d12_queue_priority >= 1) {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  } else {
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  }
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;
  ID3D12CommandQueue* direct_queue;
  if (FAILED(device->CreateCommandQueue(&queue_desc,
                                        IID_PPV_ARGS(&direct_queue)))) {
    bool queue_created = false;
    if (queue_desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
      XELOGW(
          "Failed to create a Direct3D 12 direct command queue with global "
          "realtime priority, falling back to high priority, try launching "
          "Xenia as administrator");
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      queue_created = SUCCEEDED(
          device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&direct_queue)));
    }
    if (!queue_created) {
      XELOGE("Failed to create a Direct3D 12 direct command queue");
      device->Release();
      dxgi_factory->Release();
      return false;
    }
  }

  dxgi_factory_ = dxgi_factory;
  device_ = device;
  direct_queue_ = direct_queue;

  // Get descriptor sizes for each type.
  for (uint32_t i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i) {
    descriptor_sizes_[i] =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE(i));
  }

  // Check if optional features are supported.
  // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED requires Windows 10 2004 (indicated by
  // the availability of ID3D12Device8 or D3D12_FEATURE_D3D12_OPTIONS7).
  heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_NONE;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                            &options7, sizeof(options7)))) {
    heap_flag_create_not_zeroed_ = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
  }
  ps_specified_stencil_reference_supported_ = false;
  rasterizer_ordered_views_supported_ = false;
  resource_binding_tier_ = D3D12_RESOURCE_BINDING_TIER_1;
  tiled_resources_tier_ = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
  unaligned_block_textures_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                            &options, sizeof(options)))) {
    ps_specified_stencil_reference_supported_ =
        bool(options.PSSpecifiedStencilRefSupported);
    rasterizer_ordered_views_supported_ = bool(options.ROVsSupported);
    resource_binding_tier_ = options.ResourceBindingTier;
    tiled_resources_tier_ = options.TiledResourcesTier;
  }
  programmable_sample_positions_tier_ =
      D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2,
                                            &options2, sizeof(options2)))) {
    programmable_sample_positions_tier_ =
        options2.ProgrammableSamplePositionsTier;
  }
  barycentrics_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,
                                            &options3, sizeof(options3)))) {
    barycentrics_supported_ = bool(options3.BarycentricsSupported);
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8,
                                            &options8, sizeof(options8)))) {
    unaligned_block_textures_supported_ =
        bool(options8.UnalignedBlockTexturesSupported);
  }
  alpha_blend_factor_supported_ = false;
  D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13 = {};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13,
                                            &options13, sizeof(options13)))) {
    alpha_blend_factor_supported_ = bool(options13.AlphaBlendFactorSupported);
  }
  virtual_address_bits_per_resource_ = 0;
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT virtual_address_support;
  if (SUCCEEDED(device->CheckFeatureSupport(
          D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &virtual_address_support,
          sizeof(virtual_address_support)))) {
    virtual_address_bits_per_resource_ =
        virtual_address_support.MaxGPUVirtualAddressBitsPerResource;
  }
  // Check highest supported shader model.
  highest_shader_model_ = 0x51;  // Default to SM 5.1.
  D3D12_FEATURE_DATA_SHADER_MODEL shader_model_support;
  shader_model_support.HighestShaderModel = D3D_SHADER_MODEL_6_6;
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                            &shader_model_support,
                                            sizeof(shader_model_support)))) {
    highest_shader_model_ = uint16_t(shader_model_support.HighestShaderModel);
  }
  XELOGD3D(
      "Direct3D 12 device and OS features:\n"
      "* Highest shader model: {}.{}\n"
      "* Max GPU virtual address bits per resource: {}\n"
      "* Non-zeroed heap creation: {}\n"
      "* Pixel-shader-specified stencil reference: {}\n"
      "* Programmable sample positions: tier {}\n"
      "* Rasterizer-ordered views: {}\n"
      "* Scalar alpha blend factor: {}\n"
      "* Resource binding: tier {}\n"
      "* Tiled resources: tier {}\n"
      "* Unaligned block-compressed textures: {}",
      (highest_shader_model_ >> 4) & 0xF, highest_shader_model_ & 0xF,
      virtual_address_bits_per_resource_,
      (heap_flag_create_not_zeroed_ & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) ? "yes"
                                                                         : "no",
      ps_specified_stencil_reference_supported_ ? "yes" : "no",
      uint32_t(programmable_sample_positions_tier_),
      rasterizer_ordered_views_supported_ ? "yes" : "no",
      alpha_blend_factor_supported_ ? "yes" : "no",
      uint32_t(resource_binding_tier_), uint32_t(tiled_resources_tier_),
      unaligned_block_textures_supported_ ? "yes" : "no");

  // Get the graphics analysis interface, will silently fail if PIX is not
  // attached.
  pfn_dxgi_get_debug_interface1_(0, IID_PPV_ARGS(&graphics_analysis_));
  return true;
}
uint32_t D3D12Provider::CreateUploadResource(
    D3D12_HEAP_FLAGS HeapFlags, _In_ const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState, REFIID riidResource,
    void** ppvResource, const D3D12_CLEAR_VALUE* pOptimizedClearValue) const {
  auto device = GetDevice();

  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, HeapFlags, pDesc,
          InitialResourceState, pOptimizedClearValue, riidResource,
          ppvResource))) {
    XELOGE("Failed to create the gamma ramp upload buffer");
    return UPLOAD_RESULT_CREATE_FAILED;
  }

  return UPLOAD_RESULT_CREATE_SUCCESS;
}
std::unique_ptr<Presenter> D3D12Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D12Presenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> D3D12Provider::CreateImmediateDrawer() {
  return D3D12ImmediateDrawer::Create(*this);
}

void D3D12Provider::LogD3D12DebugMessages() const {
  if (!device_) {
    XELOGE("LogD3D12DebugMessages: No device");
    return;
  }

  ID3D12InfoQueue* info_queue = nullptr;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    XELOGE(
        "LogD3D12DebugMessages: InfoQueue not available (debug layer not "
        "enabled? Use --d3d12_debug)");
    return;
  }

  UINT64 message_count = info_queue->GetNumStoredMessages();
  for (UINT64 i = 0; i < message_count; ++i) {
    SIZE_T message_size = 0;
    if (FAILED(info_queue->GetMessage(i, nullptr, &message_size))) {
      continue;
    }

    D3D12_MESSAGE* message =
        reinterpret_cast<D3D12_MESSAGE*>(alloca(message_size));
    if (FAILED(info_queue->GetMessage(i, message, &message_size))) {
      continue;
    }

    const char* severity_str = "INFO";
    switch (message->Severity) {
      case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        severity_str = "CORRUPTION";
        break;
      case D3D12_MESSAGE_SEVERITY_ERROR:
        severity_str = "ERROR";
        break;
      case D3D12_MESSAGE_SEVERITY_WARNING:
        severity_str = "WARNING";
        break;
      case D3D12_MESSAGE_SEVERITY_INFO:
        severity_str = "INFO";
        break;
      case D3D12_MESSAGE_SEVERITY_MESSAGE:
        severity_str = "MESSAGE";
        break;
    }

    if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
        message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
      XELOGE("D3D12 {}: [ID {}] {}", severity_str,
             static_cast<int>(message->ID), message->pDescription);
    } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
      XELOGW("D3D12 {}: [ID {}] {}", severity_str,
             static_cast<int>(message->ID), message->pDescription);
    } else {
      XELOGI("D3D12 {}: [ID {}] {}", severity_str,
             static_cast<int>(message->ID), message->pDescription);
    }
  }

  info_queue->ClearStoredMessages();
  info_queue->Release();
}

}  // namespace d3d12
}  // namespace ui
}  // namespace xe
