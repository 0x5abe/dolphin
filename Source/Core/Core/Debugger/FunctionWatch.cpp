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
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/Present.h"
#include "Common/Demangler.h"

// #include <iostream>
#include <fstream>
#include <regex>

namespace Core
{

size_t FunctionWatch::n_tracing = 0;

void FunctionWatch::Enable(const Core::System& system)
{
  NOTICE_LOG_FMT(POWERPC, "FunctionWatch::Enable");
  m_VI_end_field_event = VIEndFieldEvent::Register([this,&system] { OnFrameEnd(system); }, "FunctionWatch");
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
                           // Remove characters that can break TSV / C IO
                           return c == '\0'     // NUL (kills output)
                                  || c == '\r'  // carriage return
                                  || c == '\n'  // newline inside fields
                                  || c == '\t'  // tabs break alignment
                                  || c < 0x20;  // other ASCII control chars
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

  std::regex file_re(R"(^(.+\.cpp):)");
  std::regex text_re(R"(\.text\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+))");

  while (std::getline(in, line))
  {
    std::smatch m;

    if (std::regex_search(line, m, file_re))
    {
      current_file = m[1];
      SanitizeString(current_file);
      continue;
    }

    if (!current_file.empty() && std::regex_search(line, m, text_re))
    {
      addr_t start = std::stoul(m[1], nullptr, 16);
      addr_t end = std::stoul(m[2], nullptr, 16);

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
  bool has_value;  // true if fuzzy_match_percent exists
  double percent;  // valid only if has_value == true
};

static std::unordered_map<u32, FuzzyInfo> LoadFuzzyReport(const std::string& path)
{
  std::unordered_map<u32, FuzzyInfo> out;

  std::ifstream in(path);
  if (!in)
    return out;  // missing report.json => treat all as "not attempted"

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

      // --- virtual_address (required) ---
      auto meta_it = fn.find("metadata");
      if (meta_it == fn.end() || !meta_it->second.is<picojson::object>())
        continue;

      const auto& meta = meta_it->second.get<picojson::object>();
      auto va_it = meta.find("virtual_address");
      if (va_it == meta.end())
        continue;

      u32 addr = 0;
      if (va_it->second.is<std::string>())
        addr = static_cast<u32>(std::stoul(va_it->second.get<std::string>(), nullptr, 10));
      else if (va_it->second.is<double>())
        addr = static_cast<u32>(va_it->second.get<double>());
      else
        continue;

      // --- fuzzy_match_percent (optional) ---
      FuzzyInfo fi{};
      auto fuzzy_it = fn.find("fuzzy_match_percent");
      if (fuzzy_it != fn.end() && fuzzy_it->second.is<double>())
      {
        fi.has_value = true;
        fi.percent = fuzzy_it->second.get<double>();
      }

      out[addr] = fi;
    }
  }

  return out;
}

static bool ShouldShowFunction(u32 addr, const std::unordered_map<u32, FuzzyInfo>& fuzzy,
                               double threshold)
{
  // threshold == 0 => show everything
  if (threshold <= 0.0)
    return true;

  auto it = fuzzy.find(addr);

  // No fuzzy_match_percent => NOT attempted => ALWAYS show
  if (it == fuzzy.end() || !it->second.has_value)
    return true;

  // Has fuzzy_match_percent => show ONLY if below threshold
  return it->second.percent < threshold;
}

void FunctionWatch::Dump(const Core::System& system)
{
  PPCSymbolDB& symbol_db = system.GetPPCSymbolDB();

  // ----------------------------
  // CONFIG
  // ----------------------------
  const double FUZZY_THRESHOLD = 95.0;         // 0 = show everything
  const bool SORT_UNDER_FILE_BY_FUZZY = false;  // toggle here

  // ----------------------------
  // Load fuzzy report
  // ----------------------------
  const auto fuzzy = LoadFuzzyReport(".\\report.json");

  struct Entry
  {
    addr_t addr;
    std::string name;
    std::string file;
    std::size_t n_frames;
    std::size_t total_heat;
    bool has_fuzzy;
    double fuzzy;  // valid iff has_fuzzy
  };

  struct Table
  {
    const char* title;
    addr_t begin;
    addr_t end;
    std::vector<Entry> entries;
  };

  Table tables[] = {
      {"Engine", ENGINE_BEGIN, ENGINE_END, {}},
      {"LibGC", LIBGC_BEGIN, LIBGC_END, {}},
      {"Rat", RAT_BEGIN, RAT_END, {}},
  };

  auto mappings = LoadSplits(".\\splits.txt");

  // ----------------------------
  // Collect + filter
  // ----------------------------
  for (const auto& [addr, frameMap] : m_heatmap)
  {
    auto* symbol = symbol_db.GetSymbolFromAddr(addr);
    if (!symbol)
      continue;

    if (!ShouldShowFunction(addr, fuzzy, FUZZY_THRESHOLD))
      continue;

    const std::string* cpp_file = FindFileForAddress(addr, mappings);

    std::string demangled = demangler::Demangler::Demangle(symbol->function_name);
    if (demangled == "int::")
      demangled = symbol->function_name;

    bool has_fuzzy = false;
    double fuzzy_val = 0.0;
    if (auto it = fuzzy.find(addr); it != fuzzy.end() && it->second.has_value)
    {
      has_fuzzy = true;
      fuzzy_val = it->second.percent;
    }

    Entry e{
        addr,
        demangled,
        cpp_file ? *cpp_file : "<unknown>",
        frameMap.size(),
        static_cast<std::size_t>(symbol->num_calls),
        has_fuzzy,
        fuzzy_val,
    };

    for (auto& table : tables)
    {
      if (addr >= table.begin && addr < table.end)
      {
        table.entries.push_back(std::move(e));
        break;
      }
    }
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

  // ----------------------------
  // Emit
  // ----------------------------
  for (auto& table : tables)
  {
    if (table.entries.empty())
      continue;

    std::sort(table.entries.begin(), table.entries.end(), [](const Entry& a, const Entry& b) {
      if (a.total_heat != b.total_heat)
        return a.total_heat > b.total_heat;
      return a.name < b.name;
    });

    fmt::print(out, "\n{} - {} total functions\n", table.title, table.entries.size());
    fmt::print(out, "{}\n", std::string(80, '='));

    fmt::print(out, "{:<12} {:<99} {:>10} {:>14} {:<45}\n", "addr", "func_name", "n_frames",
               "total_heat", "file");

    fmt::print(out, "{}\n",
               std::string(ADDR_W + 1 + NAME_W + 1 + FRAMES_W + 1 + HEAT_W + 1 + FILE_W, '-'));

    for (const auto& e : table.entries)
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
    for (const auto& e : table.entries)
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

      // ----------------------------
      // CORRECT fuzzy sorting
      // ----------------------------
      std::sort(fs->entries.begin(), fs->entries.end(), [&](const Entry* a, const Entry* b) {
        if (SORT_UNDER_FILE_BY_FUZZY)
        {
          const double fa = a->has_fuzzy ? a->fuzzy : 0.0;
          const double fb = b->has_fuzzy ? b->fuzzy : 0.0;

          if (fa != fb)
            return fa > fb;  // HIGHER fuzzy first
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

bool FunctionWatch::IsMagma(u32 addr) {
  return m_magma_addrs.count(addr) != 0;
}

void FunctionWatch::OnFrameEnd(const Core::System& system)
{
  auto& symDB = system.GetPPCSymbolDB();
  size_t i = 0;
  size_t magmas_ignored = 0;
  symDB.ForEachSymbolWithMutation([&](Common::Symbol& symbol) {
    if (!(symbol.address >= RAT_BEGIN && symbol.address < RAT_END) &&    // ! Rat
        !(symbol.address >= LIBGC_BEGIN && symbol.address < LIBGC_END) && // ! LibGC
        !(symbol.address >= ENGINE_BEGIN && symbol.address < ENGINE_END))   // ! Engine
    {
      return;
    }
    if (symbol.num_calls_this_frame != 0)
    {
      m_heatmap[symbol.address][g_presenter->FrameCount()] = symbol.num_calls_this_frame;
      symbol.num_calls += symbol.num_calls_this_frame;


      // cancel tracing for magma functions
      // wehre a magma function is a function that has a high heat
      // where heat is amount of hits
      // where a hit is an execution
      // so, a magma funciton is a function that is executed often
      // so, this block cancels tracing fro functions that are executed often.
      // TODO: make this condition customizable in the to-be-created Function Watch Dialog UI (like
      // Branch Watch Dialog)
      // TODO: keep track of magma fns and show the user in the FWFW dialog so they can choose to
      // trace them anyway, or manually mark other fns as magma
      if (symbol.num_calls > 1'000'000 || symbol.num_calls_this_frame > 1'000)
      {
        m_magma_addrs.insert(symbol.address);
        magmas_ignored++;
      }
      symbol.num_calls_this_frame = 0;

      i++;
    }
  });
  NOTICE_LOG_FMT(POWERPC, "{}/{} fns ({} magma) hit frame {}", i, n_tracing, magmas_ignored, g_presenter->FrameCount());
}

size_t FunctionWatch::MagmaCount() {
  return m_magma_addrs.size();
}

}  // namespace Core
