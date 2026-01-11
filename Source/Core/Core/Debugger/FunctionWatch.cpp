#include "Core/Debugger/FunctionWatch.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <numeric>
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

void FunctionWatch::Dump(const Core::System& system)
{

  //// this is probably a terrible way to serialize :/
  //// this code can break and segfault easily, however, i do not care! :)
  //// btw u are assumed to be using little endian
  //// this should prolly use some vecctor or other stl dynalloc shiz 
  //// but idc i just wanna get this data serialized and outta here so i dont have to use cpp anymore
  //size_t buffer_size = 0;

  //for (const auto& [addr, frameMap] : m_heatmap)
  //{
  //  buffer_size += frameMap.size() * (sizeof(framenum_t) + sizeof(hit_count_t));
  //  buffer_size += sizeof(addr_t) + sizeof(u32);
  //}

  //buffer_size += 4 * sizeof(u32);
  //constexpr const char header[32] = "idk how to serialize things";
  //buffer_size += sizeof(header);

  //u32* buffer = (u32*) malloc(buffer_size);
  //size_t write_head = 0;
  //memcpy(buffer, header, sizeof(header));
  //write_head += 8;
  //buffer[write_head] = (u32) m_heatmap.size(); write_head++;
  //for (const auto& [addr, frameMap] : m_heatmap) {
  //  buffer[write_head] = addr; write_head++;
  //  buffer[write_head] = (u32) frameMap.size(); write_head++;
  //  for (const auto& [framenum, heat] : frameMap) {
  //    buffer[write_head] = framenum; write_head++;
  //    buffer[write_head] = heat; write_head++;
  //  }
  //}

  //// ~~12 bytes short!~~ scratch that LOTS of KILObytes short LOL
  //NOTICE_LOG_FMT(POWERPC, "Exporting FWFW data. buffer_size: {}, final write_head: {}", buffer_size, write_head);

  //std::ofstream outFile("funcs.bin", std::ios::binary);
  //if (!outFile) {
  //  ERROR_LOG_FMT(POWERPC, "Error opening file for writing.\n");
  //}
  //outFile.write((char*) buffer, buffer_size);
  //outFile.close();
  //NOTICE_LOG_FMT(POWERPC, "Data written successfully.\n");
  //free(buffer);

  PPCSymbolDB& symbol_db = system.GetPPCSymbolDB();

  // Address ranges — EXACTLY from OnFrameEnd

  struct Entry
  {
    addr_t addr;
    std::string name;  // demangled
    std::string file;  // cpp file
    std::size_t n_frames;
    std::size_t total_heat;
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

  auto mappings = LoadSplits(".\\splits.txt"); // Put splits.txt in the same folder

  for (const auto& [addr, frameMap] : m_heatmap)
  {
    auto* symbol = symbol_db.GetSymbolFromAddr(addr);
    if (!symbol)
      continue;

    const std::string* file = FindFileForAddress(addr, mappings);

    std::string demangled = demangler::Demangler::Demangle(symbol->function_name);

    // Fallback for autogenerated sinit-style symbols
    if (demangled == "int::")
    {
      demangled = symbol->function_name;
    }

    Entry e{
        addr,
        demangled,
        file ? *file : "<unknown>",
        frameMap.size(),
        static_cast<std::size_t>(symbol->num_calls),
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

  auto* file = std::fopen(
      ".\\funcs.tsv", "w");

  if (!file)
  {
    ERROR_LOG_FMT(POWERPC, "Error opening funcs.tsv for writing.\n");
    return;
  }

  constexpr int ADDR_W = 12;
  constexpr int NAME_W = 99;
  constexpr int FRAMES_W = 10;
  constexpr int HEAT_W = 14;
  constexpr int FILE_W = 45;

  for (auto& table : tables)
  {
    if (table.entries.empty())
      continue;

    std::sort(table.entries.begin(), table.entries.end(), [](const Entry& a, const Entry& b) {
      if (a.total_heat != b.total_heat)
        return a.total_heat > b.total_heat;

      return a.name < b.name;
    });

    fmt::print(file, "\n{} - {} total functions\n", table.title, table.entries.size());
    fmt::print(file, "{}\n", std::string(80, '='));

    fmt::print(file, "{:<12} {:<99} {:>10} {:>14} {:<45}\n", "addr", "func_name", "n_frames",
               "total_heat", "file");

    fmt::print(file, "{}\n",
               std::string(ADDR_W + 1 + NAME_W + 1 + FRAMES_W + 1 + HEAT_W + 1 + FILE_W, '-'));

    for (const auto& e : table.entries)
    {
      fmt::print(file, "0x{:08X} {:<99} {:>10} {:>14} {:<45}\n", e.addr, TruncateSymbol(e.name),
                 e.n_frames, e.total_heat, e.file);
    }

    struct FileStats
    {
      std::size_t funcs = 0;
      std::size_t heat = 0;
    };

    std::unordered_map<std::string, FileStats> per_file;

    for (const auto& e : table.entries)
    {
      auto& fs = per_file[e.file];
      fs.funcs++;
      fs.heat += e.total_heat;
    }

    std::vector<std::pair<std::string, FileStats>> files;
    files.reserve(per_file.size());

    for (auto& it : per_file)
      files.push_back(it);

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
      if (a.second.heat != b.second.heat)
        return a.second.heat > b.second.heat;

      return a.first < b.first;
    });

    fmt::print(file, "\n-- File priority (by total_heat) -- file count: {}\n", files.size());

    for (const auto& [fname, stats] : files)
    {
      fmt::print(file, "{:<45} funcs:{:>5}  heat:{:>10}\n", fname, stats.funcs, stats.heat);
    }
  }

  std::fclose(file);
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
