#include "Core/Debugger/FunctionWatch.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <picojson.h>
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoEvents.h"

#include <fstream>
#include <regex>

namespace Core
{

size_t FunctionWatch::n_tracing = 0;

void FunctionWatch::Enable(const Core::System& system)
{
  NOTICE_LOG_FMT(POWERPC, "FunctionWatch::Enable");
  m_VI_end_field_event =
      VIEndFieldEvent::Register([this, &system] { OnFrameEnd(system); }, "FunctionWatch");
}

void FunctionWatch::Disable()
{
  NOTICE_LOG_FMT(POWERPC, "FunctionWatch::Disable");
  m_VI_end_field_event.reset();
}

constexpr std::size_t MAX_SYMBOL_WIDTH = 97;

static std::string TruncateSymbol(const std::string& s)
{
  if (s.size() <= MAX_SYMBOL_WIDTH)
    return s;
  return s.substr(0, MAX_SYMBOL_WIDTH - 3) + "...";
}

static void SanitizeString(std::string& s)
{
  s.erase(std::remove_if(s.begin(), s.end(),
                         [](unsigned char c) {
                           return c == '\0' || c == '\r' || c == '\n' || c == '\t' || c < 0x20;
                         }),
          s.end());
}

std::vector<FunctionWatch::FileMapping> FunctionWatch::LoadSplits(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("Failed to open splits.txt");

  std::vector<FileMapping> result;
  std::string line;
  std::string current_file;

  const std::regex file_re(R"(^\s*([^:]+):\s*$)");
  const std::regex text_re(R"(^\s*\.text\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+))");

  while (std::getline(in, line))
  {
    std::smatch m;

    if (std::regex_match(line, m, file_re))
    {
      current_file = m[1].str();
      SanitizeString(current_file);
      continue;
    }

    if (!current_file.empty() && std::regex_search(line, m, text_re))
    {
      const addr_t start = std::stoul(m[1].str(), nullptr, 16);
      const addr_t end = std::stoul(m[2].str(), nullptr, 16);
      if (start >= end)
        continue;
      result.push_back({current_file, {start, end}});
    }
  }

  return result;
}

// SABE: Credit to KooShnoo for most of the code
// Files need to be/are saved to Source\Core\DolphinQt\

constexpr u32 RAT_BEGIN = 0x800065A0;
constexpr u32 RAT_END = 0x801B4368;
constexpr u32 LIBGC_BEGIN = 0x801F3BF0;
constexpr u32 LIBGC_END = 0x80220548;
constexpr u32 ENGINE_BEGIN = 0x80220548;
constexpr u32 ENGINE_END = 0x803125F4;

struct FuzzyInfo
{
  bool has_value = false;
  double percent = 0.0;
  std::string demangled_name;
};

using FuzzyMap = std::unordered_map<std::string, FuzzyInfo>;

static FuzzyMap LoadFuzzyReport(const std::string& path)
{
  FuzzyMap out;

  std::ifstream in(path);
  if (!in)
    return out;

  picojson::value v;
  in >> v;
  if (!v.is<picojson::object>())
    return out;

  const auto& root = v.get<picojson::object>();
  auto units_it = root.find("units");
  if (units_it == root.end() || !units_it->second.is<picojson::array>())
    return out;

  for (const auto& unit_val : units_it->second.get<picojson::array>())
  {
    if (!unit_val.is<picojson::object>())
      continue;

    const auto& unit = unit_val.get<picojson::object>();
    auto funcs_it = unit.find("functions");
    if (funcs_it == unit.end() || !funcs_it->second.is<picojson::array>())
      continue;

    for (const auto& fn_val : funcs_it->second.get<picojson::array>())
    {
      if (!fn_val.is<picojson::object>())
        continue;

      const auto& fn = fn_val.get<picojson::object>();

      auto name_it = fn.find("name");
      if (name_it == fn.end() || !name_it->second.is<std::string>())
        continue;

      std::string mangled = name_it->second.get<std::string>();
      SanitizeString(mangled);
      if (mangled.empty())
        continue;

      FuzzyInfo fi{};

      auto meta_it = fn.find("metadata");
      if (meta_it != fn.end() && meta_it->second.is<picojson::object>())
      {
        const auto& meta = meta_it->second.get<picojson::object>();
        auto dem_it = meta.find("demangled_name");
        if (dem_it != meta.end() && dem_it->second.is<std::string>())
        {
          fi.demangled_name = dem_it->second.get<std::string>();
          SanitizeString(fi.demangled_name);
        }
      }

      auto fuzzy_it = fn.find("fuzzy_match_percent");
      if (fuzzy_it != fn.end() && fuzzy_it->second.is<double>())
      {
        fi.has_value = true;
        fi.percent = fuzzy_it->second.get<double>();
      }

      auto it = out.find(mangled);
      if (it == out.end())
      {
        out.emplace(mangled, fi);
      }
      else
      {
        auto& cur = it->second;
        if (cur.demangled_name.empty() && !fi.demangled_name.empty())
          cur.demangled_name = fi.demangled_name;

        if (!cur.has_value && fi.has_value)
        {
          cur.has_value = true;
          cur.percent = fi.percent;
        }
        else if (cur.has_value && fi.has_value && fi.percent > cur.percent)
        {
          cur.percent = fi.percent;
        }
      }
    }
  }

  return out;
}

static bool ShouldShowFunction(const std::string& mangled_name, const FuzzyMap& fuzzy,
                               double threshold)
{
  if (threshold <= 0.0)
    return true;

  auto it = fuzzy.find(mangled_name);
  if (it == fuzzy.end() || !it->second.has_value)
    return true;

  return it->second.percent < threshold;
}

static const char* ClassifyCategory(u32 addr, const std::string& file)
{
  if (file.rfind("Rat/", 0) == 0 || file.rfind("Rat\\", 0) == 0)
    return "Rat";
  if (file.rfind("Engine/", 0) == 0 || file.rfind("Engine\\", 0) == 0)
    return "Engine";
  if (file.rfind("LibGC/", 0) == 0 || file.rfind("LibGC\\", 0) == 0)
    return "LibGC";
  if (file.rfind("SB/Game/", 0) == 0 || file.rfind("SB/Core/x/", 0) == 0 ||
      file.rfind("SB/Core/gc/", 0) == 0)
    return "Game";
  if (file.rfind("bink/", 0) == 0)
    return "Bink";
  if (file.rfind("rwsdk/", 0) == 0)
    return "Renderware";
  return "Other";
}

void FunctionWatch::Dump(const Core::System& system)
{
  PPCSymbolDB& symbol_db = system.GetPPCSymbolDB();

  const double FUZZY_THRESHOLD = 100.0;
  const bool SORT_UNDER_FILE_BY_FUZZY = true;

  const FuzzyMap fuzzy = LoadFuzzyReport(".\\report.json");

  struct Entry
  {
    addr_t addr;
    std::string name;
    std::string mangled;
    std::string file;
    std::size_t n_frames;
    std::size_t total_heat;
    bool has_fuzzy;
    double fuzzy;
  };

  std::unordered_map<std::string, std::vector<Entry>> tables;
  auto mappings = LoadSplits(".\\splits.txt");

  for (const auto& kv : m_heatmap)
  {
    const addr_t addr = kv.first;
    const auto& frameMap = kv.second;

    auto* symbol = symbol_db.GetSymbolFromAddr(addr);
    if (!symbol)
      continue;

    const std::string mangled = symbol->function_name;

    if (!ShouldShowFunction(mangled, fuzzy, FUZZY_THRESHOLD))
      continue;

    const std::string* cpp_file = nullptr;
    for (const auto& m : mappings)
    {
      if (addr >= m.text.start && addr < m.text.end)
      {
        cpp_file = &m.file;
        break;
      }
    }
    if (!cpp_file)
      continue;

    const std::string& file = *cpp_file;

    const char* category = ClassifyCategory(addr, file);
    if (std::strcmp(category, "Other") == 0)
      continue;

    std::string name = mangled;
    bool has_fuzzy = false;
    double fuzzy_val = 0.0;

    auto fit = fuzzy.find(mangled);
    if (fit != fuzzy.end())
    {
      if (!fit->second.demangled_name.empty())
        name = fit->second.demangled_name;
      if (fit->second.has_value)
      {
        has_fuzzy = true;
        fuzzy_val = fit->second.percent;
      }
    }

    SanitizeString(name);

    tables[category].push_back(Entry{addr, name, mangled, file, frameMap.size(),
                                     static_cast<std::size_t>(symbol->num_calls), has_fuzzy,
                                     fuzzy_val});
  }

  FILE* out = std::fopen(".\\funcs.tsv", "w");
  if (!out)
  {
    ERROR_LOG_FMT(POWERPC, "Error opening funcs.tsv for writing.");
    return;
  }

  constexpr int ADDR_W = 12;
  constexpr int NAME_W = 99;
  constexpr int FRAMES_W = 10;
  constexpr int HEAT_W = 14;
  constexpr int FILE_W = 45;

  for (auto& [category, entries] : tables)
  {
    if (entries.empty())
      continue;

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      if (a.total_heat != b.total_heat)
        return a.total_heat > b.total_heat;
      return a.name < b.name;
    });

    fmt::print(out, "\n{} - {} total functions\n", category, entries.size());
    fmt::print(out, "{}\n", std::string(80, '='));
    fmt::print(out, "{:<12} {:<99} {:>10} {:>14} {:<45}\n", "addr", "func_name", "n_frames",
               "total_heat", "file");
    fmt::print(out, "{}\n",
               std::string(ADDR_W + 1 + NAME_W + 1 + FRAMES_W + 1 + HEAT_W + 1 + FILE_W, '-'));

    for (const auto& e : entries)
    {
      fmt::print(out, "0x{:08X} {:<99} {:>10} {:>14} {:<45}\n", e.addr, TruncateSymbol(e.name),
                 e.n_frames, e.total_heat, e.file);
    }

    struct FileStats
    {
      std::size_t funcs = 0;
      std::size_t heat = 0;
      std::vector<const Entry*> entries;
    };

    std::unordered_map<std::string, FileStats> per_file;
    for (const auto& e : entries)
    {
      auto& fs = per_file[e.file];
      fs.funcs++;
      fs.heat += e.total_heat;
      fs.entries.push_back(&e);
    }

    std::vector<std::pair<std::string, FileStats*>> files;
    for (auto& [fname, fs] : per_file)
      files.emplace_back(fname, &fs);

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
      if (a.second->heat != b.second->heat)
        return a.second->heat > b.second->heat;
      return a.first < b.first;
    });

    fmt::print(out, "\n-- File priority (by total_heat) -- file count: {}\n", files.size());

    for (const auto& [fname, fs] : files)
    {
      fmt::print(out, "\n  {:<45} funcs:{:>5}  heat:{:>10}\n", fname, fs->funcs, fs->heat);

      std::sort(fs->entries.begin(), fs->entries.end(), [&](const Entry* a, const Entry* b) {
        if (SORT_UNDER_FILE_BY_FUZZY)
        {
          const double fa = a->has_fuzzy ? a->fuzzy : 0.0;
          const double fb = b->has_fuzzy ? b->fuzzy : 0.0;
          if (fa != fb)
            return fa > fb;
        }
        if (a->total_heat != b->total_heat)
          return a->total_heat > b->total_heat;
        return a->name < b->name;
      });

      for (const Entry* e : fs->entries)
      {
        fmt::print(out, "    0x{:08X} {:<97} heat:{:>8}  fuzzy:{}\n", e->addr,
                   TruncateSymbol(e->name), e->total_heat,
                   e->has_fuzzy ? fmt::format("{:.2f}%", e->fuzzy) : "N/A");
      }
    }
  }

  std::fclose(out);
}

bool FunctionWatch::IsMagma(u32 addr)
{
  return m_magma_addrs.count(addr) != 0;
}

void FunctionWatch::OnFrameEnd(const Core::System& system)
{
  auto& symDB = system.GetPPCSymbolDB();
  size_t i = 0;
  size_t magmas_ignored = 0;

  symDB.ForEachSymbolWithMutation([&](Common::Symbol& symbol) {
    if (symbol.num_calls_this_frame != 0)
    {
      m_heatmap[symbol.address][g_presenter->FrameCount()] = symbol.num_calls_this_frame;
      symbol.num_calls += symbol.num_calls_this_frame;

      if (symbol.num_calls > 1'000'000 || symbol.num_calls_this_frame > 1'000)
      {
        m_magma_addrs.insert(symbol.address);
        magmas_ignored++;
      }
      symbol.num_calls_this_frame = 0;
      i++;
    }
  });

  NOTICE_LOG_FMT(POWERPC, "{}/{} fns ({} magma) hit frame {}", i, n_tracing, magmas_ignored,
                 g_presenter->FrameCount());
}

size_t FunctionWatch::MagmaCount()
{
  return m_magma_addrs.size();
}

}  // namespace Core
