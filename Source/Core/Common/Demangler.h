
#pragma once

#include <string>
#include <vector>

// Credit to Arookas - https://github.com/intns/mapdas/blob/main/Helpers/Demangler.cs

namespace demangler
{

class Demangler
{
public:
  static std::string Demangle(const std::string& symbol);

private:
  // ------------------------------------------------------------
  // StringStream
  // ------------------------------------------------------------
  class StringStream
  {
  public:
    explicit StringStream(const std::string& data);

    char Read();
    char Peek() const;
    char operator[](int index) const;

    int Position = 0;
    int Length() const;

  private:
    std::string Data;
  };

  // ------------------------------------------------------------
  // Component model
  // ------------------------------------------------------------
  enum class ComponentType
  {
    Const,
    Pointer,
    Reference,
    Unsigned,
    Ellipsis,
    Void,
    Bool,
    Char,
    WChar,
    Short,
    Int,
    Long,
    LongLong,
    Float,
    Double,
    Type,
    Func,
    Array
  };

  struct ComponentInfo
  {
    ComponentType type;
    int length;        // array dimension
    std::string name;  // type or return type
    std::string prms;  // parameters

    explicit ComponentInfo(ComponentType t);
    explicit ComponentInfo(int array_len);
    explicit ComponentInfo(const std::string& type_name);
    ComponentInfo(const std::string& params, const std::string& ret);
    ComponentInfo(ComponentType t, int len, const std::string& n, const std::string& p);
  };

  // ------------------------------------------------------------
  // Demangling helpers
  // ------------------------------------------------------------
  static void DemangleTemplate(StringStream& input, std::string& output);
  static void DemangleComponents(const std::vector<ComponentInfo>& components, size_t start,
                                 std::string& output);
  static void DemangleType(StringStream& input, std::string& output);
  static int ScanNameEnd(const StringStream& input);
  static std::string DemangleName(StringStream& input);
};

}  // namespace demangler
