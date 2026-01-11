// Framewise Function Watch Tool

#pragma once

#include <unordered_map>
#include <unordered_set>
#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "Core/System.h"

namespace Core {

/// Framewise Function Watch Tool
class FunctionWatch {
private:
    // maps [function_address : [framenum : hit_count]]
    std::unordered_map<u32, std::unordered_map<u32, u32>> m_heatmap;
    using hit_count_t = decltype(m_heatmap)::value_type::second_type::value_type::second_type;
    using framenum_t = decltype(m_heatmap)::value_type::second_type::key_type;
    using addr_t = decltype(m_heatmap)::key_type;

public:
    struct TextRange
    {
      addr_t start;
      addr_t end;
    };

    struct FileMapping
    {
      std::string file;
      TextRange text;
    };

    static size_t n_tracing;

    void Enable(const Core::System& system);
    void Disable();
    bool Enabled() const { return m_VI_end_field_event != nullptr; }
    void Dump(const Core::System& system);
    bool IsMagma(u32 addr);
    size_t MagmaCount();
private:
    void OnFrameEnd(const Core::System& system);
    std::vector<FileMapping> LoadSplits(const std::string& path);

    // these are memory addresses of functions that have been identified as running too often, and are therefore ignored in fwfw analysis
    std::unordered_set<u32> m_magma_addrs;

    Common::EventHook m_VI_end_field_event;
};

}
